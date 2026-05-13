# Implementation plan: `parallel := true` in TIGER loader

## Goal

Replace the standalone shell script [scripts/parallel_download_state.sh](../scripts/parallel_download_state.sh) with a platform-agnostic C++ implementation built into the existing loader table functions. Default behavior changes to use the parallel download + local ingest path; pass `parallel := false` to fall back to the current `/vsicurl/` serial path for benchmarking/comparison.

## Constraints

- **Modify existing functions, do not add new ones.** The functions to update:
  - `load_tiger_state(state, source := NULL, year := 2025, target_db := NULL, target_schema := 'tiger', temp_dir := NULL, build_containment := true, parallel := true)`
  - `load_tiger_states(states[], source := NULL, year := 2025, target_db := NULL, target_schema := 'tiger', temp_dir := NULL, build_containment := true, parallel := true)`
  - `load_tiger_all_states(source := NULL, year := 2025, target_db := NULL, target_schema := 'tiger', temp_dir := NULL, build_containment := true, parallel := true)`
  - `load_tiger_nation(source := NULL, year := 2025, target_db := NULL, target_schema := 'tiger', temp_dir := NULL, parallel := true)` — **also gets the parallel path** so we can completely eliminate `/vsicurl/` from the loader. Fixes the [Windows MSVC `/vsizip/{/vsicurl/...}` SSL-cert-chain issue](https://github.com/duckdb/duckdb-spatial/issues/270) for free.
- `parallel := true` semantics:
  1. For each state in the list: create a temp dir, download all county zips for that state in parallel via DuckDB's bundled HTTP client (cpp-httplib), call the existing local-source ingest path against the temp dir, then `rm -rf` the temp dir before moving to the next state.
  2. Disk usage bounded at `max(state_size)` rather than `sum(state_sizes)` — critical for `load_tiger_all_states` (full nationwide load would otherwise need ~50 GB peak instead of ~10 GB peak for TX).
- `parallel := false` semantics: identical to today's `/vsicurl/`-only behavior. Useful for A/B benchmarking.
- **Platform-agnostic**: no `curl`/`xargs`/`grep`/shell deps. C++17 `std::filesystem` for paths/cleanup, DuckDB's bundled `httplib::Client` for HTTP.
- **Cross-platform path handling**: works on Linux/macOS/Windows (MSVC + MinGW).
- **`source` parameter**: when caller passes their own local source dir, the `parallel` flag is irrelevant (we already have local files). The parallel path only kicks in when `source` is NULL (i.e., HTTP mode).
- **`temp_dir` parameter**: when set, parallel downloads land there; when NULL, use the OS temp dir.

## Background: why this is worth doing

Empirical bench on a single state (NJ, 21 counties, ~835 MB of zips, M4 Max):
- `/vsicurl/` serial: **116s**, peak RSS 9.06 GB, 1.4 GB final disk.
- Parallel download (via shell script) + local ingest: **53s** total (30s download + 23s ingest), peak RSS 9.11 GB, 2.3 GB peak disk (zips + DB), 1.4 GB final after zips deleted.

**~2.2× speedup**, no RSS regression. The bottleneck in the serial path is GDAL not exploiting HTTP Range requests for shapefile reads (empirically verified — see `CLAUDE.md` § Loader performance).

**Windows correctness as a bonus.** A user reported `CALL load_tiger_nation();` fails on Windows MSVC with `Could not open GDAL dataset at: /vsizip/{/vsicurl/https://...}/`. Investigation (May 2026) traced it to GDAL's bundled curl+OpenSSL on Windows being unable to verify `www2.census.gov`'s cert chain — no `/etc/ssl/certs`, vcpkg's static OpenSSL ships no CA bundle, GDAL's curl doesn't read the Windows native cert store. Same root cause documented in QGIS #28557 and acknowledged by the duckdb-spatial maintainer ([duckdb-spatial #270](https://github.com/duckdb/duckdb-spatial/issues/270)). Switching off `/vsicurl/` to `httpfs`-backed downloads (which uses mbedTLS with bundled certs and works on Windows) sidesteps this entirely.

## Existing loader internals to integrate with

[src/loader.cpp](../src/loader.cpp) is the relevant file. Key existing pieces:
- The `StatePlan` struct used as shared bind-data across all three `load_tiger_*` TableFunctions.
- `BuildVsiPath` (the `<source>/<SUBDIR>/<zip>.zip/<inner>` Census-nested layout assembly).
- The serial per-county INSERT loop and the loader_progress checkpoint scheme.
- The post-state derived-table builds + ZCTA clip + edge_containment hook.

The PARALLEL path will download zips into a state-specific subdirectory under `temp_dir` (or OS tempdir) **matching the Census-nested layout that the existing local-source ingest expects** (`<dir>/EDGES/`, `<dir>/FACES/`, `<dir>/FEATNAMES/`, `<dir>/ADDR/`, `<dir>/PLACE/`, `<dir>/COUSUB/`). Once downloads finish, the existing local-source code path just works on this dir.

The county enumeration question: the shell script scrapes the Census HTML directory index (`https://www2.census.gov/geo/tiger/TIGER2025/EDGES/`) to discover which counties exist per state. We have to do the same in C++ since hard-coding the (state, county) FIPS list breaks when Census adjusts. The cpp-httplib client returns HTML, then a regex extracts `tl_2025_<FIPS>_<table>.zip` filenames.

## DuckDB internals available

- `httplib::Client` is vendored under `duckdb/third_party/httplib/` (or similar).
- DuckDB's `FileSystem` / `LocalFileSystem` for cross-platform path ops.
- `Connection::Query` for the SQL ingest part (already used elsewhere in loader.cpp).
- `HTTPUtil::Get(db)` — backed by httpfs when loaded; returns built-in client otherwise. Use this rather than including `httplib.hpp` directly so TLS works on Windows for free.

---

## 1. Files to modify / create

### Single new C++ pair (recommended)

The work is large enough (HTML scraper, downloader pool, per-state dir lifecycle) that inlining it into `loader.cpp` (already 1,536 lines) will make that file hard to navigate. Carve out one focused unit:

- **`src/include/us_geocoder_parallel_download.hpp`** (new, ~40 lines) — public surface used by `loader.cpp`.
- **`src/parallel_download.cpp`** (new, ~350-450 lines) — implementation: HTML index scrape, thread-pool downloader, idempotency, retry, cleanup.
- **`src/loader.cpp`** (modify) — bind, glue, and the per-state flow change in `DoLoadState`.
- **`CMakeLists.txt`** (modify line 213-216) — add `src/parallel_download.cpp` to `EXTENSION_SOURCES`.

### Modifications to `loader.cpp` by line range

| Lines | Change |
| --- | --- |
| 1-19 | Add `#include "us_geocoder_parallel_download.hpp"`. |
| 119-132 (`LoaderBindData` body) | Add `bool parallel = true; std::string temp_dir; int32_t parallel_workers = 16;` fields. Update `Copy()` (134-144) and `Equals()` (146-150) accordingly. |
| 550-577 (`ApplyYearSourceTarget`) | Read `parallel` and `temp_dir` named params; default `parallel = true` only when `bind.source` was *not* provided by the caller (i.e. HTTP source mode). When the user passes a local source, force `bind.parallel = false` silently (`parallel` is a no-op there — see "semantics" §below). |
| 902-1118 (`DoLoadState`) | Insert a *prelude* block before the existing state-level/county loops: if `bind.parallel` and source is HTTP, build a `ParallelStatePlan`, call `DownloadStateZips(...)`, swap `bind.source` for the local temp dir for the remainder of *this state's* execution, then `RemoveDirectory(temp_dir)` at the end. The existing local-source ingest path below it then "just works" against the temp dir. Detailed flow in §3. |
| 1426-1432 (`AddLoaderNamedParams`) | Register the two new named parameters: `parallel := BOOLEAN`, `temp_dir := VARCHAR`, `parallel_workers := INTEGER` (optional knob — see §5). |
| 651-712 (`DoLoadNation` / `LoadTigerNationExecute`) | Add a parallel prelude identical in shape to `DoLoadState`'s. Nation has 3 hard-coded zips (`STATE/tl_<year>_us_state.zip`, `COUNTY/tl_<year>_us_county.zip`, `ZCTA520/tl_<year>_us_zcta520.zip`) — no scraping needed. ZCTA520 is ~0.5 GB, the other two ~10 MB each. Parallel-3 isn't a big perf win but eliminates `/vsicurl/` everywhere, which fixes the Windows MSVC cert-chain issue (see § Background) and reduces our cross-platform support surface. |

No other functions need to change. The post-state derived-table build, ZCTA clip, edge_containment, ANALYZE and progress ledger all happen *after* the per-state ingest, so they're agnostic to whether the zips were local-mirror or `/vsicurl/`.

---

## 2. New helper function signatures (header)

```cpp
// src/include/us_geocoder_parallel_download.hpp
namespace duckdb { namespace us_geocoder {

struct ParallelDownloadOptions {
    std::string source_root;     // e.g. "https://www2.census.gov/geo/tiger/TIGER2025"
    std::string statefp;         // 2-digit
    std::string state_abbrev;    // for logging
    int year = 2025;
    std::string dest_dir;        // absolute path; will be created
    int workers = 16;
    int max_attempts = 3;        // (2s, 5s, 15s backoff like RetryableExecuteInsert)
};

struct ParallelDownloadResult {
    size_t files_total = 0;
    size_t files_downloaded = 0;
    size_t files_skipped = 0;    // already on disk
    size_t bytes_downloaded = 0;
    double elapsed_seconds = 0;
};

// Throws IOException on unrecoverable failure (any file fails all retries).
// Lays out Census-nested layout under dest_dir:
//   dest_dir/PLACE/tl_<year>_<fips>_place.zip
//   dest_dir/COUSUB/tl_<year>_<fips>_cousub.zip
//   dest_dir/EDGES/tl_<year>_<fips><cfp>_edges.zip     (scraped)
//   dest_dir/FACES/...                                  (scraped)
//   dest_dir/FEATNAMES/...                              (scraped)
//   dest_dir/ADDR/...                                   (scraped)
ParallelDownloadResult
DownloadStateZips(ClientContext &context, const ParallelDownloadOptions &opts);

}} // namespace
```

### Internal helpers in `parallel_download.cpp` (not exposed)

- `static std::vector<std::string> ScrapeCensusIndex(ClientContext&, const std::string &url, const std::regex &name_re);`
  Issues a `GET` on the directory URL via `HTTPUtil::Get(db).InitializeClient(...)` (reuses the public DuckDB HTTP abstraction; httpfs supplies SSL when loaded). Matches `tl_<year>_<fips><cfp>_<table>.zip` filenames from the Apache index HTML using `std::regex`. Returns sorted-unique vector.

- `static bool DownloadOneFile(ClientContext&, const std::string &url, const std::string &dest_path, int max_attempts, std::string &err_out);`
  Streams the body via `GetRequestInfo`'s `content_handler` callback (avoid buffering the whole zip in RAM — `addr` zips can be 50 MB+). Writes to `dest_path + ".tmp"`, atomic-renames on success. Retries on 408/429/5xx + transport errors with exponential backoff (2s, 5s, 15s). Idempotent: skips if `FileExists(dest_path) && GetFileSize(dest_path) > 0`.

- `static std::string ResolveTempBase(ClientContext&, const std::string &explicit_temp_dir);`
  If `explicit_temp_dir` non-empty, return it. Else: try `FileSystem::GetEnvVariable("TMPDIR")` (Unix), then `"TEMP"` / `"TMP"` (Windows), else fall back to `"."` (working dir) with a warning. Avoids C++17 `<filesystem>` dependency in case the extension is built with older toolchains.

- `static std::string MakeStateTempDir(FileSystem&, const std::string &base, const std::string &state_abbrev, int year);`
  Returns `<base>/us_geocoder_tiger_<YEAR>_<STATE>_<pid>_<ts>` (collision-resistant if two CALLs run concurrently — both use distinct dirs).

---

## 3. Lifecycle: how `parallel := true` flows

The existing `LoadTigerStateExecute` (line 1120) drives N states sequentially; per-state work is in `DoLoadState`. We hook in only at `DoLoadState`, so all three TableFunctions (`load_tiger_state`, `load_tiger_states`, `load_tiger_all_states`) share the change.

**Bind phase** (`LoadTigerStateBind` / `LoadTigerStatesBind` / `LoadTigerAllStatesBind`): the existing call to `ApplyYearSourceTarget` is extended to read `parallel` and `temp_dir`. No new bind logic — bind just records the flags onto `LoaderBindData`.

**Init phase** (`LoaderGlobalState::Init`): unchanged. The init can't create per-state temp dirs yet because the same `LoaderGlobalState` is shared across all N states.

**Execute phase**: `LoadTigerStateExecute` iterates states (existing line 1129-1142). Per state, it calls `DoLoadState`. Inside `DoLoadState` (line 902), the new prelude:

```cpp
if (bind.parallel && source_is_http(bind.source) && !state_already_fully_loaded) {
    // 1. Resolve temp base.
    auto temp_base = ResolveTempBase(context, bind.temp_dir);
    auto state_dir = MakeStateTempDir(fs, temp_base, state.abbrev, bind.year);

    // 2. Make a *local* copy of bind with source/parallel patched.
    LoaderBindData local_bind = bind;          // copy semantics — already shallow
    local_bind.source = state_dir;             // local-source mode for the rest of this state
    local_bind.parallel = false;               // belt-and-suspenders; prevents recursion

    // 3. Download into state_dir (with RAII cleanup).
    ParallelDownloadOptions opts{ bind.source, state.fips, state.abbrev, bind.year,
                                  state_dir, bind.parallel_workers, /*max_attempts=*/3 };
    StateDirCleanup raii(fs, state_dir);       // unique_ptr-style RAII: RemoveDirectory on dtor
    auto dl = DownloadStateZips(context, opts);
    out.push_back({"parallel_download:" + state.abbrev, (int64_t)dl.files_downloaded});

    // 4. Re-enter the existing ingest body using local_bind. Lift the
    //    rest of DoLoadState's body into a helper DoLoadStateImpl(...)
    //    that takes the bind by reference; both branches call it.
    DoLoadStateImpl(context, local_bind, state, out);

    // 5. raii destructor runs → RemoveDirectory(state_dir) wipes ~5-50 GB of zips.
    return;
}
// else: existing /vsicurl/ serial path
DoLoadStateImpl(context, bind, state, out);
```

The refactor that makes this clean: split the existing 217-line `DoLoadState` body (lines 902-1118) into:
- The new prelude above (sitting in `DoLoadState`).
- A renamed `DoLoadStateImpl(context, bind, state, out)` containing the existing lines 904-1118 verbatim. No logic changes inside it.

The reason this works without further intrusion: `BuildVsiPath` (line 207) already auto-detects HTTP vs local from the `source` prefix. Swapping `bind.source` to the local temp dir reroutes every downstream `BuildVsiPath` call to `/vsizip/<local>/SUBDIR/...zip/...`. The existing retry loop still applies — even on local files GDAL occasionally returns spurious `decompression failed` for corrupted-mid-write zips, and `IsRetriableLoaderError` already covers that.

**Idempotency interaction**: if `loader_progress` already shows the state's counties as done, the existing skip-progress checks (line 1013 et al) will skip every single insert. In that case `DownloadStateZips` is wasted work. Add a *fast-path check* before downloading: query `loader_progress` to count remaining `state:<fips>:%` entries that are *not* done; if zero, skip the download entirely. Optional polish — happy path is ~free since downloads are skipped on `FileExists`, but worth doing because even one HTTP GET of the directory index is 100s of ms.

---

## 4. Cross-platform considerations

| Concern | Strategy |
| --- | --- |
| Recursive directory remove | DuckDB's `FileSystem::RemoveDirectory(path)` already recurses (verified in DuckDB's `local_file_system.cpp` — Windows uses `SHFileOperation` / iterative `RemoveDirectoryW`, POSIX uses `nftw`). Do **not** use `std::filesystem::remove_all`: requires C++17 `<filesystem>` link (`-lstdc++fs` on gcc7/8) and the extension's ABI floor is C++11 per CLAUDE.md line 45. |
| Path joining | `FileSystem::JoinPath(a, b)` and `FileSystem::PathSeparator(path)`. **Important asymmetry**: for the *VSI path* (`/vsizip/<source>/SUBDIR/...zip`) keep forward-slashes. `BuildVsiPath` (line 207) already does this unconditionally with `'/'` — GDAL accepts forward slashes on Windows. So: use FS-level helpers for `RemoveDirectory` / `CreateDirectoryRecursive` / `FileExists` (Windows-correct), but pass the same path string to `BuildVsiPath` unchanged (`/vsizip/` mode tolerates the forward slashes the helpers may have rewritten — or just ensure the path string returned by `MakeStateTempDir` uses `'/'`). Simplest: store the temp dir as forward-slash-only, never call `ConvertSeparators`. |
| TLS on Windows | `HTTPUtil::Get(db).InitializeClient(...)` returns the httpfs-backed client when httpfs is loaded (mbedTLS-linked) and otherwise the bundled `duckdb_httplib::Client` (no TLS). Since `EnsureHttpfsIfRemote` (loader.cpp line 645) is already called at the top of `DoLoadState`, httpfs is guaranteed loaded by the time `DownloadStateZips` runs. **This is the whole reason to go through `HTTPUtil` rather than including `httplib.hpp` directly** — it gives us TLS on every platform that has httpfs, with no extra link deps. |
| Temp dir resolution | `std::filesystem::temp_directory_path()` is acceptable here (cheap, C++17, only used during the prelude — not in headers), but it can throw on POSIX if `TMPDIR` is unset *and* `/tmp` is missing (rare). Safer: try env vars in order `TMPDIR`, `TEMP`, `TMP`; fall back to `"."`; emit a warning if none set. Mirror DuckDB's own approach in `LocalFileSystem`. |
| MSVC raw-string literal limit | The cap (16,380 chars) only applies to single literals; our regex is short, no risk. |
| Threading | `std::thread` + a counting semaphore for back-pressure works on all three toolchains (MSVC, MinGW, gcc, clang). Avoid `std::async` — implementation-defined whether it actually creates a thread (libstdc++ runs deferred-policy inline). |

