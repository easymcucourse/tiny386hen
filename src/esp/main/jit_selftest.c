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

/* Mirror ActionType ordinals in jit_lx7.c (ACT_NONE=0). */
#define ACT_NOP     1
#define ACT_MOV_RI  3

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

static const uint8_t code_nop[] = { 0x90 };
static const uint8_t code_mov_eax[] = { 0xB8, 0x78, 0x56, 0x34, 0x12 };
static const uint8_t code_mov_ebx_neg[] = { 0xBB, 0x00, 0x00, 0x00, 0x80 };

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
	return true;
}

static void setup_cpu(CPUI386 *cpu, uint8_t *mem, const SelftestCase *tc)
{
	cpui386_reset_pm(cpu, ST_CODE_BASE);
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
	};
	FlashReadProbe flash_before;
	FlashReadProbe flash_after;
	bool flash_before_ok;
	const unsigned case_count = (unsigned)(sizeof(cases) / sizeof(cases[0]));
	const unsigned check_count = case_count + 1u; /* x86 cases + flash before/after */
	uint8_t *mem;
	CPUI386 *cpu;
	int failures = 0;

	if (!jit_pool_ready()) {
		esp_rom_printf("[jit_selftest] FAIL: JIT code pool unavailable\n");
		return 1;
	}

	esp_rom_printf("[jit_selftest] start (%u cases + flash probe)\n", case_count);

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
		if (!run_case(cpu, mem, &cases[i]))
			failures++;
	}

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
