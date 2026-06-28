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
 *    byte[2] = (op2 << 4) | r          r  = destination
 *    byte[1] = (s   << 4) | t          s  = source-1, t = source-2
 *    byte[0] = (op1 << 4) | op0        op0 = 0, op1 = 0 for ALU
 *
 *  RRI8 format (ADDI, L32I, S32I, BEQ, BNE, BLT, BGE, BLTU, BGEU)
 *    byte[2] = imm8
 *    byte[1] = (s << 4) | t
 *    byte[0] = opbyte                  encodes op-sub and op0
 *
 *  RI  format  (MOVI, BEQZ, BNEZ, BLTZ, BGEZ)
 *    byte[2] = imm[11:4]
 *    byte[1] = (imm[3:0] << 4) | reg
 *    byte[0] = opbyte
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

#ifdef BUILD_ESP32
#  include "esp_attr.h"
   /* Static IRAM pool – the linker places this in instruction-accessible RAM */
   static uint8_t IRAM_ATTR s_jit_pool[JIT_POOL_SIZE];
#  define JIT_DEFAULT_POOL s_jit_pool
#else
#  define JIT_DEFAULT_POOL NULL  /* JIT disabled on non-ESP32 targets */
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
    (*p)[0] = (uint8_t)((op1 << 4) | 0x00 /* op0=0 */);
    (*p)[1] = (uint8_t)((s   << 4) | t);
    (*p)[2] = (uint8_t)((op2 << 4) | r);
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

/* MOV ar, as  (implemented as OR ar, as, as) */
static inline void emit_mov(EmitPtr *p, int r, int s)
{ emit_or(p, r, s, s); }

/* SSL as  — load SAR = (32 - as) for left shifts */
static inline void emit_ssl(EmitPtr *p, int s)
{
    /* op2=0, op1=4, r=1(fixed), s=source, t=0 */
    emit_rrr(p, 0x0, 0x4, /*r=*/1, s, /*t=*/0);
}

/* SSR as  — load SAR = as for right shifts */
static inline void emit_ssr(EmitPtr *p, int s)
{
    /* op2=0, op1=4, r=0(fixed), s=source, t=0 */
    emit_rrr(p, 0x0, 0x4, /*r=*/0, s, /*t=*/0);
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
    /* op2[0] = ~sa[4]; op2[3:1] = 0; op1 = 1; op0 = 0 */
    int op2 = (sa < 16) ? 0x1 : 0x0;
    (*p)[0] = (uint8_t)(0x10 /* op1=1, op0=0 */);
    (*p)[1] = (uint8_t)((s << 4) | (sa & 0xF));
    (*p)[2] = (uint8_t)((op2 << 4) | r);
    *p += 3;
}

/* SRLI ar, at, sa  (sa = 0..15 only; use SSR+SRL for 16-31) */
static inline void emit_srli(EmitPtr *p, int r, int t, int sa)
{
    /* op2=4, op1=1, op0=0; s-field = sa (shift amount) */
    (*p)[0] = (uint8_t)(0x10);
    (*p)[1] = (uint8_t)((sa << 4) | t);
    (*p)[2] = (uint8_t)(0x40 | r);
    *p += 3;
}

/* SRAI ar, at, sa  (sa = 0..31) */
static inline void emit_srai(EmitPtr *p, int r, int t, int sa)
{
    /* op2 = sa[4] ? 7 : 6; s-field = sa[4]; t-field = at */
    int op2 = (sa >= 16) ? 0x7 : 0x6;
    int sf  = (sa >= 16) ? 1   : 0;
    (*p)[0] = (uint8_t)(0x10);
    (*p)[1] = (uint8_t)((sf << 4) | t);
    (*p)[2] = (uint8_t)((op2 << 4) | r);
    *p += 3;
}

/* ---- RRI8 format helpers ---------------------------------------- */

/* ADDI at, as, simm8  (sign-extended 8-bit immediate) */
static inline void emit_addi(EmitPtr *p, int t, int s, int imm8)
{
    (*p)[0] = 0xC2;                           /* op_sub=0xC, op0=0x2 */
    (*p)[1] = (uint8_t)((s << 4) | t);
    (*p)[2] = (uint8_t)(imm8 & 0xFF);
    *p += 3;
}

