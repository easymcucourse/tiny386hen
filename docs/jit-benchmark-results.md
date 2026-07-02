# tiny386hen JIT Benchmark Results

## Protocol

Use fixed windows and keep serial output low-frequency. A benchmark run should
record at least three captures for the same firmware before treating a delta as
real. Single-run movement below 5% is noise; a regression above 10% should leave
the candidate gate disabled until explained.

Required counters per snapshot:

- wall time in milliseconds
- `ips`, guest cycle delta, `pc_steps`, and `step_count`
- JIT hits, misses, cache misses, translate attempts, successful translations
- sticky NOJIT hits, persistent NOJIT sets, hot-threshold skips, bails,
  host-buffer-full, pool epoch, pool flushes
- SMC flushes, invalidations, bitmap misses, bitmap-hit scans, false-positive
  scans, and true overlap invalidations
- emitted x86 bytes, emitted host bytes, linked exits, helper-call actions
- windowed JIT guest instruction delta and JIT guest percentage
- unsupported opcode total and top unsupported opcodes

## Capture

Build-time profiles:

- `-DTINY386_BENCH_PROFILE=0`: normal boot, no extra `[bench]` snapshots
- `-DTINY386_BENCH_PROFILE=1`: selftest benchmark labeling
- `-DTINY386_BENCH_PROFILE=2`: DOS microbench labeling

Useful A/B gates:

- `-DTINY386_JIT_LEVEL=0` versus default level 3
- `-DTINY386_JIT_ENABLE_LINKING=0`
- `-DTINY386_JIT_ENABLE_MEM_HELPERS=0`
- `-DTINY386_JIT_ENABLE_PUSH_IMM8=1`
- `-DTINY386_JIT_ENABLE_CMPTEST_JCC=0`
- `-DTINY386_JIT_ENABLE_SMC_BITMAP=0`
- `-DTINY386_JIT_HOT_THRESHOLD=1` versus default `2`
- `-DTINY386_JIT_UNSUPPORTED_HIST=0`

Capture and summarize:

```powershell
python tools/bench_capture.py --port COM19 --duration 45 --csv bench.csv
python tools/bench_capture.py --input serial.log --csv bench.csv --phase-csv bench.phase.csv
```

## Current Decisions

| Candidate | Evidence | Expected benefit | Risk | Default gate | Validation |
| --- | --- | --- | --- | --- | --- |
| Extend opcode coverage for `6A PUSH imm8` | Task 5.5 boot capture showed `6A` dominating unsupported hits; P7.3 selftest passed but the first benchmark became helper/translation heavy. | Can remove the dominant unsupported opcode, but only helps if helper-call stack blocks are not churned. | Stack semantics and page/SMC interaction must stay exact; helper-call PUSH currently expands translated block count badly. | Implemented but disabled by default: `TINY386_JIT_ENABLE_PUSH_IMM8=0`. | Keep as an experiment gate until stack coverage is broader or SMC/helper churn is reduced. |
| Block linking | Existing Task 3.4 link stub is conservative and now independently gateable. | Reduce C dispatch on hot fallthrough paths. | Xtensa windowed ABI makes raw jumps unsafe; linked invalidation must remain correct. | Enabled. | Compare `linked_exits`, wall time, and SMC selftests with gate on/off. |
| Helper-call memory actions | P7 fixed-phase gate bisect showed this is the DOS-stage net-negative item: default level3 missed VGA1 in 45s, while only changing `TINY386_JIT_ENABLE_MEM_HELPERS=0` reached VGA1 in all 3 runs. | More coverage when explicitly testing memory opcodes, but cold helper blocks currently cost more than they save. | May slow blocks that execute from PSRAM and bounce through C helpers; also increases translated SMC-sensitive blocks. | Disabled by default: `TINY386_JIT_ENABLE_MEM_HELPERS=0`. | Keep as an explicit experiment gate until inline memory fast paths or stronger phase filters exist. |
| Inline memory fast path | Task 5.9 found no evidence that memory helper cost is the top bottleneck. | Potentially large if RAM-only aligned accesses dominate later. | High: paging, MMIO, split access, and SMC semantics. | Disabled/not implemented. | Only revisit after counters show helper actions dominate a phase. |
| SMC bitmap prefilter | Task 4.4 showed naive store scans slowed boot badly; bitmap avoids most scans. | Keeps store-triggered invalidation cheap. | Hash collisions can still cause extra scans, but not missed invalidations. | Enabled. | Compare `smc_flushes`, invalidations, and boot wall time with gate on/off. |
| Persistent NOJIT + hot threshold | P6 showed `1588` bails but only `271` sticky NOJIT hits, implying direct-mapped NOJIT churn. | Reduce repeated unsupported scans and avoid translating one-shot cold blocks. | Stale NOJIT entries can preserve interpreter fallback after code replacement; correctness is preserved but performance analysis must account for it. | Enabled, threshold `2`. | Compare `translate_attempts`, `bailed`, `sticky_nojit`, `nojit_sets`, `hot_skips`, and phase wall time. |
| Unsupported histogram | Directly identifies next opcode candidates. | Better prioritization. | Serial/stat overhead if dumped too often. | Enabled with low-frequency stats. | Compare with gate off when doing precise timing. |