---

## 5. Concurrency model

Pool size: **default 16** (mirrors `parallel_download_state.sh` line 25). Expose two knobs:

- Function arg `parallel_workers := INT` (the per-call override).
- Session setting `us_geocoder_parallel_workers` (optional — see §11 phasing; only worth adding if a user has reason to globally tune this across all `load_tiger_*` calls).

The pool is *per-state*: one fresh pool per call to `DownloadStateZips`. Cleaned up when that call returns. Per-state means a `load_tiger_all_states` doesn't oversaturate by spinning up 51 × 16 = 816 threads — the outer loop is sequential, only the inner downloads are concurrent.

**Implementation**: a fixed-size `std::vector<std::thread>` consuming a `std::queue<DownloadTask>` protected by a mutex + condvar. Each worker pops a URL, calls `DownloadOneFile`, retries with backoff, pushes a result-struct onto a shared `std::vector<Result>` under the same mutex. Main thread joins all workers at the end. On any worker reporting a fatal failure (3 attempts exhausted), set an atomic `bool fail_flag`; remaining workers drain their current task and exit early. After join, if fail_flag was set, throw `IOException("us_geocoder parallel_download (<state>): %s", err_msg)` — error message format mirrors the existing `RetryableExecuteInsert` style so the rest of the user-facing logging is uniform.

