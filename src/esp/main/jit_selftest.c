/*
 * jit_selftest.c - board-side differential self-test for tiny386 JIT.
 *
 * For each built-in x86 byte sequence:
 *   1. Clone CPUI386 initial state
 *   2. Path A: interpreter-only execution
 *   3. Path B: JIT-only execution (with per-case action whitelist)
 *   4. Compare 8xGPR + full EFLAGS + next_ip
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "jit_selftest.h"
#include "jit_lx7.h"

#if defined(BUILD_ESP32) && defined(TINY386_ENABLE_JIT)

#define ST_PHYS_SIZE   8192u
#define ST_CODE_BASE   0x1000u
#define ST_FLASH_PROBE_BYTES 256u
#define ST_SEG_DS      3

/* Mirror ActionType ordinals in jit_lx7.c (ACT_NONE=0). */
#define ACT_NOP     1
#define ACT_MOV_RR  2
#define ACT_MOV_RI  3
#define ACT_ALU_RR  4
#define ACT_ALU_RI  5
#define ACT_NOT_R   6
#define ACT_NEG_R   7
#define ACT_INC_R   8
#define ACT_DEC_R   9
#define ACT_SHx_RI  10
#define ACT_CMP_RR  12
#define ACT_CMP_RI  13
#define ACT_TEST_RR 14
#define ACT_JMP     15
#define ACT_JCC     16
#define ACT_XCHG_EAX_R 19
#define ACT_MOV_RM32   21
#define ACT_MOV_MR32   22
#define ACT_MOV_RM8    23
#define ACT_MOV_MR8    24
#define ACT_MOV_RM16   25
#define ACT_MOV_MR16   26
#define ACT_CMP_RM32   27
#define ACT_CMP_MR32   28
#define ACT_TEST_MR32  29

#define JIT_ST_ALLOW(act)  (1u << (unsigned)(act))

enum {
	FL_CF = 0x001,
	FL_PF = 0x004,
	FL_AF = 0x010,
	FL_ZF = 0x040,
	FL_SF = 0x080,
	FL_OF = 0x800,
};

typedef struct {
	const char *name;
	const uint8_t *code;
	uint8_t code_len;
	uint32_t jit_allow;
	void (*init)(CPUI386 *cpu);
} SelftestCase;

typedef struct {
	const char *label;
	uint32_t bytes;
	uint32_t sum32;
	uint32_t xor32;
	uint8_t first;
	uint8_t last;
} FlashReadProbe;

typedef enum {
	ALU_ST_ADD_RR,
	ALU_ST_SUB_RR,
	ALU_ST_AND_RR,
	ALU_ST_OR_RR,
	ALU_ST_XOR_RR,
	ALU_ST_ADD_RI,
	ALU_ST_SUB_RI,
	ALU_ST_AND_RI,
	ALU_ST_OR_RI,
	ALU_ST_XOR_RI,
} AluSelftestOp;

typedef struct {
	const char *name;
	AluSelftestOp op;
	uint32_t eax;
	uint32_t ebx;
	int32_t imm;
} AluSelftestCase;

typedef enum {
	SHIFT_ST_SHL,
	SHIFT_ST_SHR,
	SHIFT_ST_SAR,
} ShiftSelftestOp;

typedef struct {
	const char *name;
	ShiftSelftestOp op;
	uint32_t eax;
	uint8_t count;
} ShiftSelftestCase;

typedef enum {
	BR_SRC_CMP_RR,
	BR_SRC_CMP_RI,
	BR_SRC_TEST_RR,
	BR_SRC_CMP_RM32,
	BR_SRC_CMP_MR32,
	BR_SRC_TEST_MR32,
} BranchSourceOp;

typedef struct {
	const char *name;
	BranchSourceOp src_op;
	uint8_t cc;
	uint32_t eax;
	uint32_t ebx;
	int8_t imm8;
	bool expect_taken;
} BranchSelftestCase;

static const uint8_t code_nop[] = { 0x90 };
static const uint8_t code_mov_eax[] = { 0xB8, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t code_mov_ebx_neg[] = { 0xBB, 0x00, 0x00, 0x00, 0x80 };
static const uint8_t code_mov_eax_ebx[] = { 0x89, 0xD8 };
static const uint8_t code_mov_edi_eax[] = { 0x8B, 0xF8 };
static const uint8_t code_nop_then_mov_eax[] = {
	0x90,
	0xB8, 0x78, 0x56, 0x34, 0x12,
};
static const uint8_t code_mov_eax_nop_mov_ebx[] = {
	0xB8, 0x78, 0x56, 0x34, 0x12,
	0x90,
	0xBB, 0x00, 0x00, 0x00, 0x80,
};
static const uint8_t code_link_mov_eax_nop[] = {
	0xB8, 0x78, 0x56, 0x34, 0x12,
	0x90,
};
static const uint8_t code_block_mov_chain[] = {
	0xB8, 0x78, 0x56, 0x34, 0x12, /* mov eax,0x12345678 */
	0x8B, 0xD8,                   /* mov ebx,eax */
	0x90,                         /* nop */
};
static const uint8_t code_block_add_add_cover_flags[] = {
	0x01, 0xD8,                   /* add eax,ebx; flags dead */
	0x01, 0xC8,                   /* add eax,ecx; flags live */
};
static const uint8_t code_not_eax[] = { 0xF7, 0xD0 };
static const uint8_t code_neg_eax[] = { 0xF7, 0xD8 };
static const uint8_t code_inc_eax[] = { 0x40 };
static const uint8_t code_dec_eax[] = { 0x48 };
static const uint8_t code_xchg_eax_ebx[] = { 0x93 };
static const uint8_t code_xchg_eax_edi[] = { 0x97 };
static const uint8_t code_mov_eax_mem_ebx_disp8[] = { 0x8B, 0x43, 0x20 };
static const uint8_t code_mov_mem_ebx_disp8_eax[] = { 0x89, 0x43, 0x24 };
static const uint8_t code_mov_eax_mem_esp_disp8[] = { 0x8B, 0x44, 0x24, 0x20 };
static const uint8_t code_mov_mem_esp_disp8_eax[] = { 0x89, 0x44, 0x24, 0x24 };
static const uint8_t code_mov_eax_mem_disp32[] = {
	0x8B, 0x05, 0x20, 0x18, 0x00, 0x00,
};
static const uint8_t code_mov_mem_disp32_eax[] = {
	0x89, 0x05, 0x24, 0x18, 0x00, 0x00,
};
static const uint8_t code_mov_eax_moffs32[] = {
	0xA1, 0x20, 0x18, 0x00, 0x00,
};
static const uint8_t code_mov_moffs32_eax[] = {
	0xA3, 0x24, 0x18, 0x00, 0x00,
};
static const uint8_t code_mov_al_mem_ebx_disp8[] = { 0x8A, 0x43, 0x20 };
static const uint8_t code_mov_ah_mem_ebx_disp8[] = { 0x8A, 0x63, 0x20 };
static const uint8_t code_mov_mem_ebx_disp8_cl[] = { 0x88, 0x4B, 0x24 };
static const uint8_t code_mov_mem_ebx_disp8_ah[] = { 0x88, 0x63, 0x24 };
static const uint8_t code_mov_ax_mem_ebx_disp8[] = { 0x66, 0x8B, 0x43, 0x20 };
static const uint8_t code_mov_mem_ebx_disp8_ax[] = { 0x66, 0x89, 0x43, 0x24 };
static const uint8_t code_mov_eax_mem_sib_scale4[] = { 0x8B, 0x44, 0x8B, 0x20 };
static const uint8_t code_mov_mem_sib_scale4_eax[] = { 0x89, 0x44, 0x8B, 0x24 };
static const uint8_t code_jmp_rel8[] = {
	0xEB, 0x03,                   /* jmp +3 */
	0xB8, 0x11, 0x11, 0x11, 0x11, /* skipped */
	0xBB, 0x22, 0x22, 0x22, 0x22, /* target */
};
static const uint8_t code_jmp_rel32[] = {
	0xE9, 0x05, 0x00, 0x00, 0x00, /* jmp +5 */
	0xB8, 0x33, 0x33, 0x33, 0x33, /* skipped */
	0xBB, 0x44, 0x44, 0x44, 0x44, /* target */
};
static const uint8_t code_mov_cr3_eax[] = {
	0x0F, 0x22, 0xD8,             /* mov cr3,eax */
};

static uint8_t build_alu_code(const AluSelftestCase *tc, uint8_t *code)
{
	uint32_t imm = (uint32_t)tc->imm;

	switch (tc->op) {
	case ALU_ST_ADD_RR:
		code[0] = 0x01; code[1] = 0xD8; /* add eax,ebx */
		return 2;
	case ALU_ST_SUB_RR:
		code[0] = 0x29; code[1] = 0xD8; /* sub eax,ebx */
		return 2;
	case ALU_ST_AND_RR:
		code[0] = 0x21; code[1] = 0xD8; /* and eax,ebx */
		return 2;
	case ALU_ST_OR_RR:
		code[0] = 0x09; code[1] = 0xD8; /* or eax,ebx */
		return 2;
	case ALU_ST_XOR_RR:
		code[0] = 0x31; code[1] = 0xD8; /* xor eax,ebx */
		return 2;
	case ALU_ST_ADD_RI:
		code[0] = 0x81; code[1] = 0xC0; /* add eax,imm32 */
		break;
	case ALU_ST_SUB_RI:
		code[0] = 0x81; code[1] = 0xE8; /* sub eax,imm32 */
		break;
	case ALU_ST_AND_RI:
		code[0] = 0x81; code[1] = 0xE0; /* and eax,imm32 */
		break;
	case ALU_ST_OR_RI:
		code[0] = 0x81; code[1] = 0xC8; /* or eax,imm32 */
		break;
	case ALU_ST_XOR_RI:
		code[0] = 0x81; code[1] = 0xF0; /* xor eax,imm32 */
		break;
	default:
		return 0;
	}

	code[2] = (uint8_t)imm;
	code[3] = (uint8_t)(imm >> 8);
	code[4] = (uint8_t)(imm >> 16);
	code[5] = (uint8_t)(imm >> 24);
	return 6;
}

static uint8_t build_shift_code(const ShiftSelftestCase *tc, uint8_t *code)
{
	code[0] = 0xC1;
	switch (tc->op) {
	case SHIFT_ST_SHL:
		code[1] = 0xE0; /* shl eax,imm8 */
		break;
	case SHIFT_ST_SHR:
		code[1] = 0xE8; /* shr eax,imm8 */
		break;
	case SHIFT_ST_SAR:
		code[1] = 0xF8; /* sar eax,imm8 */
		break;
	default:
		return 0;
	}
	code[2] = tc->count;
	return 3;
}

static uint8_t build_branch_code(const BranchSelftestCase *tc, uint8_t *code)
{
	uint8_t len = 0;

	switch (tc->src_op) {
	case BR_SRC_CMP_RR:
		code[len++] = 0x39; code[len++] = 0xD8; /* cmp eax,ebx */
		break;
	case BR_SRC_CMP_RI:
		code[len++] = 0x83; code[len++] = 0xF8; code[len++] = (uint8_t)tc->imm8; /* cmp eax,imm8 */
		break;
	case BR_SRC_TEST_RR:
		code[len++] = 0x85; code[len++] = 0xD8; /* test eax,ebx */
		break;
	case BR_SRC_CMP_RM32:
		code[len++] = 0x3B; code[len++] = 0x43; code[len++] = 0x20; /* cmp eax,[ebx+0x20] */
		break;
	case BR_SRC_CMP_MR32:
		code[len++] = 0x39; code[len++] = 0x43; code[len++] = 0x20; /* cmp [ebx+0x20],eax */
		break;
	case BR_SRC_TEST_MR32:
		code[len++] = 0x85; code[len++] = 0x43; code[len++] = 0x20; /* test [ebx+0x20],eax */
		break;
	default:
		return 0;
	}

	code[len++] = (uint8_t)(0x70u | (tc->cc & 0x0fu)); /* jcc +5 */
	code[len++] = 0x05;
	code[len++] = 0xB8; code[len++] = 0x11; code[len++] = 0x11; code[len++] = 0x11; code[len++] = 0x11;
	code[len++] = 0xBB; code[len++] = 0x22; code[len++] = 0x22; code[len++] = 0x22; code[len++] = 0x22;
	return len;
}

static const esp_partition_t *find_flash_probe_partition(void)
{
	static const char *labels[] = {
		"ini",
		"resources",
		"bios.bin",
	};

	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
		const esp_partition_t *part =
			esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
						 ESP_PARTITION_SUBTYPE_ANY,
						 labels[i]);
		if (part)
			return part;
	}
	return NULL;
}

