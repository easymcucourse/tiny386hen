/*
 * jit_x86.h - architecture-neutral x86 JIT/decoder interface.
 *
 * This header describes the x86-side contract shared by the current Xtensa LX7
 * backend and future backends such as RISC-V.  Backend-specific emitters,
 * register maps, cache maintenance, and executable memory allocation live in
 * their own headers/sources.
 */

#ifndef JIT_X86_H
#define JIT_X86_H

#include <stdbool.h>
#include <stdint.h>
#include "i386.h"

typedef struct FPU FPU;

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

/* Number of cached basic blocks (power-of-2 for fast modulo). */
#define JIT_CACHE_ENTRIES   512

/* Max host bytes emitted per basic block. */
#define JIT_BLOCK_MAXBYTES  256

/* Max x86 instructions scanned in one basic block before giving up. */
#define JIT_SCAN_LIMIT      64

/*
 * Total generated-code pool (bytes).  ESP32-S3 currently uses a PSRAM pool
 * with MMU page alignment; other backends may map this to their own allocator.
 */
#define JIT_POOL_SIZE       65536

/*
 * Enable sticky NOJIT bail reasons in JITBlock.bail.
 * Set to 0 to keep the metadata quiet in very tight diagnostic builds.
 */
#ifndef TINY386_JIT_BAIL_REASONS
#define TINY386_JIT_BAIL_REASONS 1
#endif

/*
 * Verbose translation/execution trace.  Keep disabled by default because the
 * serial path is slow enough to perturb normal boot smoke tests.
 */
#ifndef TINY386_JIT_TRACE
#define TINY386_JIT_TRACE 0
#endif

/*
 * Periodic stats dump cadence, measured in JIT attempts.  Set to 0 to disable.
 */
#ifndef TINY386_JIT_STATS_PERIOD
#define TINY386_JIT_STATS_PERIOD 0
#endif

/* Guest page granularity used for future self-modifying-code tracking. */
#define JIT_GUEST_PAGE_SIZE 4096u
#define JIT_GUEST_PAGE_MASK (~(JIT_GUEST_PAGE_SIZE - 1u))

/*
 * Maximum guest byte range recorded for one translated block.
 * This is metadata only; JIT_SCAN_LIMIT and JIT_BLOCK_MAXBYTES are still the
 * actual translation safety rails.
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
#define JIT_CC_DEC32        9
#define JIT_CC_INC32        12
#define JIT_CC_SAR          19
#define JIT_CC_SHL          20
#define JIT_CC_SHR          21
#define JIT_CC_AND          26
#define JIT_CC_OR           27
#define JIT_CC_XOR          28

/* CPUI386 cc.mask values (arithmetic vs logical). */
#define JIT_CC_MASK_ARITH   0x8D5
#define JIT_CC_MASK_ARITH_NO_CF 0x8D4
#define JIT_CC_MASK_LOGIC   0x8C5

/* ------------------------------------------------------------------ */
/* Generic x86 command model                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    X86_JIT_ISA_I386 = 1u << 0,
    X86_JIT_ISA_I387 = 1u << 1,
} X86JitIsa;

typedef enum {
    X86_CMD_NONE = 0,
    X86_CMD_I386,
    X86_CMD_I387,
    X86_CMD_UNSUPPORTED,
} X86CommandKind;

typedef enum {
    X86_I386_CMD_NONE = 0,
    X86_I386_CMD_NOP,
    X86_I386_CMD_MOV_RR,
    X86_I386_CMD_MOV_RI,
    X86_I386_CMD_ALU_RR,
    X86_I386_CMD_ALU_RI,
    X86_I386_CMD_NOT_R,
    X86_I386_CMD_NEG_R,
    X86_I386_CMD_INC_R,
    X86_I386_CMD_DEC_R,
    X86_I386_CMD_SHIFT_RI,
    X86_I386_CMD_SHIFT_CL,
    X86_I386_CMD_CMP_RR,
    X86_I386_CMD_CMP_RI,
    X86_I386_CMD_TEST_RR,
    X86_I386_CMD_JMP,
    X86_I386_CMD_JCC,
    X86_I386_CMD_CWDE,
    X86_I386_CMD_CDQ,
    X86_I386_CMD_XCHG_EAX_R,
    X86_I386_CMD_BSWAP_R,
    X86_I386_CMD_UNSUPPORTED,
} X86I386Command;

typedef enum {
    X86_I387_CMD_NONE = 0,
    X86_I387_CMD_ESC_D8,
    X86_I387_CMD_ESC_D9,
    X86_I387_CMD_ESC_DA,
    X86_I387_CMD_ESC_DB,
    X86_I387_CMD_ESC_DC,
    X86_I387_CMD_ESC_DD,
    X86_I387_CMD_ESC_DE,
    X86_I387_CMD_ESC_DF,
    X86_I387_CMD_UNSUPPORTED,
} X86I387Command;

typedef struct {
    X86CommandKind kind;
    uint32_t eip;
    uint8_t length;
    bool code16;
    union {
        X86I386Command i386;
        X86I387Command i387;
    } command;
} X86DecodedCommand;

/*
 * Frontend declarations for future shared decoders/executors.  They are not
 * implemented yet; the current LX7 backend still uses its local bring-up
 * decoder while this API settles.
 */
bool x86_decode_i386_command(const uint8_t *code, uint32_t eip,
                             bool code16, X86DecodedCommand *out);
bool x86_decode_i387_command(const uint8_t *code, uint32_t eip,
                             bool code16, X86DecodedCommand *out);
bool x86_execute_i386_command(CPUI386 *cpu, const X86DecodedCommand *command);
bool x86_execute_i387_command(FPU *fpu, CPUI386 *cpu,
                              const X86DecodedCommand *command);
const char *x86_command_name(const X86DecodedCommand *command);

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
    uint8_t      *host_code;     /* Pointer into backend executable code alias. */
    uint16_t      host_len;      /* Bytes of generated host code. */
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
    uint8_t *pool;          /* Pointer to backend write alias/code pool. */
    uint32_t pool_used;     /* Bytes consumed in pool. */
    uint32_t pool_epoch;    /* Increments whenever the pool flushes. */
    uint32_t hits;          /* Execution stats. */
    uint32_t misses;
    uint32_t bailed;        /* Blocks that could not be compiled. */
    uint32_t invalidations; /* Cache entries dropped by SMC/reset. */
    uint32_t smc_flushes;   /* Page/range invalidations requested. */
    uint32_t bail_counts[JIT_BAIL_POOL_FULL + 1u];
    uint32_t stats_ticks;   /* Attempts since last periodic stats dump. */
} JITState;

/* ------------------------------------------------------------------ */
/* Public backend API                                                  */
/* ------------------------------------------------------------------ */

/**
 * jit_init() - must be called once before any translation.
 *
 * The second argument is backend-owned scratch/code-pool input.  ESP32-S3 LX7
 * ignores it and uses the internally allocated PSRAM dual-map pool.
 */
void jit_init(JITState *jit, uint8_t *code_pool);

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
 * Called automatically by jit_try_execute() on a cache miss; exposed here for
 * testing and backend bring-up.
 */
JITBlock *jit_translate(JITState *jit, CPUI386 *cpu);

/**
 * jit_invalidate_all() - flush the entire cache.
 */
void jit_invalidate_all(JITState *jit);

/**
 * jit_invalidate_page() - flush blocks associated with one physical page.
 */
void jit_invalidate_page(JITState *jit, uint32_t paddr);

#endif /* JIT_X86_H */
