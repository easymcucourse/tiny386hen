# JIT selftest / WDT debugging helpers (source from GDB or launch.json)
set pagination off
set print pretty on

# Task 0.3 entry points
break jit_selftest_run
break run_case
break jit_cpu_step_jit
break jit_try_execute
break jit_translate
break jit_commit_code

# Useful when investigating PSRAM pool / cache sync issues
# break jit_acquire_pool

commands 1
  silent
  printf "[gdb] jit_selftest_run\n"
  continue
end

commands 5
  silent
  printf "[gdb] jit_translate paddr=%#x\n", paddr
  continue
end

commands 6
  silent
  printf "[gdb] jit_commit_code dst=%p len=%u\n", dst, len
  continue
end