static bool read_flash_probe(const char *phase, FlashReadProbe *probe)
{
	uint8_t buf[ST_FLASH_PROBE_BYTES];
	const esp_partition_t *part = find_flash_probe_partition();
	uint32_t bytes;
	uint32_t sum = 0;
	uint32_t x = 0;
	esp_err_t err;

	memset(probe, 0, sizeof(*probe));
	if (!part) {
		esp_rom_printf("[jit_selftest] FAIL flash_%s: no probe partition\n", phase);
		return false;
	}

	bytes = part->size < ST_FLASH_PROBE_BYTES ? (uint32_t)part->size : ST_FLASH_PROBE_BYTES;
	if (bytes == 0) {
		esp_rom_printf("[jit_selftest] FAIL flash_%s: empty partition %s\n",
			       phase, part->label);
		return false;
	}

	memset(buf, 0, sizeof(buf));
	err = esp_partition_read(part, 0, buf, bytes);
	if (err != ESP_OK) {
		esp_rom_printf("[jit_selftest] FAIL flash_%s: read %s err=%d\n",
			       phase, part->label, (int)err);
		return false;
	}

	for (uint32_t i = 0; i < bytes; i++) {
		sum += buf[i];
		x ^= (uint32_t)buf[i] << ((i & 3u) * 8u);
	}

	probe->label = part->label;
	probe->bytes = bytes;
	probe->sum32 = sum;
	probe->xor32 = x;
	probe->first = buf[0];
	probe->last = buf[bytes - 1u];

	esp_rom_printf("[jit_selftest] flash_%s %s bytes=%u sum=0x%08" PRIx32
		       " xor=0x%08" PRIx32 " first=%02x last=%02x\n",
		       phase, probe->label, (unsigned)probe->bytes,
		       probe->sum32, probe->xor32, probe->first, probe->last);
	return true;
}

static bool compare_flash_probe(const FlashReadProbe *before,
				const FlashReadProbe *after)
{
	if (strcmp(before->label, after->label) != 0 ||
	    before->bytes != after->bytes ||
	    before->sum32 != after->sum32 ||
	    before->xor32 != after->xor32 ||
	    before->first != after->first ||
	    before->last != after->last) {
		esp_rom_printf("[jit_selftest] FAIL flash_after_jit: before %s/%u "
			       "sum=0x%08" PRIx32 " xor=0x%08" PRIx32
			       " first=%02x last=%02x, after %s/%u "
			       "sum=0x%08" PRIx32 " xor=0x%08" PRIx32
			       " first=%02x last=%02x\n",
			       before->label, (unsigned)before->bytes,
			       before->sum32, before->xor32,
			       before->first, before->last,
			       after->label, (unsigned)after->bytes,
			       after->sum32, after->xor32,
			       after->first, after->last);
		return false;
	}

	esp_rom_printf("[jit_selftest] PASS flash_before_after_jit (%s %u bytes)\n",
		       before->label, (unsigned)before->bytes);
	return true;
}

static void init_nop_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x11111111u * (unsigned)(i + 1));
	cpu_setflags(cpu, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF, 0);
}

static void init_mov_eax_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0xA0000000u + (unsigned)i);
	cpu_setflags(cpu, FL_CF | FL_OF, 0);
	cpu_setflags(cpu, 0, FL_ZF);
}

static void init_mov_ebx_state(CPUI386 *cpu)
{
	cpui386_set_gpr(cpu, 0, 0x01234567u);
	cpui386_set_gpr(cpu, 1, 0x89abcdefu);
	cpui386_set_gpr(cpu, 3, 0xdeadbeefu);
	cpu_setflags(cpu, FL_SF | FL_ZF, 0);
	cpu_setflags(cpu, 0, FL_CF | FL_OF);
}

static void init_mov_rr_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x10203040u + 0x11111111u * (unsigned)i);
	cpu_setflags(cpu, FL_CF | FL_AF | FL_OF, 0);
	cpu_setflags(cpu, 0, FL_ZF | FL_SF | FL_PF);
}

static void init_not_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x01020304u + 0x11111111u * (unsigned)i);
	cpui386_set_gpr(cpu, 0, 0x55aa00ffu);
	cpu_setflags(cpu, 0, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
}

static void init_neg_zero_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x21222324u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0x00000000u);
	cpu_setflags(cpu, 0, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
}

static void init_neg_min_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x31323334u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0x80000000u);
	cpu_setflags(cpu, FL_CF | FL_OF, 0);
	cpu_setflags(cpu, 0, FL_AF | FL_ZF | FL_SF | FL_PF);
}

static void init_inc_cf_set_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x41424344u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0x7fffffffu);
	cpu_setflags(cpu, FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF, FL_CF);
}

static void init_inc_cf_clear_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x51525354u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0xffffffffu);
	cpu_setflags(cpu, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF, 0);
}

