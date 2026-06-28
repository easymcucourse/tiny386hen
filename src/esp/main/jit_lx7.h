/*
 * jit_lx7.h - layered JIT for tiny386 on Xtensa LX7 (ESP32-S3).
 *
 * Current scope: low-risk register-only blocks.
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
 * DOSBox-X dynrec pieces worth borrowing here:
 *   - Reference scope: design ideas only. This file does not copy DOSBox-X
 *     source code, macros, instruction emitters, cache implementation, or
 *     generated-code bytes; the LX7 JIT remains a separate implementation.
 *   - Cache entries describe both guest byte range and generated host range.
 *   - A block has a small, explicit exit reason instead of "just returned".
 *   - Code pages remember which translated blocks touch them, so SMC can
 *     invalidate precise blocks rather than flushing the whole pool.
 *   - Unsupported instructions are cached as NOJIT to avoid repeatedly
 *     rediscovering the same hard opcode.
 *
 * ESP32-S3 constraints that differ from DOSBox-X:
 *   - Generated code must live in instruction-accessible IRAM.
 *   - There is no mmap/mprotect/dual RW-RX mapping model.
 *   - Cache size is intentionally tiny; flushing all is acceptable as a first
 *     implementation, but the metadata below leaves room for precise SMC.
 *
 * PORTING NOTE:
 *   On Linux/other hosts the LX7 backend is a no-op; the interpreter runs
 *   unchanged. Compile with -DBUILD_ESP32 to activate the JIT.
 */

#ifndef JIT_LX7_H
#define JIT_LX7_H

#include <stdint.h>
#include <stdbool.h>
#include "i386.h"

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

/* Number of cached basic blocks (power-of-2 for fast modulo). */
#define JIT_CACHE_ENTRIES   512

/* Max LX7 bytes emitted per basic block. */
#define JIT_BLOCK_MAXBYTES  256

/* Max x86 instructions scanned in one basic block before giving up. */
#define JIT_SCAN_LIMIT      64

/* Total IRAM code pool (bytes). Must be <= available IRAM headroom. */
#define JIT_POOL_SIZE       65536

/*
 * Enable sticky NOJIT bail reasons in JITBlock.bail.
 * Set to 0 to keep the metadata quiet in very tight diagnostic builds.
 */
#ifndef TINY386_JIT_BAIL_REASONS
#define TINY386_JIT_BAIL_REASONS 1
#endif

/* Guest page granularity used for future self-modifying-code tracking. */
#define JIT_GUEST_PAGE_SIZE 4096u
#define JIT_GUEST_PAGE_MASK (~(JIT_GUEST_PAGE_SIZE - 1u))

/*
 * Maximum guest byte range recorded for one translated block.
 * This is metadata only; JIT_SCAN_LIMIT and JIT_BLOCK_MAXBYTES are still the
 * actual translation safety rails. Keeping the range explicit mirrors the
 * DOSBox-X dynrec cache-block model and makes future SMC invalidation local.
 */
#define JIT_BLOCK_MAX_GUEST_BYTES 128u

/* CPUI386 cc struct offsets in bytes (verified via static assert in i386.c). */
#define JIT_CC_OP_OFF       844
#define JIT_CC_DST_OFF      848
#define JIT_CC_DST2_OFF     852
#define JIT_CC_SRC1_OFF     856
#define JIT_CC_SRC2_OFF     860
#define JIT_CC_MASK_OFF     864

/* CPUI386 cc.op values (verified via static assert in i386.c). */
#define JIT_CC_ADD          1
#define JIT_CC_SUB          3
#define JIT_CC_NEG32        6
#define JIT_CC_AND          26
#define JIT_CC_OR           27
#define JIT_CC_XOR          28

/* CPUI386 cc.mask values (arithmetic vs logical). */
#define JIT_CC_MASK_ARITH   0x8D5
#define JIT_CC_MASK_LOGIC   0x8C5

/* ------------------------------------------------------------------ */
/* x86 to LX7 register mapping                                         */
/*   x86 GPR index 0-7 (EAX..EDI) mapped to LX7 a3..a10.              */
/*   a2  = cpu*  (preserved across entire block).                     */
/*   a11-a13 = scratch temporaries.                                   */
/*   a0  = CALL0 return address.                                      */
/* ------------------------------------------------------------------ */
#define LX7_CPU_REG     2   /* a2 holds CPUI386*. */
#define LX7_GPR_BASE    3   /* a3 = EAX, a4 = ECX, ..., a10 = EDI. */
#define LX7_TMP0       11
#define LX7_TMP1       12
#define LX7_TMP2       13

/* Convert x86 GPR index (0-7) to LX7 register number. */
#define X86REG_TO_LX7(i)  ((i) + LX7_GPR_BASE)

/* ------------------------------------------------------------------ */
/* JIT block status                                                    */
/* ------------------------------------------------------------------ */
typedef enum {
    JIT_EMPTY   = 0,   /* Slot unused. */
    JIT_VALID   = 1,   /* Translated and ready. */
    JIT_NOJIT   = 2,   /* Marked un-translatable. */
} JITStatus;

/*
 * Why a translated block returned to C.
 *
 * The current emitter always returns through the epilogue and updates next_ip,
 * so most blocks are effectively JIT_EXIT_FALLTHROUGH or JIT_EXIT_DIRECT_JMP.
 * This enum is deliberately ahead of the implementation: DOSBox-X keeps these
 * return classes explicit so direct block linking and slow-path exits can be
 * added without guessing what a cached block means.
 */