**Why not DuckDB's `TaskScheduler`?** Two reasons:
1. `TaskScheduler` is geared toward query-pipeline tasks; tasks have a `TaskExecutionResult` enum and integrate with the optimizer's metering. Using it for blocking HTTP downloads inside a `TableFunction.execute()` callback is fighting the abstraction.
2. We're running inside an `execute()` callback that holds locks DuckDB doesn't expect to be re-entered. Confirmed by the existing loader code: every concurrency attempt that used `TaskScheduler` got reverted (CLAUDE.md lines 56-77). Std threads sidestep that entirely.

---

## 6. Error handling + retry

**Per-file retries are critical under parallelism** — 16 concurrent requests means ~16× more chances per second to hit a transient 403/429/5xx from the Census CDN, especially Cloudflare WAF behavior the shell script's `.wgetrc` already accommodates ([benchmark/pg/load_tiger_via_pg.sh:50-61](../benchmark/pg/load_tiger_via_pg.sh#L50-L61)). The new path mirrors the existing serial loader's retry semantics exactly so user-facing logging stays uniform.

- **Retry triggers per file, per attempt**:
  - Transport error (`HTTPResponse::HasRequestError()` true — DNS failure, connection reset, TLS handshake failure, etc.).
  - Status codes covered by `HTTPResponse::ShouldRetry()`: 408, 418, 429, 500, 503, 504.
  - Plus 502 (Bad Gateway) and 507 (Insufficient Storage) — observed empirically on the Census CDN (see CLAUDE.md retry hints).