static void init_dec_cf_set_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x61626364u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0x80000000u);
	cpu_setflags(cpu, FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF, FL_CF);
}

static void init_dec_cf_clear_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x71727374u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0x00000001u);
	cpu_setflags(cpu, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF, 0);
}

static void init_dead_flags_add_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x81828384u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0xffffffffu); /* EAX */
	cpui386_set_gpr(cpu, 1, 0x7fffffffu); /* ECX */
	cpui386_set_gpr(cpu, 3, 0x00000001u); /* EBX */
	cpu_setflags(cpu, 0, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
}

static void init_mem_state(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x23242526u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, 0x00000001u);
	cpui386_set_gpr(cpu, 3, 0x00001800u);
	cpui386_set_gpr(cpu, 4, 0x00001800u);
	cpu_setflags(cpu, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF, 0);
}

static void init_mem_sib_state(CPUI386 *cpu)
{
	init_mem_state(cpu);
	cpui386_set_gpr(cpu, 1, 0x00000004u);
	cpui386_set_gpr(cpu, 3, 0x00001800u);
}

static const char *gpr_name(int i)
{
	static const char *names[] = {
		"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"
	};
	return names[i];
}

static const char *flag_name(uword bit)
{
	switch (bit) {
	case 0x001: return "CF";
	case 0x004: return "PF";
	case 0x010: return "AF";
	case 0x040: return "ZF";
	case 0x080: return "SF";
	case 0x800: return "OF";
	default: return "?";
	}
}

static int bit_index(uword bit)
{
	int idx = 0;
	while ((bit >>= 1u) != 0)
		idx++;
	return idx;
}

static bool compare_snapshots(const char *case_name,
			      const JITCpuSnapshot *interp,
			      const JITCpuSnapshot *jit)
{
	for (int i = 0; i < 8; i++) {
		if (interp->gpr[i] != jit->gpr[i]) {
			esp_rom_printf(
				"[jit_selftest] FAIL %s: %s 0x%08" PRIx32 " != 0x%08" PRIx32 "\n",
				case_name, gpr_name(i),
				interp->gpr[i], jit->gpr[i]);
			return false;
		}
	}

	if (interp->flags != jit->flags) {
		uword diff = interp->flags ^ jit->flags;
		uword first = diff & (~diff + 1u);
		esp_rom_printf(
			"[jit_selftest] FAIL %s: EFLAGS bit%d(%s) interp=%d jit=%d "
			"(interp=0x%08" PRIx32 " jit=0x%08" PRIx32 ")\n",
			case_name,
			bit_index(first), flag_name(first),
			(int)((interp->flags & first) != 0),
			(int)((jit->flags & first) != 0),
			(uint32_t)interp->flags, (uint32_t)jit->flags);
		return false;
	}

	if (interp->next_ip != jit->next_ip) {
		esp_rom_printf(
			"[jit_selftest] FAIL %s: next_ip 0x%08" PRIx32 " != 0x%08" PRIx32 "\n",
			case_name, interp->next_ip, jit->next_ip);
		return false;
	}
	if (interp->ip != jit->ip) {
		esp_rom_printf(
			"[jit_selftest] FAIL %s: ip 0x%08" PRIx32 " != 0x%08" PRIx32 "\n",
			case_name, interp->ip, jit->ip);
		return false;
	}
	if (interp->cycle != jit->cycle) {
		esp_rom_printf(
			"[jit_selftest] FAIL %s: cycle %" PRIu32 " != %" PRIu32 "\n",
			case_name, interp->cycle, jit->cycle);
		return false;
	}
	return true;
}

static void setup_cpu(CPUI386 *cpu, uint8_t *mem, const SelftestCase *tc)
{
	cpui386_reset_pm(cpu, ST_CODE_BASE);
	memset(mem + ST_CODE_BASE, 0xcc, 64);
	memcpy(mem + ST_CODE_BASE, tc->code, tc->code_len);
	if (tc->init)
		tc->init(cpu);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
}

static bool run_case(CPUI386 *cpu, uint8_t *mem, const SelftestCase *tc)
{
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot jit_result;
	int interp_steps;
	int jit_steps;

	setup_cpu(cpu, mem, tc);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, 1);
	jit_cpu_snapshot(cpu, &interp_result);

	jit_cpu_invalidate_cache(cpu);
	jit_cpu_restore(cpu, &baseline);
	jit_selftest_set_allowed_actions(tc->jit_allow);
	jit_steps = jit_cpu_step_jit(cpu, 1);
	jit_selftest_clear_allowed_actions();
	jit_cpu_snapshot(cpu, &jit_result);

	if (interp_steps <= 0) {
		esp_rom_printf( "[jit_selftest] FAIL %s: interpreter executed 0 insns\n",
			tc->name);
		return false;
	}
	if (jit_steps <= 0) {
		esp_rom_printf( "[jit_selftest] FAIL %s: JIT executed 0 insns\n",
			tc->name);
		return false;
	}

	if (!compare_snapshots(tc->name, &interp_result, &jit_result)) {
		return false;
	}

	esp_rom_printf( "[jit_selftest] PASS %s (interp=%d jit=%d)\n",
		tc->name, interp_steps, jit_steps);
	return true;
}

static bool run_mixed_case(CPUI386 *cpu, uint8_t *mem, const SelftestCase *tc,
			   int interp_total_steps, int interp_after_jit_steps)
{
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot mixed_result;
	int interp_steps;
	int jit_steps;
	int tail_steps;

	setup_cpu(cpu, mem, tc);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, interp_total_steps);
	jit_cpu_snapshot(cpu, &interp_result);

	jit_cpu_invalidate_cache(cpu);
	jit_cpu_restore(cpu, &baseline);
	jit_selftest_set_allowed_actions(tc->jit_allow);
	jit_steps = jit_cpu_step_jit(cpu, 1);
	jit_selftest_clear_allowed_actions();
	tail_steps = jit_cpu_step_interp(cpu, interp_after_jit_steps);
	jit_cpu_snapshot(cpu, &mixed_result);

	if (interp_steps != interp_total_steps) {
		esp_rom_printf("[jit_selftest] FAIL %s_mixed: interpreter steps %d != %d\n",
			       tc->name, interp_steps, interp_total_steps);
		return false;
	}
	if (jit_steps <= 0) {
		esp_rom_printf("[jit_selftest] FAIL %s_mixed: JIT executed 0 insns\n",
			       tc->name);
		return false;
	}
	if (tail_steps != interp_after_jit_steps) {
		esp_rom_printf("[jit_selftest] FAIL %s_mixed: tail steps %d != %d\n",
			       tc->name, tail_steps, interp_after_jit_steps);
		return false;
	}
	if (jit_steps + tail_steps != interp_total_steps) {
		esp_rom_printf("[jit_selftest] FAIL %s_mixed: total steps %d != %d\n",
			       tc->name, jit_steps + tail_steps, interp_total_steps);
		return false;
	}
	if (!compare_snapshots(tc->name, &interp_result, &mixed_result))
		return false;

	esp_rom_printf("[jit_selftest] PASS %s_mixed (interp=%d jit=%d tail=%d)\n",
		       tc->name, interp_steps, jit_steps, tail_steps);
	return true;
}

static bool run_block_case(CPUI386 *cpu, uint8_t *mem, const SelftestCase *tc,
			   int expected_steps)
{
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot jit_result;
	int interp_steps;
	int jit_steps;

	setup_cpu(cpu, mem, tc);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, expected_steps);
	jit_cpu_snapshot(cpu, &interp_result);

	jit_cpu_invalidate_cache(cpu);
	jit_cpu_restore(cpu, &baseline);
	jit_selftest_set_allowed_actions(tc->jit_allow);
	jit_steps = jit_cpu_step_jit(cpu, expected_steps);
	jit_selftest_clear_allowed_actions();
	jit_cpu_snapshot(cpu, &jit_result);

	if (interp_steps != expected_steps) {
		esp_rom_printf("[jit_selftest] FAIL %s_block: interpreter steps %d != %d\n",
			       tc->name, interp_steps, expected_steps);
		return false;
	}
	if (jit_steps != expected_steps) {
		esp_rom_printf("[jit_selftest] FAIL %s_block: JIT steps %d != %d\n",
			       tc->name, jit_steps, expected_steps);
		return false;
	}
	if (!compare_snapshots(tc->name, &interp_result, &jit_result))
		return false;

	esp_rom_printf("[jit_selftest] PASS %s_block (interp=%d jit=%d)\n",
		       tc->name, interp_steps, jit_steps);
	return true;
}