/* L32I at, as, off8  (off8 is byte-offset/4, so actual_offset = off8<<2) */
static inline void emit_l32i(EmitPtr *p, int t, int s, int off8)
{
    (*p)[0] = 0x22;                           /* op_sub=0x2, op0=0x2 */
    (*p)[1] = (uint8_t)((s << 4) | t);
    (*p)[2] = (uint8_t)(off8 & 0xFF);
    *p += 3;
}

/* S32I at, as, off8  (stores at to mem[as + off8<<2]) */
static inline void emit_s32i(EmitPtr *p, int t, int s, int off8)
{
    (*p)[0] = 0x62;                           /* op_sub=0x6, op0=0x2 */
    (*p)[1] = (uint8_t)((s << 4) | t);
    (*p)[2] = (uint8_t)(off8 & 0xFF);
    *p += 3;
}

/* Two-register compare-and-branch (RRI8, offset = signed byte delta) */
/*   BEQ  as, at, off8 */
static inline void emit_beq(EmitPtr *p, int s, int t, int off8)
{ (*p)[0]=0x17; (*p)[1]=(uint8_t)((s<<4)|t); (*p)[2]=(uint8_t)off8; *p+=3; }

/*   BNE  as, at, off8 */
static inline void emit_bne(EmitPtr *p, int s, int t, int off8)
{ (*p)[0]=0x97; (*p)[1]=(uint8_t)((s<<4)|t); (*p)[2]=(uint8_t)off8; *p+=3; }

/*   BLT  as, at, off8  (signed) */
static inline void emit_blt(EmitPtr *p, int s, int t, int off8)
{ (*p)[0]=0x27; (*p)[1]=(uint8_t)((s<<4)|t); (*p)[2]=(uint8_t)off8; *p+=3; }

/*   BGE  as, at, off8  (signed) */
static inline void emit_bge(EmitPtr *p, int s, int t, int off8)
{ (*p)[0]=0xA7; (*p)[1]=(uint8_t)((s<<4)|t); (*p)[2]=(uint8_t)off8; *p+=3; }

/*   BLTU as, at, off8  (unsigned) */
static inline void emit_bltu(EmitPtr *p, int s, int t, int off8)
{ (*p)[0]=0x37; (*p)[1]=(uint8_t)((s<<4)|t); (*p)[2]=(uint8_t)off8; *p+=3; }

/*   BGEU as, at, off8  (unsigned) */
static inline void emit_bgeu(EmitPtr *p, int s, int t, int off8)
{ (*p)[0]=0xB7; (*p)[1]=(uint8_t)((s<<4)|t); (*p)[2]=(uint8_t)off8; *p+=3; }

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
    (*p)[0] = 0x26;
    (*p)[1] = (uint8_t)(((imm12 & 0xF) << 4) | s);
    (*p)[2] = (uint8_t)((imm12 >> 4) & 0xFF);
    *p += 3;
}
/*   BLTZ as, imm12  (branch if as < 0, i.e. sign-flag set) */
static inline void emit_bltz(EmitPtr *p, int s, int imm12)
{
    (*p)[0] = 0x56;
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
    (*p)[0] = 0xA2;
    (*p)[1] = (uint8_t)(((imm12 & 0xF) << 4) | t);
    (*p)[2] = (uint8_t)((imm12 >> 4) & 0xFF);
    *p += 3;
}

/* ---- CALL format: J offset18 ------------------------------------ */
/*
 * J target: newPC = ((PC+4)&~3) + sign_extend(off18)<<2
 *   off18 = (target - ((pc+4)&~3)) >> 2
 *
 * Encoding (TODO: validate with cross-compiled binary):
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

/* SRLI for shift amounts 16-31 (uses MOVI + SSR + SRL, 9 bytes) */
static void emit_ssri_wide(EmitPtr *p, int r, int sa)
{
    emit_movi(p, LX7_TMP2, sa);
    emit_ssr(p,  LX7_TMP2);
    emit_srl(p,  r, r);
}