- **Backoff per attempt**: `{2s, 5s, 15s}`. Identical to `RetryableExecuteInsert` (loader.cpp:295) so logs/output read uniformly.
- **Cache-bust on retry**: append `?cb=<rand>` on attempts 2+ via `MakeCacheBust()` (loader.cpp:187) to bypass any sticky bad-cache edge response. Reuse the existing helper — either expose via a small `loader_util.hpp` or move to `parallel_download.cpp` and re-include from loader.cpp.
- **Per-worker aggregation**: each worker retries its own file independently. A worker that exhausts its 3 attempts on a file sets an atomic `fail_flag`; remaining in-flight workers finish their *current* file, then drain the queue and exit. After all workers join, if `fail_flag` was set, throw a single `IOException` naming the first failed file. Avoids "16 workers each throwing in parallel" chaos.
- **Per-state vs per-file boundaries**: any single file failing all 3 attempts aborts the whole state. The temp dir is **left in place** on failure (RAII `StateDirCleanup::disarm()` *not* called) so the user can re-run and skip already-downloaded zips via the idempotency check.

Index-scrape failures: if the directory-index HTTP fetch (`https://...TIGER2025/EDGES/`) fails after retries, we cannot enumerate counties for that table-type. Treat as state-fatal, throw immediately. No fallback to a hardcoded county list (per requirements — Census adjusts annually). One mitigation: cache the scrape result in-memory for the lifetime of the TableFunction execute, so `load_tiger_all_states` doesn't re-scrape `EDGES/` for every state. (Still scrapes per-`<sub>` URL, so the win is limited; defer unless benchmark shows it matters.)