static bool run_linked_exit_case(CPUI386 *cpu, uint8_t *mem)
{
	static const SelftestCase tc = {
		.name = "LINK_FALLTHROUGH_MOV_EAX_NOP",
		.code = code_link_mov_eax_nop,
		.code_len = sizeof(code_link_mov_eax_nop),
		.jit_allow = JIT_ST_ALLOW(ACT_MOV_RI),
		.init = init_mov_eax_state,
	};
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot jit_result;
	int interp_steps;
	int jit_steps;

	setup_cpu(cpu, mem, &tc);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, 2);
	jit_cpu_snapshot(cpu, &interp_result);

	jit_cpu_invalidate_cache(cpu);
	jit_cpu_restore(cpu, &baseline);

	jit_selftest_set_allowed_actions(JIT_ST_ALLOW(ACT_NOP));
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE + 5);
	if (!jit_cpu_translate_current(cpu)) {
		jit_selftest_clear_allowed_actions();
		esp_rom_printf("[jit_selftest] FAIL %s_link: target translate failed\n",
			       tc.name);
		return false;
	}

	jit_selftest_set_allowed_actions(tc.jit_allow);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
	jit_steps = jit_cpu_step_jit(cpu, 2);
	jit_selftest_clear_allowed_actions();
	jit_cpu_snapshot(cpu, &jit_result);

	if (interp_steps != 2) {
		esp_rom_printf("[jit_selftest] FAIL %s_link: interpreter steps %d != 2\n",
			       tc.name, interp_steps);
		return false;
	}
	if (jit_steps != 2) {
		esp_rom_printf("[jit_selftest] FAIL %s_link: JIT steps %d != 2\n",
			       tc.name, jit_steps);
		return false;
	}
	if (!compare_snapshots(tc.name, &interp_result, &jit_result))
		return false;

	esp_rom_printf("[jit_selftest] PASS %s_link (interp=%d jit=%d)\n",
		       tc.name, interp_steps, jit_steps);
	return true;
}

static void setup_mem_fixture(uint8_t *mem)
{
	mem[0x1820] = 0x44;
	mem[0x1821] = 0x33;
	mem[0x1822] = 0x22;
	mem[0x1823] = 0x11;
	mem[0x1824] = 0xaa;
	mem[0x1825] = 0xbb;
	mem[0x1826] = 0xcc;
	mem[0x1827] = 0xdd;
	mem[0x1830] = 0x88;
	mem[0x1831] = 0x77;
	mem[0x1832] = 0x66;
	mem[0x1833] = 0x55;
	mem[0x1834] = 0x12;
	mem[0x1835] = 0x34;
	mem[0x1836] = 0x56;
	mem[0x1837] = 0x78;
}

static bool run_memory_mov_case(CPUI386 *cpu, uint8_t *mem,
				const SelftestCase *tc, uint32_t probe_addr)
{
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot jit_result;
	uint32_t interp_mem;
	uint32_t jit_mem;
	int interp_steps;
	int jit_steps;

	setup_cpu(cpu, mem, tc);
	setup_mem_fixture(mem);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, 1);
	jit_cpu_snapshot(cpu, &interp_result);
	memcpy(&interp_mem, mem + probe_addr, sizeof(interp_mem));

	setup_cpu(cpu, mem, tc);
	setup_mem_fixture(mem);
	jit_cpu_restore(cpu, &baseline);
	jit_cpu_invalidate_cache(cpu);
	jit_selftest_set_allowed_actions(tc->jit_allow);
	jit_steps = jit_cpu_step_jit(cpu, 1);
	jit_selftest_clear_allowed_actions();
	jit_cpu_snapshot(cpu, &jit_result);
	memcpy(&jit_mem, mem + probe_addr, sizeof(jit_mem));

	if (interp_steps != 1 || jit_steps != 1) {
		esp_rom_printf("[jit_selftest] FAIL %s_mem: steps interp=%d jit=%d\n",
			       tc->name, interp_steps, jit_steps);
		return false;
	}
	if (!compare_snapshots(tc->name, &interp_result, &jit_result))
		return false;
	if (interp_mem != jit_mem) {
		esp_rom_printf("[jit_selftest] FAIL %s_mem: mem[0x%04" PRIx32
			       "] 0x%08" PRIx32 " != 0x%08" PRIx32 "\n",
			       tc->name, probe_addr, interp_mem, jit_mem);
		return false;
	}

	esp_rom_printf("[jit_selftest] PASS %s_mem (interp=%d jit=%d)\n",
		       tc->name, interp_steps, jit_steps);
	return true;
}

static void setup_alu_cpu(CPUI386 *cpu, uint8_t *mem,
			  const AluSelftestCase *tc)
{
	uint8_t code[8];
	uint8_t code_len = build_alu_code(tc, code);

	cpui386_reset_pm(cpu, ST_CODE_BASE);
	memset(mem + ST_CODE_BASE, 0xcc, 64);
	memcpy(mem + ST_CODE_BASE, code, code_len);
	cpui386_set_gpr(cpu, 0, tc->eax);
	cpui386_set_gpr(cpu, 3, tc->ebx);
	for (int i = 1; i < 8; i++) {
		if (i != 3)
			cpui386_set_gpr(cpu, i, 0x51525354u + (uint32_t)i);
	}
	cpu_setflags(cpu, 0, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
}

static bool run_alu_case(CPUI386 *cpu, uint8_t *mem,
			 const AluSelftestCase *tc)
{
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot jit_result;
	uint32_t allow;
	int interp_steps;
	int jit_steps;

	setup_alu_cpu(cpu, mem, tc);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, 1);
	jit_cpu_snapshot(cpu, &interp_result);

	jit_cpu_invalidate_cache(cpu);
	jit_cpu_restore(cpu, &baseline);
	allow = (tc->op == ALU_ST_ADD_RR || tc->op == ALU_ST_SUB_RR ||
		 tc->op == ALU_ST_AND_RR || tc->op == ALU_ST_OR_RR ||
		 tc->op == ALU_ST_XOR_RR) ?
		JIT_ST_ALLOW(ACT_ALU_RR) : JIT_ST_ALLOW(ACT_ALU_RI);
	jit_selftest_set_allowed_actions(allow);
	jit_steps = jit_cpu_step_jit(cpu, 1);
	jit_selftest_clear_allowed_actions();
	jit_cpu_snapshot(cpu, &jit_result);

	if (interp_steps != 1) {
		esp_rom_printf("[jit_selftest] FAIL %s: interpreter steps %d != 1\n",
			       tc->name, interp_steps);
		return false;
	}
	if (jit_steps != 1) {
		esp_rom_printf("[jit_selftest] FAIL %s: JIT steps %d != 1\n",
			       tc->name, jit_steps);
		return false;
	}
	if (!compare_snapshots(tc->name, &interp_result, &jit_result))
		return false;

	esp_rom_printf("[jit_selftest] PASS %s (interp=%d jit=%d)\n",
		       tc->name, interp_steps, jit_steps);
	return true;
}

static void setup_shift_cpu(CPUI386 *cpu, uint8_t *mem,
			    const ShiftSelftestCase *tc)
{
	uint8_t code[4];
	uint8_t code_len = build_shift_code(tc, code);

	cpui386_reset_pm(cpu, ST_CODE_BASE);
	memset(mem + ST_CODE_BASE, 0xcc, 64);
	memcpy(mem + ST_CODE_BASE, code, code_len);
	for (int i = 0; i < 8; i++)
		cpui386_set_gpr(cpu, i, 0x91929394u + (uint32_t)i);
	cpui386_set_gpr(cpu, 0, tc->eax);
	cpu_setflags(cpu, 0, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
}

static bool run_shift_case(CPUI386 *cpu, uint8_t *mem,
			   const ShiftSelftestCase *tc)
{
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot jit_result;
	int interp_steps;
	int jit_steps;

	setup_shift_cpu(cpu, mem, tc);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, 1);
	jit_cpu_snapshot(cpu, &interp_result);

	jit_cpu_invalidate_cache(cpu);
	jit_cpu_restore(cpu, &baseline);
	jit_selftest_set_allowed_actions(JIT_ST_ALLOW(ACT_SHx_RI));
	jit_steps = jit_cpu_step_jit(cpu, 1);
	jit_selftest_clear_allowed_actions();
	jit_cpu_snapshot(cpu, &jit_result);

	if (interp_steps != 1) {
		esp_rom_printf("[jit_selftest] FAIL %s: interpreter steps %d != 1\n",
			       tc->name, interp_steps);
		return false;
	}
	if (jit_steps != 1) {
		esp_rom_printf("[jit_selftest] FAIL %s: JIT steps %d != 1\n",
			       tc->name, jit_steps);
		return false;
	}
	if (!compare_snapshots(tc->name, &interp_result, &jit_result))
		return false;

	esp_rom_printf("[jit_selftest] PASS %s (interp=%d jit=%d)\n",
		       tc->name, interp_steps, jit_steps);
	return true;
}

