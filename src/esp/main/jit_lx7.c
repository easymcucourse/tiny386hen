/*
 * jit_lx7.c — Layered JIT: low-difficulty x86 → Xtensa LX7
 *
 * Handles only register-to-register ALU, immediate ALU, shifts,
 * CMP/TEST+Jcc fusion, and unconditional jumps.
 * Everything else falls back to the tiny386 interpreter.
 *
 * ── Xtensa LX7 binary encoding reference ──────────────────────────
 *
 *  All instructions are 3 bytes, stored little-endian:
 *    byte[0] = bits[ 7: 0]
 *    byte[1] = bits[15: 8]
 *    byte[2] = bits[23:16]
 *
 *  RRR format  (ADD, SUB, AND, OR, XOR, NEG, SLL, SRL, SRA, SSL, SSR)
 *    byte[2] = (t   << 4) | op1        t  = source-2, op1 = 0 for ALU
 *    byte[1] = (r   << 4) | s          r  = destination, s = source-1
 *    byte[0] = (op2 << 4) | op0        op0 = 0
 *
 *  RRI8 format (ADDI, L32I, S32I, BEQ, BNE, BLT, BGE, BLTU, BGEU)
 *    byte[2] = imm8
 *    byte[1] = (op_sub << 4) | s
 *    byte[0] = (t << 4) | op0
 *
 *  RI  format  (MOVI, BEQZ, BNEZ, BLTZ, BGEZ)
 *    byte[2] = imm[11:4]
 *    byte[1] = (imm[3:0] << 4) | reg
 *    byte[0] = opbyte
 *
 *  Shift-immediate format (SLLI, SRLI, SRAI)
 *    SLLI: byte[0] = (((32 - sa) & 0xf) << 4)
 *          byte[1] = (r << 4) | s
 *          byte[2] = ((((32 - sa) >> 4) & 1) << 4) | 1
 *    SRLI: byte[0] = (t << 4)
 *          byte[1] = (r << 4) | sa[3:0]
 *          byte[2] = 0x41
 *    SRAI: byte[0] = (t << 4)
 *          byte[1] = (r << 4) | sa[3:0]
 *          byte[2] = (sa[4] ? 0x31 : 0x21)
 *
 *  CALL format (J)
 *    bits[23:6]  = 18-bit signed word offset from aligned (PC+4)
 *    bits[ 5:0]  = 0b000110
 *
 *  Encoding values verified against Ghidra-Xtensa .sinc definitions.
 *  TODO: cross-validate the J/CALL-format with a cross-compiled binary.
 */

#include "jit_lx7.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#ifndef BUILD_ESP32
#error "jit_lx7.c requires BUILD_ESP32"
#endif

#include "sdkconfig.h"

#if !defined(CONFIG_SPIRAM) || !CONFIG_SPIRAM
#error "jit_lx7.c requires CONFIG_SPIRAM=1"
#endif

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_rom_sys.h"
#include "hal/mmu_types.h"

#if TINY386_JIT_TRACE
#define JIT_TRACEF(...) esp_rom_printf(__VA_ARGS__)
#else
#define JIT_TRACEF(...) do { } while (0)
#endif

static inline uint32_t jit_ccount(void)
{
    uint32_t ccount;
    __asm__ __volatile__("rsr.ccount %0" : "=a"(ccount));
    return ccount;
}

static inline uint32_t jit_cycles_since(uint32_t start)
{
    return jit_ccount() - start;
}

static uint8_t *s_jit_pool_write;
static uint8_t *s_jit_pool_exec;
static bool     s_jit_pool_psram;
static bool     s_jit_pool_tried;

#define JIT_LINK_NONE 0xffffu

static uint8_t *jit_pool_exec_for(const uint8_t *write_ptr)
{
    if (!write_ptr)
        return NULL;
    if (s_jit_pool_psram && s_jit_pool_write && s_jit_pool_exec)
        return s_jit_pool_exec + (write_ptr - s_jit_pool_write);
    return NULL;
}

static uint8_t *jit_acquire_pool(void)
{
    if (!s_jit_pool_tried) {
        s_jit_pool_tried = true;
        s_jit_pool_write = (uint8_t *)heap_caps_aligned_alloc(
            CONFIG_MMU_PAGE_SIZE,
            JIT_POOL_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_jit_pool_write) {
            esp_paddr_t  paddr  = 0;
            mmu_target_t target = 0;
            if (esp_mmu_vaddr_to_paddr(s_jit_pool_write, &paddr, &target) == ESP_OK
                && target == MMU_TARGET_PSRAM0) {
                void *exec = NULL;
                esp_err_t err = esp_mmu_map(
                    paddr,
                    JIT_POOL_SIZE,
                    MMU_TARGET_PSRAM0,
                    MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT,
                    ESP_MMU_MMAP_FLAG_PADDR_SHARED,
                    &exec);
                if (err == ESP_OK && exec) {
                    s_jit_pool_exec = (uint8_t *)exec;
                    s_jit_pool_psram = true;
                    /* Pre-invalidate I-cache for the entire exec region
                     * to clear stale lines from prior mappings. */
                    esp_cache_msync(exec, JIT_POOL_SIZE,
                                    ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                                    ESP_CACHE_MSYNC_FLAG_TYPE_INST);
                    __asm__ __volatile__("isync" ::: "memory");
                } else {
                    esp_rom_printf("[jit] psram exec map err=%d\n", (int)err);
                    heap_caps_free(s_jit_pool_write);
                    s_jit_pool_write = NULL;
                }
            } else {
                esp_rom_printf("[jit] psram vaddr->paddr failed\n");
                heap_caps_free(s_jit_pool_write);
                s_jit_pool_write = NULL;
            }
        }
        esp_rom_printf("[jit] pool write=%p exec=%p psram=%d\n",
                       (void *)s_jit_pool_write,
                       (void *)s_jit_pool_exec,
                       (int)s_jit_pool_psram);
    }
    return s_jit_pool_write;
}

#define JIT_DEFAULT_POOL (jit_acquire_pool())

bool jit_pool_ready(void)
{
    uint8_t *pool = jit_acquire_pool();
    bool ready = (pool != NULL) && s_jit_pool_psram && (s_jit_pool_exec != NULL);
    if (!ready)
        esp_rom_printf("[jit] PSRAM exec pool unavailable — JIT disabled\n");
    return ready;
}

#ifndef TINY386_JIT_LEVEL
#define TINY386_JIT_LEVEL 0
#endif

#ifndef TINY386_JIT_SINGLE_INSN_BLOCK
#define TINY386_JIT_SINGLE_INSN_BLOCK 0
#endif

/*
 * Bring-up gates for actions that are structurally simple but still need
 * board-level smoke tests. This follows the DOSBox-X dynrec habit of keeping
 * block kinds explicit and independently disableable while a backend matures.
 */
#ifndef TINY386_JIT_ENABLE_MOV_RR
#define TINY386_JIT_ENABLE_MOV_RR 1
#endif

#ifndef TINY386_JIT_ENABLE_MOV_RI
#define TINY386_JIT_ENABLE_MOV_RI 1
#endif

#ifndef TINY386_JIT_ENABLE_JMP
#define TINY386_JIT_ENABLE_JMP 0
#endif

#ifndef TINY386_JIT_MOV_RR_SINGLE_BLOCK
#define TINY386_JIT_MOV_RR_SINGLE_BLOCK 1
#endif

#ifndef TINY386_JIT_JMP_SINGLE_BLOCK
#define TINY386_JIT_JMP_SINGLE_BLOCK 1
#endif

/* ================================================================== */
/*  Section 1 — LX7 Binary Emitter                                    */
/* ================================================================== */

/*
 * Each emit_* function writes N bytes at *buf and advances *buf by N.
 * Register arguments use the physical LX7 register number (0-15).
 */

typedef uint8_t *EmitPtr;

/* ---- RRR format helpers ----------------------------------------- */

/* op2 codes for RRR ALU instructions (op1=0, op0=0) */
#define LX7_OP2_AND  0x1
#define LX7_OP2_OR   0x2
#define LX7_OP2_XOR  0x3
#define LX7_OP2_NEG  0x6  /* NEG ar, at  (as=0 fixed) */
#define LX7_OP2_ADD  0x8
#define LX7_OP2_SUB  0xC

/* Shift RRR (op1=1, op0=0): */
#define LX7_OP2_SRL  0x9  /* SRL ar, at (as=0) – shift by SAR register */
#define LX7_OP2_SRA  0x8  /* SRA ar, at (as=0) – shift by SAR register, op1=1 */
#define LX7_OP2_SLL  0xA  /* SLL ar, as (at=0) – shift by SAR register */

/* Set-shift-amount (op1=4, op0=0): */
#define LX7_OP2_SSL  0x0  /* SSL as – load SAR for left shift  (ar=1 fixed) */
#define LX7_OP2_SSR  0x0  /* SSR as – load SAR for right shift (ar=0 fixed) */

static inline void emit_rrr(EmitPtr *p, int op2, int op1, int r, int s, int t)
{
    (*p)[0] = (uint8_t)((t   << 4) | 0x00 /* op0=0 */);
    (*p)[1] = (uint8_t)((r   << 4) | s);
    (*p)[2] = (uint8_t)((op2 << 4) | op1);
    *p += 3;
}

/* ADD ar, as, at */
static inline void emit_add(EmitPtr *p, int r, int s, int t)
{ emit_rrr(p, LX7_OP2_ADD, 0, r, s, t); }

/* SUB ar, as, at */
static inline void emit_sub(EmitPtr *p, int r, int s, int t)
{ emit_rrr(p, LX7_OP2_SUB, 0, r, s, t); }

/* AND ar, as, at */
static inline void emit_and(EmitPtr *p, int r, int s, int t)
{ emit_rrr(p, LX7_OP2_AND, 0, r, s, t); }

/* OR  ar, as, at  (also used as MOV ar, as = OR ar, as, as) */
static inline void emit_or(EmitPtr *p, int r, int s, int t)
{ emit_rrr(p, LX7_OP2_OR,  0, r, s, t); }

/* XOR ar, as, at */
static inline void emit_xor(EmitPtr *p, int r, int s, int t)
{ emit_rrr(p, LX7_OP2_XOR, 0, r, s, t); }

/* NEG ar, at  (negate: ar = 0 - at) */
static inline void emit_neg(EmitPtr *p, int r, int t)
{ emit_rrr(p, LX7_OP2_NEG, 0, r, /*s=*/0, t); }

/* MOV.N ar, as.  The 3-byte OR alias is legal in objdump but traps on board. */
static inline void emit_mov(EmitPtr *p, int r, int s)
{
    (*p)[0] = (uint8_t)((r << 4) | 0x0d);
    (*p)[1] = (uint8_t)s;
    *p += 2;
}

/* SSL as  — load SAR = (32 - as) for left shifts */
static inline void emit_ssl(EmitPtr *p, int s)
{
    /* op2=4, op1=0, r=1(fixed), s=source, t=0 */
    emit_rrr(p, 0x4, 0x0, /*r=*/1, s, /*t=*/0);
}

/* SSR as  — load SAR = as for right shifts */
static inline void emit_ssr(EmitPtr *p, int s)
{
    /* op2=4, op1=0, r=0(fixed), s=source, t=0 */
    emit_rrr(p, 0x4, 0x0, /*r=*/0, s, /*t=*/0);
}

/* SLL ar, as  — shift as left by (32-SAR), result in ar */
static inline void emit_sll(EmitPtr *p, int r, int s)
{ emit_rrr(p, LX7_OP2_SLL, 0x1, r, s, /*t=*/0); }

/* SRL ar, at  — shift at right logically by SAR, result in ar */
static inline void emit_srl(EmitPtr *p, int r, int t)
{ emit_rrr(p, LX7_OP2_SRL, 0x1, r, /*s=*/0, t); }

/* SRA ar, at  — shift at right arithmetically by SAR, result in ar */
static inline void emit_sra(EmitPtr *p, int r, int t)
{ emit_rrr(p, LX7_OP2_SRA, 0x1, r, /*s=*/0, t); }

/* ---- Immediate shift: SLLI ar, as, sa  (sa = 1..31) ------------- */
static inline void emit_slli(EmitPtr *p, int r, int s, int sa)
{
    int n = 32 - (sa & 31);
    (*p)[0] = (uint8_t)((n & 0x0F) << 4);
    (*p)[1] = (uint8_t)((r << 4) | s);
    (*p)[2] = (uint8_t)(((n >> 4) << 4) | 0x01);
    *p += 3;
}

/* SRLI ar, at, sa  (sa = 0..15 only; use SSR+SRL for 16-31) */
static inline void emit_srli(EmitPtr *p, int r, int t, int sa)
{
    (*p)[0] = (uint8_t)(t << 4);
    (*p)[1] = (uint8_t)((r << 4) | (sa & 0x0F));
    (*p)[2] = 0x41;
    *p += 3;
}

/* SRAI ar, at, sa  (sa = 0..31) */
static inline void emit_srai(EmitPtr *p, int r, int t, int sa)
{
    (*p)[0] = (uint8_t)(t << 4);
    (*p)[1] = (uint8_t)((r << 4) | (sa & 0x0F));
    (*p)[2] = (uint8_t)((sa & 0x10) ? 0x31 : 0x21);
    *p += 3;
}

/* ---- RRI8 format helpers ---------------------------------------- */

static inline void emit_rri8(EmitPtr *p, int op_sub, int t, int s, int op0, int imm8)
{
    (*p)[0] = (uint8_t)((t << 4) | op0);
    (*p)[1] = (uint8_t)((op_sub << 4) | s);
    (*p)[2] = (uint8_t)(imm8 & 0xFF);
    *p += 3;
}

/* ADDI at, as, simm8  (sign-extended 8-bit immediate) */
static inline void emit_addi(EmitPtr *p, int t, int s, int imm8)
{
    emit_rri8(p, 0xC, t, s, 0x2, imm8);
}

/* L32I at, as, off8  (off8 is byte-offset/4, so actual_offset = off8<<2) */
static inline void emit_l32i(EmitPtr *p, int t, int s, int off8)
{
    emit_rri8(p, 0x2, t, s, 0x2, off8);
}

/* S32I at, as, off8  (stores at to mem[as + off8<<2]) */
static inline void emit_s32i(EmitPtr *p, int t, int s, int off8)
{
    emit_rri8(p, 0x6, t, s, 0x2, off8);
}

/* Two-register compare-and-branch (RRI8, offset = signed byte delta) */
static inline void emit_beq(EmitPtr *p, int s, int t, int off8)
{ emit_rri8(p, 0x1, t, s, 0x7, off8); }

/*   BNE  as, at, off8 */
static inline void emit_bne(EmitPtr *p, int s, int t, int off8)
{ emit_rri8(p, 0x9, t, s, 0x7, off8); }

/*   BLT  as, at, off8  (signed) */
static inline void emit_blt(EmitPtr *p, int s, int t, int off8)
{ emit_rri8(p, 0x2, t, s, 0x7, off8); }

/*   BGE  as, at, off8  (signed) */
static inline void emit_bge(EmitPtr *p, int s, int t, int off8)
{ emit_rri8(p, 0xA, t, s, 0x7, off8); }

/*   BLTU as, at, off8  (unsigned) */
static inline void emit_bltu(EmitPtr *p, int s, int t, int off8)
{ emit_rri8(p, 0x3, t, s, 0x7, off8); }

/*   BGEU as, at, off8  (unsigned) */
static inline void emit_bgeu(EmitPtr *p, int s, int t, int off8)
{ emit_rri8(p, 0xB, t, s, 0x7, off8); }

/* ---- RI format: zero-compare branches (offset = signed 12-bit) -- */
/*   BEQZ as, imm12 */
static inline void emit_beqz(EmitPtr *p, int s, int imm12)
{
    (*p)[0] = 0x16;
    (*p)[1] = (uint8_t)(((imm12 & 0xF) << 4) | s);
    (*p)[2] = (uint8_t)((imm12 >> 4) & 0xFF);
    *p += 3;
}
/*   BNEZ as, imm12 */
static inline void emit_bnez(EmitPtr *p, int s, int imm12)
{
    (*p)[0] = 0x56;
    (*p)[1] = (uint8_t)(((imm12 & 0xF) << 4) | s);
    (*p)[2] = (uint8_t)((imm12 >> 4) & 0xFF);
    *p += 3;
}
/*   BLTZ as, imm12  (branch if as < 0, i.e. sign-flag set) */
static inline void emit_bltz(EmitPtr *p, int s, int imm12)
{
    (*p)[0] = 0x96;
    (*p)[1] = (uint8_t)(((imm12 & 0xF) << 4) | s);
    (*p)[2] = (uint8_t)((imm12 >> 4) & 0xFF);
    *p += 3;
}
/*   BGEZ as, imm12  (branch if as >= 0, i.e. sign-flag clear) */
static inline void emit_bgez(EmitPtr *p, int s, int imm12)
{
    (*p)[0] = 0xD6;
    (*p)[1] = (uint8_t)(((imm12 & 0xF) << 4) | s);
    (*p)[2] = (uint8_t)((imm12 >> 4) & 0xFF);
    *p += 3;
}

/* ---- RI format: MOVI at, simm12 ---------------------------------- */
static inline void emit_movi(EmitPtr *p, int t, int imm12)
{
    (*p)[0] = (uint8_t)((t << 4) | 0x2);
    (*p)[1] = (uint8_t)(0xA0 | ((imm12 >> 8) & 0xF));
    (*p)[2] = (uint8_t)(imm12 & 0xFF);
    *p += 3;
}

/* ---- CALL format: J offset18 ------------------------------------ */
/*
 * J target: newPC = PC + 4 + sign_extend(off18)
 *   off18 = target - (pc + 4)
 *
 * Encoding:
 *   bits[5:0]  = 0b000110 = 6
 *   bits[23:6] = off18[17:0]
 */