---

## 7. Idempotency

Three layers, in order from cheapest to most expensive:

1. **`loader_progress`-driven skip** (already exists): a state with all per-county sections marked done in `loader_progress` skips even the download. Add the precheck in the prelude described in §3.
2. **`FileExists + GetFileSize > 0` skip** in `DownloadOneFile`: matches the shell script's `[ -s "$dst" ]` check. Crucial when a previous run was killed mid-state — half-downloaded zips left as `<name>.zip.tmp` (the atomic-rename target) are visible to neither check, so they get cleanly re-downloaded.
3. **`.tmp` rename atomicity**: never expose a partially-written `.zip` to the ingest pass. On Windows, `MoveFile` (used by `FileSystem::MoveFile`) is atomic for same-volume moves. On POSIX `rename(2)` is atomic.

**Failure mode worth documenting**: if a download succeeded but the `.tmp → .zip` rename failed (e.g. user `kill -9` between the two), the next run sees the `.tmp` file but no `.zip` and re-downloads. No corruption risk; just wasted bytes. Note this in `DownloadStateZips` source as a documented limitation.

---

## 8. Tests

Add **`test/sql/loader_parallel.test`** covering:

```
# name: test/sql/loader_parallel.test
# group: [sql]

require us_geocoder

# parallel := false is accepted positionally + named — preserves binder coverage of the new flag
statement error
CALL load_tiger_state('RI', '/nonexistent/tiger', parallel := false);
----
us_geocoder loader

statement error
CALL load_tiger_state('RI', '/nonexistent/tiger', parallel := true);
----
us_geocoder loader

# parallel := true with non-HTTP source is a no-op (force serial); proves the
# bind doesn't reject local + parallel := true and the execute fast-paths through
# DoLoadStateImpl directly. Same error as parallel := false.
statement error
CALL load_tiger_state('RI', '/nonexistent/tiger', parallel := true, temp_dir := '/tmp/foo');
----
us_geocoder loader

# named-param argument types
statement error
CALL load_tiger_state('RI', parallel := 'yes');
----
type
```