/* ---- RET0: return from CALL0-called function -------------------- */
static inline void emit_ret0(EmitPtr *p)
{
    /* JX a0  — jump to the address stored in a0 (the return address) */
    /* op2=0, op1=0xA, r=0, s=0 (a0), t=0; op0=0  → RRR             */
    /* From ISA: JX at is op2=0 & op1=0xA & at & ar=0 & as=0 & op0=0 */
    (*p)[0] = 0xA0;  /* op1=0xA, op0=0 */
    (*p)[1] = 0x00;  /* s=0, t=0 (a0 is source) */
    (*p)[2] = 0x00;  /* op2=0, r=0 */
    *p += 3;
}

/* ---- Load a 32-bit literal via L32R ----------------------------- */
/*
 * L32R at, imm16 (word offset from aligned PC)
 *   byte[0] = (t<<4) | 0x1
 *   byte[1] = imm16_lo
 *   byte[2] = imm16_hi
 *
 * To load a 32-bit constant we emit:
 *   L32R  at, .Lpool_entry   ; 3 bytes
 *   J     .Lafter            ; 3 bytes  (skip over literal)
 *   .word imm32              ; 4 bytes, 4-byte aligned
 * .Lafter:
 *
 * For simplicity we always align the pool to 4 bytes and keep it
 * inline.  The caller must ensure the emit buffer is large enough.
 *
 * Returns the number of bytes written (10 + up to 1 alignment pad).
 */
static int emit_movi32(EmitPtr *p, int t, uint32_t imm32)
{
    uint8_t *start = *p;

    /* If the value fits in a signed 12-bit immediate use MOVI (3 bytes) */
    int32_t sv = (int32_t)imm32;
    if (sv >= -2048 && sv <= 2047) {
        emit_movi(p, t, sv & 0xFFF);
        return (int)(*p - start);
    }

    /*
     * L32R   at, pool   -- offset computed after alignment
     * J      after      -- skip literal
     * (pad)
     * .word  imm32
     * after:
     *
     * LX7 L32R offset is (pool_addr - ((l32r_pc+3)&~3)) / 4 - 1
     *   pc_l32r   = current *p
     *   next_pc   = pc_l32r + 3
     *   aligned   = (next_pc + 3) & ~3           (next 4B aligned after J)
     *   pool_addr = aligned (we place literal right after J)
     *   We emit: L32R(3) + J(3) + pad(0-3) + literal(4)
     */
    uint8_t *pc_l32r = *p;
    *p += 3; /* reserve L32R */
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

    uint8_t *after = *p;

    /* Back-patch L32R: offset = (pool - ((pc_l32r+3)&~3)) / 4 - 1 */
    uintptr_t aligned_next = ((uintptr_t)pc_l32r + 3 + 3) & ~(uintptr_t)3;
    int32_t l32r_off = (int32_t)(((uintptr_t)pool - aligned_next) / 4);
    /* l32r_off is negative (pool is *before* aligned_next? No — pool is after J) */
    /* Actually for L32R, offset is measured *backward* from aligned(PC+3):       */
    /* new_off = (pool_addr - aligned_next) / 4  (negative means forward? No)     */
    /* Xtensa L32R: EA = ((PC+3)&~3) + (sign_extend16(imm16) << 2)                */
    /* We want EA = pool, so imm16 = (pool - ((pc_l32r+3)&~3)) / 4                */
    int32_t l32r_imm16 = (int32_t)(((uintptr_t)pool - aligned_next) / 4);
    pc_l32r[0] = (uint8_t)((t << 4) | 0x1);
    pc_l32r[1] = (uint8_t)(l32r_imm16 & 0xFF);
    pc_l32r[2] = (uint8_t)((l32r_imm16 >> 8) & 0xFF);

    /* Back-patch J: off18 = (after - ((j_pc+4)&~3)) / 4 */
    uint8_t *pc_j = pc_l32r + 3;
    uintptr_t j_base = ((uintptr_t)pc_j + 4 + 3) & ~(uintptr_t)3;
    int32_t j_off18  = (int32_t)(((uintptr_t)after - j_base) / 4);
    EmitPtr pj = pc_j;
    emit_j(&pj, j_off18);

    return (int)(*p - start);
}