static inline void emit_j(EmitPtr *p, int32_t off18)
{
    uint32_t enc = (uint32_t)(((uint32_t)off18 & 0x3FFFF) << 6) | 0x06;
    (*p)[0] = (uint8_t)(enc & 0xFF);
    (*p)[1] = (uint8_t)((enc >>  8) & 0xFF);
    (*p)[2] = (uint8_t)((enc >> 16) & 0xFF);
    *p += 3;
}

static inline void emit_j_target(EmitPtr *p, const uint8_t *target)
{
    const uint8_t *pc = *p;
    emit_j(p, (int32_t)(target - (pc + 4)));
}

static inline bool call8_target_reachable(const uint8_t *pc, const uint8_t *target)
{
    intptr_t base = ((intptr_t)pc + 4) & ~(intptr_t)3;
    intptr_t delta = (intptr_t)target - base;

    if ((delta & 3) != 0)
        return false;

    int32_t off = (int32_t)(delta >> 2);
    return off >= -131072 && off <= 131071;
}

static inline bool emit_call8_target(EmitPtr *p, const uint8_t *target)
{
    const uint8_t *pc = *p;
    intptr_t base = ((intptr_t)pc + 4) & ~(intptr_t)3;
    int32_t off = (int32_t)(((intptr_t)target - base) >> 2);

    if (!call8_target_reachable(pc, target))
        return false;

    uint32_t enc = ((uint32_t)off & 0x3ffffu) << 6 | 0x25u;
    (*p)[0] = (uint8_t)(enc & 0xff);
    (*p)[1] = (uint8_t)((enc >> 8) & 0xff);
    (*p)[2] = (uint8_t)((enc >> 16) & 0xff);
    *p += 3;
    return true;
}

static inline void emit_callx8(EmitPtr *p, int s)
{
    (*p)[0] = 0xe0;
    (*p)[1] = (uint8_t)s;
    (*p)[2] = 0x00;
    *p += 3;
}

/* SRLI for shift amounts 16-31 (uses MOVI + SSR + SRL, 9 bytes) */
static void emit_ssri_wide(EmitPtr *p, int r, int sa)
{
    emit_movi(p, LX7_TMP2, sa);
    emit_ssr(p,  LX7_TMP2);
    emit_srl(p,  r, r);
}

static void emit_srli_any(EmitPtr *p, int r, int sa)
{
    if (sa <= 15) {
        emit_srli(p, r, r, sa);
    } else {
        emit_ssri_wide(p, r, sa);
    }
}

/* ---- RETW.N: windowed return ------------------------------------- */
static inline void emit_retw(EmitPtr *p)
{
    /* RETW.N — narrow windowed return: encoding 0x1D 0xF0            */
    (*p)[0] = 0x1D;
    (*p)[1] = 0xF0;
    *p += 2;
}

static inline void emit_entry32(EmitPtr *p)
{
    /* ENTRY a1, 32 — windowed ABI prologue (allocates 32-byte frame) */
    (*p)[0] = 0x36;
    (*p)[1] = 0x41;
    (*p)[2] = 0x00;
    *p += 3;
}

/* Use macros in the translation hot path to force direct byte emission. */
#define emit_rrr(p, op2, op1, r, s, t) do {                 \
    (*(p))[0] = (uint8_t)(((t) << 4) | 0x00);                \
    (*(p))[1] = (uint8_t)(((r) << 4) | (s));                 \
    (*(p))[2] = (uint8_t)(((op2) << 4) | (op1));             \
    *(p) += 3;                                               \
} while (0)

#define emit_add(p, r, s, t) emit_rrr((p), LX7_OP2_ADD, 0, (r), (s), (t))
#define emit_sub(p, r, s, t) emit_rrr((p), LX7_OP2_SUB, 0, (r), (s), (t))
#define emit_and(p, r, s, t) emit_rrr((p), LX7_OP2_AND, 0, (r), (s), (t))
#define emit_or(p, r, s, t)  emit_rrr((p), LX7_OP2_OR,  0, (r), (s), (t))
#define emit_xor(p, r, s, t) emit_rrr((p), LX7_OP2_XOR, 0, (r), (s), (t))
#define emit_neg(p, r, t)    emit_rrr((p), LX7_OP2_NEG, 0, (r), 0, (t))
#define emit_ssl(p, s)       emit_rrr((p), 0x4, 0x0, 1, (s), 0)
#define emit_ssr(p, s)       emit_rrr((p), 0x4, 0x0, 0, (s), 0)
#define emit_sll(p, r, s)    emit_rrr((p), LX7_OP2_SLL, 0x1, (r), (s), 0)
#define emit_srl(p, r, t)    emit_rrr((p), LX7_OP2_SRL, 0x1, (r), 0, (t))
#define emit_sra(p, r, t)    emit_rrr((p), LX7_OP2_SRA, 0x1, (r), 0, (t))

#define emit_mov(p, r, s) do {                              \
    (*(p))[0] = (uint8_t)(((r) << 4) | 0x0d);                \
    (*(p))[1] = (uint8_t)(s);                                \
    *(p) += 2;                                               \
} while (0)

#define emit_slli(p, r, s, sa) do {                         \
    int _emit_n = 32 - ((sa) & 31);                          \
    (*(p))[0] = (uint8_t)((_emit_n & 0x0F) << 4);            \
    (*(p))[1] = (uint8_t)(((r) << 4) | (s));                 \
    (*(p))[2] = (uint8_t)(((_emit_n >> 4) << 4) | 0x01);     \
    *(p) += 3;                                               \
} while (0)

#define emit_srli(p, r, t, sa) do {                         \
    (*(p))[0] = (uint8_t)((t) << 4);                         \
    (*(p))[1] = (uint8_t)(((r) << 4) | ((sa) & 0x0F));       \
    (*(p))[2] = 0x41;                                        \
    *(p) += 3;                                               \
} while (0)

#define emit_srai(p, r, t, sa) do {                         \
    (*(p))[0] = (uint8_t)((t) << 4);                         \
    (*(p))[1] = (uint8_t)(((r) << 4) | ((sa) & 0x0F));       \
    (*(p))[2] = (uint8_t)(((sa) & 0x10) ? 0x31 : 0x21);      \
    *(p) += 3;                                               \
} while (0)

#define emit_rri8(p, op_sub, t, s, op0, imm8) do {          \
    (*(p))[0] = (uint8_t)(((t) << 4) | (op0));               \
    (*(p))[1] = (uint8_t)(((op_sub) << 4) | (s));            \
    (*(p))[2] = (uint8_t)((imm8) & 0xFF);                    \
    *(p) += 3;                                               \
} while (0)

#define emit_addi(p, t, s, imm8) emit_rri8((p), 0xC, (t), (s), 0x2, (imm8))
#define emit_l32i(p, t, s, off8) emit_rri8((p), 0x2, (t), (s), 0x2, (off8))
#define emit_s32i(p, t, s, off8) emit_rri8((p), 0x6, (t), (s), 0x2, (off8))
#define emit_beq(p, s, t, off8)  emit_rri8((p), 0x1, (t), (s), 0x7, (off8))
#define emit_bne(p, s, t, off8)  emit_rri8((p), 0x9, (t), (s), 0x7, (off8))
#define emit_blt(p, s, t, off8)  emit_rri8((p), 0x2, (t), (s), 0x7, (off8))
#define emit_bge(p, s, t, off8)  emit_rri8((p), 0xA, (t), (s), 0x7, (off8))
#define emit_bltu(p, s, t, off8) emit_rri8((p), 0x3, (t), (s), 0x7, (off8))
#define emit_bgeu(p, s, t, off8) emit_rri8((p), 0xB, (t), (s), 0x7, (off8))

#define emit_beqz(p, s, imm12) do {                         \
    (*(p))[0] = 0x16;                                        \
    (*(p))[1] = (uint8_t)((((imm12) & 0xF) << 4) | (s));     \
    (*(p))[2] = (uint8_t)(((imm12) >> 4) & 0xFF);            \
    *(p) += 3;                                               \
} while (0)

#define emit_bnez(p, s, imm12) do {                         \
    (*(p))[0] = 0x56;                                        \
    (*(p))[1] = (uint8_t)((((imm12) & 0xF) << 4) | (s));     \
    (*(p))[2] = (uint8_t)(((imm12) >> 4) & 0xFF);            \
    *(p) += 3;                                               \
} while (0)

#define emit_bltz(p, s, imm12) do {                         \
    (*(p))[0] = 0x96;                                        \
    (*(p))[1] = (uint8_t)((((imm12) & 0xF) << 4) | (s));     \
    (*(p))[2] = (uint8_t)(((imm12) >> 4) & 0xFF);            \
    *(p) += 3;                                               \
} while (0)

#define emit_bgez(p, s, imm12) do {                         \
    (*(p))[0] = 0xD6;                                        \
    (*(p))[1] = (uint8_t)((((imm12) & 0xF) << 4) | (s));     \
    (*(p))[2] = (uint8_t)(((imm12) >> 4) & 0xFF);            \
    *(p) += 3;                                               \
} while (0)

#define emit_movi(p, t, imm12) do {                         \
    (*(p))[0] = (uint8_t)(((t) << 4) | 0x2);                 \
    (*(p))[1] = (uint8_t)(0xA0 | (((imm12) >> 8) & 0xF));    \
    (*(p))[2] = (uint8_t)((imm12) & 0xFF);                   \
    *(p) += 3;                                               \
} while (0)

#define emit_j(p, off18) do {                               \
    uint32_t _emit_enc =                                    \
        (uint32_t)(((uint32_t)(off18) & 0x3FFFF) << 6) | 0x06u; \
    (*(p))[0] = (uint8_t)(_emit_enc & 0xFF);                 \
    (*(p))[1] = (uint8_t)((_emit_enc >> 8) & 0xFF);          \
    (*(p))[2] = (uint8_t)((_emit_enc >> 16) & 0xFF);         \
    *(p) += 3;                                               \
} while (0)

#define emit_j_target(p, target) do {                       \
    const uint8_t *_emit_pc = *(p);                          \
    emit_j((p), (int32_t)((target) - (_emit_pc + 4)));       \
} while (0)

#define call8_target_reachable(pc, target) ({                \
    intptr_t _emit_base = ((intptr_t)(pc) + 4) & ~(intptr_t)3; \
    intptr_t _emit_delta = (intptr_t)(target) - _emit_base;  \
    int32_t _emit_off = (int32_t)(_emit_delta >> 2);         \
    ((_emit_delta & 3) == 0) && _emit_off >= -131072 &&      \
        _emit_off <= 131071;                                \
})

#define emit_callx8(p, s) do {                              \
    (*(p))[0] = 0xe0;                                        \
    (*(p))[1] = (uint8_t)(s);                                \
    (*(p))[2] = 0x00;                                        \
    *(p) += 3;                                               \
} while (0)

#define emit_retw(p) do {                                   \
    (*(p))[0] = 0x1D;                                        \
    (*(p))[1] = 0xF0;                                        \
    *(p) += 2;                                               \
} while (0)

#define emit_entry32(p) do {                                \
    (*(p))[0] = 0x36;                                        \
    (*(p))[1] = 0x41;                                        \
    (*(p))[2] = 0x00;                                        \
    *(p) += 3;                                               \
} while (0)

/* ---- Load a 32-bit literal via L32R ----------------------------- */
/*
 * L32R at, imm16 (word offset from aligned PC)
 *   byte[0] = (t<<4) | 0x1
 *   byte[1] = imm16_lo
 *   byte[2] = imm16_hi
 *
 * To load a 32-bit constant we emit:
 *   J     .Lafter_literal    ; 3 bytes  (skip over literal)
 *   .word imm32              ; 4 bytes, 4-byte aligned
 * .Lafter_literal:
 *   L32R  at, .Lpool_entry   ; 3 bytes, loads backward literal
 *
 * LX7 accepts this backward literal form; forward inline literals were
 * observed to assemble out of range on the ESP32-S3 toolchain.
 */
static IRAM_ATTR int emit_movi32(EmitPtr *p, int t, uint32_t imm32)
{
    uint8_t *start = *p;

    /* If the value fits in a signed 12-bit immediate use MOVI (3 bytes) */
    int32_t sv = (int32_t)imm32;
    if (sv >= -2048 && sv <= 2047) {
        emit_movi(p, t, sv & 0xFFF);
        return (int)(*p - start);
    }

    uint8_t *pc_j = *p;
    *p += 3; /* reserve J to skip literal */

    /* Align to 4 bytes for the literal */
    uintptr_t cur = (uintptr_t)*p;
    int pad = (int)(((cur + 3) & ~(uintptr_t)3) - cur);
    *p += pad;

    uint8_t *pool = *p;
    /* Write literal */
    (*p)[0] = (uint8_t)(imm32 & 0xFF);
    (*p)[1] = (uint8_t)((imm32 >>  8) & 0xFF);
    (*p)[2] = (uint8_t)((imm32 >> 16) & 0xFF);
    (*p)[3] = (uint8_t)((imm32 >> 24) & 0xFF);
    *p += 4;

    uint8_t *pc_l32r = *p;
    *p += 3; /* reserve L32R */

    /* Back-patch J: off18 is a signed byte offset from pc + 4. */
    EmitPtr pj = pc_j;
    emit_j_target(&pj, pc_l32r);

    /* Back-patch L32R: EA = ((PC + 3) & ~3) + (sign_extend16(imm16) << 2). */
    uintptr_t aligned_pc = ((uintptr_t)pc_l32r + 3) & ~(uintptr_t)3;
    int32_t l32r_imm16 = (int32_t)(((intptr_t)pool - (intptr_t)aligned_pc) / 4);
    pc_l32r[0] = (uint8_t)((t << 4) | 0x1);
    pc_l32r[1] = (uint8_t)(l32r_imm16 & 0xFF);
    pc_l32r[2] = (uint8_t)((l32r_imm16 >> 8) & 0xFF);

    return (int)(*p - start);
}

/* ================================================================== */
/*  Section 2 — Block Prologue / Epilogue                             */
/*                                                                     */
/*  Calling convention: windowed (ENTRY a1,32 + RETW.N)               */
/*    Called from C via normal function pointer (compiler uses CALL8). */
/*    a0  = return address (set by CALL8, used by RETW.N)             */
/*    a1  = SP (adjusted by ENTRY)                                    */
/*    a2  = CPUI386* (arg0, preserved throughout block)               */
/*    a3  = EAX, a4 = ECX, a5 = EDX, a6 = EBX                        */
/*    a7  = ESP, a8 = EBP, a9 = ESI, a10 = EDI                       */
/*    a11-a13 = temporaries (flags/literals)                          */
/* ================================================================== */

/* Byte offset of gprx[i].r32 in CPUI386 (struct starts with gprx[8]) */
#define GPR_OFF(i)  ((i) * 4)           /* 0, 4, 8, … 28 */
#define NEXT_IP_OFF (8 * 4 + 4)         /* gprx[8]*4 bytes + ip(4) = 36 */

/* Emit prologue: ENTRY + load only x86 GPRs read by this block. */
static IRAM_ATTR void emit_prologue(EmitPtr *p, int cpu_reg, uint8_t load_mask)
{
    emit_entry32(p);
    for (int i = 0; i < 8; i++) {
        if (!(load_mask & (1u << i)))
            continue;
        /* L32I a(LX7_GPR_BASE+i), a(cpu_reg), GPR_OFF(i)/4 */
        emit_l32i(p, LX7_GPR_BASE + i, cpu_reg, GPR_OFF(i) / 4);
    }
}

/* Emit epilogue: store modified x86 GPRs back, set next_ip, RETW.N */
static IRAM_ATTR void emit_epilogue(EmitPtr *p, int cpu_reg, uint32_t next_ip, uint8_t store_mask)
{
    for (int i = 0; i < 8; i++) {
        if (!(store_mask & (1u << i)))
            continue;
        emit_s32i(p, LX7_GPR_BASE + i, cpu_reg, GPR_OFF(i) / 4);
    }
    /* Set cpu->next_ip = next_ip */
    emit_movi32(p, LX7_TMP0, next_ip);
    emit_s32i(p, LX7_TMP0, cpu_reg, NEXT_IP_OFF / 4);
    emit_retw(p);
}

static IRAM_ATTR bool emit_linked_exit(EmitPtr *p, const uint8_t *code_end, int cpu_reg,
                                       const JITBlock *target, uint8_t store_mask)
{
    int store_count = 0;

    if (!target || target->status != JIT_VALID || !target->host_code)
        return false;
    if ((target->flags & JIT_BLOCKF_LINKED_EXIT) != 0)
        return false;
    if (*p + 40 >= code_end)
        return false;

    for (int i = 0; i < 8; i++) {
        if (store_mask & (1u << i))
            store_count++;
    }
    if (!call8_target_reachable(*p + store_count * 3 + 2, target->host_code))
        return false;

    for (int i = 0; i < 8; i++) {
        if (!(store_mask & (1u << i)))
            continue;
        emit_s32i(p, LX7_GPR_BASE + i, cpu_reg, GPR_OFF(i) / 4);
    }

    /*
     * CALL8 rotates the register window; callee a2 is caller a10.
     * Preserve the normal C ABI shape by putting CPUI386* in outgoing arg0.
     */
    emit_mov(p, 10, cpu_reg);
    if (!emit_call8_target(p, target->host_code))
        return false;
    emit_retw(p);
    return true;
}

static IRAM_ATTR void emit_store_gprs(EmitPtr *p, int cpu_reg, uint8_t store_mask)
{
    for (int i = 0; i < 8; i++) {
        if (store_mask & (1u << i))
            emit_s32i(p, LX7_GPR_BASE + i, cpu_reg, GPR_OFF(i) / 4);
    }
}