## Result Matrix

Fill this table only with repeated captures from the same firmware and workload.

| Date | Board/port | Profile | Gate delta | Run count | Window | Mean ips | Mean wall ms | JIT hit/miss | Translate/bail | Unsupported top | Result |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | `TINY386_JIT_ENABLE_LINKING=0` | 1 | 45s boot/DOS window | sample windows: 550362, 1034608, 789002, 869913, then 33758/10665 during `6A` hotspot | 45s capture | final snapshot `189/1859` | final stats `1613/1588` | `6A` 1551 of 1580 | Harness works; repeat twice more before drawing perf conclusions. |
| 2026-07-02 | COM19 / CH340K | normal profile | P7 first-slice default gates | 1 | 45s smoke | samples: 550355, 1040892, 794359, 874859, 724328, 522177, then 16452/16580 during DOS hotspot | 45s capture | not emitted in normal profile | not emitted in normal profile | not emitted in normal profile | Reached `Booting from 0000:7c00` and `set VGA mode 1`; use benchmark profile for P7 counter comparison. |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | P7 level3, hot threshold `2` | 3 | fixed markers: VGA3 -> boot sector -> VGA1 | final bench mean `10649` | VGA3 -> boot mean `7279.7 ms`; VGA1 not reached in 45s | final snapshots `171/1256`, `171/1552`, `171/1552` | final `26/16/10`; NOJIT sets `10`; hot skips `46` | unsupported total `8`; SMC scans were false positives (`302`, `302`, `302`) | Translation churn is fixed, but level3 still does not reach `set VGA mode 1` within the capture window. |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | P7 level0 baseline | 3 | fixed markers: VGA3 -> boot sector -> VGA1 | final bench mean `16580` | VGA3 -> boot mean `7278.0 ms`; VGA3 -> VGA1 mean `19905.7 ms` | final snapshots `0/1334`, `0/1337`, `0/1339` | final `16/0/16`, `17/0/17`, `19/0/19` | unsupported totals `16`, `17`, `19`; no SMC scans | Boot-sector timing is effectively tied with level3, while DOS reaches VGA1 reliably. Keep opcode coverage as the next priority. |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | P7 level3 + `TINY386_JIT_ENABLE_PUSH_IMM8=1` | 3 | fixed markers: VGA3 -> boot sector -> VGA1 | final bench mean `10660` | VGA3 -> boot mean `7280.0 ms`; VGA1 not reached in 45s | final snapshots `1405/1553`, `1400/1548`, `1344/1492` | final mean `1236/1228/8`; helper actions mean `1218`; hot skips `47` | `6A` gone; remaining unsupported total `6` | Do not enable by default: repeated runs confirm no phase benefit and a large translation/helper/SMC-churn increase. |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | P7 fixed-phase level ladder (`0/1/2/3`) | 3 each | fixed markers: VGA3 -> boot sector -> VGA1 | final snapshots: level0/1/2 about `16452-16580`, level3 `10665`, `10665`, one late high sample | VGA3 -> boot means: `7278.0`, `7278.0`, `7282.0`, `7280.0 ms`; VGA3 -> VGA1 means: `19906.0`, `19905.3`, `19912.0`, `NA` | level3 final `171/1256`, `171/1256`, `170/737` | level2 final about `20/5/15`; level3 final `26/16/10`, `26/16/10`, `25/15/10` | level3 unsupported total `8`; level3 SMC scans `302` | Level0/1/2 all reach VGA1; only level3 misses VGA1, so the tax is inside level3 gates rather than the generic JIT prestep entry. |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | P7 level3 + `TINY386_JIT_ENABLE_MEM_HELPERS=0` | 3 | fixed markers: VGA3 -> boot sector -> VGA1 | final bench mean `16452` | VGA3 -> boot mean `7282.0 ms`; VGA3 -> VGA1 mean `19875.7 ms` | final snapshots `166/1331`, `166/1328`, `166/1330` | final `24/11/13` in all runs | unsupported total `10-11`; helper actions `0`; SMC scans `293` | Single-variable bisect identifies helper-call memory actions as the DOS-stage net-negative gate. Default changed to off so cold helper translation cost is zero unless explicitly enabled. |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | P7 new default level3 (`TINY386_JIT_ENABLE_MEM_HELPERS=0` by default) | 3 | fixed markers: VGA3 -> boot sector -> VGA1 | final bench mean `16452` | VGA3 -> boot mean `7282.0 ms`; VGA3 -> VGA1 mean `19876.0 ms` | final snapshots `166/1331`, `166/1330`, `166/1330` | final `24/11/13` in all runs | unsupported total `11`; helper actions `0`; SMC scans `293` | Fresh build without passing the mem-helper gate explicitly matched the gate run and reached VGA1 in all captures. |
