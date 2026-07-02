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

/*
 * Unsupported-opcode hot histogram. Keys 0x000-0x0ff are one-byte opcodes;
 * keys 0x100-0x1ff are 0F xx extended opcodes.
 */
#ifndef TINY386_JIT_UNSUPPORTED_HIST
#define TINY386_JIT_UNSUPPORTED_HIST 1
#endif

#ifndef TINY386_JIT_UNSUPPORTED_TOP
#define TINY386_JIT_UNSUPPORTED_TOP 8
#endif

#ifndef TINY386_BENCH_PROFILE
#define TINY386_BENCH_PROFILE 0
#endif

#ifndef TINY386_JIT_SELFTEST_ONLY
#define TINY386_JIT_SELFTEST_ONLY 0
#endif

#ifndef TINY386_JIT_SELFTEST_AT_BOOT
#define TINY386_JIT_SELFTEST_AT_BOOT 0
#endif

#ifndef TINY386_JIT_LEVEL
#define TINY386_JIT_LEVEL 0
#endif

#ifndef TINY386_JIT_ENABLE_LINKING
#define TINY386_JIT_ENABLE_LINKING 1
#endif

#ifndef TINY386_JIT_ENABLE_MOV_RR
#define TINY386_JIT_ENABLE_MOV_RR 1
#endif

#ifndef TINY386_JIT_ENABLE_MOV_RI
#define TINY386_JIT_ENABLE_MOV_RI 1
#endif

#ifndef TINY386_JIT_ENABLE_JMP
#define TINY386_JIT_ENABLE_JMP 0
#endif

#ifndef TINY386_JIT_ENABLE_MEM_HELPERS
#define TINY386_JIT_ENABLE_MEM_HELPERS 0
#endif

#ifndef TINY386_JIT_ENABLE_PUSH_IMM8
#define TINY386_JIT_ENABLE_PUSH_IMM8 0
#endif

#ifndef TINY386_JIT_ENABLE_INLINE_MEM
#define TINY386_JIT_ENABLE_INLINE_MEM 0
#endif

#ifndef TINY386_JIT_ENABLE_STACK_FASTPATH
#define TINY386_JIT_ENABLE_STACK_FASTPATH 0
#endif

#ifndef TINY386_JIT_ENABLE_CMPTEST_JCC
#define TINY386_JIT_ENABLE_CMPTEST_JCC 1
#endif

#ifndef TINY386_JIT_ENABLE_SMC_BITMAP
#define TINY386_JIT_ENABLE_SMC_BITMAP 1
#endif

#ifndef TINY386_JIT_ONLY_OPCODE
#define TINY386_JIT_ONLY_OPCODE -1
#endif

#ifndef TINY386_JIT_HOT_THRESHOLD
#define TINY386_JIT_HOT_THRESHOLD 2
#endif

#ifndef TINY386_JIT_PRESTEP_COOLDOWN
#define TINY386_JIT_PRESTEP_COOLDOWN 0
#endif

#ifndef TINY386_JIT_PRESTEP_COOLDOWN_HOTSKIP
#define TINY386_JIT_PRESTEP_COOLDOWN_HOTSKIP 0
#endif

#ifndef TINY386_JIT_PRESTEP_COOLDOWN_NOJIT
#define TINY386_JIT_PRESTEP_COOLDOWN_NOJIT 4
#endif

#define JIT_UNSUPPORTED_HIST_SIZE 512u

#define JIT_NOJIT_ENTRIES 512u
#define JIT_HOT_ENTRIES   512u
#define JIT_NOJIT_HIST_ENTRIES 32u
#define JIT_NOJIT_HIST_TOP     8u

/* Guest page granularity used for future self-modifying-code tracking. */
#define JIT_GUEST_PAGE_SIZE 4096u
#define JIT_GUEST_PAGE_MASK (~(JIT_GUEST_PAGE_SIZE - 1u))
#define JIT_SMC_PAGE_BITS   1024u
#define JIT_SMC_PAGE_WORDS  (JIT_SMC_PAGE_BITS / 32u)

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

/* CPUI386 phys_mem offsets in bytes (verified via static assert in i386.c). */
#define JIT_PHYS_MEM_OFF       876
#define JIT_PHYS_MEM_SIZE_OFF  880

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
    JIT_BLOCKF_LINKED_EXIT   = 1u << 5, /* Exit calls a cached successor block. */
} JITBlockFlags;

typedef struct {
    uint32_t guest_page;     /* Physical page base, masked with JIT_GUEST_PAGE_MASK. */
    uint16_t first_slot;     /* Optional index into JITState.blocks, 0xffff = empty. */
    uint16_t active_blocks;  /* Number of valid blocks that start/touch this page. */
} JITPageTrack;

typedef struct {
    uint32_t guest_paddr;
    uint8_t  valid;
    uint8_t  reserved;
    uint16_t bail;
    uint16_t opcode_key;
    uint16_t reserved2;
} JITNojitEntry;