**Network-gated integration test** — `test/sql/loader_parallel_network.test`:

```
require us_geocoder

# Only run when the user opts in: real Census fetches are slow + flaky.
require-env US_GEOCODER_NETWORK_TESTS

# DC is the smallest non-empty TIGER state: 1 county, 4 county zips + 2
# state-level zips = ~10 MB total. ~5-10s on a fast connection.
statement ok
CALL load_tiger_nation();

statement ok
CALL load_tiger_state('DC', parallel := true);

query I
SELECT COUNT(*) > 0 FROM tiger.edges WHERE statefp = '11';
----
true
```

DC is the right test state: smallest of the 51 (one county, FIPS 11001). Hawaii (5 counties) or Rhode Island (5 counties) would be the smallest multi-county options.

**No HTTP mocking attempt.** The httpfs HTTP client doesn't expose a mock-hook surface; standing up a local fixture HTTP server would require either WireMock-style infra in CMake or wiring an in-process test server with the bundled httplib `Server` class — disproportionate to what this PR delivers. Network-gated suffices; CI doesn't run network tests by default.

---

## 9. Docs

- **`README.md` lines 186-198** ("Faster HTTP loads" section): rewrite to describe `parallel := true` as the **default** behavior. Show the one-liner `CALL load_tiger_state('NJ');` doing the parallel download + local ingest under the hood, with a note about the temp-dir mechanism and `temp_dir :=` override. Add a sentence that `parallel := false` reverts to the legacy `/vsicurl/` path for benchmarking. Update the speed table (line 178-184): NJ goes from "~5-8 min" to "~53 s"; add NJ to the table.
- **`docs/api.md` lines 26-58** (the three `load_tiger_*` entries): document the two new named parameters (`parallel`, `temp_dir`) including the local-source no-op rule. Note that `parallel_workers` (if exposed) defaults to 16.
- **`docs/quickstart.md`**: no change needed — the user-facing call shape doesn't change.
- **`scripts/parallel_download_state.sh`**: keep in tree for at least one release as a deprecated convenience for users who want to pre-stage zips for other tools. Add a 3-line header comment marking it deprecated and pointing at `parallel := true`. **Do not delete it in this PR** — it works as a useful smoke-test reference for the C++ implementation while you're debugging the new path. Schedule deletion for a follow-up PR after the new path has shipped on `main` and seen real-world use.
- **`CLAUDE.md` § Loader performance**: add a paragraph noting that the May/Nov 2026 work flipped the loader to parallel-download-by-default, validates the empirical 2.2× NJ measurement, and clarifies why this is the *one* parallelism strategy that *did* pan out (parallel HTTP fetches saturate the CDN, vs the previously-tried parallel parse / parallel write / parallel CTAS strategies all of which were CPU-bound and DuckDB-internal-serialization-bound).

