# Task 3.4 retry record

Date: 2026-07-01
Board: ESP32-S3 / ILI9341 / COM19

## Goal

Retry Task 3.4 direct block linking after the earlier blocker: JIT blocks are full Xtensa windowed ABI functions (`ENTRY a1,32` + `RETW.N`), so a raw `J` into another block entry is unsafe.

## Attempts

1. Re-checked the old blocker.
   - Raw direct `J` to another block entry is still unsafe because it would execute another `ENTRY` without a matching call-window setup.
   - Decided to try a conservative link-stub instead of reusing the unsafe jump idea.

2. Verified `CALL8` encoding locally.
   - Assembled and linked a tiny Xtensa sample with `xtensa-esp32s3-elf-as/ld/objdump`.
   - Confirmed final `CALL8` encoding uses opcode low bits `0x25` and a signed word offset from `((PC + 4) & ~3)`.

3. Implemented a first safe direct-link form.
   - Added `JIT_BLOCKF_LINKED_EXIT`, `link_slot`, `link_paddr`, and `link_x86_insns` metadata to `JITBlock`.
   - Added LX7 `CALL8` emitter and a linked fallthrough exit:
     - store source block dirty GPRs,
     - move `CPUI386*` from current `a2` into outgoing arg0 (`a10`),
     - `CALL8` the cached successor block,
     - `RETW.N` back to C after the successor returns.
   - Limited linking to already-valid, unlinked fallthrough successor blocks.
   - Added dependency invalidation: invalidating/replacing a target block also invalidates source blocks that link to it.
   - `jit_try_execute()` now returns source insns plus linked successor insns for step/cycle accounting.

4. Added verification coverage.
   - Added `LINK_FALLTHROUGH_MOV_EAX_NOP` board selftest.
   - Added `jit_cpu_translate_current()` helper so selftest can pre-translate a target without touching opaque `CPUI386` internals.
   - Made `TINY386_JIT_SELFTEST_ONLY` / `TINY386_JIT_SELFTEST_AT_BOOT` CMake-overridable while preserving normal defaults.

## Validation

- Default firmware build passed:
  `idf.py -C make/esp-ili9341 -B build_ili9341 -DBOARD=ili9341 -DTINY386_JIT_SELFTEST_ONLY=0 build`
- Selftest-only firmware build passed:
  `idf.py -C make/esp-ili9341 -B build_ili9341 -DBOARD=ili9341 -DTINY386_JIT_SELFTEST_ONLY=1 build`
- COM19 selftest-only app-flash passed.
- Serial result:
  - `PASS LINK_FALLTHROUGH_MOV_EAX_NOP_link (interp=2 jit=2)`
  - `summary: 97/97 PASS`
- Normal firmware was rebuilt, app-flashed back to COM19, and a 45s boot capture reached:
  - `Booting from 0000:7c00`
  - `set VGA mode 1`
  - no WDT or panic observed.

## Remaining risk / next retry target

- This is a conservative first slice: fallthrough links only when the successor is already cached.
- Taken Jcc linking and later patching of unresolved exits are still TODO.
- The implementation intentionally uses `CALL8` rather than a raw direct `J`, so it avoids corrupting Xtensa windowed ABI state but still pays the successor prologue/epilogue cost.