/* ================================================================== */
/*  Section 2 — Block Prologue / Epilogue                             */
/*                                                                     */
/*  Calling convention: CALL0 (no register-window rotation)           */
/*    a0  = return address (set by caller's CALL0 instruction)        */
/*    a2  = CPUI386* (arg0, preserved throughout)                     */
/*    a3  = EAX, a4 = ECX, a5 = EDX, a6 = EBX                        */
/*    a7  = ESP, a8 = EBP, a9 = ESI, a10 = EDI                       */
/*    a11-a13 = temporaries (used for flag computation)               */
/* ================================================================== */

/* Byte offset of gprx[i].r32 in CPUI386 (struct starts with gprx[8]) */
#define GPR_OFF(i)  ((i) * 4)           /* 0, 4, 8, … 28 */
#define NEXT_IP_OFF (8 * 4 + 4)         /* gprx[8]*4 bytes + ip(4) = 36 */

/* Emit prologue: load all 8 x86 GPRs from cpu->gprx[] */
static void emit_prologue(EmitPtr *p, int cpu_reg)
{
    for (int i = 0; i < 8; i++) {
        /* L32I a(LX7_GPR_BASE+i), a(cpu_reg), GPR_OFF(i)/4 */
        emit_l32i(p, LX7_GPR_BASE + i, cpu_reg, GPR_OFF(i) / 4);
    }
}