---

## 10. Risks

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| `HTTPUtil` API instability between DuckDB releases | Medium | The vendored DuckDB version is pinned in-tree (`duckdb/` is a submodule snapshot). Pin against the current API; on DuckDB upgrade, port any signature changes. |
| `HTTPUtil::Get(db)` returns a non-TLS client if `EnsureHttpfsIfRemote` silently fails to load httpfs (corporate firewall, locked-down env) | Low | Already called at line 645. Add an explicit check after the call: if `source` is HTTPS but `HTTPUtil::Get(db).GetName() == "Built-In"`, throw with a clear "load `httpfs` first" message. |
| Threading inside a `TableFunction.execute()` callback breaks DuckDB invariants | Low to Medium | `execute()` runs on a regular pipeline thread; std::thread inside it is fine *as long as we don't touch DuckDB-managed state from the workers*. Our workers touch only the network and the local filesystem — no `Connection`, no `ClientContext`, no catalog. Safe. The shared `ClientContext&` passed to `DownloadStateZips` is only used during `ScrapeCensusIndex` and on the calling thread. |
| `std::regex` ICU/locale variance on Windows + AppleClang | Low | Use `std::regex::ECMAScript` (the default), no character classes that depend on locale, ASCII-only patterns. Pattern is just `tl_<year>_<fips><cfp>_<table>\.zip` — fully ASCII. |
| Census changes its HTML directory format | Medium over a 5-year horizon, low this year | Match permissively: find `href="tl_..."` and `>tl_..."` both. Add a unit test that asserts our regex matches a fixture HTML snapshot stored in `test/data/census_edges_index_2025.html` (snapshot under 5 KB, cheap to maintain). |
| `temp_dir` defaults to a location with insufficient disk (e.g. `/tmp` is tmpfs on a host with 8 GB RAM, TX is ~10 GB) | Medium | Document in api.md. Optionally call `FileSystem::GetAvailableDiskSpace(temp_base)` before downloading; if returned size < a heuristic (state_size × 2), warn to stderr. Don't fail — the user might know better. |
| Two concurrent CALLs to `load_tiger_state('NJ')` from different sessions race on the same temp dir | Medium (rare in practice but possible) | `MakeStateTempDir` includes pid + a ns-resolution timestamp, so collisions need an actual clock-skew + pid-reuse combo. Treat as not-an-issue. |
| `RemoveDirectory` failing on Windows because GDAL still holds a file handle from a previous retry | Low | Sequence: ensure the previous SQL `RetryableExecuteInsert` for this state has fully completed before `StateDirCleanup` runs. Since `DoLoadStateImpl` returns only after all per-county INSERTs commit, by RAII order this is guaranteed. If it does fail, log a warning (don't throw — data load already succeeded) and let the OS clean up on reboot. |
| Cloudflare WAF blocking 16-concurrent UAs that look identical | Medium under heavy load | The bundled httplib client sends `User-Agent: cpp-httplib/X.Y`. The shell script worked around this by switching to a Firefox UA. If we hit 403s in practice, do the same — set a desktop-browser UA on every request. Cheap mitigation if needed. |

---

## 11. Phasing

Single PR. The Windows MSVC cert-chain bug is in scope (resolved decision #3), so we don't have the luxury of shipping a wiring-only PR first that leaves Windows broken. One commit/PR lands:

1. `parallel`, `temp_dir`, `parallel_workers` named params on all four `load_tiger_*` TableFunctions (`state`, `states`, `all_states`, `nation`).
2. Refactor `DoLoadState` → `DoLoadStateImpl` and `DoLoadNation` → `DoLoadNationImpl` so the parallel prelude can wrap either.
3. New `src/parallel_download.cpp` + header: `ScrapeCensusIndex`, `DownloadOneFile`, worker pool, retries/backoff, RAII temp-dir cleanup.
4. Wire the parallel prelude into both `DoLoadState` and `DoLoadNation`.
5. Default `parallel := true` everywhere (per resolved decision #3 the user wants this immediately).
6. Bind-level tests in `test/sql/loader_parallel.test`. Network-gated integration test in `test/sql/loader_parallel_network.test`.
7. Add deprecation header to `scripts/parallel_download_state.sh` pointing at `parallel := true`; do NOT delete the script (resolved decision #2).
8. Update `README.md`, `docs/api.md`, `CLAUDE.md` § Loader performance.

---

## Resolved decisions

1. **`parallel_workers` knob**: function arg only, default 16, documented only as an inline comment in the bind code. No session setting.
2. **`parallel_download_state.sh`**: **deprecate** in this PR — add a deprecation header pointing at `parallel := true` and a removal target one release out. Don't delete yet (kept for users mid-migration + as a portable smoke-test reference).
3. **`load_tiger_nation` parallel path**: in scope. Goal is to completely eliminate `/vsicurl/` so cross-platform support (Windows MSVC SSL issue) and surface area both get cleaner. Nation downloads `STATE/tl_<year>_us_state.zip`, `COUNTY/tl_<year>_us_county.zip`, `ZCTA520/tl_<year>_us_zcta520.zip` into the same kind of state-named temp dir (`<base>/us_geocoder_tiger_<YEAR>_nation_<pid>_<ts>/`). No scraping needed — the 3 filenames are known.
4. **`temp_dir` semantics**: option (c). User pins the base; we own state-suffixed subdirs (`<base>/us_geocoder_tiger_<YEAR>_<STATE>_<pid>_<ts>/`) and `RemoveDirectory` each one after its state's ingest finishes.
5. **Local source + `parallel := true`**: emit a warning to stderr (via `Printer::Print` or `context.client_data->log_manager`) explaining `parallel` has no effect with a local source, then silently force `parallel = false` and proceed. Don't fail — the binding is still valid.

---

## Critical files for implementation

- [src/loader.cpp](../src/loader.cpp)
- `src/parallel_download.cpp` *(new)*
- `src/include/us_geocoder_parallel_download.hpp` *(new)*
- [CMakeLists.txt](../CMakeLists.txt)
- [duckdb/src/include/duckdb/common/http_util.hpp](../duckdb/src/include/duckdb/common/http_util.hpp) *(read-only reference — the API the new code targets)*
