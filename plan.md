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

## 2026-07-02 NOJIT Suppression and Opcode Triage

### Plan Added and Executed

- Add NOJIT-specific suppression/cooldown instead of treating every persistent
  NOJIT table hit as another full JIT miss path.
- Add a NOJIT hot histogram by physical address, bail reason, opcode key, and
  hit count, so opcode work can be chosen from actual repeated fallbacks.
- Decide whether adding more opcode support is useful only after the histogram
  identifies a hot true-unsupported opcode.

### Implementation

- Added `TINY386_JIT_PRESTEP_COOLDOWN_NOJIT`, default `4`, and wired it through
  the ESP-IDF build cache.
- On NOJIT table hits and sticky NOJIT hits, set the short prestep cooldown and
  count it with `nojit_cooldown_sets`.
- Added `nojit_hot` reporting for top NOJIT entries:
  `paddr`, `bail`, `op`, and `hits`.
- Updated `tools/bench_capture.py` with the new `nojit_cooldown_sets` column and
  fixed `nojit_hot hits=` parsing so it does not overwrite `jit_hits`.

### Validation

- `git diff --check`: passed, with only CRLF normalization warnings.
- ESP-IDF benchmark build passed:
  - `idf.py -C make/esp-ili9341 -B build_ili9341 -DBOARD=ili9341 -DTINY386_BENCH_PROFILE=2 build`
- Flashed COM19 successfully.
- Captured three 45s benchmark runs:
  - `serial_COM19_nojitcool4_run1_20260702.*`
  - `serial_COM19_nojitcool4_run2_20260702.*`
  - `serial_COM19_nojitcool4_run3_20260702.*`

### Three-Run Result

- Phase timing:
  - VGA3 to boot sector mean: `7351.0 ms`
  - boot sector to VGA1 mean: `12546.7 ms`
  - VGA3 to VGA1 mean: `19897.7 ms`
- Final 40s snapshot means:
  - `jit_hits=85.0`
  - `jit_misses=223.3`
  - apparent hit rate: about `27.6%`
  - `try_entries=2822.3`
  - `block_entries=85.0`
  - `miss_nojit_table=182.0`
  - `miss_hot_skip=34.3`
  - `miss_translate_bail=7.0`
  - `cache_misses=48.3`, all from `cache_empty`
  - `cache_conflict=0`
  - `smc_overlap_invalidations=0`
  - `smc_blocks_invalidated=0`
  - `full_flushes=23134.7`
  - `full_flush_invalidations=7.0`
  - `helper_actions=0`
  - `unsupported_total=7.0`
  - `lookup_cycles=354985.7`
  - `prestep_cooldown_skips=727.3`
  - `nojit_cooldown_sets=182.0`
- Compared with the previous no-NOJIT-cooldown run:
  - `miss_nojit_table`: `885.7` -> `182.0`, down about `79%`
  - `jit_misses`: `945.7` -> `223.3`, down about `76%`
  - `lookup_cycles`: `681967.7` -> `354985.7`, down about `48%`
  - VGA3 to VGA1: `19881.7 ms` -> `19897.7 ms`, effectively unchanged

### Opcode Decision

- Final `nojit_hot` entries were stable across the three runs:
  - `paddr=0x00110890 bail=disabled op=6a hits=77-82`
  - `paddr=0x001108a4 bail=disabled op=6a hits=45-50`
  - `paddr=0x000f37b9 bail=disabled op=4a hits=46`
  - `paddr=0x000f37ad bail=unsupported_opcode op=02 hits=8`
  - `paddr=0x000f37ba bail=unsupported_opcode op=c6 hits=1`
- More opcode coverage is not the next high-confidence fix. The hot entries are
  mostly `bail=disabled`, not true missing decode:
  - `6A` is `PUSH imm8`, already present behind a gate; earlier P7 testing made
    it look net-negative/helper-heavy when enabled.
  - `4A` is `DEC r32`, also a gated/unsafe-flags case rather than a missing
    decoder.
  - True unsupported `02` and `C6` are currently low-frequency in this window.
- Keep these opcodes disabled for now. Revisit opcode work only if the histogram
  shows a hot true-unsupported opcode or if the gated opcode can be made
  flags-safe and non-helper-heavy.

### DOSBENCH ASM Correction

- The first NOJIT-cooldown analysis used 45s captures and reached
  `BENCH_START`, but did not wait long enough for all `tools/dosbench.asm`
  `BENCH_CASE` results. That was not sufficient for the opcode decision.
- Re-ran the current NOJIT-cooldown build with three 90s captures:
  - `serial_COM19_nojitcool4_dosbench_run1_20260702.*`
  - `serial_COM19_nojitcool4_dosbench_run2_20260702.*`
  - `serial_COM19_nojitcool4_dosbench_run3_20260702.*`
- All three runs reached `BENCH_END AUTO` without panic/WDT.
- DOSBENCH asm case ticks:
  - `ALU`: `327, 327, 327`, mean `327.0`
  - `BRANCH`: `292, 292, 292`, mean `292.0`
  - `STACK`: `41, 41, 42`, mean `41.3`
  - `MEM`: `3, 3, 2`, mean `2.7`
  - `SMC`: `0, 0, 0`, mean `0.0`