typedef enum {
    JIT_EXIT_UNKNOWN = 0,
    JIT_EXIT_FALLTHROUGH,
    JIT_EXIT_DIRECT_JMP,
    JIT_EXIT_COND_TAKEN,
    JIT_EXIT_COND_NOT_TAKEN,
    JIT_EXIT_INTERPRETER,    /* Unsupported opcode or state needs slow path. */
    JIT_EXIT_SMC,            /* Current block was invalidated while running. */
    JIT_EXIT_CYCLES,         /* Future cycle budget exit point. */
} JITExitKind;

/*
 * First reason a candidate block could not be translated.
 *
 * This is meant for serial diagnostics and for deciding whether NOJIT should
 * be sticky. For example, an unsupported opcode is sticky; pool exhaustion is
 * not, because a later global flush may make translation possible.
 */
typedef enum {
    JIT_BAIL_NONE = 0,
    JIT_BAIL_DISABLED,
    JIT_BAIL_CODE16,
    JIT_BAIL_PAGING,
    JIT_BAIL_OUT_OF_GUEST_MEM,
    JIT_BAIL_UNSUPPORTED_PREFIX,
    JIT_BAIL_UNSUPPORTED_OPCODE,
    JIT_BAIL_MEMORY_OPERAND,
    JIT_BAIL_FLAGS_UNSAFE,
    JIT_BAIL_HOST_BUFFER_FULL,
    JIT_BAIL_POOL_FULL,
} JITBailReason;

typedef enum {
    JIT_BLOCKF_NONE          = 0,
    JIT_BLOCKF_TOUCHES_CODE  = 1u << 0, /* Reserve for SMC/page tracking. */
    JIT_BLOCKF_ENDS_JMP      = 1u << 1,
    JIT_BLOCKF_ENDS_JCC      = 1u << 2,
    JIT_BLOCKF_PARTIAL       = 1u << 3, /* Emitted a safe prefix of a block. */
    JIT_BLOCKF_STICKY_NOJIT  = 1u << 4, /* Repeated attempts should bail fast. */
} JITBlockFlags;

/*
 * Lightweight page directory for future precise invalidation.
 *
 * DOSBox-X installs a code-page handler and invalidates only blocks whose
 * original guest bytes overlap a write. tiny386 does not have that hook here
 * yet, so this structure is only a contract: when memory writes grow an SMC
 * callback, the writer can call jit_invalidate_page() first, and later a more
 * precise range invalidator can use page/write bitmaps.
 */
typedef struct {
    uint32_t guest_page;     /* Physical page base, masked with JIT_GUEST_PAGE_MASK. */
    uint16_t first_slot;     /* Optional index into JITState.blocks, 0xffff = empty. */
    uint16_t active_blocks;  /* Number of valid blocks that start/touch this page. */
} JITPageTrack;

/* ------------------------------------------------------------------ */
/* One cached basic block                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t      guest_paddr;   /* Physical address of first x86 byte. */
    uint32_t      guest_end;     /* One-past-last guest byte translated. */
    uint32_t      guest_cs_base; /* CS.base at translation time. */
    uint8_t      *host_code;     /* Pointer into the IRAM code pool. */
    uint16_t      host_len;      /* Bytes of LX7 code. */
    uint16_t      x86_len;       /* Bytes consumed in x86 stream. */
    uint16_t      x86_insns;     /* Guest instructions consumed. */
    uint16_t      flags;         /* JITBlockFlags bitset. */
    JITExitKind   exit_kind;     /* Expected normal exit path. */
    JITBailReason bail;          /* Reason when status == JIT_NOJIT. */
    JITStatus     status;
} JITBlock;

/* ------------------------------------------------------------------ */
/* Global JIT state                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    JITBlock blocks[JIT_CACHE_ENTRIES];
    uint8_t *pool;          /* Pointer to IRAM code pool. */
    uint32_t pool_used;     /* Bytes consumed in pool. */
    uint32_t pool_epoch;    /* Increments whenever the pool flushes. */
    uint32_t hits;          /* Execution stats. */
    uint32_t misses;
    uint32_t bailed;        /* Blocks that could not be compiled. */
    uint32_t invalidations; /* Cache entries dropped by SMC/reset. */
    uint32_t smc_flushes;   /* Page/range invalidations requested. */
} JITState;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * jit_init() - must be called once before any translation.
 * On ESP32 the pool pointer must be in IRAM (caller provides it via
 * a static IRAM_ATTR buffer). On other platforms pass NULL to
 * disable the JIT silently.
 */
void jit_init(JITState *jit, uint8_t *iram_pool);

/**
 * jit_try_execute() - attempt to run a JIT-compiled block.
 *
 * Returns the number of guest instructions executed, or 0 if the block is not
 * cached/translatable and the caller should interpret. On a non-zero return,
 * cpu->next_ip has already been advanced past the block.
 */
int jit_try_execute(JITState *jit, CPUI386 *cpu);

/**
 * jit_translate() - translate the basic block at cpu's current PC.
 * Called automatically by jit_try_execute() on a cache miss; exposed
 * here for testing.
 */
JITBlock *jit_translate(JITState *jit, CPUI386 *cpu);

/**
 * jit_invalidate_all() - flush the entire cache.
 *
 * Borrowed design note from DOSBox-X dynrec: flushing all is simple and safe
 * for CR3/paging-mode changes, pool wraparound, and debug recovery. Later,
 * page/range invalidation should be preferred for ordinary guest code writes.
 */
void jit_invalidate_all(JITState *jit);

/**
 * jit_invalidate_page() - flush blocks associated with one physical page.
 *
 * Current implementation invalidates blocks whose first byte falls in the
 * page. The header records guest_end/flags so this can evolve into DOSBox-X
 * style overlap invalidation: invalidate any block whose source byte range
 * intersects the written guest range.
 */
void jit_invalidate_page(JITState *jit, uint32_t paddr);

#endif /* JIT_LX7_H */