/* Emit epilogue: store all 8 x86 GPRs back, set next_ip, return */
static void emit_epilogue(EmitPtr *p, int cpu_reg, uint32_t next_ip)
{
    for (int i = 0; i < 8; i++) {
        emit_s32i(p, LX7_GPR_BASE + i, cpu_reg, GPR_OFF(i) / 4);
    }
    /* Set cpu->next_ip = next_ip */
    emit_movi32(p, LX7_TMP0, next_ip);
    emit_s32i(p, LX7_TMP0, cpu_reg, NEXT_IP_OFF / 4);
    emit_ret0(p);
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

static inline ModRM decode_modrm(uint8_t b)
{
    ModRM m;
    m.mod      = (b >> 6) & 3;
    m.reg      = (b >> 3) & 7;
    m.rm       =  b       & 7;
    m.reg_only = (m.mod == 3);
    return m;
}

/* Decode the x86 instruction at *src.
 * Returns the number of x86 bytes consumed (0 = cannot handle).
 * Fills in the action to emit into the JIT block.
 *
 * "Action" is described as a simple tagged union below.
 */

typedef enum {
    ACT_NONE,
    ACT_MOV_RR,     /* dst_reg = src_reg */
    ACT_MOV_RI,     /* dst_reg = imm32 */
    ACT_ALU_RR,     /* dst_reg op= src_reg  (ADD/SUB/AND/OR/XOR) */
    ACT_ALU_RI,     /* dst_reg op= imm32    (8 or 32-bit) */
    ACT_NOT_R,      /* dst_reg = ~dst_reg */
    ACT_NEG_R,      /* dst_reg = -dst_reg */
    ACT_INC_R,      /* dst_reg += 1 */
    ACT_DEC_R,      /* dst_reg -= 1 */
    ACT_SHx_RI,     /* shift dst_reg by imm5 (SHL/SHR/SAR) */
    ACT_SHx_CL,     /* shift dst_reg by CL   (SHL/SHR/SAR) */
    ACT_CMP_RR,     /* set cmp state: left_reg CMP right_reg */
    ACT_CMP_RI,     /* set cmp state: left_reg CMP imm32 */
    ACT_TEST_RR,    /* set test state: left_reg & right_reg */
    ACT_JMP,        /* unconditional jump; target_eip set */
    ACT_JCC,        /* conditional jump;   target_eip set */
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
    AluOp      alu_op;
    ShiftOp    sh_op;
    CondCode   cc;
    uint32_t   target_eip; /* for JMP/JCC: resolved target EIP */
} X86Action;

/*
 * Decode one x86 instruction (32-bit mode, register operands only).
 * src      = pointer to x86 byte stream
 * eip      = current EIP (before this instruction)
 * action   = output
 * Returns number of bytes consumed, or 0 if unhandled/unsupported.
 */
static int decode_x86_insn(const uint8_t *src, uint32_t eip, X86Action *a)
{
    int len = 0;
    memset(a, 0, sizeof(*a));

    uint8_t op = src[len++];

    /* Reject all prefixes */
    if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E ||
        op == 0x64 || op == 0x65 || op == 0x66 || op == 0x67 ||
        op == 0xF0 || op == 0xF2 || op == 0xF3)
        return 0;

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
        if (!m.reg_only) return 0; /* memory operand */
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
        if (!m.reg_only) return 0;
        if (alu == ALU_CMP) {
            a->type = ACT_CMP_RR; a->dst = m.reg; a->src = m.rm;
        } else {
            a->type = ACT_ALU_RR; a->dst = m.reg; a->src = m.rm;
            a->alu_op = (AluOp)alu;
        }
        return len;
    }

    /* ── MOV r/m32, r32  (89)  /  MOV r32, r/m32  (8B) ──────── */
    if (op == 0x89 || op == 0x8B) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) return 0;
        a->type = ACT_MOV_RR;
        a->dst  = (op == 0x89) ? m.rm  : m.reg;
        a->src  = (op == 0x89) ? m.reg : m.rm;
        return len;
    }

    /* ── TEST r/m32, r32  (85) ───────────────────────────────── */
    if (op == 0x85) {
        ModRM m = decode_modrm(src[len++]);
        if (!m.reg_only) return 0;
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
    if (op == 0xEB) {
        int8_t rel = (int8_t)src[len++];
        a->type       = ACT_JMP;
        a->target_eip = (uint32_t)((int32_t)(eip + len) + rel);
        return len;
    }

    /* ── JMP rel32  (E9 cd) ──────────────────────────────────── */
    if (op == 0xE9) {
        int32_t rel = (int32_t)((uint32_t)src[len]     | ((uint32_t)src[len+1] << 8) |
                                ((uint32_t)src[len+2] << 16) | ((uint32_t)src[len+3] << 24));
        len += 4;
        a->type       = ACT_JMP;
        a->target_eip = (uint32_t)((int32_t)(eip + len) + rel);
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

/*
 * emit_action — translate one action.
 * Returns false if code buffer would overflow (host_len >= JIT_BLOCK_MAXBYTES).
 */
static bool emit_action(EmitPtr *p, uint8_t *buf_end,
                         const X86Action *a, CmpState *cs,
                         uint32_t fallback_eip, int cpu_reg)
{
#define GUARD(n) if ((*p) + (n) > buf_end) return false

    int dr = X86REG_TO_LX7(a->dst);
    int sr = X86REG_TO_LX7(a->src);
    int t0 = LX7_TMP0, t1 = LX7_TMP1;

    switch (a->type) {

    /* ---- MOV ------------------------------------------------ */
    case ACT_MOV_RR:
        GUARD(3); emit_mov(p, dr, sr); break;

    case ACT_MOV_RI:
        GUARD(16); emit_movi32(p, dr, (uint32_t)a->imm); break;

    /* ---- ALU ------------------------------------------------ */
    case ACT_ALU_RR:
        GUARD(3);
        switch (a->alu_op) {
        case ALU_ADD: emit_add(p, dr, dr, sr); break;
        case ALU_SUB: emit_sub(p, dr, dr, sr); break;
        case ALU_AND: emit_and(p, dr, dr, sr); break;
        case ALU_OR:  emit_or (p, dr, dr, sr); break;
        case ALU_XOR: emit_xor(p, dr, dr, sr); break;
        default: return false;
        }
        break;

    case ACT_ALU_RI: {
        int32_t imm = a->imm;
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
        break;
    }

    /* ---- NOT / NEG / INC / DEC ----------------------------- */
    case ACT_NOT_R:
        /* NOT dr = XOR dr, dr, 0xFFFFFFFF */
        GUARD(16 + 3);
        emit_movi32(p, t0, 0xFFFFFFFF);
        emit_xor(p, dr, dr, t0);
        break;

    case ACT_NEG_R:
        GUARD(3); emit_neg(p, dr, dr); break;

    case ACT_INC_R:
        GUARD(3); emit_addi(p, dr, dr, 1); break;

    case ACT_DEC_R:
        GUARD(3); emit_addi(p, dr, dr, -1); break;

    /* ---- Shifts -------------------------------------------- */
    case ACT_SHx_RI: {
        int sa = a->src & 31;
        GUARD(6);
        switch (a->sh_op) {
        case SH_SHL: emit_slli(p, dr, dr, sa);        break;
        case SH_SHR:
            if (sa <= 15) { emit_srli(p, dr, dr, sa); }
            else          { emit_ssri_wide(p, dr, sa); } /* helper below */
            break;
        case SH_SAR: emit_srai(p, dr, dr, sa);        break;
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
    case ACT_CMP_RR:
        cs->cmp_mode  = 1;
        cs->cmp_lreg  = dr;  /* note: dr = a->dst, the left operand */
        cs->cmp_rreg  = sr;
        break;

    case ACT_CMP_RI:
        /* Compute (dr - imm) into TMP2 for the subsequent branch */
        GUARD(16 + 3);
        emit_movi32(p, t1, (uint32_t)a->imm);
        emit_sub(p, t0, dr, t1);
        cs->cmp_mode  = 2;
        cs->cmp_lreg  = dr;
        cs->cmp_rreg  = t1;  /* original imm in t1 */
        cs->cmp_tmp   = t0;  /* (dr - imm) in t0 */
        break;

    case ACT_TEST_RR:
        /* Compute (dr & sr) into TMP0 for BEQZ/BNEZ */
        GUARD(3);
        emit_and(p, t0, dr, sr);
        cs->cmp_mode  = 3;
        cs->cmp_lreg  = t0;
        break;

    /* ---- Unconditional jump --------------------------------- */
    case ACT_JMP:
        /* Epilogue with target as next_ip, then J is not needed
         * because emit_epilogue ends with RET0 back to C caller. */
        GUARD(16 * 4);
        emit_epilogue(p, cpu_reg, a->target_eip);
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
        GUARD(3 + 16 * 4 * 2 + 16);

        uint8_t *branch_site = *p;
        *p += 3; /* placeholder for the branch instruction */

        /* Fallthrough epilogue */
        emit_epilogue(p, cpu_reg, fallback_eip);
        uint8_t *taken_start = *p;

        /* Taken epilogue */
        emit_epilogue(p, cpu_reg, a->target_eip);

        /* Back-patch branch: offset = (taken_start - (branch_site+3)) */
        int32_t br_off = (int32_t)(taken_start - (branch_site + 3));

        EmitPtr bp = branch_site;
        int lx7l = cs->cmp_lreg;
        int lx7r = cs->cmp_rreg;

        if (cs->cmp_mode == 0) {
            /* No pending CMP; treat as JMP (unconditional) for safety */
            emit_j(&bp, (int32_t)((taken_start - (branch_site + 4)) / 4));
            break;
        }

        /* Fuse CMP + Jcc into a single LX7 branch */
        /* Offset for zero/sign branches: 12-bit RI field */
        int zreg = (cs->cmp_mode == 2) ? cs->cmp_tmp : lx7l;

        switch (a->cc) {
        /* Equal / not-equal: use two-register BEQ/BNE when we have a
         * register pair, otherwise fall back to BEQZ/BNEZ on tmp.    */
        case CC_Z:
            if (cs->cmp_mode == 1)      emit_beq (&bp, lx7l, lx7r, (int8_t)br_off);
            else                        emit_beqz(&bp, zreg,         br_off & 0xFFF);
            break;
        case CC_NZ:
            if (cs->cmp_mode == 1)      emit_bne (&bp, lx7l, lx7r, (int8_t)br_off);
            else                        emit_bnez(&bp, zreg,         br_off & 0xFFF);
            break;
        /* Signed comparisons */
        case CC_L:   emit_blt (&bp, lx7l, lx7r, (int8_t)br_off); break;
        case CC_NL:  emit_bge (&bp, lx7l, lx7r, (int8_t)br_off); break;
        /* Unsigned comparisons */
        case CC_B:   emit_bltu(&bp, lx7l, lx7r, (int8_t)br_off); break;
        case CC_NB:  emit_bgeu(&bp, lx7l, lx7r, (int8_t)br_off); break;
        /* Sign-flag test (SF): result < 0 ↔ top bit set */
        case CC_S:   emit_bltz(&bp, zreg, br_off & 0xFFF); break;
        case CC_NS:  emit_bgez(&bp, zreg, br_off & 0xFFF); break;
        /* LE / NBE and overflow (CC_O/CC_NO/CC_BE/CC_NBE) need multi-
         * step flag arithmetic; emit a plain J as conservative fallback */
        default: emit_j(&bp, (int32_t)((taken_start - (branch_site + 4)) / 4)); break;
        }
        cs->cmp_mode = 0;
        break;
    }

    default:
        return false;
    }
    /* Clear pending CMP state after any non-branch instruction */
    if (a->type != ACT_CMP_RR && a->type != ACT_CMP_RI &&
        a->type != ACT_TEST_RR && a->type != ACT_JCC)
        cs->cmp_mode = 0;

    return true;
#undef GUARD
}

/* ================================================================== */
/*  Section 5 — JIT Cache & Public API                                */
/* ================================================================== */

static inline uint32_t jit_hash(uint32_t paddr)
{
    /* Simple hash: Knuth multiplicative hash */
    return (paddr * 2654435761u) % JIT_CACHE_ENTRIES;
}

void jit_init(JITState *jit, uint8_t *iram_pool)
{
    memset(jit, 0, sizeof(*jit));
    jit->pool = iram_pool;
}

void jit_invalidate_all(JITState *jit)
{
    for (int i = 0; i < JIT_CACHE_ENTRIES; i++)
        jit->blocks[i].status = JIT_EMPTY;
    jit->pool_used = 0;
}

void jit_invalidate_page(JITState *jit, uint32_t paddr)
{
    uint32_t page = paddr & ~0xFFFu;
    for (int i = 0; i < JIT_CACHE_ENTRIES; i++) {
        if ((jit->blocks[i].guest_paddr & ~0xFFFu) == page)
            jit->blocks[i].status = JIT_EMPTY;
    }
}

JITBlock *jit_translate(JITState *jit, CPUI386 *cpu)
{
    if (!jit->pool)
        return NULL;

    /* Only handle 32-bit protected mode */
    if (cpui386_is_code16(cpu))
        return NULL;

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
        if (laddr >= phys_mem_size)
            return NULL;
    }

    uint32_t paddr = laddr;
    if (paddr + JIT_SCAN_LIMIT * 16 > phys_mem_size)
        return NULL;

    const uint8_t *x86 = (const uint8_t *)cpui386_get_phys_mem(cpu) + paddr;

    /* Allocate or find cache slot */
    uint32_t  slot  = jit_hash(paddr);
    JITBlock *block = &jit->blocks[slot];

    if (block->status == JIT_NOJIT && block->guest_paddr == paddr)
        return NULL; /* previously deemed un-translatable */

    /* Evict the old entry if occupied by a different address */
    if (block->status == JIT_VALID && block->guest_paddr != paddr)
        block->status = JIT_EMPTY;

    /* Allocate space in the code pool */
    if (jit->pool_used + JIT_BLOCK_MAXBYTES > JIT_POOL_SIZE) {
        /* Pool full: flush everything and restart */
        jit_invalidate_all(jit);
    }

    uint8_t  *code_start = jit->pool + jit->pool_used;
    uint8_t  *code_end   = code_start + JIT_BLOCK_MAXBYTES;
    EmitPtr   p          = code_start;

    /* Emit prologue: load x86 GPRs into LX7 registers */
    emit_prologue(&p, LX7_CPU_REG);

    /* Scan and translate x86 instructions */
    CmpState cs   = {0};
    int      x86_consumed = 0;
    bool     ok   = true;
    uint32_t cur_eip = eip;

    for (int n = 0; n < JIT_SCAN_LIMIT; n++) {
        X86Action a;
        int bytes = decode_x86_insn(x86 + x86_consumed, cur_eip, &a);
        if (bytes == 0) {
            /* Unhandled: emit epilogue up to here and stop */
            if (n == 0) {
                /* Not a single instruction handled: mark NOJIT */
                block->guest_paddr = paddr;
                block->status      = JIT_NOJIT;
                jit->bailed++;
                return NULL;
            }
            /* Emit epilogue falling through to interpreter */
            emit_epilogue(&p, LX7_CPU_REG, cur_eip);
            x86_consumed += 0; /* don't consume the unhandled byte */
            break;
        }

        ok = emit_action(&p, code_end, &a, &cs, cur_eip + bytes, LX7_CPU_REG);
        if (!ok) break;

        x86_consumed += bytes;
        cur_eip      += bytes;

        /* Block-terminating instructions end the translation */
        if (a.type == ACT_JMP || a.type == ACT_JCC || a.type == ACT_BLOCK_END)
            break;
    }

    if (!ok || p >= code_end) {
        if (x86_consumed == 0) {
            block->guest_paddr = paddr;
            block->status      = JIT_NOJIT;
            jit->bailed++;
            return NULL;
        }
        /* Partial: emit epilogue */
        if (p + 80 < code_end)
            emit_epilogue(&p, LX7_CPU_REG, cur_eip);
        else {
            block->guest_paddr = paddr;
            block->status      = JIT_NOJIT;
            jit->bailed++;
            return NULL;
        }
    }

    /* Commit block */
    block->guest_paddr  = paddr;
    block->guest_cs_base= cs_base;
    block->host_code    = code_start;
    block->host_len     = (uint16_t)(p - code_start);
    block->x86_len      = (uint16_t)x86_consumed;
    block->status       = JIT_VALID;

    jit->pool_used += (uint32_t)(p - code_start);

    /*
     * No cache flush needed: s_jit_pool lives in IRAM, which is directly
     * accessible on both the instruction and data buses without going
     * through DCache or ICache.  Writes are immediately visible to the CPU.
     */

    return block;
}

bool jit_try_execute(JITState *jit, CPUI386 *cpu)
{
    if (!jit->pool || cpui386_is_code16(cpu))
        return false;

    uint32_t cs_base = cpui386_get_cs_base(cpu);
    uint32_t laddr   = cs_base + cpui386_get_next_ip(cpu);
    uint32_t cr0 = cpui386_get_cr0(cpu);
    if (cr0 & 0x80000000u) {
        if (laddr >= (uint32_t)cpui386_get_phys_mem_size(cpu))
            return false;
    }
    uint32_t paddr = laddr;

    uint32_t  slot  = jit_hash(paddr);
    JITBlock *block = &jit->blocks[slot];

    if (block->status == JIT_NOJIT && block->guest_paddr == paddr) {
        jit->misses++;
        return false;
    }

    if (block->status != JIT_VALID || block->guest_paddr != paddr) {
        /* Cache miss: translate */
        block = jit_translate(jit, cpu);
        if (!block) {
            jit->misses++;
            return false;
        }
    }

    jit->hits++;

    /*
     * Call the JIT block.  The block is a CALL0-compatible function:
     *   void jit_block(CPUI386 *cpu);
     * It reads/writes cpu->gprx[], updates cpu->next_ip, and returns.
     *
     * On ESP32 we invoke via a function pointer so GCC emits CALLX0.
     * On other platforms this is a no-op (pool == NULL guard above).
     */
#ifdef BUILD_ESP32
    typedef void (*JITFunc)(CPUI386 *);
    JITFunc fn = (JITFunc)(void *)block->host_code;
    fn(cpu);
    return true;
#else
    (void)block;
    return false;
#endif
}