static IRAM_ATTR void emit_load_gprs(EmitPtr *p, int cpu_reg, uint8_t load_mask)
{
    for (int i = 0; i < 8; i++) {
        if (load_mask & (1u << i))
            emit_l32i(p, LX7_GPR_BASE + i, cpu_reg, GPR_OFF(i) / 4);
    }
}

static void __attribute__((noinline))
jit_helper_mov_rm32(CPUI386 *cpu, uint32_t addr, uint32_t dst)
{
    uint32_t value = 0;
    if (cpu_load32(cpu, 3, addr, &value))
        cpui386_set_gpr(cpu, (int)dst, value);
}

static void __attribute__((noinline))
jit_helper_mov_mr32(CPUI386 *cpu, uint32_t addr, uint32_t value)
{
    (void)cpu_store32(cpu, 3, addr, value);
}

static uint32_t __attribute__((noinline))
jit_helper_load8(CPUI386 *cpu, uint32_t addr)
{
    uint8_t value = 0;
    (void)cpu_load8(cpu, 3, addr, &value);
    return value;
}

static uint32_t __attribute__((noinline))
jit_helper_load16(CPUI386 *cpu, uint32_t addr)
{
    uint16_t value = 0;
    (void)cpu_load16(cpu, 3, addr, &value);
    return value;
}

static uint32_t __attribute__((noinline))
jit_helper_load32(CPUI386 *cpu, uint32_t addr)
{
    uint32_t value = 0;
    (void)cpu_load32(cpu, 3, addr, &value);
    return value;
}

static void __attribute__((noinline))
jit_helper_store8(CPUI386 *cpu, uint32_t addr, uint32_t value)
{
    (void)cpu_store8(cpu, 3, addr, (uint8_t)value);
}

static void __attribute__((noinline))
jit_helper_store16(CPUI386 *cpu, uint32_t addr, uint32_t value)
{
    (void)cpu_store16(cpu, 3, addr, (uint16_t)value);
}

static void __attribute__((noinline))
jit_helper_store32(CPUI386 *cpu, uint32_t addr, uint32_t value)
{
    (void)cpu_store32(cpu, 3, addr, value);
}

static void __attribute__((noinline))
jit_helper_push_imm32(CPUI386 *cpu, uint32_t value)
{
    uint32_t old_esp = *(uint32_t *)((uint8_t *)cpu + GPR_OFF(4));
    uint32_t new_esp = old_esp - 4u;

    if (cpu_store32(cpu, 2, new_esp, value))
        *(uint32_t *)((uint8_t *)cpu + GPR_OFF(4)) = new_esp;
}

/* ================================================================== */
/*  Section 3 — x86 Instruction Decoder (scan-only, no execution)     */
/* ================================================================== */

/*
 * We read x86 bytes directly from cpu->phys_mem via the pre-decoded
 * physical address.  The scanner only runs when:
 *   - CPU is in 32-bit protected mode  (code16 == false)
 *   - No 0x66/0x67 operand-size or address-size prefix
 *   - No segment-override prefix (0x26/0x2E/0x36/0x3E/0x64/0x65)
 * Any prefix causes immediate bail-out.
 */

typedef struct {
    /* ModRM-derived operands */
    int  mod;       /* 0-3 */
    int  reg;       /* 0-7 */
    int  rm;        /* 0-7 */
    bool reg_only;  /* true when mod==3 (register operand only) */
} ModRM;

typedef struct {
    int base;
    int index;
    int scale;
    int32_t disp;
    int len;
    bool has_base;
    bool has_index;
} MemOp;

static inline IRAM_ATTR ModRM decode_modrm(uint8_t b)
{
    ModRM m;
    m.mod      = (b >> 6) & 3;
    m.reg      = (b >> 3) & 7;
    m.rm       =  b       & 7;
    m.reg_only = (m.mod == 3);
    return m;
}

static IRAM_ATTR bool decode_simple_memop(const uint8_t *src, const ModRM *m, MemOp *mem)
{
    int len = 0;
    int32_t disp = 0;
    int base = m->rm;
    int index = -1;
    int scale = 0;
    bool has_base = true;
    bool has_index = false;

    if (m->reg_only)
        return false;

    if (m->rm == 4) {
        uint8_t sib = src[len++];
        int sib_scale = (sib >> 6) & 3;
        int sib_index = (sib >> 3) & 7;
        int sib_base = sib & 7;

        if (sib_index != 4) {
            has_index = true;
            index = sib_index;
            scale = sib_scale;
        }
        if (m->mod == 0 && sib_base == 5)
            has_base = false;
        base = sib_base;
    } else if (m->mod == 0 && m->rm == 5) {
        has_base = false;
    }

    if (m->mod == 1) {
        disp = (int32_t)(int8_t)src[len++];
    } else if (m->mod == 2) {
        disp = (int32_t)((uint32_t)src[len] |
                         ((uint32_t)src[len + 1] << 8) |
                         ((uint32_t)src[len + 2] << 16) |
                         ((uint32_t)src[len + 3] << 24));
        len += 4;
    } else if (!has_base) {
        disp = (int32_t)((uint32_t)src[len] |
                         ((uint32_t)src[len + 1] << 8) |
                         ((uint32_t)src[len + 2] << 16) |
                         ((uint32_t)src[len + 3] << 24));
        len += 4;
    }

    mem->base = base;
    mem->index = index;
    mem->scale = scale;
    mem->disp = disp;
    mem->len = len;
    mem->has_base = has_base;
    mem->has_index = has_index;
    return true;
}

/* Decode the x86 instruction at *src.
 * Returns the number of x86 bytes consumed (0 = cannot handle).
 * Fills in the action to emit into the JIT block.
 *
 * "Action" is described as a simple tagged union below.
 */

typedef enum {
    ACT_NONE,
    ACT_NOP,        /* no-op; only advances next_ip; 30s serial smoke test passed. */
    ACT_MOV_RR,     /* dst_reg = src_reg; failed in level2 tests, see jit_action_enabled(). */
    ACT_MOV_RI,     /* dst_reg = imm32 */
    ACT_ALU_RR,     /* dst_reg op= src_reg  (ADD/SUB/AND/OR/XOR) */
    ACT_ALU_RI,     /* dst_reg op= imm32    (8 or 32-bit) */
    ACT_NOT_R,      /* dst_reg = ~dst_reg */
    ACT_NEG_R,      /* dst_reg = -dst_reg */
    ACT_INC_R,      /* dst_reg += 1; failed because x86 flags are not updated yet. */
    ACT_DEC_R,      /* dst_reg -= 1; failed because x86 flags are not updated yet. */
    ACT_SHx_RI,     /* shift dst_reg by imm5 (SHL/SHR/SAR) */
    ACT_SHx_CL,     /* shift dst_reg by CL   (SHL/SHR/SAR) */
    ACT_CMP_RR,     /* set cmp state: left_reg CMP right_reg */
    ACT_CMP_RI,     /* set cmp state: left_reg CMP imm32 */
    ACT_TEST_RR,    /* set test state: left_reg & right_reg */
    ACT_JMP,        /* unconditional jump; failed in BIOS relocation tests; needs tracing. */
    ACT_JCC,        /* conditional jump;   target_eip set */
    ACT_CWDE,       /* EAX = sign_extend(AX), no flags */
    ACT_CDQ,        /* EDX = sign_extend(EAX), no flags */
    ACT_XCHG_EAX_R, /* swap EAX with another GPR, no flags */
    ACT_BSWAP_R,    /* byte-swap a 32-bit GPR, no flags */
    ACT_MOV_RM32,   /* dst_reg = dword ptr [base_reg + disp] or [disp32] */
    ACT_MOV_MR32,   /* dword ptr [base_reg + disp] or [disp32] = src_reg */
    ACT_MOV_RM8,    /* dst r8 = byte ptr [mem] */
    ACT_MOV_MR8,    /* byte ptr [mem] = src r8 */
    ACT_MOV_RM16,   /* dst r16 = word ptr [mem] */
    ACT_MOV_MR16,   /* word ptr [mem] = src r16 */
    ACT_CMP_RM32,   /* cmp r32, dword ptr [mem] */
    ACT_CMP_MR32,   /* cmp dword ptr [mem], r32 */
    ACT_TEST_MR32,  /* test dword ptr [mem], r32 */
    ACT_PUSH_IMM8,  /* push sign-extended imm8 through SS:ESP */
    ACT_BLOCK_END,  /* last instruction of block, no jump */
} ActionType;

typedef enum {
    ALU_ADD=0, ALU_OR=1, ALU_ADC=2, ALU_SBB=3,
    ALU_AND=4, ALU_SUB=5, ALU_XOR=6, ALU_CMP=7,
} AluOp;

typedef enum {
    SH_SHL=4, SH_SHR=5, SH_SAR=7,
} ShiftOp;

/* x86 condition codes (low nibble of Jcc opcode) */
typedef enum {
    CC_O=0,CC_NO=1,CC_B=2,CC_NB=3,CC_Z=4,CC_NZ=5,
    CC_BE=6,CC_NBE=7,CC_S=8,CC_NS=9,
    CC_L=12,CC_NL=13,CC_LE=14,CC_NLE=15,
} CondCode;

typedef struct {
    ActionType type;
    int        dst;       /* x86 GPR index 0-7 */
    int        src;       /* x86 GPR index 0-7 (or shift amount) */
    int32_t    imm;       /* immediate value */
    int        mem_base;  /* x86 base GPR, or -1 for direct [disp32] */
    int        mem_index; /* x86 index GPR, or -1 for no index */
    int        mem_scale; /* SIB scale shift: 0,1,2,3 */
    int32_t    mem_disp;  /* signed displacement for simple memory operands */
    AluOp      alu_op;
    ShiftOp    sh_op;
    CondCode   cc;
    uint32_t   target_eip; /* for JMP/JCC: resolved target EIP */
    bool       flags_dead;
} X86Action;

static X86Action s_jit_scan_actions[JIT_SCAN_LIMIT];
static uint8_t   s_jit_scan_action_bytes[JIT_SCAN_LIMIT];

static const char *jit_action_name(ActionType type)
{
    switch (type) {
    case ACT_NONE:        return "NONE";
    case ACT_NOP:         return "NOP";
    case ACT_MOV_RR:      return "MOV_RR";
    case ACT_MOV_RI:      return "MOV_RI";
    case ACT_ALU_RR:      return "ALU_RR";
    case ACT_ALU_RI:      return "ALU_RI";
    case ACT_NOT_R:       return "NOT_R";
    case ACT_NEG_R:       return "NEG_R";
    case ACT_INC_R:       return "INC_R";
    case ACT_DEC_R:       return "DEC_R";
    case ACT_SHx_RI:      return "SHx_RI";
    case ACT_SHx_CL:      return "SHx_CL";
    case ACT_CMP_RR:      return "CMP_RR";
    case ACT_CMP_RI:      return "CMP_RI";
    case ACT_TEST_RR:     return "TEST_RR";
    case ACT_JMP:         return "JMP";
    case ACT_JCC:         return "JCC";
    case ACT_CWDE:        return "CWDE";
    case ACT_CDQ:         return "CDQ";
    case ACT_XCHG_EAX_R:  return "XCHG_EAX_R";
    case ACT_BSWAP_R:     return "BSWAP_R";
    case ACT_MOV_RM32:    return "MOV_RM32";
    case ACT_MOV_MR32:    return "MOV_MR32";
    case ACT_MOV_RM8:     return "MOV_RM8";
    case ACT_MOV_MR8:     return "MOV_MR8";
    case ACT_MOV_RM16:    return "MOV_RM16";
    case ACT_MOV_MR16:    return "MOV_MR16";
    case ACT_CMP_RM32:    return "CMP_RM32";
    case ACT_CMP_MR32:    return "CMP_MR32";
    case ACT_TEST_MR32:   return "TEST_MR32";
    case ACT_PUSH_IMM8:   return "PUSH_IMM8";
    case ACT_BLOCK_END:   return "BLOCK_END";
    default:              return "?";
    }
}

static IRAM_ATTR void jit_action_reg_masks(const X86Action *a, uint8_t *read_mask, uint8_t *write_mask)
{
    int dst_reg = a->dst & 7;
    int src_reg = a->src & 7;
    uint8_t dst = (uint8_t)(1u << dst_reg);
    uint8_t src = (uint8_t)(1u << src_reg);

    switch (a->type) {
    case ACT_MOV_RR:
        *read_mask |= src;
        *write_mask |= dst;
        break;
    case ACT_MOV_RI:
        *write_mask |= dst;
        break;
    case ACT_ALU_RR:
    case ACT_CMP_RR:
    case ACT_TEST_RR:
        *read_mask |= dst | src;
        if (a->type == ACT_ALU_RR)
            *write_mask |= dst;
        break;
    case ACT_ALU_RI:
    case ACT_NOT_R:
    case ACT_NEG_R:
    case ACT_INC_R:
    case ACT_DEC_R:
    case ACT_SHx_RI:
    case ACT_CMP_RI:
        *read_mask |= dst;
        if (a->type != ACT_CMP_RI)
            *write_mask |= dst;
        break;
    case ACT_SHx_CL:
        *read_mask |= dst | (1u << 1);
        *write_mask |= dst;
        break;
    case ACT_CWDE:
        *read_mask |= 1u << 0;
        *write_mask |= 1u << 0;
        break;
    case ACT_CDQ:
        *read_mask |= 1u << 0;
        *write_mask |= 1u << 2;
        break;
    case ACT_XCHG_EAX_R:
        *read_mask |= (1u << 0) | src;
        *write_mask |= (1u << 0) | src;
        break;
    case ACT_BSWAP_R:
        *read_mask |= dst;
        *write_mask |= dst;
        break;
    case ACT_MOV_RM32:
        dst = (uint8_t)(1u << (a->dst & 3));
        if (a->mem_base >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_base);
        if (a->mem_index >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_index);
        *write_mask |= dst;
        break;
    case ACT_MOV_RM8:
        dst = (uint8_t)(1u << (a->dst & 3));
        *read_mask |= dst;
        if (a->mem_base >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_base);
        if (a->mem_index >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_index);
        *write_mask |= dst;
        break;
    case ACT_MOV_RM16:
        *read_mask |= dst;
        if (a->mem_base >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_base);
        if (a->mem_index >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_index);
        *write_mask |= dst;
        break;
    case ACT_MOV_MR32:
    case ACT_MOV_MR16:
        *read_mask |= src;
        if (a->mem_base >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_base);
        if (a->mem_index >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_index);
        break;
    case ACT_MOV_MR8:
        *read_mask |= (uint8_t)(1u << (a->src & 3));
        if (a->mem_base >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_base);
        if (a->mem_index >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_index);
        break;
    case ACT_CMP_RM32:
        *read_mask |= dst;
        if (a->mem_base >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_base);
        if (a->mem_index >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_index);
        break;
    case ACT_CMP_MR32:
    case ACT_TEST_MR32:
        *read_mask |= src;
        if (a->mem_base >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_base);
        if (a->mem_index >= 0)
            *read_mask |= (uint8_t)(1u << a->mem_index);
        break;
    case ACT_PUSH_IMM8:
        *read_mask |= (uint8_t)(1u << 4);
        *write_mask |= (uint8_t)(1u << 4);
        break;
    default:
        break;
    }
}

static IRAM_ATTR void jit_actions_reg_masks(const X86Action *actions, int count,
                                            uint8_t *load_mask, uint8_t *store_mask)
{
    uint8_t read_mask = 0;
    uint8_t write_mask = 0;

    for (int i = 0; i < count; i++)
        jit_action_reg_masks(&actions[i], &read_mask, &write_mask);

    *load_mask = read_mask;
    *store_mask = write_mask;
}

/*
 * Decode one x86 instruction (32-bit mode, register operands only).
 * src      = pointer to x86 byte stream
 * eip      = current EIP (before this instruction)
 * action   = output
 * Returns number of bytes consumed, or 0 if unhandled/unsupported.
 */