static void setup_branch_cpu(CPUI386 *cpu, uint8_t *mem,
			     const BranchSelftestCase *tc)
{
	uint8_t code[16];
	uint8_t code_len = build_branch_code(tc, code);

	cpui386_reset_pm(cpu, ST_CODE_BASE);
	memset(mem + ST_CODE_BASE, 0xcc, 64);
	setup_mem_fixture(mem);
	memcpy(mem + ST_CODE_BASE, code, code_len);
	cpui386_set_gpr(cpu, 0, tc->eax);
	cpui386_set_gpr(cpu, 3, tc->ebx);
	if (tc->src_op == BR_SRC_CMP_RM32 || tc->src_op == BR_SRC_CMP_MR32 ||
	    tc->src_op == BR_SRC_TEST_MR32)
		cpui386_set_gpr(cpu, 3, 0x00001800u);
	for (int i = 1; i < 8; i++) {
		if (i != 3)
			cpui386_set_gpr(cpu, i, 0x71727374u + (uint32_t)i);
	}
	cpu_setflags(cpu, 0, FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
}

static bool run_branch_case(CPUI386 *cpu, uint8_t *mem,
			    const BranchSelftestCase *tc)
{
	JITCpuSnapshot baseline;
	JITCpuSnapshot interp_result;
	JITCpuSnapshot jit_result;
	uint32_t allow = JIT_ST_ALLOW(ACT_JCC);
	int interp_steps;
	int jit_steps;

	switch (tc->src_op) {
	case BR_SRC_CMP_RR:  allow |= JIT_ST_ALLOW(ACT_CMP_RR); break;
	case BR_SRC_CMP_RI:  allow |= JIT_ST_ALLOW(ACT_CMP_RI); break;
	case BR_SRC_TEST_RR: allow |= JIT_ST_ALLOW(ACT_TEST_RR); break;
	case BR_SRC_CMP_RM32: allow |= JIT_ST_ALLOW(ACT_CMP_RM32); break;
	case BR_SRC_CMP_MR32: allow |= JIT_ST_ALLOW(ACT_CMP_MR32); break;
	case BR_SRC_TEST_MR32: allow |= JIT_ST_ALLOW(ACT_TEST_MR32); break;
	}

	setup_branch_cpu(cpu, mem, tc);
	jit_cpu_snapshot(cpu, &baseline);

	jit_cpu_restore(cpu, &baseline);
	interp_steps = jit_cpu_step_interp(cpu, 2);
	jit_cpu_snapshot(cpu, &interp_result);

	jit_cpu_invalidate_cache(cpu);
	jit_cpu_restore(cpu, &baseline);
	jit_selftest_set_allowed_actions(allow);
	jit_steps = jit_cpu_step_jit(cpu, 2);
	jit_selftest_clear_allowed_actions();
	jit_cpu_snapshot(cpu, &jit_result);

	if (interp_steps != 2) {
		esp_rom_printf("[jit_selftest] FAIL %s: interpreter steps %d != 2\n",
			       tc->name, interp_steps);
		return false;
	}
	if (jit_steps != 2) {
		esp_rom_printf("[jit_selftest] FAIL %s: JIT steps %d != 2\n",
			       tc->name, jit_steps);
		return false;
	}
	if (!compare_snapshots(tc->name, &interp_result, &jit_result))
		return false;

	esp_rom_printf("[jit_selftest] PASS %s (interp=%d jit=%d taken=%d)\n",
		       tc->name, interp_steps, jit_steps, (int)tc->expect_taken);
	return true;
}

static bool run_cache_metadata_cases(CPUI386 *cpu, uint8_t *mem)
{
	const uint32_t addr1 = ST_CODE_BASE;
	const uint32_t addr2 = ST_CODE_BASE + JIT_CACHE_ENTRIES;
	uint32_t slot1 = (addr1 * 2654435761u) % JIT_CACHE_ENTRIES;
	uint32_t slot2 = (addr2 * 2654435761u) % JIT_CACHE_ENTRIES;
	uint32_t epoch_before;

	memset(mem + addr1, 0xcc, 64);
	memset(mem + addr2, 0xcc, 64);
	mem[addr1 + 0] = 0xB8; mem[addr1 + 1] = 0x11; mem[addr1 + 2] = 0x11;
	mem[addr1 + 3] = 0x11; mem[addr1 + 4] = 0x11;
	mem[addr2 + 0] = 0xB8; mem[addr2 + 1] = 0x22; mem[addr2 + 2] = 0x22;
	mem[addr2 + 3] = 0x22; mem[addr2 + 4] = 0x22;

	if (slot1 != slot2) {
		esp_rom_printf("[jit_selftest] FAIL CACHE_CONFLICT: slots %u/%u differ\n",
			       (unsigned)slot1, (unsigned)slot2);
		return false;
	}

	jit_cpu_invalidate_cache(cpu);
	cpui386_reset_pm(cpu, addr1);
	jit_cpu_prepare_exec(cpu, addr1);
	if (jit_cpu_step_jit(cpu, 1) != 1 || jit_cpu_get_gpr(cpu, 0) != 0x11111111u) {
		esp_rom_printf("[jit_selftest] FAIL CACHE_CONFLICT: first block result bad\n");
		return false;
	}

	cpui386_set_gpr(cpu, 0, 0);
	jit_cpu_prepare_exec(cpu, addr2);
	if (jit_cpu_step_jit(cpu, 1) != 1 || jit_cpu_get_gpr(cpu, 0) != 0x22222222u) {
		esp_rom_printf("[jit_selftest] FAIL CACHE_CONFLICT: second block result bad\n");
		return false;
	}
	esp_rom_printf("[jit_selftest] PASS CACHE_CONFLICT (slot=%u)\n", (unsigned)slot1);

	epoch_before = jit_selftest_get_pool_epoch(cpu);
	jit_selftest_force_pool_used(cpu, JIT_POOL_SIZE - 4u);
	cpui386_set_gpr(cpu, 0, 0);
	jit_cpu_prepare_exec(cpu, addr1);
	if (jit_cpu_step_jit(cpu, 1) != 1 || jit_cpu_get_gpr(cpu, 0) != 0x11111111u) {
		esp_rom_printf("[jit_selftest] FAIL CACHE_POOL_FULL: execution after flush failed\n");
		return false;
	}
	if (jit_selftest_get_pool_epoch(cpu) == epoch_before) {
		esp_rom_printf("[jit_selftest] FAIL CACHE_POOL_FULL: epoch did not advance\n");
		return false;
	}
	esp_rom_printf("[jit_selftest] PASS CACHE_POOL_FULL (epoch %u->%u)\n",
		       (unsigned)epoch_before, (unsigned)jit_selftest_get_pool_epoch(cpu));
	return true;
}

static bool run_state_invalidation_cases(CPUI386 *cpu, uint8_t *mem)
{
	uint32_t epoch_before;
	uint32_t epoch_after;

	memset(mem + ST_CODE_BASE, 0xcc, 64);
	memcpy(mem + ST_CODE_BASE, code_mov_eax, sizeof(code_mov_eax));

	jit_cpu_invalidate_cache(cpu);
	cpui386_reset_pm(cpu, ST_CODE_BASE);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
	if (jit_cpu_step_jit(cpu, 1) != 1 || jit_cpu_get_gpr(cpu, 0) != 0x12345678u) {
		esp_rom_printf("[jit_selftest] FAIL STATE_CR3_INVALIDATE: warmup JIT block failed\n");
		return false;
	}

	epoch_before = jit_selftest_get_pool_epoch(cpu);
	memset(mem + ST_CODE_BASE, 0xcc, 64);
	memcpy(mem + ST_CODE_BASE, code_mov_cr3_eax, sizeof(code_mov_cr3_eax));
	cpui386_set_gpr(cpu, 0, 0x00003000u);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
	if (jit_cpu_step_interp(cpu, 1) != 1) {
		esp_rom_printf("[jit_selftest] FAIL STATE_CR3_INVALIDATE: interpreter MOV CR3 failed\n");
		return false;
	}

	epoch_after = jit_selftest_get_pool_epoch(cpu);
	if (epoch_after == epoch_before) {
		esp_rom_printf("[jit_selftest] FAIL STATE_CR3_INVALIDATE: epoch did not advance\n");
		return false;
	}
	esp_rom_printf("[jit_selftest] PASS STATE_CR3_INVALIDATE (epoch %u->%u)\n",
		       (unsigned)epoch_before, (unsigned)epoch_after);
	return true;
}

static bool run_smc_invalidation_cases(CPUI386 *cpu, uint8_t *mem)
{
	uint32_t invalid_before;
	uint32_t flush_before;

	memset(mem + ST_CODE_BASE, 0xcc, 64);
	memcpy(mem + ST_CODE_BASE, code_mov_eax, sizeof(code_mov_eax));

	jit_cpu_invalidate_cache(cpu);
	cpui386_reset_pm(cpu, ST_CODE_BASE);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
	if (jit_cpu_step_jit(cpu, 1) != 1 || jit_cpu_get_gpr(cpu, 0) != 0x12345678u) {
		esp_rom_printf("[jit_selftest] FAIL SMC_RANGE_OVERLAP: warmup JIT block failed\n");
		return false;
	}

	invalid_before = jit_selftest_get_invalidations(cpu);
	flush_before = jit_selftest_get_smc_flushes(cpu);
	if (!cpu_store8(cpu, ST_SEG_DS, ST_CODE_BASE + 16u, 0x90)) {
		esp_rom_printf("[jit_selftest] FAIL SMC_RANGE_NONOVERLAP: cpu_store8 failed\n");
		return false;
	}
	if (jit_selftest_get_smc_flushes(cpu) != flush_before + 1u ||
	    jit_selftest_get_invalidations(cpu) != invalid_before) {
		esp_rom_printf("[jit_selftest] FAIL SMC_RANGE_NONOVERLAP: flush/invalidation %u/%u -> %u/%u\n",
			       (unsigned)flush_before, (unsigned)invalid_before,
			       (unsigned)jit_selftest_get_smc_flushes(cpu),
			       (unsigned)jit_selftest_get_invalidations(cpu));
		return false;
	}
	esp_rom_printf("[jit_selftest] PASS SMC_RANGE_NONOVERLAP (invalidations=%u)\n",
		       (unsigned)invalid_before);

	flush_before = jit_selftest_get_smc_flushes(cpu);
	if (!cpu_store8(cpu, ST_SEG_DS, ST_CODE_BASE + 1u, 0x9a)) {
		esp_rom_printf("[jit_selftest] FAIL SMC_RANGE_OVERLAP: cpu_store8 failed\n");
		return false;
	}
	if (jit_selftest_get_smc_flushes(cpu) != flush_before + 1u ||
	    jit_selftest_get_invalidations(cpu) != invalid_before + 1u) {
		esp_rom_printf("[jit_selftest] FAIL SMC_RANGE_OVERLAP: flush/invalidation %u/%u -> %u/%u\n",
			       (unsigned)flush_before, (unsigned)invalid_before,
			       (unsigned)jit_selftest_get_smc_flushes(cpu),
			       (unsigned)jit_selftest_get_invalidations(cpu));
		return false;
	}

	cpui386_set_gpr(cpu, 0, 0);
	jit_cpu_prepare_exec(cpu, ST_CODE_BASE);
	if (jit_cpu_step_jit(cpu, 1) != 1 || jit_cpu_get_gpr(cpu, 0) != 0x1234569au) {
		esp_rom_printf("[jit_selftest] FAIL SMC_RANGE_OVERLAP: modified block did not retranslate\n");
		return false;
	}
	esp_rom_printf("[jit_selftest] PASS SMC_RANGE_OVERLAP (invalidations %u->%u)\n",
		       (unsigned)invalid_before,
		       (unsigned)jit_selftest_get_invalidations(cpu));
	return true;
}

int jit_selftest_run(void)
{
	static const SelftestCase cases[] = {
		{
			.name = "NOP",
			.code = code_nop,
			.code_len = sizeof(code_nop),
			.jit_allow = JIT_ST_ALLOW(ACT_NOP),
			.init = init_nop_state,
		},
		{
			.name = "MOV_EAX_imm32",
			.code = code_mov_eax,
			.code_len = sizeof(code_mov_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RI),
			.init = init_mov_eax_state,
		},
		{
			.name = "MOV_EBX_imm32_neg",
			.code = code_mov_ebx_neg,
			.code_len = sizeof(code_mov_ebx_neg),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RI),
			.init = init_mov_ebx_state,
		},
		{
			.name = "MOV_EAX_EBX",
			.code = code_mov_eax_ebx,
			.code_len = sizeof(code_mov_eax_ebx),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RR),
			.init = init_mov_rr_state,
		},
		{
			.name = "MOV_EDI_EAX",
			.code = code_mov_edi_eax,
			.code_len = sizeof(code_mov_edi_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RR),
			.init = init_mov_rr_state,
		},
		{
			.name = "NOT_EAX_flags_preserve",
			.code = code_not_eax,
			.code_len = sizeof(code_not_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_NOT_R),
			.init = init_not_state,
		},
		{
			.name = "NEG_EAX_zero",
			.code = code_neg_eax,
			.code_len = sizeof(code_neg_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_NEG_R),
			.init = init_neg_zero_state,
		},
		{
			.name = "NEG_EAX_min",
			.code = code_neg_eax,
			.code_len = sizeof(code_neg_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_NEG_R),
			.init = init_neg_min_state,
		},
		{
			.name = "INC_EAX_cf_set_overflow",
			.code = code_inc_eax,
			.code_len = sizeof(code_inc_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_INC_R),
			.init = init_inc_cf_set_state,
		},
		{
			.name = "INC_EAX_cf_clear_zero",
			.code = code_inc_eax,
			.code_len = sizeof(code_inc_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_INC_R),
			.init = init_inc_cf_clear_state,
		},
		{
			.name = "DEC_EAX_cf_set_overflow",
			.code = code_dec_eax,
			.code_len = sizeof(code_dec_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_DEC_R),
			.init = init_dec_cf_set_state,
		},
		{
			.name = "DEC_EAX_cf_clear_zero",
			.code = code_dec_eax,
			.code_len = sizeof(code_dec_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_DEC_R),
			.init = init_dec_cf_clear_state,
		},
		{
			.name = "XCHG_EAX_EBX",
			.code = code_xchg_eax_ebx,
			.code_len = sizeof(code_xchg_eax_ebx),
			.jit_allow = JIT_ST_ALLOW(ACT_XCHG_EAX_R),
			.init = init_mov_rr_state,
		},
		{
			.name = "XCHG_EAX_EDI",
			.code = code_xchg_eax_edi,
			.code_len = sizeof(code_xchg_eax_edi),
			.jit_allow = JIT_ST_ALLOW(ACT_XCHG_EAX_R),
			.init = init_mov_rr_state,
		},
		{
			.name = "MOV_EAX_mem_EBX_disp8",
			.code = code_mov_eax_mem_ebx_disp8,
			.code_len = sizeof(code_mov_eax_mem_ebx_disp8),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_mem_EBX_disp8_EAX",
			.code = code_mov_mem_ebx_disp8_eax,
			.code_len = sizeof(code_mov_mem_ebx_disp8_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_EAX_mem_ESP_disp8",
			.code = code_mov_eax_mem_esp_disp8,
			.code_len = sizeof(code_mov_eax_mem_esp_disp8),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_mem_ESP_disp8_EAX",
			.code = code_mov_mem_esp_disp8_eax,
			.code_len = sizeof(code_mov_mem_esp_disp8_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_EAX_mem_disp32",
			.code = code_mov_eax_mem_disp32,
			.code_len = sizeof(code_mov_eax_mem_disp32),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_mem_disp32_EAX",
			.code = code_mov_mem_disp32_eax,
			.code_len = sizeof(code_mov_mem_disp32_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_EAX_moffs32",
			.code = code_mov_eax_moffs32,
			.code_len = sizeof(code_mov_eax_moffs32),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_moffs32_EAX",
			.code = code_mov_moffs32_eax,
			.code_len = sizeof(code_mov_moffs32_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR32),
			.init = init_mem_state,
		},
		{
			.name = "MOV_AL_mem_EBX_disp8",
			.code = code_mov_al_mem_ebx_disp8,
			.code_len = sizeof(code_mov_al_mem_ebx_disp8),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM8),
			.init = init_mem_state,
		},
		{
			.name = "MOV_AH_mem_EBX_disp8",
			.code = code_mov_ah_mem_ebx_disp8,
			.code_len = sizeof(code_mov_ah_mem_ebx_disp8),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM8),
			.init = init_mem_state,
		},
		{
			.name = "MOV_mem_EBX_disp8_CL",
			.code = code_mov_mem_ebx_disp8_cl,
			.code_len = sizeof(code_mov_mem_ebx_disp8_cl),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR8),
			.init = init_mem_state,
		},
		{
			.name = "MOV_mem_EBX_disp8_AH",
			.code = code_mov_mem_ebx_disp8_ah,
			.code_len = sizeof(code_mov_mem_ebx_disp8_ah),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR8),
			.init = init_mem_state,
		},
		{
			.name = "MOV_AX_mem_EBX_disp8",
			.code = code_mov_ax_mem_ebx_disp8,
			.code_len = sizeof(code_mov_ax_mem_ebx_disp8),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM16),
			.init = init_mem_state,
		},
		{
			.name = "MOV_mem_EBX_disp8_AX",
			.code = code_mov_mem_ebx_disp8_ax,
			.code_len = sizeof(code_mov_mem_ebx_disp8_ax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR16),
			.init = init_mem_state,
		},
		{
			.name = "MOV_EAX_mem_SIB_scale4",
			.code = code_mov_eax_mem_sib_scale4,
			.code_len = sizeof(code_mov_eax_mem_sib_scale4),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_RM32),
			.init = init_mem_sib_state,
		},
		{
			.name = "MOV_mem_SIB_scale4_EAX",
			.code = code_mov_mem_sib_scale4_eax,
			.code_len = sizeof(code_mov_mem_sib_scale4_eax),
			.jit_allow = JIT_ST_ALLOW(ACT_MOV_MR32),
			.init = init_mem_sib_state,
		},
	};
	static const SelftestCase mixed_nop_mov = {
		.name = "MIXED_NOP_THEN_MOV_EAX",
		.code = code_nop_then_mov_eax,
		.code_len = sizeof(code_nop_then_mov_eax),
		.jit_allow = JIT_ST_ALLOW(ACT_NOP),
		.init = init_mov_eax_state,
	};
	static const SelftestCase mixed_mov_nop_mov = {
		.name = "MIXED_MOV_EAX_NOP_MOV_EBX",
		.code = code_mov_eax_nop_mov_ebx,
		.code_len = sizeof(code_mov_eax_nop_mov_ebx),
		.jit_allow = JIT_ST_ALLOW(ACT_MOV_RI),
		.init = init_mov_ebx_state,
	};
	static const SelftestCase block_mov_chain = {
		.name = "BLOCK_MOV_CHAIN",
		.code = code_block_mov_chain,
		.code_len = sizeof(code_block_mov_chain),
		.jit_allow = JIT_ST_ALLOW(ACT_MOV_RI) |
			     JIT_ST_ALLOW(ACT_MOV_RR) |
			     JIT_ST_ALLOW(ACT_NOP),
		.init = init_mov_rr_state,
	};
	static const SelftestCase block_add_add_cover_flags = {
		.name = "BLOCK_ADD_ADD_COVER_FLAGS",
		.code = code_block_add_add_cover_flags,
		.code_len = sizeof(code_block_add_add_cover_flags),
		.jit_allow = JIT_ST_ALLOW(ACT_ALU_RR),
		.init = init_dead_flags_add_state,
	};
	static const SelftestCase block_jmp_rel8 = {
		.name = "BLOCK_JMP_REL8",
		.code = code_jmp_rel8,
		.code_len = sizeof(code_jmp_rel8),
		.jit_allow = JIT_ST_ALLOW(ACT_JMP),
		.init = init_mov_ebx_state,
	};
	static const SelftestCase block_jmp_rel32 = {
		.name = "BLOCK_JMP_REL32",
		.code = code_jmp_rel32,
		.code_len = sizeof(code_jmp_rel32),
		.jit_allow = JIT_ST_ALLOW(ACT_JMP),
		.init = init_mov_ebx_state,
	};
	static const AluSelftestCase alu_cases[] = {
		{ "ADD_RR_zero",      ALU_ST_ADD_RR, 0x00000000u, 0x00000000u, 0 },
		{ "ADD_RR_carry",     ALU_ST_ADD_RR, 0xffffffffu, 0x00000001u, 0 },
		{ "ADD_RR_overflow",  ALU_ST_ADD_RR, 0x7fffffffu, 0x00000001u, 0 },
		{ "ADD_RR_aux",       ALU_ST_ADD_RR, 0x0000000fu, 0x00000001u, 0 },
		{ "SUB_RR_zero",      ALU_ST_SUB_RR, 0x12345678u, 0x12345678u, 0 },
		{ "SUB_RR_borrow",    ALU_ST_SUB_RR, 0x00000000u, 0x00000001u, 0 },
		{ "SUB_RR_overflow",  ALU_ST_SUB_RR, 0x80000000u, 0x00000001u, 0 },
		{ "SUB_RR_aux",       ALU_ST_SUB_RR, 0x00000010u, 0x00000001u, 0 },
		{ "ADD_RI_carry",     ALU_ST_ADD_RI, 0xffffffffu, 0, 1 },
		{ "ADD_RI_overflow",  ALU_ST_ADD_RI, 0x7fffffffu, 0, 1 },
		{ "SUB_RI_borrow",    ALU_ST_SUB_RI, 0x00000000u, 0, 1 },
		{ "SUB_RI_overflow",  ALU_ST_SUB_RI, 0x80000000u, 0, 1 },
		{ "ADD_RI_negative",  ALU_ST_ADD_RI, 0x00000000u, 0, -1 },
		{ "SUB_RI_negative",  ALU_ST_SUB_RI, 0x00000000u, 0, -1 },
		{ "AND_RR_zero",      ALU_ST_AND_RR, 0x0f0f0000u, 0x0000f0f0u, 0 },
		{ "AND_RR_sign",      ALU_ST_AND_RR, 0x80000001u, 0xffffffffu, 0 },
		{ "OR_RR_sign",       ALU_ST_OR_RR,  0x00000000u, 0x80000000u, 0 },
		{ "OR_RR_parity",     ALU_ST_OR_RR,  0x00000001u, 0x00000002u, 0 },
		{ "XOR_RR_zero",      ALU_ST_XOR_RR, 0xa5a5a5a5u, 0xa5a5a5a5u, 0 },
		{ "XOR_RR_sign",      ALU_ST_XOR_RR, 0x7fffffffu, 0xffffffffu, 0 },
		{ "AND_RI_zero",      ALU_ST_AND_RI, 0xf0f0f0f0u, 0, 0x0f0f0f0f },
		{ "AND_RI_sign",      ALU_ST_AND_RI, 0x80000001u, 0, -1 },
		{ "OR_RI_sign",       ALU_ST_OR_RI,  0x00000000u, 0, (int32_t)0x80000000u },
		{ "OR_RI_parity",     ALU_ST_OR_RI,  0x00000001u, 0, 0x00000002 },
		{ "XOR_RI_zero",      ALU_ST_XOR_RI, 0x12345678u, 0, 0x12345678 },
		{ "XOR_RI_sign",      ALU_ST_XOR_RI, 0x7fffffffu, 0, -1 },
	};
	static const ShiftSelftestCase shift_cases[] = {
		{ "SHL_RI_count0", SHIFT_ST_SHL, 0x12345678u, 0 },
		{ "SHL_RI_count1", SHIFT_ST_SHL, 0x40000001u, 1 },
		{ "SHL_RI_count4", SHIFT_ST_SHL, 0x0800000fu, 4 },
		{ "SHL_RI_count31", SHIFT_ST_SHL, 0x00000003u, 31 },
		{ "SHR_RI_count0", SHIFT_ST_SHR, 0x87654321u, 0 },
		{ "SHR_RI_count1", SHIFT_ST_SHR, 0x80000001u, 1 },
		{ "SHR_RI_count4", SHIFT_ST_SHR, 0xf000000fu, 4 },
		{ "SHR_RI_count31", SHIFT_ST_SHR, 0x80000000u, 31 },
		{ "SAR_RI_count0", SHIFT_ST_SAR, 0x87654321u, 0 },
		{ "SAR_RI_count1", SHIFT_ST_SAR, 0x80000001u, 1 },
		{ "SAR_RI_count4", SHIFT_ST_SAR, 0xf000000fu, 4 },
		{ "SAR_RI_count31", SHIFT_ST_SAR, 0x80000000u, 31 },
	};
	static const BranchSelftestCase branch_cases[] = {
		{ "CMP_RR_JZ_taken",    BR_SRC_CMP_RR,  4, 5,          5,          0, true },
		{ "CMP_RR_JZ_not",      BR_SRC_CMP_RR,  4, 5,          6,          0, false },
		{ "CMP_RR_JNZ_taken",   BR_SRC_CMP_RR,  5, 5,          6,          0, true },
		{ "CMP_RR_JNZ_not",     BR_SRC_CMP_RR,  5, 5,          5,          0, false },
		{ "CMP_RR_JB_taken",    BR_SRC_CMP_RR,  2, 1,          2,          0, true },
		{ "CMP_RR_JB_not",      BR_SRC_CMP_RR,  2, 2,          1,          0, false },
		{ "CMP_RR_JNB_taken",   BR_SRC_CMP_RR,  3, 2,          1,          0, true },
		{ "CMP_RR_JNB_not",     BR_SRC_CMP_RR,  3, 1,          2,          0, false },
		{ "CMP_RR_JBE_taken",   BR_SRC_CMP_RR,  6, 1,          2,          0, true },
		{ "CMP_RR_JBE_not",     BR_SRC_CMP_RR,  6, 3,          2,          0, false },
		{ "CMP_RR_JNBE_taken",  BR_SRC_CMP_RR,  7, 3,          2,          0, true },
		{ "CMP_RR_JNBE_not",    BR_SRC_CMP_RR,  7, 1,          2,          0, false },
		{ "CMP_RR_JL_taken",    BR_SRC_CMP_RR, 12, 0xffffffffu,1,          0, true },
		{ "CMP_RR_JL_not",      BR_SRC_CMP_RR, 12, 1,          0xffffffffu,0, false },
		{ "CMP_RR_JNL_taken",   BR_SRC_CMP_RR, 13, 1,          0xffffffffu,0, true },
		{ "CMP_RR_JNL_not",     BR_SRC_CMP_RR, 13, 0xffffffffu,1,          0, false },
		{ "CMP_RR_JLE_taken",   BR_SRC_CMP_RR, 14, 0xffffffffu,1,          0, true },
		{ "CMP_RR_JLE_not",     BR_SRC_CMP_RR, 14, 1,          0xffffffffu,0, false },
		{ "CMP_RR_JNLE_taken",  BR_SRC_CMP_RR, 15, 1,          0xffffffffu,0, true },
		{ "CMP_RR_JNLE_not",    BR_SRC_CMP_RR, 15, 0xffffffffu,1,          0, false },
		{ "CMP_RI_JS_taken",    BR_SRC_CMP_RI,  8, 1,          0,          2, true },
		{ "CMP_RI_JS_not",      BR_SRC_CMP_RI,  8, 2,          0,          1, false },
		{ "CMP_RI_JNS_taken",   BR_SRC_CMP_RI,  9, 2,          0,          1, true },
		{ "CMP_RI_JNS_not",     BR_SRC_CMP_RI,  9, 1,          0,          2, false },
		{ "TEST_RR_JZ_taken",   BR_SRC_TEST_RR, 4, 0x0f0f0000u,0x0000f0f0u,0, true },
		{ "TEST_RR_JZ_not",     BR_SRC_TEST_RR, 4, 0x0f0f0000u,0x000f0000u,0, false },
		{ "TEST_RR_JNZ_taken",  BR_SRC_TEST_RR, 5, 0x0f0f0000u,0x000f0000u,0, true },
		{ "TEST_RR_JNZ_not",    BR_SRC_TEST_RR, 5, 0x0f0f0000u,0x0000f0f0u,0, false },
		{ "TEST_RR_JS_taken",   BR_SRC_TEST_RR, 8, 0x80000000u,0xffffffffu,0, true },
		{ "TEST_RR_JS_not",     BR_SRC_TEST_RR, 8, 0x7fffffffu,0xffffffffu,0, false },
		{ "TEST_RR_JNS_taken",  BR_SRC_TEST_RR, 9, 0x7fffffffu,0xffffffffu,0, true },
		{ "TEST_RR_JNS_not",    BR_SRC_TEST_RR, 9, 0x80000000u,0xffffffffu,0, false },
		{ "CMP_RM32_JZ_taken",  BR_SRC_CMP_RM32, 4, 0x11223344u,0,          0, true },
		{ "CMP_RM32_JZ_not",    BR_SRC_CMP_RM32, 4, 0x11223345u,0,          0, false },
		{ "CMP_MR32_JB_taken",  BR_SRC_CMP_MR32, 2, 0x22334455u,0,          0, true },
		{ "CMP_MR32_JB_not",    BR_SRC_CMP_MR32, 2, 0x00000001u,0,          0, false },
		{ "TEST_MR32_JZ_taken", BR_SRC_TEST_MR32,4, 0x00000002u,0,          0, true },
		{ "TEST_MR32_JZ_not",   BR_SRC_TEST_MR32,4, 0x00000004u,0,          0, false },
	};
	FlashReadProbe flash_before;
	FlashReadProbe flash_after;
	bool flash_before_ok;
	const unsigned case_count = (unsigned)(sizeof(cases) / sizeof(cases[0]));
	const unsigned mixed_count = 2u;
	const unsigned block_count = 4u;
	const unsigned alu_count = (unsigned)(sizeof(alu_cases) / sizeof(alu_cases[0]));
	const unsigned shift_count = (unsigned)(sizeof(shift_cases) / sizeof(shift_cases[0]));
	const unsigned branch_count = (unsigned)(sizeof(branch_cases) / sizeof(branch_cases[0]));
	const unsigned cache_count = 2u;
	const unsigned state_count = 1u;
	const unsigned smc_count = 2u;
	const unsigned link_count = 1u;
	const unsigned check_count = case_count + mixed_count + block_count + alu_count + shift_count + branch_count + cache_count + state_count + smc_count + link_count + 1u;
	uint8_t *mem;
	CPUI386 *cpu;
	int failures = 0;

	if (!jit_pool_ready()) {
		esp_rom_printf("[jit_selftest] FAIL: JIT code pool unavailable\n");
		return 1;
	}

	esp_rom_printf("[jit_selftest] start (%u cases + %u mixed + %u block + %u alu + %u shift + %u branch + %u cache + %u state + %u smc + %u link + flash probe)\n",
		       case_count, mixed_count, block_count, alu_count, shift_count, branch_count, cache_count, state_count, smc_count, link_count);

	flash_before_ok = read_flash_probe("before_jit", &flash_before);
	if (!flash_before_ok)
		failures++;

	mem = (uint8_t *)heap_caps_malloc(ST_PHYS_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (!mem) {
		esp_rom_printf("[jit_selftest] FAIL: need %u bytes internal RAM\n",
			(unsigned)ST_PHYS_SIZE);
		return 1;
	}
	memset(mem, 0xcc, ST_PHYS_SIZE);

	cpu = cpui386_new_internal(5, (char *)mem, ST_PHYS_SIZE);
	if (!cpu) {
		esp_rom_printf("[jit_selftest] FAIL: cpui386_new_internal\n");
		free(mem);
		return 1;
	}

	for (size_t i = 0; i < case_count; i++) {
		if (cases[i].jit_allow == JIT_ST_ALLOW(ACT_MOV_RM32) ||
		    cases[i].jit_allow == JIT_ST_ALLOW(ACT_MOV_MR32) ||
		    cases[i].jit_allow == JIT_ST_ALLOW(ACT_MOV_RM8) ||
		    cases[i].jit_allow == JIT_ST_ALLOW(ACT_MOV_MR8) ||
		    cases[i].jit_allow == JIT_ST_ALLOW(ACT_MOV_RM16) ||
		    cases[i].jit_allow == JIT_ST_ALLOW(ACT_MOV_MR16)) {
			uint32_t probe = 0x1820;
			if (strstr(cases[i].name, "SIB") != NULL)
				probe = strstr(cases[i].name, "_mem_SIB_") != NULL ? 0x1834 : 0x1830;
			else if (strstr(cases[i].name, "MOV_mem_") != NULL)
				probe = 0x1824;
			if (!run_memory_mov_case(cpu, mem, &cases[i], probe))
				failures++;
		} else if (!run_case(cpu, mem, &cases[i])) {
			failures++;
		}
	}
	if (!run_mixed_case(cpu, mem, &mixed_nop_mov, 2, 1))
		failures++;
	if (!run_mixed_case(cpu, mem, &mixed_mov_nop_mov, 3, 2))
		failures++;
	if (!run_block_case(cpu, mem, &block_mov_chain, 3))
		failures++;
	if (!run_block_case(cpu, mem, &block_add_add_cover_flags, 2))
		failures++;
	if (!run_mixed_case(cpu, mem, &block_jmp_rel8, 2, 1))
		failures++;
	if (!run_mixed_case(cpu, mem, &block_jmp_rel32, 2, 1))
		failures++;
	for (size_t i = 0; i < alu_count; i++) {
		if (!run_alu_case(cpu, mem, &alu_cases[i]))
			failures++;
	}
	for (size_t i = 0; i < shift_count; i++) {
		if (!run_shift_case(cpu, mem, &shift_cases[i]))
			failures++;
	}
	for (size_t i = 0; i < branch_count; i++) {
		if (!run_branch_case(cpu, mem, &branch_cases[i]))
			failures++;
	}
	if (!run_cache_metadata_cases(cpu, mem))
		failures += (int)cache_count;
	if (!run_state_invalidation_cases(cpu, mem))
		failures += (int)state_count;
	if (!run_smc_invalidation_cases(cpu, mem))
		failures += (int)smc_count;
	if (!run_linked_exit_case(cpu, mem))
		failures += (int)link_count;

	if (flash_before_ok) {
		if (!read_flash_probe("after_jit", &flash_after) ||
		    !compare_flash_probe(&flash_before, &flash_after)) {
			failures++;
		}
	}

	jit_cpu_invalidate_cache(cpu);
	cpui386_delete_internal(cpu);
	free(mem);

	esp_rom_printf("[jit_selftest] summary: %d/%u PASS\n",
		(int)(check_count - (unsigned)failures), check_count);
	return failures;
}

#else

int jit_selftest_run(void)
{
	return 0;
}

#endif /* BUILD_ESP32 && TINY386_ENABLE_JIT */
