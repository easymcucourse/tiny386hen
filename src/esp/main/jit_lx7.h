/*
 * jit_lx7.h - Xtensa LX7 backend for the generic x86 JIT interface.
 *
 * Current scope: low-risk register-only i386 blocks.
 *   - Register ALU : MOV, ADD, SUB, AND, OR, XOR, NOT, NEG, INC, DEC.
 *   - Immediate ALU: same ops with 8/32-bit immediates.
 *   - Shifts       : SHL/SHR/SAR by imm8 or CL, register-only.
 *   - CMP/TEST + Jcc fusion into native LX7 branches.
 *   - Unconditional JMP rel8 / rel32, gated until target tracing is solid.
 *
 * Current fallback surface:
 *   - Any memory operand. The LX7 backend does not own TLB/page-walk logic.
 *   - CALL / RET. These need x86 stack semantics and block-link boundaries.
 *   - Privileged/system instructions.
 *   - FPU, MMX, SSE.
 *   - 16-bit operand-size/address-size forms.
 *
 * ESP32-S3 constraints:
 *   - Generated code uses a PSRAM dual mapping: DBUS data alias for writes,
 *     IBUS exec alias for instruction fetch.
 *   - IRAM/DIRAM execution pools are intentionally abandoned. With ESP-IDF
 *     MEMPROT enabled, IRAM is effectively RX and board tests repeatedly hit
 *     CACHEERR when trying to write generated code there.
 *
 * PORTING NOTE:
 *   Generic x86 JIT state, block metadata, i386/i387 command declarations, and
 *   public JIT entry points live in jit_x86.h.  Future backends such as RISC-V
 *   should include jit_x86.h directly and provide their own backend header.
 */

#ifndef JIT_LX7_H
#define JIT_LX7_H

#include "jit_x86.h"

/* ------------------------------------------------------------------ */
/* x86 to LX7 register mapping                                         */
/*   x86 GPR index 0-7 (EAX..EDI) mapped to LX7 a3..a10.              */
/*   a2  = cpu*  (preserved across entire block).                     */
/*   a11-a13 = scratch temporaries.                                   */
/*   a0  = windowed ABI return address.                               */
/* ------------------------------------------------------------------ */
#define LX7_CPU_REG     2   /* a2 holds CPUI386*. */
#define LX7_GPR_BASE    3   /* a3 = EAX, a4 = ECX, ..., a10 = EDI. */
#define LX7_TMP0       11
#define LX7_TMP1       12
#define LX7_TMP2       13

/* Convert x86 GPR index (0-7) to LX7 register number. */
#define X86REG_TO_LX7(i)  ((i) + LX7_GPR_BASE)

#if defined(BUILD_ESP32) && defined(TINY386_ENABLE_JIT)
bool jit_pool_ready(void);

/*
 * Selftest-only action whitelist override.  While non-zero, jit_action_enabled()
 * uses this bitmask instead of TINY386_JIT_LEVEL gates so individual opcodes can
 * be exercised in isolation on the board.
 */
void jit_selftest_set_allowed_actions(uint32_t mask);
void jit_selftest_clear_allowed_actions(void);
#endif

#endif /* JIT_LX7_H */