- Existing three-run baselines for comparison:
  - JIT level0/interpreter:
    - `ALU=322.7`, `BRANCH=289.3`, `STACK=40.3`, `MEM=2.3`, `SMC=0.7`
  - JIT level3 default:
    - `ALU=323.0`, `BRANCH=289.0`, `STACK=40.0`, `MEM=2.3`, `SMC=0.7`
  - previous generic `cool4_nohotskip`:
    - `ALU=323.0`, `BRANCH=288.0`, `STACK=40.7`, `MEM=2.0`, `SMC=1.0`
- Corrected conclusion:
  - NOJIT cooldown reduces JIT lookup/fallback accounting cost, but it does not
    improve the DOSBENCH asm workload. On these cases it is slightly slower than
    the existing level3/default and interpreter baselines.
  - Opcode work should be judged by DOSBENCH asm deltas first, not by VGA phase
    timing or by lower lookup counters alone.
  - For now, do not enable or add opcode support just because `nojit_hot` points
    at `6A`/`4A`. A useful opcode change must improve at least one asm case
    without regressing the rest beyond normal 1-tick noise.

### Single-Opcode DOSBENCH Comparison

- Added a test-only compile-time gate:
  - `TINY386_JIT_ONLY_OPCODE=-1`: normal behavior.
  - `TINY386_JIT_ONLY_OPCODE=0xNN`: only that decoded opcode key may be JIT
    enabled; all other decoded actions are treated as disabled.
  - Extended `X86Action` with `opcode_key` so this is per opcode, not per broad
    action class.
- Built and tested three configs, each with three 90s DOSBENCH captures:
  - NOJIT baseline:
    - build: `build_opcode_nojit`
    - flags: `TINY386_JIT_LEVEL=0`
    - logs: `serial_COM19_opcode_nojit_run1..3_20260702.*`
  - single opcode `6A`:
    - build: `build_opcode_6a`
    - flags: `TINY386_JIT_LEVEL=3`, `TINY386_JIT_ONLY_OPCODE=0x6A`,
      `TINY386_JIT_ENABLE_PUSH_IMM8=1`, `TINY386_JIT_ENABLE_MEM_HELPERS=1`
    - logs: `serial_COM19_opcode_6a_run1..3_20260702.*`
  - single opcode `4A`:
    - build: `build_opcode_4a`
    - flags: `TINY386_JIT_LEVEL=3`, `TINY386_JIT_ONLY_OPCODE=0x4A`
    - logs: `serial_COM19_opcode_4a_run1..3_20260702.*`
- All nine captures reached `BENCH_END AUTO`; no panic/WDT was observed.

| config | ALU | BRANCH | STACK | MEM | SMC | translated | block_entries | helper_actions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| NOJIT | 327.7 | 292.7 | 41.3 | 3.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| only `6A` | 328.0 | 293.0 | 42.0 | 3.0 | 0.0 | 426.7 | 426.7 | 426.7 |
| only `4A` | 327.7 | 293.0 | 41.7 | 3.0 | 0.0 | 0.0 | 0.0 | 0.0 |

- Single-opcode conclusion:
  - `6A` (`PUSH imm8`) does get translated and executed, but every translated
    block still uses the helper path. DOSBENCH does not improve versus NOJIT;
    `STACK` is worse by about `0.7` tick and other cases are within/noisily
    worse than NOJIT.
  - `4A` (`DEC EDX`) produces no translated blocks in this workload under the
    existing flags-dead safety rule, so it is effectively equivalent to NOJIT.
  - Therefore neither hot opcode should be enabled as-is. The next useful test
    is not "more opcodes" broadly; it is a safe, non-helper implementation that
    shows a positive DOSBENCH case delta when enabled alone.

### Runtime INI Opcode Gates

- Added runtime `[jit]` keys so single-opcode DOSBENCH tests no longer require
  rebuilding the main firmware for each opcode:
  - `level`: runtime replacement for `TINY386_JIT_LEVEL`; `0` is the NOJIT
    baseline, `3` is the current full action gate level.
  - `only_opcode` / `opcode`: runtime replacement for
    `TINY386_JIT_ONLY_OPCODE`; accepts decimal or hex such as `0x6A`, and `-1`,
    `off`, or `none` restores normal multi-opcode gating.
  - `push_imm8`, `mem_helpers`, `inline_mem`, `stack_fastpath`,
    `cmptest_jcc`, `mov_ri`, `mov_rr`, `jmp`: runtime replacements for the
    matching action gates. Omitted keys keep the firmware build defaults.
- Example DOSBENCH asm test matrix using only `tiny386.ini` changes:
  - NOJIT baseline:
    - `[jit] level = 0`
  - only `6A` (`PUSH imm8`) helper test:
    - `[jit] level = 3`
    - `only_opcode = 0x6A`
    - `push_imm8 = 1`
    - `mem_helpers = 1`
  - only `4A` (`DEC EDX`) test:
    - `[jit] level = 3`
    - `only_opcode = 0x4A`
- Boot now prints one `[jit_config]` line with the effective runtime gate set.
  Use that serial line before each DOSBENCH capture to confirm the ini setting
  under test is actually active.