static IRAM_ATTR int decode_x86_insn(const uint8_t *src, uint32_t eip, X86Action *a)
{
    int len = 0;
    memset(a, 0, sizeof(*a));
    a->mem_base = -1;
    a->mem_index = -1;

    uint8_t op = src[len++];
    bool opsz16 = false;

    /* Reject all prefixes */
    if (op == 0x66) {
        opsz16 = true;
        op = src[len++];
    }
    if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E ||
        op == 0x64 || op == 0x65 || op == 0x67 ||
        op == 0xF0 || op == 0xF2 || op == 0xF3)
        return 0;

    if (opsz16 && op != 0x89 && op != 0x8B)
        return 0;

    if (op == 0x90) {
        a->type = ACT_NOP;
        return len;
    }

    if (op >= 0x91 && op <= 0x97) {
        a->type = ACT_XCHG_EAX_R;
        a->src  = op & 7;
        return len;
    }

    if (op == 0x98) {
        a->type = ACT_CWDE;
        return len;
    }

    if (op == 0x99) {
        a->type = ACT_CDQ;
        return len;
    }

    if (op == 0x6A) {
        a->type = ACT_PUSH_IMM8;
        a->imm = (int32_t)(int8_t)src[len++];
        return len;
    }

    /* ── MOV r32, imm32   (B8+r id) ─────────────────────────── */
    if (op >= 0xB8 && op <= 0xBF) {
        a->type = ACT_MOV_RI;
        a->dst  = op - 0xB8;
        a->imm  = (int32_t)((uint32_t)src[len]     | ((uint32_t)src[len+1] << 8) |
                             ((uint32_t)src[len+2] << 16) | ((uint32_t)src[len+3] << 24));
        len += 4;
        return len;
    }

    /* ── INC r32 / DEC r32  (40-4F) ─────────────────────────── */
    /*
     * INC/DEC decode is correct, but enabling these actions is not safe yet.
     * Real x86 INC/DEC update OF/SF/ZF/AF/PF while preserving CF. The current
     * JIT emitter only changes the GPR, so BIOS code that branches on flags
     * after INC/DEC can diverge from the interpreter.
     *
     * Observed failures:
     *   - INC-only: WDT after the SeaBIOS PCI/MPTABLE area.
     *   - DEC-only: WDT around/after "Relocating init ...".
     */
    if (op >= 0x40 && op <= 0x47) {
        a->type = ACT_INC_R; a->dst = op - 0x40; return len;
    }
    if (op >= 0x48 && op <= 0x4F) {
        a->type = ACT_DEC_R; a->dst = op - 0x48; return len;
    }

    /* ── ALU r/m32, r32   (01,09,11,19,21,29,31,39) ─────────── */
    if ((op & 0x07) == 0x01 && op <= 0x39) {
        uint8_t alu = (op >> 3) & 7;
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) {
            MemOp mem;
            if (alu != ALU_CMP || !decode_simple_memop(src + len, &m, &mem))
                return 0;
            len += mem.len;
            a->type = ACT_CMP_MR32;
            a->src = m.reg;
            a->mem_base = mem.has_base ? mem.base : -1;
            a->mem_index = mem.has_index ? mem.index : -1;
            a->mem_scale = mem.scale;
            a->mem_disp = mem.disp;
            return len;
        }
        if (alu == ALU_CMP) {
            a->type = ACT_CMP_RR; a->dst = m.rm; a->src = m.reg;
        } else {
            a->type = ACT_ALU_RR; a->dst = m.rm; a->src = m.reg;
            a->alu_op = (AluOp)alu;
        }
        return len;
    }

    /* ── ALU r32, r/m32   (03,0B,13,1B,23,2B,33,3B) ─────────── */
    if ((op & 0x07) == 0x03 && op <= 0x3B) {
        uint8_t alu = (op >> 3) & 7;
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) {
            MemOp mem;
            if (alu != ALU_CMP || !decode_simple_memop(src + len, &m, &mem))
                return 0;
            len += mem.len;
            a->type = ACT_CMP_RM32;
            a->dst = m.reg;
            a->mem_base = mem.has_base ? mem.base : -1;
            a->mem_index = mem.has_index ? mem.index : -1;
            a->mem_scale = mem.scale;
            a->mem_disp = mem.disp;
            return len;
        }
        if (alu == ALU_CMP) {
            a->type = ACT_CMP_RR; a->dst = m.reg; a->src = m.rm;
        } else {
            a->type = ACT_ALU_RR; a->dst = m.reg; a->src = m.rm;
            a->alu_op = (AluOp)alu;
        }
        return len;
    }

    /* ── MOV r/m32, r32  (89)  /  MOV r32, r/m32  (8B) ──────── */
    /*
     * Register-only MOV has no x86 flag side effects, so it should be an easy
     * JIT action. In practice level2 MOV_RR-only tests still tripped WDT after
     * the SeaBIOS PCI/MPTABLE area, even after verifying the Xtensa mov.n bytes
     * with an assembler-generated table. Treat this as unresolved: likely an
     * interaction with block caching, stale translation, register save/restore,
     * or another interpreter/JIT state mismatch.
     */
    if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) {
            MemOp mem;
            if (!decode_simple_memop(src + len, &m, &mem))
                return 0;
            len += mem.len;
            if (op == 0x8A) {
                a->type = ACT_MOV_RM8;
                a->dst = m.reg;
            } else if (op == 0x88) {
                a->type = ACT_MOV_MR8;
                a->src = m.reg;
            } else if (op == 0x8B && opsz16) {
                a->type = ACT_MOV_RM16;
                a->dst = m.reg;
            } else if (op == 0x89 && opsz16) {
                a->type = ACT_MOV_MR16;
                a->src = m.reg;
            } else if (op == 0x8B) {
                a->type = ACT_MOV_RM32;
                a->dst = m.reg;
            } else {
                a->type = ACT_MOV_MR32;
                a->src = m.reg;
            }
            a->mem_base = mem.has_base ? mem.base : -1;
            a->mem_index = mem.has_index ? mem.index : -1;
            a->mem_scale = mem.scale;
            a->mem_disp = mem.disp;
            return len;
        }
        if (opsz16 || op == 0x88 || op == 0x8A)
            return 0;
        a->type = ACT_MOV_RR;
        a->dst  = (op == 0x89) ? m.rm  : m.reg;
        a->src  = (op == 0x89) ? m.reg : m.rm;
        return len;
    }

    if (op == 0xA1 || op == 0xA3) {
        int32_t disp = (int32_t)((uint32_t)src[len] |
                                 ((uint32_t)src[len + 1] << 8) |
                                 ((uint32_t)src[len + 2] << 16) |
                                 ((uint32_t)src[len + 3] << 24));
        len += 4;
        if (op == 0xA1) {
            a->type = ACT_MOV_RM32;
            a->dst = 0;
        } else {
            a->type = ACT_MOV_MR32;
            a->src = 0;
        }
        a->mem_base = -1;
        a->mem_index = -1;
        a->mem_disp = disp;
        return len;
    }

    /* ── TEST r/m32, r32  (85) ───────────────────────────────── */
    if (op == 0x85) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) {
            MemOp mem;
            if (!decode_simple_memop(src + len, &m, &mem))
                return 0;
            len += mem.len;
            a->type = ACT_TEST_MR32;
            a->src = m.reg;
            a->mem_base = mem.has_base ? mem.base : -1;
            a->mem_index = mem.has_index ? mem.index : -1;
            a->mem_scale = mem.scale;
            a->mem_disp = mem.disp;
            return len;
        }
        a->type = ACT_TEST_RR; a->dst = m.rm; a->src = m.reg;
        return len;
    }

    /* ── ALU r/m32, imm32 / imm8  (81 /0-7, 83 /0-7) ────────── */
    if (op == 0x81 || op == 0x83) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) return 0;
        int32_t imm;
        if (op == 0x83) {
            imm = (int32_t)(int8_t)src[len++]; /* sign-extend 8-bit */
        } else {
            imm = (int32_t)((uint32_t)src[len]     | ((uint32_t)src[len+1] << 8) |
                             ((uint32_t)src[len+2] << 16) | ((uint32_t)src[len+3] << 24));
            len += 4;
        }
        if (m.reg == ALU_CMP) {
            a->type = ACT_CMP_RI; a->dst = m.rm; a->imm = imm;
        } else {
            a->type = ACT_ALU_RI; a->dst = m.rm; a->imm = imm;
            a->alu_op = (AluOp)m.reg;
        }
        return len;
    }

    /* ── NOT / NEG r/m32  (F7 /2, F7 /3) ────────────────────── */
    if (op == 0xF7) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) return 0;
        if (m.reg == 2) { a->type = ACT_NOT_R; a->dst = m.rm; return len; }
        if (m.reg == 3) { a->type = ACT_NEG_R; a->dst = m.rm; return len; }
        return 0; /* MUL/DIV/IDIV not handled */
    }

    /* ── SHL/SHR/SAR r/m32, imm8  (C1 /4,5,7) ──────────────── */
    if (op == 0xC1) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) return 0;
        if (m.reg != SH_SHL && m.reg != SH_SHR && m.reg != SH_SAR) return 0;
        a->type   = ACT_SHx_RI;
        a->dst    = m.rm;
        a->sh_op  = (ShiftOp)m.reg;
        a->src    = src[len++] & 0x1F; /* imm5 */
        return len;
    }

    /* ── SHL/SHR/SAR r/m32, CL  (D3 /4,5,7) ────────────────── */
    if (op == 0xD3) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) return 0;
        if (m.reg != SH_SHL && m.reg != SH_SHR && m.reg != SH_SAR) return 0;
        a->type  = ACT_SHx_CL;
        a->dst   = m.rm;
        a->sh_op = (ShiftOp)m.reg;
        return len;
    }

    /* ── JMP rel8  (EB cb) ───────────────────────────────────── */
    /*
     * JMP target arithmetic uses 32-bit wraparound and emit_j_target() uses
     * the Xtensa assembler-confirmed rule target - (pc + 4). Even so, JMP-only
     * tests WDT during SeaBIOS relocation. Do not re-enable blindly; first add
     * a trace that compares decoded target_eip with interpreter execution.
     */
    if (op == 0xEB) {
        int8_t rel = (int8_t)src[len++];
        a->type       = ACT_JMP;
        a->target_eip = eip + (uint32_t)len + (uint32_t)(int32_t)rel;
        return len;
    }

    /* ── JMP rel32  (E9 cd) ──────────────────────────────────── */
    /*
     * Same failure status as rel8 JMP above. The decoder is kept so target
     * tracing can be added without rediscovering the instruction format.
     */
    if (op == 0xE9) {
        int32_t rel = (int32_t)((uint32_t)src[len]     | ((uint32_t)src[len+1] << 8) |
                                ((uint32_t)src[len+2] << 16) | ((uint32_t)src[len+3] << 24));
        len += 4;
        a->type       = ACT_JMP;
        a->target_eip = eip + (uint32_t)len + (uint32_t)rel;
        return len;
    }

    /* ── Jcc rel8  (70-7F) ───────────────────────────────────── */
    if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)src[len++];
        a->type       = ACT_JCC;
        a->cc         = (CondCode)(op & 0xF);
        a->target_eip = (uint32_t)((int32_t)(eip + len) + rel);
        return len;
    }

    /* ── Jcc rel32  (0F 80-8F) ───────────────────────────────── */
    if (op == 0x0F) {
        uint8_t op2 = src[len++];
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            a->type = ACT_BSWAP_R;
            a->dst  = op2 & 7;
            return len;
        }
        if (op2 >= 0x80 && op2 <= 0x8F) {
            int32_t rel = (int32_t)((uint32_t)src[len]     | ((uint32_t)src[len+1] << 8) |
                                    ((uint32_t)src[len+2] << 16) | ((uint32_t)src[len+3] << 24));
            len += 4;
            a->type       = ACT_JCC;
            a->cc         = (CondCode)(op2 & 0xF);
            a->target_eip = (uint32_t)((int32_t)(eip + len) + rel);
            return len;
        }
        return 0;
    }

    return 0; /* unhandled opcode */
}

/* ================================================================== */
/*  Section 4 — Code Generator                                        */
/* ================================================================== */

/*
 * Translates one X86Action into LX7 instructions.
 *
 * cmp_lreg / cmp_rreg / cmp_mode track the most recent CMP/TEST so
 * we can fuse it with the next Jcc.
 *
 *   cmp_mode: 0 = no pending CMP
 *             1 = CMP lreg, rreg  (use BEQ/BNE/BLT/BGE/BLTU/BGEU)
 *             2 = CMP lreg, imm32 (use BEQZ/BNEZ against (lreg-imm))
 *             3 = TEST lreg & rreg (use BEQZ/BNEZ against AND result)
 */
typedef struct {
    int      cmp_mode;
    int      cmp_lreg;  /* LX7 register */
    int      cmp_rreg;  /* LX7 register */
    int      cmp_tmp;   /* LX7 register holding (lreg - imm) for mode 2 */
} CmpState;

static IRAM_ATTR void emit_mem_address(EmitPtr *p, const X86Action *a, int dst, int tmp)
{
    if (a->mem_base >= 0) {
        emit_mov(p, dst, X86REG_TO_LX7(a->mem_base));
    } else {
        emit_movi(p, dst, 0);
    }

    if (a->mem_index >= 0) {
        emit_mov(p, tmp, X86REG_TO_LX7(a->mem_index));
        if (a->mem_scale != 0)
            emit_slli(p, tmp, tmp, a->mem_scale);
        emit_add(p, dst, dst, tmp);
    }

    if (a->mem_disp != 0 || (a->mem_base < 0 && a->mem_index < 0)) {
        emit_movi32(p, tmp, (uint32_t)a->mem_disp);
        emit_add(p, dst, dst, tmp);
    }
}

static IRAM_ATTR void emit_guest_ptr32(EmitPtr *p, const X86Action *a,
                                       int cpu_reg, int addr_reg,
                                       int tmp_reg, int ptr_reg)
{
    emit_mem_address(p, a, addr_reg, tmp_reg);
    emit_l32i(p, ptr_reg, cpu_reg, JIT_PHYS_MEM_OFF / 4);
    emit_add(p, ptr_reg, ptr_reg, addr_reg);
}

static IRAM_ATTR void emit_merge_u8(EmitPtr *p, int dst_reg, int value_reg, int tmp_reg, bool high)
{
    uint32_t clear_mask = high ? 0xffff00ffu : 0xffffff00u;

    emit_movi32(p, tmp_reg, clear_mask);
    emit_and(p, dst_reg, dst_reg, tmp_reg);
    emit_movi32(p, tmp_reg, 0xffu);
    emit_and(p, value_reg, value_reg, tmp_reg);
    if (high)
        emit_slli(p, value_reg, value_reg, 8);
    emit_or(p, dst_reg, dst_reg, value_reg);
}

static IRAM_ATTR void emit_merge_u16(EmitPtr *p, int dst_reg, int value_reg, int tmp_reg)
{
    emit_movi32(p, tmp_reg, 0xffff0000u);
    emit_and(p, dst_reg, dst_reg, tmp_reg);
    emit_movi32(p, tmp_reg, 0x0000ffffu);
    emit_and(p, value_reg, value_reg, tmp_reg);
    emit_or(p, dst_reg, dst_reg, value_reg);
}

/*
 * emit_action — translate one action.
 * Returns false if code buffer would overflow (host_len >= JIT_BLOCK_MAXBYTES).
 */
