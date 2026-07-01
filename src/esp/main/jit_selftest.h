#ifndef JIT_SELFTEST_H
#define JIT_SELFTEST_H

#include <stdint.h>
#include "i386.h"

/*
 * Snapshot of guest CPU state used by jit_selftest to compare interpreter
 * vs JIT paths.  EFLAGS are stored materialized (cpu_getflags()).
 */
typedef struct JITCpuSnapshot {
	uint32_t gpr[8];
	uint32_t ip;
	uint32_t next_ip;
	uint32_t cycle;
	uword flags;
} JITCpuSnapshot;

int jit_selftest_run(void);

#if defined(BUILD_ESP32) && defined(TINY386_ENABLE_JIT)

CPUI386 *cpui386_new_internal(int gen, char *phys_mem, long phys_mem_size);
void cpui386_delete_internal(CPUI386 *cpu);

void jit_cpu_snapshot(CPUI386 *cpu, JITCpuSnapshot *snap);
void jit_cpu_restore(CPUI386 *cpu, const JITCpuSnapshot *snap);
uint32_t jit_cpu_get_gpr(CPUI386 *cpu, int reg);
int jit_cpu_step_interp(CPUI386 *cpu, int stepcount);
int jit_cpu_step_jit(CPUI386 *cpu, int max_insns);
void jit_cpu_prepare_exec(CPUI386 *cpu, uint32_t ip);
void jit_cpu_invalidate_cache(CPUI386 *cpu);

#endif /* BUILD_ESP32 && TINY386_ENABLE_JIT */

#endif /* JIT_SELFTEST_H */