typedef struct {
    uint32_t guest_paddr;
    uint8_t  valid;
    uint8_t  hits;
    uint16_t reserved;
} JITHotEntry;

typedef struct {
    uint32_t guest_paddr;
    uint32_t hits;
    uint16_t bail;
    uint16_t opcode_key;
    uint8_t  valid;
    uint8_t  reserved[3];
} JITNojitHistEntry;

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
    uint16_t      link_x86_insns;/* Additional insns executed by linked exit. */
    uint16_t      flags;         /* JITBlockFlags bitset. */
    uint16_t      link_slot;     /* Linked successor slot, 0xffff = none. */
    uint32_t      link_paddr;    /* Linked successor physical entry address. */
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
    uint32_t translate_attempts;
    uint32_t translated;
    uint32_t cache_misses;
    uint32_t cache_empty_slot_misses;
    uint32_t cache_conflict_misses;
    uint32_t cache_nojit_slot_misses;
    uint32_t cache_other_slot_misses;
    uint32_t sticky_nojit_hits;
    uint32_t miss_nojit_table;
    uint32_t miss_sticky_block;
    uint32_t miss_hot_skip;
    uint32_t miss_translate_bail;
    uint32_t nojit_table_sets;
    uint32_t hot_threshold_skips;
    uint32_t nojit_cooldown_sets;
    uint32_t jit_guest_insns;
    uint32_t emitted_x86_bytes;
    uint32_t emitted_host_bytes;
    uint32_t linked_exits;
    uint32_t helper_call_actions;
    uint32_t host_buffer_full;
    uint32_t pool_flushes;
    uint32_t invalidations; /* Cache entries dropped by SMC/reset. */
    uint32_t smc_flushes;   /* Page/range invalidations requested. */
    uint32_t smc_bitmap_misses;
    uint32_t smc_scans;
    uint32_t smc_false_positives;
    uint32_t smc_overlap_invalidations;
    uint32_t smc_valid_blocks_scanned;
    uint32_t smc_blocks_invalidated;
    uint32_t cache_conflict_invalidations;
    uint32_t full_flushes;
    uint32_t full_flush_invalidations;
    uint32_t pool_full_invalidations;
    uint32_t smc_page_bitmap[JIT_SMC_PAGE_WORDS]; /* Hashed pages with translated code. */
    JITNojitEntry nojit_table[JIT_NOJIT_ENTRIES];
    JITNojitHistEntry nojit_hist[JIT_NOJIT_HIST_ENTRIES];
    JITHotEntry hot_table[JIT_HOT_ENTRIES];
    uint32_t bail_counts[JIT_BAIL_POOL_FULL + 1u];
    uint32_t unsupported_opcode_counts[JIT_UNSUPPORTED_HIST_SIZE];
    uint32_t unsupported_opcode_total;
    uint32_t try_entries;
    uint32_t block_entries;
    uint32_t block_exits;
    uint32_t interp_exits;
    uint32_t prestep_cooldown;
    uint32_t prestep_cooldown_skips;
    uint64_t try_cycles;
    uint64_t lookup_cycles;
    uint64_t translate_cycles;
    uint64_t exec_cycles;
    uint64_t guest_ptr_cycles;
    uint64_t guest_scan_cycles;
    uint32_t guest_scan_bytes;
    uint32_t stats_ticks;   /* Attempts since last periodic stats dump. */
    uint32_t snapshot_last_jit_guest_insns;
} JITState;

typedef struct {
    int level;
    int only_opcode;
    uint8_t enable_mov_ri;
    uint8_t enable_mov_rr;
    uint8_t enable_jmp;
    uint8_t enable_mem_helpers;
    uint8_t enable_push_imm8;
    uint8_t enable_inline_mem;
    uint8_t enable_stack_fastpath;
    uint8_t enable_cmptest_jcc;
} JITRuntimeConfig;

/* ------------------------------------------------------------------ */
/* Public backend API                                                  */
/* ------------------------------------------------------------------ */

void jit_get_runtime_config(JITRuntimeConfig *config);
void jit_set_runtime_config(const JITRuntimeConfig *config);

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
 * jit_invalidate_range() - flush blocks overlapping a physical byte range.
 */
void jit_invalidate_range(JITState *jit, uint32_t paddr, uint32_t size);

/**
 * jit_invalidate_page() - flush blocks associated with one physical page.
 */
void jit_invalidate_page(JITState *jit, uint32_t paddr);

/**
 * jit_dump_perf_snapshot() - emit one low-frequency benchmark snapshot.
 *
 * The caller supplies wall-time and emulator-loop counters; the backend appends
 * JIT hit/miss/translation/cache/SMC counters in one stable line.
 */
void jit_dump_perf_snapshot(JITState *jit, const char *phase,
                            uint32_t ms, long ips, long cycles,
                            uint32_t pc_steps, uint32_t step_count,
                            uint32_t step_batch);

#endif /* JIT_X86_H */