static IRAM_ATTR bool emit_action(EmitPtr *p, uint8_t *buf_end,
                                  const X86Action *a, CmpState *cs,
                                  uint32_t fallback_eip, int cpu_reg,
                                  uint8_t load_mask, uint8_t store_mask)
{
#define GUARD(n) if ((*p) + (n) > buf_end) return false

    int dr = X86REG_TO_LX7(a->dst);
    int sr = X86REG_TO_LX7(a->src);
    int t0 = LX7_TMP0, t1 = LX7_TMP1, t2 = LX7_TMP2;

    switch (a->type) {

    case ACT_NOP:
        break;

    case ACT_CWDE:
        GUARD(6);
        emit_slli(p, X86REG_TO_LX7(0), X86REG_TO_LX7(0), 16);
        emit_srai(p, X86REG_TO_LX7(0), X86REG_TO_LX7(0), 16);
        break;

    case ACT_CDQ:
        GUARD(3);
        emit_srai(p, X86REG_TO_LX7(2), X86REG_TO_LX7(0), 31);
        break;

    case ACT_XCHG_EAX_R:
        GUARD(6);
        emit_mov(p, t0, X86REG_TO_LX7(0));
        emit_mov(p, X86REG_TO_LX7(0), sr);
        emit_mov(p, sr, t0);
        break;

    case ACT_BSWAP_R: {
        int t2 = LX7_TMP2;
        GUARD(56);
        emit_mov(p, t0, dr);          /* b0 -> bits 24..31 */
        emit_mov(p, t1, dr);          /* b1 -> bits 16..23 */
        emit_mov(p, t2, dr);          /* b2 -> bits  8..15 */
        emit_slli(p, t0, t0, 24);
        emit_srli(p, t1, t1, 8);
        emit_slli(p, t1, t1, 24);
        emit_srli(p, t1, t1, 8);
        emit_srli(p, t2, t2, 8);
        emit_srli(p, t2, t2, 8);
        emit_slli(p, t2, t2, 24);
        emit_srli(p, t2, t2, 8);
        emit_srli(p, t2, t2, 8);
        emit_srli(p, dr, dr, 8);      /* b3 -> bits 0..7 */
        emit_srli(p, dr, dr, 8);
        emit_srli(p, dr, dr, 8);
        emit_or(p, dr, dr, t0);
        emit_or(p, dr, dr, t1);
        emit_or(p, dr, dr, t2);
        break;
    }

    case ACT_MOV_RM32:
        if (TINY386_JIT_ENABLE_INLINE_MEM) {
            GUARD(48);
            emit_guest_ptr32(p, a, cpu_reg, t0, t1, t2);
            emit_l32i(p, dr, t2, 0);
            break;
        }
        GUARD(112);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_mov_rm32);
        emit_movi(p, 12, a->dst);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        break;

    case ACT_MOV_MR32:
        if (TINY386_JIT_ENABLE_INLINE_MEM) {
            GUARD(48);
            emit_guest_ptr32(p, a, cpu_reg, t0, t1, t2);
            emit_s32i(p, sr, t2, 0);
            break;
        }
        GUARD(112);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_mov(p, 12, sr);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_mov_mr32);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        break;

    case ACT_MOV_RM8: {
        int byte_reg = a->dst & 3;
        int host_dst = X86REG_TO_LX7(byte_reg);
        bool high = (a->dst & 4) != 0;
        GUARD(160);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_load8);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_mov(p, t2, 10);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        emit_merge_u8(p, host_dst, t2, t1, high);
        break;
    }

    case ACT_MOV_MR8: {
        int byte_reg = a->src & 3;
        int host_src = X86REG_TO_LX7(byte_reg);
        bool high = (a->src & 4) != 0;
        GUARD(160);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        if (high) {
            emit_srli(p, t1, host_src, 8);
            emit_movi32(p, t2, 0xffu);
            emit_and(p, t1, t1, t2);
        } else {
            emit_movi32(p, t2, 0xffu);
            emit_and(p, t1, host_src, t2);
        }
        emit_mov(p, 12, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_store8);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        break;
    }

    case ACT_MOV_RM16: {
        int host_dst = X86REG_TO_LX7(a->dst);
        GUARD(160);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_load16);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_mov(p, t2, 10);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        emit_merge_u16(p, host_dst, t2, t1);
        break;
    }

    case ACT_MOV_MR16: {
        GUARD(160);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_movi32(p, t2, 0x0000ffffu);
        emit_and(p, t1, sr, t2);
        emit_mov(p, 12, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_store16);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        break;
    }

    case ACT_PUSH_IMM8:
        if (TINY386_JIT_ENABLE_STACK_FASTPATH) {
            GUARD(64);
            emit_addi(p, X86REG_TO_LX7(4), X86REG_TO_LX7(4), -4);
            emit_l32i(p, t2, cpu_reg, JIT_PHYS_MEM_OFF / 4);
            emit_add(p, t2, t2, X86REG_TO_LX7(4));
            emit_movi32(p, t1, (uint32_t)a->imm);
            emit_s32i(p, t1, t2, 0);
            break;
        }
        GUARD(112);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_movi32(p, 11, (uint32_t)a->imm);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_push_imm32);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        break;

    /* ---- MOV ------------------------------------------------ */
    case ACT_MOV_RR:
        /*
         * Failed as a runtime action in level2 MOV_RR-only testing. Keep the
         * stable 3-byte OR-based move for controlled experiments until the
         * state mismatch is found with translation/execution tracing.
         */
        GUARD(2); emit_mov(p, dr, sr); break;

    case ACT_MOV_RI:
        GUARD(16); emit_movi32(p, dr, (uint32_t)a->imm); break;

    /* ---- ALU ------------------------------------------------ */
    case ACT_ALU_RR: {
        bool fd = a->flags_dead;
        int cc_op = 0;
        int cc_mask = 0;
        switch (a->alu_op) {
        case ALU_ADD: cc_op = JIT_CC_ADD; cc_mask = JIT_CC_MASK_ARITH; break;
        case ALU_SUB: cc_op = JIT_CC_SUB; cc_mask = JIT_CC_MASK_ARITH; break;
        case ALU_AND: cc_op = JIT_CC_AND; cc_mask = JIT_CC_MASK_LOGIC; break;
        case ALU_OR:  cc_op = JIT_CC_OR;  cc_mask = JIT_CC_MASK_LOGIC; break;
        case ALU_XOR: cc_op = JIT_CC_XOR; cc_mask = JIT_CC_MASK_LOGIC; break;
        default: return false;
        }

        if (!fd) {
            GUARD(40);
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
            emit_s32i(p, sr, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
            switch (a->alu_op) {
            case ALU_ADD: emit_add(p, dr, dr, sr); break;
            case ALU_SUB: emit_sub(p, dr, dr, sr); break;
            case ALU_AND: emit_and(p, dr, dr, sr); break;
            case ALU_OR:  emit_or (p, dr, dr, sr); break;
            case ALU_XOR: emit_xor(p, dr, dr, sr); break;
            default: return false;
            }
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t0, cc_op);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t0, (uint32_t)cc_mask);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        } else {
            GUARD(3);
            switch (a->alu_op) {
            case ALU_ADD: emit_add(p, dr, dr, sr); break;
            case ALU_SUB: emit_sub(p, dr, dr, sr); break;
            case ALU_AND: emit_and(p, dr, dr, sr); break;
            case ALU_OR:  emit_or (p, dr, dr, sr); break;
            case ALU_XOR: emit_xor(p, dr, dr, sr); break;
            default: return false;
            }
        }
        break;
    }

    case ACT_ALU_RI: {
        int32_t imm = a->imm;
        bool fd = a->flags_dead;
        int cc_op = 0;
        int cc_mask = 0;
        switch (a->alu_op) {
        case ALU_ADD: cc_op = JIT_CC_ADD; cc_mask = JIT_CC_MASK_ARITH; break;
        case ALU_SUB: cc_op = JIT_CC_SUB; cc_mask = JIT_CC_MASK_ARITH; break;
        case ALU_AND: cc_op = JIT_CC_AND; cc_mask = JIT_CC_MASK_LOGIC; break;
        case ALU_OR:  cc_op = JIT_CC_OR;  cc_mask = JIT_CC_MASK_LOGIC; break;
        case ALU_XOR: cc_op = JIT_CC_XOR; cc_mask = JIT_CC_MASK_LOGIC; break;
        default: return false;
        }

        if (!fd) {
            GUARD(56);
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
            emit_movi32(p, t0, (uint32_t)imm);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
            switch (a->alu_op) {
            case ALU_ADD: emit_add(p, dr, dr, t0); break;
            case ALU_SUB: emit_sub(p, dr, dr, t0); break;
            case ALU_AND: emit_and(p, dr, dr, t0); break;
            case ALU_OR:  emit_or (p, dr, dr, t0); break;
            case ALU_XOR: emit_xor(p, dr, dr, t0); break;
            default: return false;
            }
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t0, cc_op);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t0, (uint32_t)cc_mask);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        } else {
            if (a->alu_op == ALU_ADD && imm >= -128 && imm <= 127) {
                GUARD(3); emit_addi(p, dr, dr, imm);
            } else if (a->alu_op == ALU_SUB && imm >= -127 && imm <= 128) {
                GUARD(3); emit_addi(p, dr, dr, -imm);
            } else {
                GUARD(16 + 3);
                emit_movi32(p, t0, (uint32_t)imm);
                switch (a->alu_op) {
                case ALU_ADD: emit_add(p, dr, dr, t0); break;
                case ALU_SUB: emit_sub(p, dr, dr, t0); break;
                case ALU_AND: emit_and(p, dr, dr, t0); break;
                case ALU_OR:  emit_or (p, dr, dr, t0); break;
                case ALU_XOR: emit_xor(p, dr, dr, t0); break;
                default: return false;
                }
            }
        }
        break;
    }

    /* ---- NOT / NEG / INC / DEC ----------------------------- */
    case ACT_NOT_R:
        /* NOT dr = XOR dr, dr, 0xFFFFFFFF */
        GUARD(16 + 3);
        emit_movi32(p, t0, 0xFFFFFFFF);
        emit_xor(p, dr, dr, t0);
        break;

    case ACT_NEG_R: {
        bool fd = a->flags_dead;
        if (!fd) {
            GUARD(40);
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
            emit_neg(p, dr, dr);
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t0, JIT_CC_NEG32);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t0, JIT_CC_MASK_ARITH);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        } else {
            GUARD(3); emit_neg(p, dr, dr);
        }
        break;
    }

    case ACT_INC_R:
    case ACT_DEC_R: {
        bool fd = a->flags_dead;
        bool inc = (a->type == ACT_INC_R);

        if (!fd) {
            GUARD(40);
            emit_addi(p, dr, dr, inc ? 1 : -1);
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t0, inc ? JIT_CC_INC32 : JIT_CC_DEC32);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t0, JIT_CC_MASK_ARITH_NO_CF);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        } else {
            GUARD(3);
            emit_addi(p, dr, dr, inc ? 1 : -1);
        }
        break;
    }

    /* ---- Shifts -------------------------------------------- */
    case ACT_SHx_RI: {
        int sa = a->src & 31;
        bool fd = a->flags_dead;
        int cc_op = 0;

        if (sa == 0)
            break;

        switch (a->sh_op) {
        case SH_SHL: cc_op = JIT_CC_SHL; break;
        case SH_SHR: cc_op = JIT_CC_SHR; break;
        case SH_SAR: cc_op = JIT_CC_SAR; break;
        default: return false;
        }

        if (!fd) {
            GUARD(64);
            if (a->sh_op == SH_SHR)
                emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);

            emit_mov(p, t0, dr);
            if (a->sh_op == SH_SHL)
                emit_srli_any(p, t0, 32 - sa);
            else
                emit_srli_any(p, t0, sa - 1);
            emit_movi(p, t1, 1);
            emit_and(p, t0, t0, t1);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST2_OFF / 4);
        } else {
            GUARD(16);
        }

        switch (a->sh_op) {
        case SH_SHL: emit_slli(p, dr, dr, sa);        break;
        case SH_SHR:
            if (sa <= 15) { emit_srli(p, dr, dr, sa); }
            else          { emit_ssri_wide(p, dr, sa); } /* helper below */
            break;
        case SH_SAR: emit_srai(p, dr, dr, sa);        break;
        }
        if (!fd) {
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t0, cc_op);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t0, JIT_CC_MASK_LOGIC);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        }
        break;
    }

    case ACT_SHx_CL: {
        /* CL is ECX = LX7 a(LX7_GPR_BASE+1) */
        int cl_reg = X86REG_TO_LX7(1 /* ECX */);
        GUARD(9);
        switch (a->sh_op) {
        case SH_SHL: emit_ssl(p, cl_reg); emit_sll(p, dr, dr); break;
        case SH_SHR: emit_ssr(p, cl_reg); emit_srl(p, dr, dr); break;
        case SH_SAR: emit_ssr(p, cl_reg); emit_sra(p, dr, dr); break;
        }
        break;
    }

    /* ---- CMP / TEST ---------------------------------------- */
    case ACT_CMP_RM32: {
        bool fd = a->flags_dead;
        if (TINY386_JIT_ENABLE_INLINE_MEM) {
            GUARD(112);
            emit_guest_ptr32(p, a, cpu_reg, t0, t1, t2);
            emit_l32i(p, t2, t2, 0);
            emit_sub(p, t0, dr, t2);
            cs->cmp_mode = 1;
            cs->cmp_lreg = dr;
            cs->cmp_rreg = t2;
            if (!fd) {
                emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
                emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
                emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
                emit_movi(p, t1, JIT_CC_SUB);
                emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
                emit_movi32(p, t1, JIT_CC_MASK_ARITH);
                emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
            }
            break;
        }
        GUARD(176);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_load32);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_mov(p, t2, 10);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        emit_sub(p, t0, dr, t2);
        cs->cmp_mode = 1;
        cs->cmp_lreg = dr;
        cs->cmp_rreg = t2;
        if (!fd) {
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
            emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t1, JIT_CC_SUB);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t1, JIT_CC_MASK_ARITH);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        }
        break;
    }

    case ACT_CMP_MR32: {
        bool fd = a->flags_dead;
        if (TINY386_JIT_ENABLE_INLINE_MEM) {
            GUARD(112);
            emit_guest_ptr32(p, a, cpu_reg, t0, t1, t2);
            emit_l32i(p, t2, t2, 0);
            emit_sub(p, t0, t2, sr);
            cs->cmp_mode = 1;
            cs->cmp_lreg = t2;
            cs->cmp_rreg = sr;
            if (!fd) {
                emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
                emit_s32i(p, sr, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
                emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
                emit_movi(p, t1, JIT_CC_SUB);
                emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
                emit_movi32(p, t1, JIT_CC_MASK_ARITH);
                emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
            }
            break;
        }
        GUARD(176);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_load32);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_mov(p, t2, 10);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        emit_sub(p, t0, t2, sr);
        cs->cmp_mode = 1;
        cs->cmp_lreg = t2;
        cs->cmp_rreg = sr;
        if (!fd) {
            emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
            emit_s32i(p, sr, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t1, JIT_CC_SUB);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t1, JIT_CC_MASK_ARITH);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        }
        break;
    }

    case ACT_TEST_MR32: {
        bool fd = a->flags_dead;
        if (TINY386_JIT_ENABLE_INLINE_MEM) {
            GUARD(96);
            emit_guest_ptr32(p, a, cpu_reg, t0, t1, t2);
            emit_l32i(p, t2, t2, 0);
            emit_and(p, t0, t2, sr);
            cs->cmp_mode = 3;
            cs->cmp_lreg = t0;
            if (!fd) {
                emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
                emit_movi(p, t1, JIT_CC_AND);
                emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
                emit_movi32(p, t1, JIT_CC_MASK_LOGIC);
                emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
            }
            break;
        }
        GUARD(160);
        emit_store_gprs(p, cpu_reg, store_mask);
        emit_mem_address(p, a, t0, t1);
        emit_movi32(p, t2, (uint32_t)(uintptr_t)jit_helper_load32);
        emit_mov(p, 10, cpu_reg);
        emit_callx8(p, t2);
        emit_mov(p, t2, 10);
        emit_load_gprs(p, cpu_reg, load_mask | store_mask);
        emit_and(p, t0, t2, sr);
        cs->cmp_mode = 3;
        cs->cmp_lreg = t0;
        if (!fd) {
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t1, JIT_CC_AND);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t1, JIT_CC_MASK_LOGIC);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        }
        break;
    }

    case ACT_CMP_RR: {
        cs->cmp_mode  = 1;
        cs->cmp_lreg  = dr;  /* note: dr = a->dst, the left operand */
        cs->cmp_rreg  = sr;

        bool fd = a->flags_dead;
        if (!fd) {
            GUARD(40);
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
            emit_s32i(p, sr, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
            emit_sub(p, t0, dr, sr);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t2, JIT_CC_SUB);
            emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t2, JIT_CC_MASK_ARITH);
            emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        }
        break;
    }

    case ACT_CMP_RI: {
        /* Compute (dr - imm) into TMP0 for the subsequent branch and cc.dst */
        bool fd = a->flags_dead;
        GUARD(16 + 3 + (fd ? 0 : 40));
        emit_movi32(p, t1, (uint32_t)a->imm);
        emit_sub(p, t0, dr, t1);
        cs->cmp_mode  = 2;
        cs->cmp_lreg  = dr;
        cs->cmp_rreg  = t1;  /* original imm in t1 */
        cs->cmp_tmp   = t0;  /* (dr - imm) in t0 */

        if (!fd) {
            emit_s32i(p, dr, LX7_CPU_REG, JIT_CC_SRC1_OFF / 4);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_SRC2_OFF / 4);
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t2, JIT_CC_SUB);
            emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t2, JIT_CC_MASK_ARITH);
            emit_s32i(p, t2, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        }
        break;
    }

    case ACT_TEST_RR: {
        /* Compute (dr & sr) into TMP0 for BEQZ/BNEZ */
        bool fd = a->flags_dead;
        GUARD(3 + (fd ? 0 : 32));
        emit_and(p, t0, dr, sr);
        cs->cmp_mode  = 3;
        cs->cmp_lreg  = t0;

        if (!fd) {
            emit_s32i(p, t0, LX7_CPU_REG, JIT_CC_DST_OFF / 4);
            emit_movi(p, t1, JIT_CC_AND);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_OP_OFF / 4);
            emit_movi32(p, t1, JIT_CC_MASK_LOGIC);
            emit_s32i(p, t1, LX7_CPU_REG, JIT_CC_MASK_OFF / 4);
        }
        break;
    }

    /* ---- Unconditional jump --------------------------------- */
    case ACT_JMP:
        /*
         * Failed in JMP-only firmware tests. The epilogue stores target_eip as
         * next_ip and returns to C; no host-side J is needed here. The suspect
         * is not the local epilogue structure alone, because rel target math
         * and Xtensa J encoding were checked separately. Before enabling this
         * again, trace the first translated JMPs and compare target_eip against
         * the interpreter path.
         */
        GUARD(16 * 4);
        emit_epilogue(p, cpu_reg, a->target_eip, store_mask);
        break;

    /* ---- Conditional jump (fused CMP/TEST + Jcc) ----------- */
    case ACT_JCC: {
        /*
         * We emit:
         *   B<cond>  taken_path
         *   <epilogue with fallthrough_eip>
         *   taken_path:
         *   <epilogue with target_eip>
         *
         * The branch offset must skip the fallthrough epilogue.
         * We don't know the size of emit_epilogue upfront, so we
         * use a fixed worst-case size (16*4 bytes) and insert a
         * placeholder, then back-patch.
         */
        int lx7l = cs->cmp_lreg;
        int lx7r = cs->cmp_rreg;
        int zreg = (cs->cmp_mode == 2) ? cs->cmp_tmp : lx7l;
        CondCode branch_cc = (CondCode)((unsigned)a->cc ^ 1u);
        bool supported = false;

        if (cs->cmp_mode == 0) {
            return false;
        }

        switch (a->cc) {
        case CC_Z:
        case CC_NZ:
            supported = true;
            break;
        case CC_L:
        case CC_NL:
        case CC_LE:
        case CC_NLE:
        case CC_B:
        case CC_NB:
        case CC_BE:
        case CC_NBE:
            supported = (cs->cmp_mode == 1 || cs->cmp_mode == 2);
            break;
        case CC_S:
        case CC_NS:
            supported = (cs->cmp_mode == 2 || cs->cmp_mode == 3);
            break;
        default:
            supported = false;
            break;
        }
        if (!supported)
            return false;

        GUARD(3 + 3 + 16 * 4 * 2 + 16);

        uint8_t *branch_site = *p;
        *p += 3; /* placeholder for inverted conditional skip */
        uint8_t *j_site = *p;
        *p += 3; /* placeholder for long jump to taken path */

        emit_epilogue(p, cpu_reg, fallback_eip, store_mask);
        uint8_t *taken_start = *p;
        emit_epilogue(p, cpu_reg, a->target_eip, store_mask);

        EmitPtr bp = branch_site;
        int32_t skip_j_off = (int32_t)((j_site + 3) - (branch_site + 4));

        switch (branch_cc) {
        /* Equal / not-equal: use two-register BEQ/BNE when we have a
         * register pair, otherwise fall back to BEQZ/BNEZ on tmp.    */
        case CC_Z:
            if (cs->cmp_mode == 1)      emit_beq (&bp, lx7l, lx7r, (int8_t)skip_j_off);
            else                        emit_beqz(&bp, zreg,         skip_j_off & 0xFFF);
            break;
        case CC_NZ:
            if (cs->cmp_mode == 1)      emit_bne (&bp, lx7l, lx7r, (int8_t)skip_j_off);
            else                        emit_bnez(&bp, zreg,         skip_j_off & 0xFFF);
            break;
        /* Signed comparisons */
        case CC_L:   emit_blt (&bp, lx7l, lx7r, (int8_t)skip_j_off); break;
        case CC_NL:  emit_bge (&bp, lx7l, lx7r, (int8_t)skip_j_off); break;
        case CC_LE:
            if (cs->cmp_mode == 1 || cs->cmp_mode == 2) emit_bge(&bp, lx7r, lx7l, (int8_t)skip_j_off);
            else                                        return false;
            break;
        case CC_NLE:
            if (cs->cmp_mode == 1 || cs->cmp_mode == 2) emit_blt(&bp, lx7r, lx7l, (int8_t)skip_j_off);
            else                                        return false;
            break;
        /* Unsigned comparisons */
        case CC_B:   emit_bltu(&bp, lx7l, lx7r, (int8_t)skip_j_off); break;
        case CC_NB:  emit_bgeu(&bp, lx7l, lx7r, (int8_t)skip_j_off); break;
        case CC_BE:
            if (cs->cmp_mode == 1 || cs->cmp_mode == 2) emit_bgeu(&bp, lx7r, lx7l, (int8_t)skip_j_off);
            else                                        return false;
            break;
        case CC_NBE:
            if (cs->cmp_mode == 1 || cs->cmp_mode == 2) emit_bltu(&bp, lx7r, lx7l, (int8_t)skip_j_off);
            else                                        return false;
            break;
        /* Sign-flag test (SF): result < 0 ↔ top bit set */
        case CC_S:
            if (cs->cmp_mode == 2 || cs->cmp_mode == 3) emit_bltz(&bp, zreg, skip_j_off & 0xFFF);
            else                                        return false;
            break;
        case CC_NS:
            if (cs->cmp_mode == 2 || cs->cmp_mode == 3) emit_bgez(&bp, zreg, skip_j_off & 0xFFF);
            else                                        return false;
            break;
        default: return false;
        }
        EmitPtr jp = j_site;
        emit_j_target(&jp, taken_start);
        cs->cmp_mode = 0;
        break;
    }

    default:
        return false;
    }
    /* Clear pending CMP state after any non-branch instruction */
    if (a->type != ACT_CMP_RR && a->type != ACT_CMP_RI &&
        a->type != ACT_TEST_RR && a->type != ACT_CMP_RM32 &&
        a->type != ACT_CMP_MR32 && a->type != ACT_TEST_MR32 &&
        a->type != ACT_JCC)
        cs->cmp_mode = 0;

    return true;
