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
- sticky NOJIT hits, bails, host-buffer-full, pool epoch, pool flushes
- SMC flushes and invalidations
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
- `-DTINY386_JIT_ENABLE_CMPTEST_JCC=0`
- `-DTINY386_JIT_ENABLE_SMC_BITMAP=0`
- `-DTINY386_JIT_UNSUPPORTED_HIST=0`

Capture and summarize:

```powershell
python tools/bench_capture.py --port COM19 --duration 45 --csv bench.csv
python tools/bench_capture.py --input serial.log --csv bench.csv
```

## Current Decisions

| Candidate | Evidence | Expected benefit | Risk | Default gate | Validation |
| --- | --- | --- | --- | --- | --- |
| Extend opcode coverage for `6A PUSH imm8` | Task 5.5 boot capture showed `6A` dominating unsupported hits. | Reduce translate bails on the current DOS boot path. | Stack semantics and page/SMC interaction must stay exact. | Not implemented yet. | Differential selftests plus 3x boot captures and DOS stack microbench. |
| Block linking | Existing Task 3.4 link stub is conservative and now independently gateable. | Reduce C dispatch on hot fallthrough paths. | Xtensa windowed ABI makes raw jumps unsafe; linked invalidation must remain correct. | Enabled. | Compare `linked_exits`, wall time, and SMC selftests with gate on/off. |
| Helper-call memory actions | Current memory forms preserve interpreter load/store helpers. | More coverage, but helper calls can dominate execution cost. | May slow blocks that execute from PSRAM and bounce through C helpers. | Enabled. | Compare `helper_actions`, `jit_guest_pct`, and DOS memory microbench with gate on/off. |
| Inline memory fast path | Task 5.9 found no evidence that memory helper cost is the top bottleneck. | Potentially large if RAM-only aligned accesses dominate later. | High: paging, MMIO, split access, and SMC semantics. | Disabled/not implemented. | Only revisit after counters show helper actions dominate a phase. |
| SMC bitmap prefilter | Task 4.4 showed naive store scans slowed boot badly; bitmap avoids most scans. | Keeps store-triggered invalidation cheap. | Hash collisions can still cause extra scans, but not missed invalidations. | Enabled. | Compare `smc_flushes`, invalidations, and boot wall time with gate on/off. |
| Unsupported histogram | Directly identifies next opcode candidates. | Better prioritization. | Serial/stat overhead if dumped too often. | Enabled with low-frequency stats. | Compare with gate off when doing precise timing. |

## Empty Result Matrix

Fill this table only with repeated captures from the same firmware and workload.

| Date | Board/port | Profile | Gate delta | Run count | Window | Mean ips | Mean wall ms | JIT hit/miss | Translate/bail | Unsupported top | Result |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-07-02 | COM19 / CH340K | `TINY386_BENCH_PROFILE=2` | `TINY386_JIT_ENABLE_LINKING=0` | 1 | 45s boot/DOS window | sample windows: 550362, 1034608, 789002, 869913, then 33758/10665 during `6A` hotspot | 45s capture | final snapshot `189/1859` | final stats `1613/1588` | `6A` 1551 of 1580 | Harness works; repeat twice more before drawing perf conclusions. |
