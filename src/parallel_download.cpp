#include "us_geocoder_parallel_download.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <queue>
#include <thread>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace duckdb {
namespace us_geocoder {

// =====================================================================
// Helpers
// =====================================================================

// Sleep for the given number of seconds. Wraps std::this_thread::sleep_for
// so the call site reads as a single line.
static void SleepSeconds(int s) {
	std::this_thread::sleep_for(std::chrono::seconds(s));
}

// Cross-platform getenv that also tries the Windows-y names. Returns "" if
// none of the env vars are set.
static std::string ReadEnv(const char *name) {
	if (auto *p = std::getenv(name)) {
		return std::string(p);
	}
	return "";
}

// Get current PID as a string. Used in MakeStateTempDir for uniqueness.
static std::string PidString() {
	return std::to_string(static_cast<long long>(getpid()));
}

// =====================================================================
// HTML directory index scraping
// =====================================================================

// Issue a GET against the Census directory URL and return the response body.
// Retries on transport / 5xx-class errors with the standard backoff.
static std::string FetchDirectoryIndex(ClientContext &context, const std::string &url,
                                       const ParallelDownloadOptions &opts) {
	auto &http_util = HTTPUtil::Get(*context.db);
	auto params = http_util.InitializeParameters(context, url);
	params->verify_ssl = true;
	params->override_verify_ssl = false;

	std::string path, proto_host_port;
	HTTPUtil::DecomposeURL(url, path, proto_host_port);
	auto client = http_util.InitializeClient(*params, proto_host_port);

	std::string last_err;
	for (int attempt = 0; attempt < opts.max_attempts; ++attempt) {
		std::string body;
		HTTPHeaders headers;
		GetRequestInfo req(
		    url, headers, *params, [](const HTTPResponse &) { return true; }, // response handler: accept all
		    [&](const_data_ptr_t data, idx_t len) {
			    body.append(reinterpret_cast<const char *>(data), len);
			    return true;
		    });
		req.try_request = true;

		unique_ptr<HTTPResponse> resp;
		try {
			resp = client->Get(req);
		} catch (std::exception &ex) {
			last_err = ex.what();
			resp = nullptr;
		}

		if (resp && resp->Success() && !body.empty()) {
			http_util.CloseClient(std::move(client));
			return body;
		}
		if (resp) {
			last_err = resp->HasRequestError() ? resp->GetRequestError() : resp->GetError();
			if (last_err.empty()) {
				last_err = "empty response body";
			}
		}

		bool last = (attempt + 1 == opts.max_attempts);
		if (last) {
			break;
		}
		int wait = (attempt < static_cast<int>(opts.backoff_seconds.size())) ? opts.backoff_seconds[attempt] : 15;
		fprintf(stderr, "[us_geocoder %s] index-scrape (%s): %s, retry %d/%d in %ds\n", opts.log_prefix.c_str(),
		        url.c_str(), last_err.c_str(), attempt + 1, opts.max_attempts - 1, wait);
		fflush(stderr);
		SleepSeconds(wait);
		// Reinitialize client for retry — keep-alive may be stale after a server-side close.
		client = http_util.InitializeClient(*params, proto_host_port);
	}
	throw IOException("us_geocoder parallel_download (index scrape %s): %s", url, last_err);
}

std::vector<std::string> ScrapeCensusIndex(ClientContext &context, const std::string &index_url,
                                           const std::regex &name_pattern, const ParallelDownloadOptions &opts) {
	auto html = FetchDirectoryIndex(context, index_url, opts);
	std::vector<std::string> names;
	auto begin = std::sregex_iterator(html.begin(), html.end(), name_pattern);
	auto end = std::sregex_iterator();
	for (auto it = begin; it != end; ++it) {
		// Group 0 is the full match — the filename.
		names.push_back(it->str(0));
	}
	// Dedup while preserving order. The Apache index typically lists each file
	// twice (anchor href + visible text), so a raw collect doubles every entry.
	std::vector<std::string> unique;
	unique.reserve(names.size());
	for (auto &n : names) {
		bool dup = false;
		for (auto &u : unique) {
			if (u == n) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			unique.push_back(n);
		}
	}
	return unique;
}

// =====================================================================
// Single-file download
// =====================================================================

// Download one file. Streams body to <dest_path>.tmp, atomic-renames on
// success. Skips early if <dest_path> already exists with size > 0.
// Returns true on success; on failure populates err_out and returns false.
// Internal retry loop with backoff — caller does not need to retry.
static bool DownloadOneFile(ClientContext &context, FileSystem &fs, const std::string &url,
                            const std::string &dest_path, const ParallelDownloadOptions &opts, std::string &err_out,
                            int64_t &bytes_out) {
	bytes_out = 0;
	if (fs.FileExists(dest_path)) {
		auto handle = fs.OpenFile(dest_path, FileOpenFlags::FILE_FLAGS_READ);
		auto sz = fs.GetFileSize(*handle);
		if (sz > 0) {
			return true; // idempotency hit
		}
	}

	auto &http_util = HTTPUtil::Get(*context.db);
	auto params = http_util.InitializeParameters(context, url);
	std::string path, proto_host_port;
	HTTPUtil::DecomposeURL(url, path, proto_host_port);
	auto client = http_util.InitializeClient(*params, proto_host_port);

	for (int attempt = 0; attempt < opts.max_attempts; ++attempt) {
		const std::string tmp_path = dest_path + ".tmp";
		// Truncate (or create) the .tmp file fresh on every attempt.
		fs.TryRemoveFile(tmp_path);
		auto out_handle =
		    fs.OpenFile(tmp_path, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE);

		int64_t bytes_this_attempt = 0;
		HTTPHeaders headers;
		GetRequestInfo req(
		    url, headers, *params, [](const HTTPResponse &) { return true; },
		    [&](const_data_ptr_t data, idx_t len) {
			    out_handle->Write(const_cast<data_ptr_t>(data), len);
			    bytes_this_attempt += static_cast<int64_t>(len);
			    return true;
		    });
		req.try_request = true;

		unique_ptr<HTTPResponse> resp;
		try {
			resp = client->Get(req);
		} catch (std::exception &ex) {
			err_out = ex.what();
			resp = nullptr;
		}

		out_handle->Close();

		if (resp && resp->Success() && bytes_this_attempt > 0) {
			// Atomic rename. Throws on failure (e.g. cross-volume rename on Windows).
			try {
				fs.MoveFile(tmp_path, dest_path);
			} catch (std::exception &ex) {
				err_out = std::string("rename failed: ") + ex.what();
				fs.TryRemoveFile(tmp_path);
				// Not retriable — return failure.
				return false;
			}
			bytes_out = bytes_this_attempt;
			return true;
		}

		fs.TryRemoveFile(tmp_path);
		if (resp) {
			err_out = resp->HasRequestError() ? resp->GetRequestError() : resp->GetError();
			if (err_out.empty()) {
				err_out = bytes_this_attempt > 0 ? "truncated response" : "empty response body";
			}
		}

		bool last = (attempt + 1 == opts.max_attempts);
		if (last) {
			break;
		}
		int wait = (attempt < static_cast<int>(opts.backoff_seconds.size())) ? opts.backoff_seconds[attempt] : 15;
		fprintf(stderr, "[us_geocoder %s] %s: %s, retry %d/%d in %ds\n", opts.log_prefix.c_str(), url.c_str(),
		        err_out.c_str(), attempt + 1, opts.max_attempts - 1, wait);
		fflush(stderr);
		SleepSeconds(wait);
		// Reinitialize client for retry.
		client = http_util.InitializeClient(*params, proto_host_port);
	}
	return false;
}

// =====================================================================
// Worker pool
// =====================================================================

namespace {

// Internal task type — wraps a DownloadTarget plus a slot for the result.
struct WorkItem {
	const DownloadTarget *target;
	bool success = false;
	bool skipped = false;
	int64_t bytes = 0;
	std::string err;
};

struct WorkQueue {
	std::queue<size_t> idx_queue; // indices into items
	std::vector<WorkItem> items;
	std::mutex m;
	std::condition_variable cv;
	std::atomic<bool> stop_flag {false};
	std::atomic<int> in_flight {0};
};

// Worker thread body. Pops indices off the queue, downloads, records result.
// On any download exhausting retries, signals stop_flag so remaining workers
// drain after their current file.
static void WorkerLoop(ClientContext *context, FileSystem *fs, const ParallelDownloadOptions *opts, WorkQueue *q) {
	while (true) {
		size_t idx;
		{
			std::unique_lock<std::mutex> lock(q->m);
			q->cv.wait(lock, [q] { return !q->idx_queue.empty() || q->stop_flag.load(); });
			if (q->stop_flag.load() && q->idx_queue.empty()) {
				return;
			}
			if (q->idx_queue.empty()) {
				continue;
			}
			idx = q->idx_queue.front();
			q->idx_queue.pop();
		}
		q->in_flight.fetch_add(1, std::memory_order_relaxed);

		auto &item = q->items[idx];
		// Skip if file already on disk (FileExists check inside DownloadOneFile too,
		// but checking here avoids the HTTP client init overhead).
		if (fs->FileExists(item.target->dest_path)) {
			auto handle = fs->OpenFile(item.target->dest_path, FileOpenFlags::FILE_FLAGS_READ);
			auto sz = fs->GetFileSize(*handle);
			if (sz > 0) {
				item.skipped = true;
				item.success = true;
				q->in_flight.fetch_sub(1, std::memory_order_relaxed);
				continue;
			}
		}

		int64_t bytes = 0;
		std::string err;
		bool ok = DownloadOneFile(*context, *fs, item.target->url, item.target->dest_path, *opts, err, bytes);
		item.success = ok;
		item.bytes = bytes;
		item.err = err;
		if (!ok) {
			q->stop_flag.store(true);
			q->cv.notify_all();
		}
		q->in_flight.fetch_sub(1, std::memory_order_relaxed);
	}
}

} // namespace

ParallelDownloadResult ParallelDownload(ClientContext &context, const std::vector<DownloadTarget> &targets,
                                        const ParallelDownloadOptions &opts) {
	ParallelDownloadResult result;
	result.files_total = targets.size();
	if (targets.empty()) {
		return result;
	}

	auto &fs = FileSystem::GetFileSystem(context);

	// Ensure parent directory of every target exists. Cheap; doesn't hit network.
	for (const auto &t : targets) {
		auto last_slash = t.dest_path.find_last_of("/\\");
		if (last_slash != std::string::npos) {
			auto parent = t.dest_path.substr(0, last_slash);
			if (!parent.empty() && !fs.DirectoryExists(parent)) {
				fs.CreateDirectoriesRecursive(parent);
			}
		}
	}

	WorkQueue q;
	q.items.reserve(targets.size());
	for (size_t i = 0; i < targets.size(); ++i) {
		WorkItem item;
		item.target = &targets[i];
		q.items.push_back(item);
		q.idx_queue.push(i);
	}

	auto t0 = std::chrono::steady_clock::now();
	const int n_workers = std::max(1, opts.workers);
	std::vector<std::thread> workers;
	workers.reserve(n_workers);
	for (int i = 0; i < n_workers; ++i) {
		workers.emplace_back(WorkerLoop, &context, &fs, &opts, &q);
	}
	// Once we've pushed all targets, signal that no more work will arrive.
	// Workers exit only when stop_flag is set AND queue is empty.
	{
		std::unique_lock<std::mutex> lock(q.m);
		q.stop_flag.store(true);
		q.cv.notify_all();
	}
	for (auto &t : workers) {
		t.join();
	}

	auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	result.elapsed_seconds = elapsed;

	// Aggregate results + find first failure for the error message.
	std::string first_err_file;
	std::string first_err_msg;
	for (auto &item : q.items) {
		if (item.success) {
			if (item.skipped) {
				++result.files_skipped;
			} else {
				++result.files_downloaded;
				result.bytes_downloaded += item.bytes;
			}
		} else {
			if (first_err_file.empty()) {
				first_err_file = item.target->url;
				first_err_msg = item.err;
			}
		}
	}
	if (!first_err_file.empty()) {
		throw IOException("us_geocoder parallel_download (%s): %s — %s", opts.log_prefix, first_err_file,
		                  first_err_msg);
	}
	return result;
}

// =====================================================================
// Temp dir helpers
// =====================================================================

std::string ResolveTempBase(ClientContext &, const std::string &explicit_temp_dir) {
	if (!explicit_temp_dir.empty()) {
		return explicit_temp_dir;
	}
	// Try the conventional env vars in order.
	for (const char *name : {"TMPDIR", "TEMP", "TMP"}) {
		auto v = ReadEnv(name);
		if (!v.empty()) {
			return v;
		}
	}
#ifdef _WIN32
	return "."; // fallback — current working dir
#else
	return "/tmp";
#endif
}

std::string MakeStateTempDir(ClientContext &context, const std::string &base, const std::string &state_label,
                             int year) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto ts_ns = std::chrono::steady_clock::now().time_since_epoch().count();
	// Forward-slashes only; BuildVsiPath in loader.cpp uses them unconditionally
	// and GDAL accepts them on Windows.
	std::string sep = "/";
	std::string sub = "us_geocoder_tiger_" + std::to_string(year) + "_" + state_label + "_" + PidString() + "_" +
	                  std::to_string(static_cast<long long>(ts_ns));
	std::string full = base;
	if (!full.empty() && full.back() != '/' && full.back() != '\\') {
		full += sep;
	}
	full += sub;
	fs.CreateDirectoriesRecursive(full);
	return full;
}

} // namespace us_geocoder
} // namespace duckdb