#undef GUARD
}

/* ================================================================== */
/*  Section 5 — JIT Cache & Public API                                */
/* ================================================================== */

static uint32_t s_jit_selftest_allow;

static inline uint32_t jit_hash(uint32_t paddr)
{
    /* Simple hash: Knuth multiplicative hash */
    return (paddr * 2654435761u) % JIT_CACHE_ENTRIES;
}

static inline uint32_t jit_nojit_hash(uint32_t paddr)
{
    return (paddr * 2654435761u) % JIT_NOJIT_ENTRIES;
}

static inline uint32_t jit_hot_hash(uint32_t paddr)
{
    return (paddr * 2654435761u) % JIT_HOT_ENTRIES;
}

static bool jit_nojit_lookup(const JITState *jit, uint32_t paddr,
                             JITBailReason *bail_out)
{
    const JITNojitEntry *entry = &jit->nojit_table[jit_nojit_hash(paddr)];

    if (!entry->valid || entry->guest_paddr != paddr)
        return false;
    if (bail_out)
        *bail_out = (JITBailReason)entry->bail;
    return true;
}

static void jit_nojit_mark(JITState *jit, uint32_t paddr, JITBailReason bail)
{
    JITNojitEntry *entry = &jit->nojit_table[jit_nojit_hash(paddr)];

    entry->guest_paddr = paddr;
    entry->valid = 1;
    entry->bail = (uint16_t)bail;
    jit->nojit_table_sets++;
}

static bool jit_hot_threshold_skip(JITState *jit, uint32_t paddr)
{
#if TINY386_JIT_HOT_THRESHOLD > 1
    JITHotEntry *entry;

    if (s_jit_selftest_allow != 0)
        return false;
#if TINY386_JIT_SELFTEST_ONLY || TINY386_JIT_SELFTEST_AT_BOOT
    return false;
#endif

    entry = &jit->hot_table[jit_hot_hash(paddr)];
    if (!entry->valid || entry->guest_paddr != paddr) {
        entry->guest_paddr = paddr;
        entry->valid = 1;
        entry->hits = 1;
        jit->hot_threshold_skips++;
        return true;
    }
    if ((uint8_t)(entry->hits + 1u) < (uint8_t)TINY386_JIT_HOT_THRESHOLD) {
        entry->hits++;
        jit->hot_threshold_skips++;
        return true;
    }
    entry->hits = (uint8_t)TINY386_JIT_HOT_THRESHOLD;
#else
    (void)jit;
    (void)paddr;
#endif
    return false;
}

static inline uint32_t jit_align4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

static void jit_mark_nojit(JITBlock *block, uint32_t paddr, uint32_t guest_end,
                           JITBailReason bail)
{
    block->guest_paddr = paddr;
    block->guest_end   = guest_end;
    block->link_x86_insns = 0;
    block->flags       = JIT_BLOCKF_STICKY_NOJIT;
    block->link_slot   = JIT_LINK_NONE;
    block->link_paddr  = 0;
    block->exit_kind   = JIT_EXIT_INTERPRETER;
#if TINY386_JIT_BAIL_REASONS
    block->bail        = bail;
#else
    (void)bail;
    block->bail        = JIT_BAIL_NONE;
#endif
    block->status      = JIT_NOJIT;
}

static const char *jit_bail_reason_name(JITBailReason reason)
{
    switch (reason) {
    case JIT_BAIL_NONE:               return "none";
    case JIT_BAIL_DISABLED:           return "disabled";
    case JIT_BAIL_CODE16:             return "code16";
    case JIT_BAIL_PAGING:             return "paging";
    case JIT_BAIL_OUT_OF_GUEST_MEM:   return "out_of_guest_mem";
    case JIT_BAIL_UNSUPPORTED_PREFIX: return "unsupported_prefix";
    case JIT_BAIL_UNSUPPORTED_OPCODE: return "unsupported_opcode";
    case JIT_BAIL_MEMORY_OPERAND:     return "memory_operand";
    case JIT_BAIL_FLAGS_UNSAFE:       return "flags_unsafe";
    case JIT_BAIL_HOST_BUFFER_FULL:   return "host_buffer_full";
    case JIT_BAIL_POOL_FULL:          return "pool_full";
    default:                          return "?";
    }
}

static void jit_count_bail(JITState *jit, JITBailReason reason)
{
    jit->bailed++;
    if ((unsigned)reason <= JIT_BAIL_POOL_FULL)
        jit->bail_counts[reason]++;
    if (reason == JIT_BAIL_HOST_BUFFER_FULL)
        jit->host_buffer_full++;
}

static uint16_t jit_unsupported_key(const uint8_t *x86, uint32_t left)
{
    if (left >= 2u && x86[0] == 0x0f)
        return (uint16_t)(0x100u | x86[1]);
    return x86[0];
}

static void jit_count_unsupported_opcode(JITState *jit, const uint8_t *x86,
                                         uint32_t left)
{
#if TINY386_JIT_UNSUPPORTED_HIST
    if (left == 0)
        return;
    uint16_t key = jit_unsupported_key(x86, left);
    jit->unsupported_opcode_counts[key]++;
    jit->unsupported_opcode_total++;
#else
    (void)jit;
    (void)x86;
    (void)left;
#endif
}

static void jit_dump_unsupported_opcodes(const JITState *jit)
{
#if TINY386_JIT_UNSUPPORTED_HIST
    bool used[JIT_UNSUPPORTED_HIST_SIZE] = {0};
    unsigned printed = 0;

    if (jit->unsupported_opcode_total == 0)
        return;

    esp_rom_printf("[jit_stats] unsupported_opcode_total %u\n",
                   (unsigned)jit->unsupported_opcode_total);

    while (printed < (unsigned)TINY386_JIT_UNSUPPORTED_TOP) {
        uint32_t best_count = 0;
        uint16_t best_key = 0;

        for (uint16_t key = 0; key < JIT_UNSUPPORTED_HIST_SIZE; key++) {
            uint32_t count = jit->unsupported_opcode_counts[key];
            if (!used[key] && count > best_count) {
                best_count = count;
                best_key = key;
            }
        }
        if (best_count == 0)
            break;

        used[best_key] = true;
        if (best_key >= 0x100u) {
            esp_rom_printf("[jit_stats] unsupported 0f %02x %u\n",
                           best_key & 0xffu, (unsigned)best_count);
        } else {
            esp_rom_printf("[jit_stats] unsupported %02x    %u\n",
                           best_key & 0xffu, (unsigned)best_count);
        }
        printed++;
    }
#else
    (void)jit;
#endif
}

static void jit_dump_stats(const JITState *jit)
{
    esp_rom_printf("[jit_stats] hits=%u misses=%u bailed=%u invalidations=%u "
                   "smc=%u pool=%u/%u epoch=%u\n",
                   (unsigned)jit->hits, (unsigned)jit->misses,
                   (unsigned)jit->bailed, (unsigned)jit->invalidations,
                   (unsigned)jit->smc_flushes, (unsigned)jit->pool_used,
                   (unsigned)JIT_POOL_SIZE, (unsigned)jit->pool_epoch);
    esp_rom_printf("[jit_stats] attempts=%u translated=%u cache_misses=%u "
                   "sticky_nojit=%u nojit_sets=%u hot_skips=%u "
                   "jit_guest_insns=%u x86_bytes=%u "
                   "host_bytes=%u linked_exits=%u helper_actions=%u "
                   "host_buffer_full=%u pool_flushes=%u\n",
                   (unsigned)jit->translate_attempts,
                   (unsigned)jit->translated,
                   (unsigned)jit->cache_misses,
                   (unsigned)jit->sticky_nojit_hits,
                   (unsigned)jit->nojit_table_sets,
                   (unsigned)jit->hot_threshold_skips,
                   (unsigned)jit->jit_guest_insns,
                   (unsigned)jit->emitted_x86_bytes,
                   (unsigned)jit->emitted_host_bytes,
                   (unsigned)jit->linked_exits,
                   (unsigned)jit->helper_call_actions,
                   (unsigned)jit->host_buffer_full,
                   (unsigned)jit->pool_flushes);
    esp_rom_printf("[jit_stats] miss_nojit_table=%u miss_sticky_block=%u "
                   "miss_hot_skip=%u miss_translate_bail=%u "
                   "cache_empty=%u cache_conflict=%u cache_nojit_slot=%u "
                   "cache_other_slot=%u\n",
                   (unsigned)jit->miss_nojit_table,
                   (unsigned)jit->miss_sticky_block,
                   (unsigned)jit->miss_hot_skip,
                   (unsigned)jit->miss_translate_bail,
                   (unsigned)jit->cache_empty_slot_misses,
                   (unsigned)jit->cache_conflict_misses,
                   (unsigned)jit->cache_nojit_slot_misses,
                   (unsigned)jit->cache_other_slot_misses);
    esp_rom_printf("[jit_stats] smc_bitmap_misses=%u smc_scans=%u "
                   "smc_false_positives=%u smc_overlap_invalidations=%u\n",
                   (unsigned)jit->smc_bitmap_misses,
                   (unsigned)jit->smc_scans,
                   (unsigned)jit->smc_false_positives,
                   (unsigned)jit->smc_overlap_invalidations);
    esp_rom_printf("[jit_stats] smc_valid_blocks_scanned=%u "
                   "smc_blocks_invalidated=%u cache_conflict_invalidations=%u "
                   "full_flushes=%u full_flush_invalidations=%u "
                   "pool_full_invalidations=%u\n",
                   (unsigned)jit->smc_valid_blocks_scanned,
                   (unsigned)jit->smc_blocks_invalidated,
                   (unsigned)jit->cache_conflict_invalidations,
                   (unsigned)jit->full_flushes,
                   (unsigned)jit->full_flush_invalidations,
                   (unsigned)jit->pool_full_invalidations);
    esp_rom_printf("[jit_stats] try_entries=%u block_entries=%u "
                   "block_exits=%u interp_exits=%u try_cycles=%llu "
                   "lookup_cycles=%llu translate_cycles=%llu "
                   "exec_cycles=%llu guest_ptr_cycles=%llu "
                   "guest_scan_cycles=%llu guest_scan_bytes=%u "
                   "prestep_cooldown=%u prestep_cooldown_skips=%u\n",
                   (unsigned)jit->try_entries,
                   (unsigned)jit->block_entries,
                   (unsigned)jit->block_exits,
                   (unsigned)jit->interp_exits,
                   (unsigned long long)jit->try_cycles,
                   (unsigned long long)jit->lookup_cycles,
                   (unsigned long long)jit->translate_cycles,
                   (unsigned long long)jit->exec_cycles,
                   (unsigned long long)jit->guest_ptr_cycles,
                   (unsigned long long)jit->guest_scan_cycles,
                   (unsigned)jit->guest_scan_bytes,
                   (unsigned)jit->prestep_cooldown,
                   (unsigned)jit->prestep_cooldown_skips);
    for (unsigned i = 1; i <= JIT_BAIL_POOL_FULL; i++) {
        if (jit->bail_counts[i] != 0) {
            esp_rom_printf("[jit_stats] bail %-20s %u\n",
                           jit_bail_reason_name((JITBailReason)i),
                           (unsigned)jit->bail_counts[i]);
        }
    }
    jit_dump_unsupported_opcodes(jit);
}

static void jit_maybe_dump_stats(JITState *jit)
{
#if TINY386_JIT_STATS_PERIOD > 0
    jit->stats_ticks++;
    if (jit->stats_ticks >= (uint32_t)TINY386_JIT_STATS_PERIOD) {
        jit->stats_ticks = 0;
        jit_dump_stats(jit);
    }
#else
    (void)jit;
#endif
}

void jit_dump_perf_snapshot(JITState *jit, const char *phase,
                            uint32_t ms, long ips, long cycles,
                            uint32_t pc_steps, uint32_t step_count,
                            uint32_t step_batch)
{
    uint32_t jit_guest_delta =
        jit->jit_guest_insns - jit->snapshot_last_jit_guest_insns;
    uint32_t jit_pct = (cycles > 0) ?
        (uint32_t)(((uint64_t)jit_guest_delta * 100u) / (uint32_t)cycles) : 0;
    jit->snapshot_last_jit_guest_insns = jit->jit_guest_insns;

    esp_rom_printf("[bench] phase=%s ms=%u ips=%ld cycles=%ld pc_steps=%u "
                   "step_count=%u step_batch=%u jit_hits=%u jit_misses=%u "
                   "cache_misses=%u translate_attempts=%u translated=%u "
                   "bailed=%u sticky_nojit=%u nojit_sets=%u hot_skips=%u "
                   "miss_nojit_table=%u miss_sticky_block=%u "
                   "miss_hot_skip=%u miss_translate_bail=%u "
                   "cache_empty=%u cache_conflict=%u cache_nojit_slot=%u "
                   "cache_other_slot=%u "
                   "jit_guest_insns=%u "
                   "jit_guest_delta=%u "
                   "jit_guest_pct=%u host_buffer_full=%u pool_epoch=%u "
                   "pool_flushes=%u smc_flushes=%u invalidations=%u "
                   "smc_bitmap_misses=%u smc_scans=%u "
                   "smc_false_positives=%u smc_overlap_invalidations=%u "
                   "smc_valid_blocks_scanned=%u smc_blocks_invalidated=%u "
                   "cache_conflict_invalidations=%u "
                   "full_flushes=%u full_flush_invalidations=%u "
                   "pool_full_invalidations=%u "
                   "linked_exits=%u helper_actions=%u emitted_x86_bytes=%u "
                   "emitted_host_bytes=%u unsupported_total=%u "
                   "try_entries=%u block_entries=%u block_exits=%u "
                   "interp_exits=%u try_cycles=%llu lookup_cycles=%llu "
                   "translate_cycles=%llu exec_cycles=%llu "
                   "guest_ptr_cycles=%llu guest_scan_cycles=%llu "
                   "guest_scan_bytes=%u prestep_cooldown=%u "
                   "prestep_cooldown_skips=%u\n",
                   phase ? phase : "boot",
                   (unsigned)ms, ips, cycles,
                   (unsigned)pc_steps, (unsigned)step_count,
                   (unsigned)step_batch,
                   (unsigned)jit->hits, (unsigned)jit->misses,
                   (unsigned)jit->cache_misses,
                   (unsigned)jit->translate_attempts,
                   (unsigned)jit->translated,
                   (unsigned)jit->bailed,
                   (unsigned)jit->sticky_nojit_hits,
                   (unsigned)jit->nojit_table_sets,
                   (unsigned)jit->hot_threshold_skips,
                   (unsigned)jit->miss_nojit_table,
                   (unsigned)jit->miss_sticky_block,
                   (unsigned)jit->miss_hot_skip,
                   (unsigned)jit->miss_translate_bail,
                   (unsigned)jit->cache_empty_slot_misses,
                   (unsigned)jit->cache_conflict_misses,
                   (unsigned)jit->cache_nojit_slot_misses,
                   (unsigned)jit->cache_other_slot_misses,
                   (unsigned)jit->jit_guest_insns,
                   (unsigned)jit_guest_delta,
                   (unsigned)jit_pct,
                   (unsigned)jit->host_buffer_full,
                   (unsigned)jit->pool_epoch,
                   (unsigned)jit->pool_flushes,
                   (unsigned)jit->smc_flushes,
                   (unsigned)jit->invalidations,
                   (unsigned)jit->smc_bitmap_misses,
                   (unsigned)jit->smc_scans,
                   (unsigned)jit->smc_false_positives,
                   (unsigned)jit->smc_overlap_invalidations,
                   (unsigned)jit->smc_valid_blocks_scanned,
                   (unsigned)jit->smc_blocks_invalidated,
                   (unsigned)jit->cache_conflict_invalidations,
                   (unsigned)jit->full_flushes,
                   (unsigned)jit->full_flush_invalidations,
                   (unsigned)jit->pool_full_invalidations,
                   (unsigned)jit->linked_exits,
                   (unsigned)jit->helper_call_actions,
                   (unsigned)jit->emitted_x86_bytes,
                   (unsigned)jit->emitted_host_bytes,
                   (unsigned)jit->unsupported_opcode_total,
                   (unsigned)jit->try_entries,
                   (unsigned)jit->block_entries,
                   (unsigned)jit->block_exits,
                   (unsigned)jit->interp_exits,
                   (unsigned long long)jit->try_cycles,
                   (unsigned long long)jit->lookup_cycles,
                   (unsigned long long)jit->translate_cycles,
                   (unsigned long long)jit->exec_cycles,
                   (unsigned long long)jit->guest_ptr_cycles,
                   (unsigned long long)jit->guest_scan_cycles,
                   (unsigned)jit->guest_scan_bytes,
                   (unsigned)jit->prestep_cooldown,
                   (unsigned)jit->prestep_cooldown_skips);
    jit_dump_unsupported_opcodes(jit);
}

