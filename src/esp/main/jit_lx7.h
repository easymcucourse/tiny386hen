/*
 * jit_lx7.h — Layered JIT for tiny386 on Xtensa LX7 (ESP32-S3)
 *
 * SCOPE: Low-difficulty instructions only
 *   - Register ALU : MOV, ADD, SUB, AND, OR, XOR, NOT, NEG, INC, DEC
 *   - Immediate ALU: same ops with 8/32-bit immediates
 *   - Shifts       : SHL/SHR/SAR by imm8 or CL (register-only)
 *   - CMP/TEST + Jcc fusion (fused into single LX7 branch)
 *   - Unconditional JMP rel8 / rel32
 *
 * NOT handled here (fall back to interpreter):
 *   - Any memory operand (requires TLB, paging)
 *   - CALL / RET (stack manipulation)
 *   - Privileged / system instructions
 *   - FPU, MMX, SSE
 *   - 16-bit operand-size prefix (0x66)
 *
 * Memory for JIT code lives in a static IRAM buffer so the Xtensa CPU can
 * execute from it without needing PSRAM-XIP or run-time mprotect().
 *
 * PORTING NOTE:
 *   On Linux/other hosts the LX7 backend is a no-op; the interpreter runs
 *   unchanged.  Compile with -DBUILD_ESP32 to activate the JIT.
 */

#ifndef JIT_LX7_H
#define JIT_LX7_H

#include <stdint.h>
#include <stdbool.h>
#include "i386.h"

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

/* Number of cached basic blocks (power-of-2 for fast modulo) */
#define JIT_CACHE_ENTRIES   128

/* Max LX7 bytes emitted per basic block                              */
#define JIT_BLOCK_MAXBYTES  512

/* Max x86 instructions scanned in one basic block before giving up   */
#define JIT_SCAN_LIMIT      64

/* Total IRAM code pool (bytes).  Must be <= available IRAM headroom. */
#define JIT_POOL_SIZE       (JIT_CACHE_ENTRIES * JIT_BLOCK_MAXBYTES)

/* ------------------------------------------------------------------ */
/* x86 → LX7 register mapping                                         */
/*   x86 GPR index 0-7 (EAX..EDI) mapped to LX7 a3..a10              */
/*   a2  = cpu*  (preserved across entire block)                      */
/*   a11-a13 = scratch temporaries                                     */
/*   a0  = CALL0 return address                                        */
/* ------------------------------------------------------------------ */
#define LX7_CPU_REG     2   /* a2 holds CPUI386* */
#define LX7_GPR_BASE    3   /* a3 = EAX, a4 = ECX, ..., a10 = EDI */
#define LX7_TMP0       11
#define LX7_TMP1       12
#define LX7_TMP2       13

/* Convert x86 GPR index (0-7) to LX7 register number */
#define X86REG_TO_LX7(i)  ((i) + LX7_GPR_BASE)

/* ------------------------------------------------------------------ */
/* JIT block status                                                    */
/* ------------------------------------------------------------------ */
typedef enum {
    JIT_EMPTY   = 0,   /* slot unused */
    JIT_VALID   = 1,   /* translated and ready */
    JIT_NOJIT   = 2,   /* marked un-translatable (hard instructions found) */
} JITStatus;

/* ------------------------------------------------------------------ */
/* One cached basic block                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t  guest_paddr;  /* physical address of first x86 byte */
    uint32_t  guest_cs_base;/* CS.base at translation time        */
    uint8_t  *host_code;    /* pointer into jit_pool[]            */
    uint16_t  host_len;     /* bytes of LX7 code                  */
    uint16_t  x86_len;      /* bytes consumed in x86 stream       */
    JITStatus status;
} JITBlock;

/* ------------------------------------------------------------------ */
/* Global JIT state                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    JITBlock  blocks[JIT_CACHE_ENTRIES];
    uint8_t  *pool;         /* pointer to IRAM code pool          */
    uint32_t  pool_used;    /* bytes consumed in pool             */
    uint32_t  hits;         /* execution stats                    */
    uint32_t  misses;
    uint32_t  bailed;       /* blocks that could not be compiled  */
} JITState;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * jit_init() — must be called once before any translation.
 * On ESP32 the pool pointer must be in IRAM (caller provides it via
 * a static IRAM_ATTR buffer).  On other platforms pass NULL to
 * disable the JIT silently.
 */
void jit_init(JITState *jit, uint8_t *iram_pool);

/**
 * jit_try_execute() — attempt to run a JIT-compiled block.
 *
 * Returns true  if a compiled block was found and executed;
 *         false if the block is not cached (caller should interpret).
 *
 * On a true return cpu->next_ip has already been advanced past the block.
 */
bool jit_try_execute(JITState *jit, CPUI386 *cpu);

/**
 * jit_translate() — translate the basic block at cpu's current PC.
 * Called automatically by jit_try_execute() on a cache miss; exposed
 * here for testing.
 */
JITBlock *jit_translate(JITState *jit, CPUI386 *cpu);

/**
 * jit_invalidate_all() — flush the entire cache (e.g. after CR3 reload).
 */
void jit_invalidate_all(JITState *jit);

/**
 * jit_invalidate_page() — flush all blocks whose first byte falls in
 * the physical page starting at paddr (called on SMC detection).
 */
void jit_invalidate_page(JITState *jit, uint32_t paddr);

#endif /* JIT_LX7_H */
