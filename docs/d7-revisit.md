# D7 revisit — final state

D7 = "Stage A is one always-on vectorized pass with `LIMIT max_results` at the end" — our deliberate divergence from PG's two-query primary+fallback structure (see [docs/parity.md § D7](parity.md)).

After full PG-2025-PAGC parity investigation and the `patch/d7` work, the three "D7-cost" divergences from the original analysis are now resolved structurally — though the resolution required digging deeper than the originally-considered options.

## Original options considered (historical context)

| Option | Closes 1076h | Closes 1073a | Closes 1145d | Cost impact |
|---|---|---|---|---|
| A — full PG verbatim port (primary+fallback, length-gated soundex) | ✓ | ✓ | ✓ | regresses Mechanisms A/B; 1.5–2× |
| B — UNION two query shapes always (Pass A + Pass B, dedup) | ✓ | partial | ✓ | ~2× always |
| **C — conditional fallback** (Pass A always; Pass B when Pass A weak) | ✓ | partial | ✓ | 1× happy / 2× weak |
| D — narrow ZIP-iteration only (multi-window in single pass) | ✗ | ✓ | ✗ | ~1.1× |
| E — status quo | ✗ | ✗ | ✗ | 1× |

We started with Option C, but the actual fixes turned out to be more granular and more PG-faithful than any of the originally-considered options.

## What was actually done

Six commits on `patch/d7`, each a small structural port of a specific PG behavior:

1. **`da5d847`** — Two-pass Option C scaffold (Pass A always; Pass B gated). Per-row `+1` (PG primary) vs `+3` (PG fallback) no-input-ZIP rating constant, discriminated by a verbatim port of PG primary's three-branch name filter (`exact_name OR length>5 AND fullname-LIKE-with-predir OR length>5 AND soundex_eq`).

2. **`9648db4`** — Bifurcate Pass B's name filter on `zip_info_exact` (mirroring PG fallback's `exact=TRUE/FALSE` iteration distinction). Closes #1112d/e (`8401 W 35W` matches PG's r=34 exactly).

3. **`1ada06e`** — Structural port of PG primary's full `zip_info` computation:
   - `±2` window when `length(streetName) > 7`, else `±1`
   - city-derived ZIPs as fallback when no input ZIP
   - `via_primary` discriminator on whether row would have passed PG primary's name filter AND has non-NULL place (mirroring PG primary's INNER JOIN to place)
   - `via_primary` drives both no-input-ZIP constant AND address rendering (PG primary clamps out-of-range house numbers; PG fallback NULLs them).
   Closes #1112a; row-1 of #1076h matches PG exactly.

4. **`f33635f`** — Structural port of PG fallback's distinct rating formulas:
   - ZIP penalty: `LEAST(diff_zip * penalty, lev_zip * penalty)` (PG fallback) vs `LEAST(diff_zip, 20) * penalty` (PG primary)
   - City penalty: `LEAST(lev(input, COALESCE(p, cs, zip.city, co)), lev(input, COALESCE(cs, co)))` (fallback) vs `lev(input, place) or 5` (primary)
   - Pass B fires loose branches when `gate fires AND (NOT zip_info_exact OR pass_a_empty)` (mirrors PG iter 2-5 firing when iter 1 is empty).
   Closes #1145d structurally (returns real candidates instead of dropping to Stage B).

5. **`dfcd06d`** — Fix the root cause of #1112d/e's earlier feared "Co Rd 37 flood" at the parser level instead of working around it with a bifurcation:
   - Our PAGC over-splits compound numeric-suffix streetnames like `35W` into `name='35' + sufdir='W'`. (PG's PAGC behaves the same way — verified by running `pagc_normalize_address`. PG's geocode happens to avoid the resulting soundex flood because its `LOOP` short-circuits after iter 1 returns enough rows; we have no equivalent loop in our table-query model.)
   - Add a post-PAGC `from_pagc` validation step that recombines `<digits><single-letter>` when the raw input contains the unspaced concatenation as a token. **Trade-off:** TIGER stores compound numeric-direction streets inconsistently — Pattern A (`name='35W'`, ~725 rows in MA/MN/CT) vs Pattern B (`name='35', sufdir='W'`, ~3500 rows). Pattern B is ~5× more common, so for those inputs the recombined parse loses the explicit sufdir match and incurs a +2 rating penalty (`numeric_streets_equal` still finds the right candidate, just rates it slightly worse). Pattern A inputs benefit. The trigger is conservative (only fires when raw text has unspaced `<digits><letter>`), so spaced inputs like `"35 W"` are unaffected.
   - With the parser fix, the geocode-level `zip_info_exact` bifurcation in Pass B (added in `9648db4`) is no longer needed and is removed. Pass B fires whenever the gate fires, mirroring PG's behavior.
   - **Side effect: #1073a now finds the actual correct address (Minneapolis 3rd Ave N r=4) and beats PG's wrong answer (Hanover 3rd St NE r=38).**

## Final state

**Parity (vs `pg_parity.duckdb` reference):**

- 29/51 strict match (was 26/51 in main)
- Of the 22 remaining divergences, the majority are now **us-better-than-PG**:
  - #1073a: we find Minneapolis r=4, PG returns Hanover r=38 (PG's wrong answer due to `LIMIT 10` in DISTINCT-ON pre-final-sort)
  - #1145a/b/c/e: Mechanism B improvements (numeric_streets_equal)
  - T18a: Mechanism A improvement (post-PAGC validation)
- The few "us-worse" divergences are sub-meter geom precision drift on row-3+ candidates.

**Performance (5 reps, 30 representative inputs):**

| Mode | main | da5d847 | dfcd06d | Δ vs main |
|---|---|---|---|---|
| Sequential (one call/input) | 13.62s | 20.00s | 23.58s | +73% |
| **Batched (LATERAL one call)** | **1.08s** | **1.21s** | **1.09s** | **+1%** |

Batched is the realistic regime for this extension (ETL); the cost is essentially noise. Sequential is slower because of the dual-formula rating expression, but sequential isn't a real use case for batch geocoding.

**Theoretical exception:** the original concern was that `zip_info_exact=TRUE AND Pass A non-empty AND min_r ≥ 30` would short-circuit us out of fallback iteration where PG would still iterate. Resolved by removing the bifurcation entirely — when the parser is fixed, the loose-match candidates rate worse than Pass A's correct match anyway, so the bifurcation isn't load-bearing.

## Mechanisms A/B preserved

- Mechanism A (`from_pagc` post-PAGC validation, T18a-class): preserved. The new parser-level numeric-suffix recombination is additive.
- Mechanism B (unconditional `numeric_streets_equal` in candidate-finding, #1145a/b/e-class): preserved. Still in `name_match_tlids_a`.