static IRAM_ATTR bool jit_action_uses_helper(const X86Action *a)
{
    switch (a->type) {
    case ACT_MOV_RM32:
    case ACT_MOV_MR32:
    case ACT_CMP_RM32:
    case ACT_CMP_MR32:
    case ACT_TEST_MR32:
        return !TINY386_JIT_ENABLE_INLINE_MEM;
    case ACT_MOV_RM8:
    case ACT_MOV_MR8:
    case ACT_MOV_RM16:
    case ACT_MOV_MR16:
        return true;
    case ACT_PUSH_IMM8:
        return !TINY386_JIT_ENABLE_STACK_FASTPATH;
    default:
        return false;
    }
}

static IRAM_ATTR uint16_t jit_block_flags_for_actions(const X86Action *actions, int count)
{
    if (count <= 0)
        return JIT_BLOCKF_NONE;

    switch (actions[count - 1].type) {
    case ACT_JMP:
        return JIT_BLOCKF_ENDS_JMP;
    case ACT_JCC:
        return JIT_BLOCKF_ENDS_JCC;
    default:
        return JIT_BLOCKF_NONE;
    }
}

static IRAM_ATTR JITExitKind jit_exit_kind_for_actions(const X86Action *actions, int count)
{
    if (count <= 0)
        return JIT_EXIT_UNKNOWN;

    switch (actions[count - 1].type) {
    case ACT_JMP:
        return JIT_EXIT_DIRECT_JMP;
    case ACT_JCC:
        return JIT_EXIT_COND_TAKEN;
    default:
        return JIT_EXIT_FALLTHROUGH;
    }
}

static JITBlock *jit_find_link_target(JITState *jit, uint32_t paddr,
                                      uint16_t *slot_out)
{
    uint16_t slot = (uint16_t)jit_hash(paddr);
    JITBlock *target = &jit->blocks[slot];

    if (target->status != JIT_VALID || target->guest_paddr != paddr)
        return NULL;
    if ((target->flags & JIT_BLOCKF_LINKED_EXIT) != 0)
        return NULL;
    if (slot_out)
        *slot_out = slot;
    return target;
}

static IRAM_ATTR bool jit_action_ends_block(const X86Action *a)
{
    return a->type == ACT_JMP || a->type == ACT_JCC || a->type == ACT_BLOCK_END;
}

void jit_selftest_set_allowed_actions(uint32_t mask)
{
    s_jit_selftest_allow = mask;
}

void jit_selftest_clear_allowed_actions(void)
{
    s_jit_selftest_allow = 0;
}

static IRAM_ATTR bool jit_action_enabled(const X86Action *a, int block_insn_index)
{
    /*
     * JIT bring-up status on ESP32-S3 / COM19, 2026-06-28:
     *
     *   ACT_NOP:
     *     Passed a 30s serial smoke test. The guest reached hard-disk boot and
     *     VGA mode 1 without WDT. This proves the call path, entry/retw ABI,
     *     basic prologue/epilogue, code commit, and next_ip advance can work.
     *
     *   ACT_MOV_RI:
     *     Re-tested after the entry/retw, PSRAM pool, and emitter changes.
     *     Task 1.2 mixed handoff cases pass, so Task 1.3 admits it as a
     *     single-instruction block baseline.
     *
     *   ACT_MOV_RR:
     *     Failed with the old 3-byte OR alias path, which trapped on board as
     *     IllegalInstruction. Task 1.4 switched MOV_RR to density MOV.N and
     *     the board differential selftest now passes.
     *
     *   ACT_JMP:
     *     Failed in JMP-only tests, usually during "Relocating init ...".
     *     rel8/rel32 target arithmetic and Xtensa J offset semantics were
     *     checked, but the firmware still diverges. Needs first-N-JMP tracing.
     *
     *   ACT_INC_R / ACT_DEC_R:
     *     Failed when each was enabled alone. The current emitter only changes
     *     the GPR and does not update EFLAGS. Correct x86 INC/DEC must update
     *     OF/SF/ZF/AF/PF and preserve CF, or be emitted only when flags are
     *     provably dead.
     *
     * Keep this gate intentionally conservative. Decode/emitter support below
     * is retained for isolated experiments, but an action should only be
     * enabled here after a boot smoke test.
     */
    (void)block_insn_index;

    if (s_jit_selftest_allow != 0)
        return ((s_jit_selftest_allow >> (unsigned)a->type) & 1u) != 0;

    if (TINY386_JIT_LEVEL <= 0)
        return false;
    if (TINY386_JIT_LEVEL >= 1 && a->type == ACT_NOP)
        return true;
    if (TINY386_JIT_LEVEL >= 1 && TINY386_JIT_ENABLE_MOV_RI &&
        a->type == ACT_MOV_RI)
        return true;
    if (TINY386_JIT_LEVEL >= 2 && a->type == ACT_NOT_R)
        return true;
    if (TINY386_JIT_LEVEL >= 2 && a->type == ACT_CWDE)
        return true;
    if (TINY386_JIT_LEVEL >= 2 && a->type == ACT_CDQ)
        return true;
    if (TINY386_JIT_LEVEL >= 2 && a->type == ACT_BSWAP_R)
        return true;
    if (TINY386_JIT_LEVEL >= 1 && TINY386_JIT_ENABLE_MOV_RR &&
        a->type == ACT_MOV_RR)
        return true;
    if (TINY386_JIT_LEVEL >= 2 && TINY386_JIT_ENABLE_JMP &&
        a->type == ACT_JMP)
        return true;
    if (TINY386_JIT_LEVEL >= 2 &&
        (a->type == ACT_ALU_RR || a->type == ACT_ALU_RI) &&
        (a->alu_op == ALU_ADD || a->alu_op == ALU_SUB ||
         a->alu_op == ALU_AND || a->alu_op == ALU_OR ||
         a->alu_op == ALU_XOR))
        return true;
    if (TINY386_JIT_LEVEL >= 2 && a->type == ACT_NEG_R)
        return true;
    if (TINY386_JIT_LEVEL >= 2 &&
        (a->type == ACT_INC_R || a->type == ACT_DEC_R) &&
        a->flags_dead)
        return true;
    if (TINY386_JIT_LEVEL >= 2 && a->type == ACT_SHx_RI)
        return true;
    if (TINY386_JIT_ENABLE_CMPTEST_JCC && TINY386_JIT_LEVEL >= 3 &&
        (a->type == ACT_CMP_RR || a->type == ACT_CMP_RI ||
         a->type == ACT_TEST_RR || a->type == ACT_JCC))
        return true;
    if (TINY386_JIT_LEVEL >= 3 && a->type == ACT_XCHG_EAX_R)
        return true;
    if ((TINY386_JIT_ENABLE_MEM_HELPERS || TINY386_JIT_ENABLE_INLINE_MEM) &&
        TINY386_JIT_LEVEL >= 3 &&
        (a->type == ACT_MOV_RM32 || a->type == ACT_MOV_MR32))
        return true;
    if ((TINY386_JIT_ENABLE_MEM_HELPERS || TINY386_JIT_ENABLE_STACK_FASTPATH) &&
        TINY386_JIT_ENABLE_PUSH_IMM8 &&
        TINY386_JIT_LEVEL >= 3 && a->type == ACT_PUSH_IMM8)
        return true;
    if ((TINY386_JIT_ENABLE_MEM_HELPERS || TINY386_JIT_ENABLE_INLINE_MEM) &&
        TINY386_JIT_LEVEL >= 3 &&
        (a->type == ACT_CMP_RM32 || a->type == ACT_CMP_MR32 ||
         a->type == ACT_TEST_MR32))
        return true;
    if (TINY386_JIT_ENABLE_MEM_HELPERS && TINY386_JIT_LEVEL >= 3 &&
        (a->type == ACT_MOV_RM8 ||
         a->type == ACT_MOV_MR8 || a->type == ACT_MOV_RM16 ||
         a->type == ACT_MOV_MR16))
        return true;
    return false;
}

/*
 * Commit generated code into the JIT pool.
 * ESP32-S3 uses only the verified PSRAM dual mapping:
 *   write alias (DBUS/D-cache) -> exec alias (IBUS/I-cache).
 * The old IRAM/DIRAM write paths are intentionally abandoned because MEMPROT
 * makes IRAM effectively RX and repeated board tests hit CACHEERR.
 */
