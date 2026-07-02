# tiny386hen Plan Notes

## 2026-07-02 JIT Low Hit-Rate Diagnosis

### Work Done

- Added miss reason counters:
  - `miss_nojit_table`
  - `miss_sticky_block`
  - `miss_hot_skip`
  - `miss_translate_bail`
- Added cache miss slot-state counters:
  - `cache_empty`
  - `cache_conflict`
  - `cache_nojit_slot`
  - `cache_other_slot`
- Added invalidation and SMC counters:
  - `smc_valid_blocks_scanned`
  - `smc_blocks_invalidated`
  - `cache_conflict_invalidations`
  - `full_flushes`
  - `full_flush_invalidations`
  - `pool_full_invalidations`
- Updated `tools/bench_capture.py` so the new counters are stable CSV columns.

### Validation

- `git diff --check`: passed, with only existing CRLF normalization warnings.
- ESP-IDF benchmark build passed:
  - `idf.py -C make/esp-ili9341 -B build_ili9341 -DBOARD=ili9341 -DTINY386_BENCH_PROFILE=2 build`
- Flashed COM19 successfully.
- Captured three 45s benchmark runs:
  - `serial_COM19_jitcounters2_run1_20260702.*`
  - `serial_COM19_jitcounters2_run2_20260702.*`
  - `serial_COM19_jitcounters2_run3_20260702.*`

### Three-Run Result

- Phase timing was stable:
  - VGA3 to boot sector mean: `7330.0 ms`
  - boot sector to VGA1 mean: `12551.7 ms`
  - VGA3 to VGA1 mean: `19881.7 ms`
- Final 40s snapshot means:
  - `jit_hits=167`
  - `jit_misses=945.7`
  - apparent hit rate: about `15.0%`
  - `try_entries=2921.3`
  - `block_entries=167`
  - JIT execution coverage: about `5.7%` of attempts
  - `miss_nojit_table=885.7`, about `93.6%` of all misses
  - `miss_hot_skip=51`
  - `miss_translate_bail=9`
  - `cache_misses=72`, all from `cache_empty`
  - `cache_conflict=0`
  - `cache_conflict_invalidations=0`
  - `smc_scans=293`
  - `smc_false_positives=293`
  - `smc_overlap_invalidations=0`
  - `smc_blocks_invalidated=0`
  - `full_flushes=23134.7`
  - `full_flush_invalidations=12`
  - `pool_full_invalidations=0`
  - `helper_actions=0`
  - `unsupported_total=7`

### Diagnosis

- The low hit rate is now mostly a JIT eligibility/accounting problem, not a
  block-cache collision problem. Persistent NOJIT entries are being hit
  repeatedly, and each NOJIT fallback is counted as a miss.
- Direct-mapped cache conflict is ruled out for this workload:
  `cache_conflict=0` and `cache_conflict_invalidations=0` in all runs.
- True SMC overlap is ruled out in this window:
  all bitmap-triggered scans were false positives and invalidated no blocks.
- `full_flushes` is very high, but most full flushes happen when there are no
  valid JIT blocks to drop. It explains `pool_epoch` churn, not the miss count.

### Next Step

- Add a cheap suppression path after NOJIT fallback, such as a short prestep
  cooldown on NOJIT table hits or a caller-side skip window.
- Success criteria:
  - lower `try_entries`
  - lower `miss_nojit_table`
  - lower `lookup_cycles`
  - no regression in VGA3 to VGA1 phase timing
