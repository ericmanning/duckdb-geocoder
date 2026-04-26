# D7 revisit options

D7 = "Stage A is one always-on vectorized pass with `LIMIT max_results` at the end" — our deliberate divergence from PG's two-query primary+fallback structure (see [docs/parity.md § D7](parity.md)).

After full PG-2025-PAGC parity investigation, three remaining first-row divergences trace specifically to D7:

| Test | PG output | Our output |
|---|---|---|
| #1076h | rating 18 | rating 16 (PG used fallback's `+3` no-input-ZIP constant; we use primary's `+1`) |
| #1073a | `10000 3rd St NE, Hanover, MN 55341` rating 38 | `212 3rd St E, Hector, MN 55342` rating 30 (PG iterated through 3 zip_info shapes; we use one window_zips) |
| #1145d | `141 W 121st St, 8512, Burnsville, MN 55337` rating 51 | Stage B drop, rating 100 (PG's fallback unconditional soundex matches `'ST'` to `'121st'`; we length-gate soundex) |

Plus 4 us-better-than-PG cases (T18a, #1145a/b/e) that depend on us keeping unconditional `numeric_streets_equal` in candidate-finding — at risk of regressing under naive PG ports.

## Options considered

| Option | Closes 1076h | Closes 1073a | Closes 1145d | Mechanism B preserved | Cost impact |
|---|---|---|---|---|---|
| A — full PG verbatim port (primary+fallback, length-gated soundex) | ✓ | ✓ | ✓ | **regresses 4 tests** | 1.5–2× |
| B — UNION two query shapes always (always run Pass A + Pass B, dedup) | ✓ | partial | ✓ | ✓ | ~2× always |
| **C — conditional fallback** (Pass A always; Pass B when Pass A weak) | ✓ | partial | ✓ | ✓ | 1× happy / 2× weak |
| D — narrow ZIP-iteration only (multi-window in single pass) | ✗ | ✓ | ✗ | ✓ | ~1.1× |
| E — status quo | ✗ | ✗ | ✗ | ✓ | 1× |

**Decision: Option C** — Pass A (current pipeline) runs always; Pass B (loosened name-match + `+3` ZIP fallback constant + zip-window expansion) runs only when Pass A's best rating ≥ 30 or returns 0 rows. Merge, dedup, return top max_results.

Trade-offs accepted:
- Pays ~2× cost for inputs that need fallback (low-quality matches, ambiguous addresses); 1× for typical inputs where Pass A finds a good match.
- May not fully close #1073a if PG's specific zip_info iteration shape isn't replicated; willing to leave that as a remaining minor divergence if Pass B's wider ZIP window gets close.

Implementation lives on `patch/d7` branch.