static void __attribute__((noinline))
jit_commit_code(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    if (!len)
        return;

    if (!s_jit_pool_psram || !s_jit_pool_write || !s_jit_pool_exec)
        return;

    memcpy(dst, src, len);
    /* Step 1: Writeback D-cache → PSRAM */
    esp_cache_msync(dst, len,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    /* Step 2: Invalidate I-cache for the exec alias so it re-fetches */
    uint8_t *exec_ptr = jit_pool_exec_for(dst);
    uint32_t line_mask = 31u; /* I-cache line = 32 bytes */
    uint8_t *inv_start = (uint8_t *)((uintptr_t)exec_ptr & ~(uintptr_t)line_mask);
    size_t   inv_len   = ((size_t)(exec_ptr + len - inv_start) + line_mask) & ~(size_t)line_mask;
    esp_cache_msync(inv_start, inv_len,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                    ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    __asm__ __volatile__("isync" ::: "memory");
}

void jit_init(JITState *jit, uint8_t *iram_pool)
{
    memset(jit, 0, sizeof(*jit));
    (void)iram_pool; /* ESP32-S3 JIT now only supports the PSRAM dual-map pool. */
    jit->pool = JIT_DEFAULT_POOL;
}

void jit_invalidate_all(JITState *jit)
{
    jit->full_flushes++;
    for (int i = 0; i < JIT_CACHE_ENTRIES; i++) {
        if (jit->blocks[i].status == JIT_VALID) {
            jit->invalidations++;
            jit->full_flush_invalidations++;
        }
        jit->blocks[i].status = JIT_EMPTY;
    }
    jit->pool_used = 0;
    jit->pool_epoch++;
    memset(jit->smc_page_bitmap, 0, sizeof(jit->smc_page_bitmap));
}

static uint32_t jit_invalidate_slot_and_links(JITState *jit, uint16_t slot)
{
    uint32_t invalidated = 0;

    if (slot >= JIT_CACHE_ENTRIES)
        return 0;

    JITBlock *victim = &jit->blocks[slot];
    if (victim->status == JIT_VALID) {
        victim->status = JIT_EMPTY;
        jit->invalidations++;
        invalidated++;
    }

    for (uint16_t i = 0; i < JIT_CACHE_ENTRIES; i++) {
        JITBlock *block = &jit->blocks[i];
        if (block->status != JIT_VALID)
            continue;
        if ((block->flags & JIT_BLOCKF_LINKED_EXIT) != 0 &&
            block->link_slot == slot) {
            block->status = JIT_EMPTY;
            jit->invalidations++;
            invalidated++;
        }
    }
    return invalidated;
}

static inline uint32_t jit_smc_page_bit(uint32_t paddr)
{
    return (paddr >> 12) & (JIT_SMC_PAGE_BITS - 1u);
}

static inline void jit_smc_mark_page(JITState *jit, uint32_t paddr)
{
    uint32_t bit = jit_smc_page_bit(paddr);
    jit->smc_page_bitmap[bit >> 5] |= 1u << (bit & 31u);
}

static bool jit_smc_range_maybe_has_code(JITState *jit, uint32_t paddr, uint32_t size)
{
#if TINY386_JIT_ENABLE_SMC_BITMAP
    uint32_t end = paddr + size - 1u;
    uint32_t page = paddr & JIT_GUEST_PAGE_MASK;
    uint32_t last_page = end & JIT_GUEST_PAGE_MASK;

    for (;;) {
        uint32_t bit = jit_smc_page_bit(page);
        if (jit->smc_page_bitmap[bit >> 5] & (1u << (bit & 31u)))
            return true;
        if (page == last_page)
            return false;
        page += JIT_GUEST_PAGE_SIZE;
    }
#else
    (void)jit;
    (void)paddr;
    (void)size;
    return true;
#endif
}

void jit_invalidate_range(JITState *jit, uint32_t paddr, uint32_t size)
{
    uint32_t range_end = paddr + size;
    bool overlapped = false;

    if (size == 0)
        return;
    if (range_end < paddr)
        range_end = UINT32_MAX;
    if (!jit_smc_range_maybe_has_code(jit, paddr, size)) {
        jit->smc_bitmap_misses++;
        return;
    }

    jit->smc_flushes++;
    jit->smc_scans++;

    for (int i = 0; i < JIT_CACHE_ENTRIES; i++) {
        JITBlock *block = &jit->blocks[i];
        if (block->status != JIT_VALID)
            continue;

        jit->smc_valid_blocks_scanned++;
        if (block->guest_paddr < range_end && block->guest_end > paddr) {
            overlapped = true;
            jit->smc_blocks_invalidated +=
                jit_invalidate_slot_and_links(jit, (uint16_t)i);
        }
    }
    if (overlapped)
        jit->smc_overlap_invalidations++;
    else
        jit->smc_false_positives++;
}

void jit_invalidate_page(JITState *jit, uint32_t paddr)
{
    uint32_t page_start = paddr & JIT_GUEST_PAGE_MASK;
    jit_invalidate_range(jit, page_start, JIT_GUEST_PAGE_SIZE);
}

IRAM_ATTR
JITBlock *jit_translate(JITState *jit, CPUI386 *cpu)
{
    if (!jit->pool)
        return NULL;
    jit->translate_attempts++;
    if (cpui386_is_code16(cpu)) {
        jit_count_bail(jit, JIT_BAIL_CODE16);
        return NULL;
    }

    /* Get physical address of current instruction */
    uint32_t cs_base  = cpui386_get_cs_base(cpu);
    uint32_t eip      = cpui386_get_next_ip(cpu);
    uint32_t laddr    = cs_base + eip;
    uint32_t cr0      = cpui386_get_cr0(cpu);
    uint32_t phys_mem_size = (uint32_t)cpui386_get_phys_mem_size(cpu);

    /* Check paging: if CR0.PG set, we need TLB; for now only handle
     * the common flat-mapping case where phys == linear. */
    if (cr0 & 0x80000000u) {
        /* Paging enabled: quick check — if laddr < phys_mem_size treat
         * as identity-mapped (common for Win9x < 4GB flat model).      */
        if (laddr >= phys_mem_size) {
            jit_count_bail(jit, JIT_BAIL_PAGING);
            return NULL;
        }
    }

    uint32_t paddr = laddr;
    JIT_TRACEF("[jit_trace] translate begin eip=0x%08x cs=0x%08x "
               "paddr=0x%08x cr0=0x%08x\n",
               (unsigned)eip, (unsigned)cs_base, (unsigned)paddr,
               (unsigned)cr0);

    /* Allocate or find cache slot */
    uint32_t  slot  = jit_hash(paddr);
    JITBlock *block = &jit->blocks[slot];
    JITBailReason nojit_bail = JIT_BAIL_NONE;

    if (jit_nojit_lookup(jit, paddr, &nojit_bail)) {
        JIT_TRACEF("[jit_trace] nojit-table translate-hit paddr=0x%08x bail=%s\n",
                   (unsigned)paddr, jit_bail_reason_name(nojit_bail));
        return NULL;
    }

    if (block->status == JIT_NOJIT && block->guest_paddr == paddr) {
        JIT_TRACEF("[jit_trace] sticky-nojit paddr=0x%08x bail=%s\n",
                   (unsigned)paddr, jit_bail_reason_name(block->bail));
        return NULL; /* previously deemed un-translatable */
    }

    if (paddr >= phys_mem_size) {
        jit_mark_nojit(block, paddr, paddr, JIT_BAIL_OUT_OF_GUEST_MEM);
        jit_nojit_mark(jit, paddr, JIT_BAIL_OUT_OF_GUEST_MEM);
        jit_count_bail(jit, JIT_BAIL_OUT_OF_GUEST_MEM);
        return NULL;
    }

    uint32_t guest_ptr_start = jit_ccount();
    const uint8_t *x86 = (const uint8_t *)cpui386_get_phys_mem(cpu) + paddr;
    uint32_t page_left = JIT_GUEST_PAGE_SIZE - (paddr & (JIT_GUEST_PAGE_SIZE - 1u));
    uint32_t mem_left  = phys_mem_size - paddr;
    uint32_t scan_left = (page_left < mem_left) ? page_left : mem_left;
    jit->guest_ptr_cycles += jit_cycles_since(guest_ptr_start);

    /* Evict the old entry if occupied by a different address */
    if (block->status == JIT_VALID && block->guest_paddr != paddr) {
        jit->cache_conflict_invalidations +=
            jit_invalidate_slot_and_links(jit, (uint16_t)slot);
    }

    /* Allocate space in the code pool */
    jit->pool_used = jit_align4(jit->pool_used);
    if (jit->pool_used + JIT_BLOCK_MAXBYTES > JIT_POOL_SIZE) {
        /* Pool full: flush everything and restart */
        uint32_t full_flush_invalidations_before = jit->full_flush_invalidations;
        jit->pool_flushes++;
        jit_invalidate_all(jit);
        jit->pool_full_invalidations +=
            jit->full_flush_invalidations - full_flush_invalidations_before;
    }

    uint8_t  *pool_raw = jit->pool + jit->pool_used;
    uint8_t   tmp_code[JIT_BLOCK_MAXBYTES] __attribute__((aligned(4)));
    /*
     * pool_write = writable PSRAM data vaddr.
     * pool_exec  = instruction-fetch PSRAM exec vaddr.
     */
    uint8_t  *pool_write = pool_raw;
    uint8_t  *pool_exec  = jit_pool_exec_for(pool_raw);
    if (!pool_exec) {
        block->status = JIT_EMPTY;
        return NULL;
    }
    if (jit->hits == 0 && jit->misses == 0) {
        esp_rom_printf("[jit] translate: raw=%p write=%p exec=%p tmp=%p psram=%d\n",
                       pool_raw, pool_write, pool_exec, tmp_code, (int)s_jit_pool_psram);
    }
    uint8_t  *code_start = tmp_code;
    uint8_t  *code_end   = tmp_code + JIT_BLOCK_MAXBYTES;
    EmitPtr   p          = tmp_code;

    /* Pass 1: Scan and decode all instructions in the block */
    X86Action *actions = s_jit_scan_actions;
    uint8_t   *action_bytes = s_jit_scan_action_bytes;
    int       insn_count = 0;
    int       x86_consumed = 0;
    uint32_t  cur_eip = eip;
    JITBailReason scan_bail = JIT_BAIL_UNSUPPORTED_OPCODE;
    uint32_t  scan_start = jit_ccount();

    for (int n = 0; n < JIT_SCAN_LIMIT; n++) {
        X86Action *a = &actions[n];
        if ((uint32_t)x86_consumed >= scan_left) {
            scan_bail = JIT_BAIL_OUT_OF_GUEST_MEM;
            break;
        }
        if (scan_left - (uint32_t)x86_consumed < 6u) {
            scan_bail = JIT_BAIL_OUT_OF_GUEST_MEM;
            break;
        }

        int bytes = decode_x86_insn(x86 + x86_consumed, cur_eip, a);
        if (bytes != 0 && (uint32_t)(x86_consumed + bytes) > scan_left) {
            scan_bail = JIT_BAIL_OUT_OF_GUEST_MEM;
            bytes = 0;
        }
        if (bytes != 0 && !jit_action_enabled(a, n)) {
            scan_bail = JIT_BAIL_DISABLED;
            bytes = 0;
        }
        if (bytes == 0) {
            break;
        }
        JIT_TRACEF("[jit_trace] scan #%d eip=0x%08x bytes=%d action=%s "
                   "dst=%d src=%d imm=0x%08x target=0x%08x\n",
                   n, (unsigned)cur_eip, bytes, jit_action_name(a->type),
                   a->dst, a->src, (unsigned)a->imm,
                   (unsigned)a->target_eip);
        action_bytes[n] = bytes;
        x86_consumed += bytes;
        cur_eip      += bytes;
        insn_count++;

        if (jit_action_ends_block(a))
            break;
#if TINY386_JIT_MOV_RR_SINGLE_BLOCK
        if (a->type == ACT_MOV_RR)
            break;
#endif
#if TINY386_JIT_JMP_SINGLE_BLOCK
        if (a->type == ACT_JMP)
            break;
#endif
    }
    jit->guest_scan_cycles += jit_cycles_since(scan_start);
    jit->guest_scan_bytes += (uint32_t)x86_consumed;

    if (insn_count == 0) {
        jit_mark_nojit(block, paddr, paddr, scan_bail);
        jit_nojit_mark(jit, paddr, scan_bail);
        jit_count_bail(jit, scan_bail);
        if (scan_bail == JIT_BAIL_UNSUPPORTED_OPCODE ||
            scan_bail == JIT_BAIL_UNSUPPORTED_PREFIX ||
            scan_bail == JIT_BAIL_DISABLED) {
            jit_count_unsupported_opcode(jit, x86 + x86_consumed,
                                         scan_left - (uint32_t)x86_consumed);
        }
        JIT_TRACEF("[jit_trace] translate bail paddr=0x%08x reason=%s\n",
                   (unsigned)paddr, jit_bail_reason_name(scan_bail));
        return NULL;
    }

    /* Backward pass: Dead flag elimination */
    bool flags_live = true;
    for (int i = insn_count - 1; i >= 0; i--) {
        X86Action *a = &actions[i];
        bool reads_flags = (a->type == ACT_JCC);
        bool writes_flags = false;

        if (a->type == ACT_ALU_RR || a->type == ACT_ALU_RI ||
            a->type == ACT_CMP_RR || a->type == ACT_CMP_RI ||
            a->type == ACT_TEST_RR || a->type == ACT_CMP_RM32 ||
            a->type == ACT_CMP_MR32 || a->type == ACT_TEST_MR32 ||
            a->type == ACT_NEG_R ||
            a->type == ACT_INC_R || a->type == ACT_DEC_R ||
            a->type == ACT_SHx_RI || a->type == ACT_SHx_CL) {
            writes_flags = true;
        }

        if (reads_flags) {
            flags_live = true;
        }

        if (writes_flags) {
            if (!flags_live) {
                a->flags_dead = true;
            } else {
                a->flags_dead = false;
                flags_live = false; // overwritten, so dead for prior instructions
            }
        }
    }

    /* Pass 2: Translate and emit Xtensa instructions */
    uint8_t  load_mask = 0;
    uint8_t  store_mask = 0;
    CmpState cs   = {0};
    int      emitted_bytes = 0;
    int      emitted_insns = 0;
    bool     ok   = true;
    uint16_t block_flags = JIT_BLOCKF_NONE;
    bool     ended_block = false;
    JITBlock *link_target = NULL;
    uint16_t  link_slot = JIT_LINK_NONE;
    cur_eip = eip;
    jit_actions_reg_masks(actions, insn_count, &load_mask, &store_mask);
    emit_prologue(&p, LX7_CPU_REG, load_mask);

    for (int n = 0; n < insn_count; n++) {
        X86Action *a = &actions[n];
        int bytes = action_bytes[n];

        ok = emit_action(&p, code_end, a, &cs, cur_eip + bytes, LX7_CPU_REG,
                         load_mask, store_mask);
        if (!ok) break;
        JIT_TRACEF("[jit_trace] emit #%d action=%s x86_eip=0x%08x "
                   "next=0x%08x host_used=%u\n",
                   n, jit_action_name(a->type), (unsigned)cur_eip,
                   (unsigned)(cur_eip + (uint32_t)bytes),
                   (unsigned)(p - code_start));

        emitted_bytes += bytes;
        emitted_insns++;
        cur_eip       += bytes;
        ended_block    = jit_action_ends_block(a);

#if TINY386_JIT_SINGLE_INSN_BLOCK
        emit_epilogue(&p, LX7_CPU_REG, cur_eip, store_mask);
        ended_block = true;
        break;
#endif
    }

    if (ok && !ended_block) {
#if TINY386_JIT_ENABLE_LINKING
        link_target = jit_find_link_target(jit, cs_base + cur_eip, &link_slot);
        if (link_target &&
            emit_linked_exit(&p, code_end, LX7_CPU_REG, link_target, store_mask)) {
            block_flags |= JIT_BLOCKF_LINKED_EXIT;
        } else
#endif
        if (p + 80 < code_end) {
            link_target = NULL;
            link_slot = JIT_LINK_NONE;
            emit_epilogue(&p, LX7_CPU_REG, cur_eip, store_mask);
        } else {
            ok = false;
        }
    }

    if (!ok || p >= code_end) {
        if (emitted_bytes == 0) {
            jit_mark_nojit(block, paddr, paddr, JIT_BAIL_HOST_BUFFER_FULL);
            jit_nojit_mark(jit, paddr, JIT_BAIL_HOST_BUFFER_FULL);
            jit_count_bail(jit, JIT_BAIL_HOST_BUFFER_FULL);
            return NULL;
        }
        /* Partial: emit epilogue */
        if (p + 80 < code_end) {
            emit_epilogue(&p, LX7_CPU_REG, cur_eip, store_mask);
            block_flags |= JIT_BLOCKF_PARTIAL;
        } else {
            jit_mark_nojit(block, paddr, paddr + (uint32_t)emitted_bytes,
                           JIT_BAIL_HOST_BUFFER_FULL);
            jit_nojit_mark(jit, paddr, JIT_BAIL_HOST_BUFFER_FULL);
            jit_count_bail(jit, JIT_BAIL_HOST_BUFFER_FULL);
            return NULL;
        }
    }

    /* Commit block */
    block->guest_paddr  = paddr;
    block->guest_end    = paddr + (uint32_t)emitted_bytes;
    block->guest_cs_base= cs_base;
    block->host_code    = pool_exec;
    block->host_len     = (uint16_t)(p - code_start);
    block->x86_len      = (uint16_t)emitted_bytes;
    block->x86_insns    = (uint16_t)emitted_insns;
    block->link_x86_insns = link_target ? link_target->x86_insns : 0;
    block->flags        = block_flags | jit_block_flags_for_actions(actions, emitted_insns);
    block->link_slot    = link_target ? link_slot : JIT_LINK_NONE;
    block->link_paddr   = link_target ? link_target->guest_paddr : 0;
    block->exit_kind    = jit_exit_kind_for_actions(actions, emitted_insns);
    block->bail         = JIT_BAIL_NONE;
    block->status       = JIT_VALID;
    jit->translated++;
    jit->emitted_x86_bytes += (uint32_t)block->x86_len;
    jit->emitted_host_bytes += (uint32_t)block->host_len;
    if ((block->flags & JIT_BLOCKF_LINKED_EXIT) != 0)
        jit->linked_exits++;
    for (int n = 0; n < emitted_insns; n++) {
        if (jit_action_uses_helper(&actions[n]))
            jit->helper_call_actions++;
    }
    jit_smc_mark_page(jit, block->guest_paddr);
    if ((block->guest_end - 1u) > (block->guest_paddr | (JIT_GUEST_PAGE_SIZE - 1u)))
        jit_smc_mark_page(jit, block->guest_end - 1u);

    jit_commit_code(pool_write, code_start, block->host_len);
    jit->pool_used += jit_align4(block->host_len);
    JIT_TRACEF("[jit_trace] translate ok paddr=0x%08x guest_end=0x%08x "
               "insns=%u x86_len=%u host=%p host_len=%u flags=0x%x exit=%u\n",
               (unsigned)block->guest_paddr, (unsigned)block->guest_end,
               (unsigned)block->x86_insns, (unsigned)block->x86_len,
               (void *)block->host_code, (unsigned)block->host_len,
               (unsigned)block->flags, (unsigned)block->exit_kind);

    return block;
}

int jit_try_execute(JITState *jit, CPUI386 *cpu)
{
    uint32_t try_start = jit_ccount();
    uint32_t lookup_start = try_start;
    jit->try_entries++;

    if (!jit->pool || cpui386_is_code16(cpu)) {
        jit->interp_exits++;
        jit->try_cycles += jit_cycles_since(try_start);
        return 0;
    }

#if TINY386_JIT_PRESTEP_COOLDOWN > 0
#define JIT_SET_PRESTEP_COOLDOWN() \
    do { jit->prestep_cooldown = (uint32_t)TINY386_JIT_PRESTEP_COOLDOWN; } while (0)
    if (jit->prestep_cooldown != 0) {
        jit->prestep_cooldown--;
        jit->prestep_cooldown_skips++;
        jit->interp_exits++;
        jit->try_cycles += jit_cycles_since(try_start);
        return 0;
    }
#else
#define JIT_SET_PRESTEP_COOLDOWN() do { } while (0)
#endif

    uint32_t cs_base = cpui386_get_cs_base(cpu);
    uint32_t eip     = cpui386_get_next_ip(cpu);
    uint32_t laddr   = cs_base + eip;
    uint32_t cr0 = cpui386_get_cr0(cpu);
    if (cr0 & 0x80000000u) {
        if (laddr >= (uint32_t)cpui386_get_phys_mem_size(cpu)) {
            jit->lookup_cycles += jit_cycles_since(lookup_start);
            jit->interp_exits++;
            jit->try_cycles += jit_cycles_since(try_start);
            return 0;
        }
    }
    uint32_t paddr = laddr;
    JIT_TRACEF("[jit_trace] try eip=0x%08x paddr=0x%08x cr0=0x%08x\n",
               (unsigned)eip, (unsigned)paddr, (unsigned)cr0);

    uint32_t  slot  = jit_hash(paddr);
    JITBlock *block = &jit->blocks[slot];
    JITBailReason nojit_bail = JIT_BAIL_NONE;

    if (jit_nojit_lookup(jit, paddr, &nojit_bail)) {
        jit->misses++;
        jit->sticky_nojit_hits++;
        jit->miss_nojit_table++;
        JIT_TRACEF("[jit_trace] nojit-table-hit paddr=0x%08x bail=%s\n",
                   (unsigned)paddr, jit_bail_reason_name(nojit_bail));
        jit_maybe_dump_stats(jit);
        jit->lookup_cycles += jit_cycles_since(lookup_start);
        jit->interp_exits++;
        JIT_SET_PRESTEP_COOLDOWN();
        jit->try_cycles += jit_cycles_since(try_start);
        return 0;
    }

    if (block->status == JIT_NOJIT && block->guest_paddr == paddr) {
        jit->misses++;
        jit->sticky_nojit_hits++;
        jit->miss_sticky_block++;
        JIT_TRACEF("[jit_trace] nojit-hit paddr=0x%08x bail=%s\n",
                   (unsigned)paddr, jit_bail_reason_name(block->bail));
        jit_maybe_dump_stats(jit);
        jit->lookup_cycles += jit_cycles_since(lookup_start);
        jit->interp_exits++;
        JIT_SET_PRESTEP_COOLDOWN();
        jit->try_cycles += jit_cycles_since(try_start);
        return 0;
    }

    if (block->status != JIT_VALID || block->guest_paddr != paddr) {
        /* Cache miss: translate */
        jit->cache_misses++;
        if (block->status == JIT_EMPTY) {
            jit->cache_empty_slot_misses++;
        } else if (block->status == JIT_VALID) {
            jit->cache_conflict_misses++;
        } else if (block->status == JIT_NOJIT) {
            jit->cache_nojit_slot_misses++;
        } else {
            jit->cache_other_slot_misses++;
        }
        if (jit_hot_threshold_skip(jit, paddr)) {
            jit->misses++;
            jit->miss_hot_skip++;
            jit_maybe_dump_stats(jit);
            jit->lookup_cycles += jit_cycles_since(lookup_start);
            jit->interp_exits++;
#if TINY386_JIT_PRESTEP_COOLDOWN_HOTSKIP
            JIT_SET_PRESTEP_COOLDOWN();
#endif
            jit->try_cycles += jit_cycles_since(try_start);
            return 0;
        }
        JIT_TRACEF("[jit_trace] miss slot=%u status=%u old_paddr=0x%08x\n",
                   (unsigned)slot, (unsigned)block->status,
                   (unsigned)block->guest_paddr);
        jit->lookup_cycles += jit_cycles_since(lookup_start);
        uint32_t translate_start = jit_ccount();
        block = jit_translate(jit, cpu);
        jit->translate_cycles += jit_cycles_since(translate_start);
        if (!block) {
            jit->misses++;
            jit->miss_translate_bail++;
            JIT_TRACEF("[jit_trace] miss-bail eip=0x%08x paddr=0x%08x\n",
                       (unsigned)eip, (unsigned)paddr);
            jit_maybe_dump_stats(jit);
            jit->interp_exits++;
            JIT_SET_PRESTEP_COOLDOWN();
            jit->try_cycles += jit_cycles_since(try_start);
            return 0;
        }
    } else {
        jit->lookup_cycles += jit_cycles_since(lookup_start);
    }

    jit->hits++;
    jit->prestep_cooldown = 0;
    jit->block_entries++;
    jit->jit_guest_insns += (uint32_t)block->x86_insns +
                            (uint32_t)block->link_x86_insns;

    typedef void (*JITFunc)(CPUI386 *);
    JITFunc fn = (JITFunc)(void *)block->host_code;
    if (jit->hits == 1) {
        esp_rom_printf("[jit] exec @%p cpu=%p\n",
                       (void *)fn, (void *)cpu);
    }
    JIT_TRACEF("[jit_trace] exec host=%p eip=0x%08x insns=%u x86_len=%u\n",
               (void *)fn, (unsigned)eip, (unsigned)block->x86_insns,
               (unsigned)block->x86_len);
    uint32_t exec_start = jit_ccount();
    fn(cpu);
    jit->exec_cycles += jit_cycles_since(exec_start);
    jit->block_exits++;
    JIT_TRACEF("[jit_trace] done host=%p next_ip=0x%08x\n",
               (void *)fn, (unsigned)cpui386_get_next_ip(cpu));
    jit_maybe_dump_stats(jit);
    jit->try_cycles += jit_cycles_since(try_start);
#undef JIT_SET_PRESTEP_COOLDOWN
    return (int)block->x86_insns + (int)block->link_x86_insns;
}
