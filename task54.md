# Task 5.4 attempt log

## 2026-07-01 attempt 1

- Goal: first conservative memory-operand slice for the LX7 JIT.
- Scope chosen: `MOV r32,[base+disp]` and `MOV [base+disp],r32` only.
- Decoder limits: 32-bit mode only, no prefixes, DS segment only through existing helpers, no SIB, no `[disp32]`, no ESP encoded through SIB.
- Runtime strategy: synchronize JIT GPRs into `CPUI386`, call `cpu_load32` / `cpu_store32` through small C helpers, then reload GPRs. This is slower than inline TLB/page walking but reuses interpreter memory semantics and SMC invalidation.
- Selftests added: `MOV_EAX_mem_EBX_disp8` and `MOV_mem_EBX_disp8_EAX`.
- Host build: PASS with ESP-IDF 5.5.1 using `idf.py -B ..\..\build_ili9341 -DBOARD=ili9341 build`.
- Board selftest-only build: PASS on COM19. New memory MOV cases passed and final summary was `99/99 PASS`.
- Normal firmware smoke: PASS on COM19. A 45s capture reached `Booting from 0000:7c00` at about 8.3s and `set VGA mode 1` at about 21.0s, with no WDT or panic.
- Result: Task 5.4 first slice succeeded. Remaining risk is performance and coverage: this helper-call path is intentionally conservative and still does not handle SIB, segment overrides, direct `[disp32]`, 8/16-bit memory forms, memory ALU/CMP/TEST, or inline TLB/page-walk fast paths.
