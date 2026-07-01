# tiny386hen x86 JIT 开发计划（当前后端：LX7 / ESP32-S3）

> 目标：把当前 x86 → Xtensa LX7 的分层 JIT，从"只有 NOP 能跑"
> 推进到"可稳定加速 SeaBIOS + DOS 启动与常见用户态代码"。
> 通用接口已开始抽离到 `jit_x86.h`，但当前唯一可运行/可验收后端仍是 ESP32-S3 LX7。
>
> 执行方式：**用 AI 编程代理按 task 实施，每天 1 个 task ≈ 2 小时**。
> 每个 task 都有明确的范围、改动文件、验收标准，单个 task 自成闭环、可独立提交。
>
> 验证方式：**直接在开发板上测试**，不依赖 QEMU。串口抓取 selftest 输出判定 PASS/FAIL。

---

## 0. 当前状态盘点（必读 — 2026-06-30 晚更新）

### 已有资产

- `jit_x86.h`：通用 x86 JIT 接口与元数据（块缓存、NOJIT/Exit/Bail、i386/i387 命令声明），供 LX7 与未来 RISC-V 后端共用。
- `jit_lx7.c`：完整的 LX7 二进制发射器（RRR/RRI8/RI/CALL 格式）、当前 bring-up 用 x86 扫描译码器、惰性标志写出、CMP/TEST+Jcc 融合、`movi32` 字面量池。
- `jit_lx7.h`：LX7 后端约束与寄存器映射；通用 JIT 状态已迁移到 `jit_x86.h`。
- `i386.c`：集成钩子已就位——步进循环里调用 `jit_try_execute()`，`cpui386_reset()` 里 `jit_init()`，惰性标志 `cc` 偏移 `_Static_assert` 锚定。
- `jit_selftest.c/.h`：板上差分自检框架（JIT vs 解释器比对 8×GPR + EFLAGS + next_ip）。
- **构建开关已接入**：`TINY386_ENABLE_JIT=1`, `TINY386_JIT_LEVEL=1`。
- **XIP-from-PSRAM 已开启**：`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`（flash 操作不再关 cache）。
- **✅ JIT 池已改用 PSRAM 方案**：写入走 DBUS data vaddr，执行走 IBUS exec vaddr，cache panic 已消除。

### Task 0.3 进展（2026-06-30）

#### DIRAM/IRAM 方案结论：不可行，直接放弃

尝试了多种 DIRAM-IRAM 写入方案，均触发 `Cache disabled but cached memory region accessed`：

| 方案 | 结果 |
|------|------|
| 直接写 IRAM 地址 (`volatile uint32_t*`) | CACHEERR panic |
| DRAM alias 写入 (`esp_ptr_diram_iram_to_dram`) | CACHEERR panic |
| jit_copy_to_iram 放 IRAM (`IRAM_ATTR`) | CACHEERR panic |
| jit_copy_to_iram 不放 IRAM（从 PSRAM XIP 执行） | CACHEERR panic |
| XIP-from-PSRAM + 关中断 + 单核 | CACHEERR panic |

根因：`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y` 启用了 PMS（权限管理系统），
将 IRAM 区域设为 **只读+执行**（`MEMPROT_OP_READ | MEMPROT_OP_EXEC`），
禁止任何写入。通过 DRAM alias 写入同样触发 cache error interrupt。

工程决策：**不再维护 IRAM/DIRAM JIT 执行路径，不再做 fallback**。ESP32-S3 后端只支持
已经在 COM20 跑通的 PSRAM 双映射方案。若 PSRAM 池创建失败，JIT 直接禁用，而不是退回
IRAM/DIRAM 尝试执行。

#### ✅ PSRAM 执行方案：已验证可行

改为从 PSRAM 分配 JIT 池并执行，**cache panic 彻底消除，代码成功从 PSRAM 取指执行**。

串口确认：
```
[jit] pool write=0x3c050000 exec=0x42800000 psram=1
[jit_selftest] start (3 cases)
[jit] translate: raw=0x3c050000 write=0x3c050000 exec=0x42800000 tmp=0x3fcbf820 psram=1
```

PC 进入 `0x42800006`（PSRAM exec 区域），说明 I-cache 成功从 PSRAM 取指。

#### ✅ Task 1.1 结果：JIT prologue/epilogue 已通过板上自检

调试起点：代码已能在 PSRAM 执行，但 JIT 生成的 prologue 在第 3 条指令处崩溃：
```
PC=0x42800006  EXCCAUSE=0x1c (LoadProhibited)  EXCVADDR=0x00001004
```

Task 1.1 审计结论：`CPUI386` GPR/`next_ip` 偏移正确；ESP-IDF 默认使用 Xtensa
windowed ABI，JIT 入口应保持 `ENTRY a1,32` + `RETW.N`，并由 C 函数指针正常调用。
真正根因是 Xtensa RRI8 宽指令编码布局错误，导致 `L32I/S32I/ADDI/branch`
发射出的字节不是工具链生成的格式（完整调试过程见下方 Task 1.1 调试记录）。

修复编码后，COM20 板上验证通过：
```
[jit] pool write=0x3c050000 exec=0x42800000 psram=1
[jit_selftest] start (3 cases)
[jit] translate: raw=0x3c050000 write=0x3c050000 exec=0x42800000 tmp=0x3fcbf820 psram=1
[jit] exec @0x42800000 cpu=0x3fccaf3c
[jit_selftest] PASS NOP (interp=1 jit=1)
[jit_selftest] PASS MOV_EAX_imm32 (interp=1 jit=1)
[jit_selftest] PASS MOV_EBX_imm32_neg (interp=1 jit=1)
[jit_selftest] summary: 3/3 PASS
[boot] selftest finished, halting
```

### 关键问题清单

1. ~~JIT 未接入构建~~ → ✅ 已解决
2. ~~零可观测性~~ → ✅ selftest 框架已就位
3. ~~jit_copy_to_iram CACHEERR panic~~ → ✅ 已解决（改用 PSRAM 池）
4. ~~JIT prologue LoadProhibited~~ → ✅ 已解决（RRI8/RI 编码修复，Task 1.1）
5. **几乎所有动作被关闭** → 从 Task 1.2/1.3 开始逐步放开并扩充 selftest

> **核心策略**：板上差分自检驱动，不用 QEMU，不盲目刷板。
> selftest 不通过，不进启动冒烟。

---

## 0.A  PSRAM JIT 池方案完整说明

### 架构概述

ESP32-S3 的 MMU 支持将同一块 PSRAM 物理页映射到**两个不同的虚拟地址**：
- **Data vaddr**（`0x3C0xxxxx`）：通过 D-cache / DBUS 访问，支持 8/16/32-bit 读写
- **Exec vaddr**（`0x428xxxxx`）：通过 I-cache / IBUS 访问，支持指令取指

利用 `ESP_MMU_MMAP_FLAG_PADDR_SHARED` 标志，对同一 PSRAM 物理页建立两个映射，
实现 **写入走 data 地址 + 执行走 exec 地址** 的 JIT 模式。

### 核心不变量

PSRAM JIT 池能稳定工作的前提是下面几条必须同时成立：

1. **同一物理页双映射**：`s_jit_pool_write` 与 `s_jit_pool_exec` 必须指向同一块
   PSRAM 物理页，只是虚拟地址和 bus 不同。
2. **写入只走 data vaddr**：所有 `memcpy`、字节 dump、校验和调试读取都只能访问
   `0x3C0xxxxx` 侧地址。
3. **执行只走 exec vaddr**：`block->host_code` 必须保存 `0x428xxxxx` 侧地址，函数指针
   调用只能跳到 exec vaddr。
4. **提交顺序固定**：先 D-cache writeback，再 I-cache invalidate，最后 `isync`。
5. **exec vaddr 不当作数据指针读**：`0x428xxxxx` 是 IBUS 取指地址。尝试用 C 指针读取
   `*(uint8_t *)0x42800000` 会触发 `LoadStoreError`，这不是 JIT 取指失败。
6. **没有 IRAM fallback**：ESP32-S3 JIT 后端只接受 PSRAM 双映射池。PSRAM 不可用时
   `jit_pool_ready()` 返回 false，selftest/运行时回退解释器。

对应到代码，`jit->pool` 和 `pool_write` 始终代表 data alias；`jit_pool_exec_for()`
只在提交完成后把 data alias 转换为 exec alias，填入 `block->host_code`。

### 池分配流程（`jit_acquire_pool()`）

```c
// 1. 从 PSRAM 分配 64KB 对齐的写入缓冲区
s_jit_pool_write = heap_caps_aligned_alloc(
    CONFIG_MMU_PAGE_SIZE,       // 64KB 对齐（ESP32-S3 MMU 页大小）
    JIT_POOL_SIZE,              // 64KB
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

// 2. 查询 data vaddr 对应的 PSRAM 物理地址
esp_paddr_t paddr;
mmu_target_t target;
esp_mmu_vaddr_to_paddr(s_jit_pool_write, &paddr, &target);

// 3. 为同一物理页创建 EXEC 映射（I-bus 侧虚拟地址）
void *exec = NULL;
esp_mmu_map(
    paddr,
    JIT_POOL_SIZE,
    MMU_TARGET_PSRAM0,
    MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT,
    ESP_MMU_MMAP_FLAG_PADDR_SHARED,  // 允许一物理多虚拟
    &exec);

s_jit_pool_exec = (uint8_t *)exec;  // 0x42800000
s_jit_pool_psram = true;
```

### 地址转换辅助函数

`jit_pool_exec_for(write_ptr)` 是整个双映射方案的边界函数：

```c
static uint8_t *jit_pool_exec_for(const uint8_t *write_ptr)
{
    if (s_jit_pool_psram && s_jit_pool_write && s_jit_pool_exec)
        return s_jit_pool_exec + (write_ptr - s_jit_pool_write);
    return NULL;
}
```

约束：
- 输入必须是 data/write 侧地址，不能传 exec 地址。
- PSRAM 模式下返回值只用于 I-cache invalidate 或函数指针执行。
- 如果 PSRAM 双映射未建立，返回 `NULL`。ESP32-S3 不再尝试 DRAM alias → IRAM alias。

`jit_init(&cpu->jit, NULL)` 在 ESP32-S3 上会忽略外部 `iram_pool` 参数，始终调用
`jit_acquire_pool()` 获取 PSRAM 池。保留参数只是为了不改公共接口和非 ESP32 构建。

### 代码写入与缓存同步（`jit_commit_code()`）

> 历史旧文档中的 `jit_copy_to_iram()` 应理解为“提交生成代码”。当前实现不包含
> IRAM/DIRAM 分支，只提交到 PSRAM write alias。

```c
// 1. 通过 data vaddr 写入（走 D-cache）
memcpy(dst, src, len);  // dst = pool_write 侧地址

// 2. Writeback D-cache → PSRAM（C2M 方向，data type）
esp_cache_msync(dst, len,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                ESP_CACHE_MSYNC_FLAG_UNALIGNED);

// 3. Invalidate I-cache（M2C 方向，instruction type，需 cache-line 对齐）
uint8_t *exec_ptr = jit_pool_exec_for(dst);
uint32_t line_mask = 31u;  // I-cache line = 32 bytes
uint8_t *inv_start = (uint8_t *)((uintptr_t)exec_ptr & ~(uintptr_t)line_mask);
size_t   inv_len   = ((exec_ptr + len - inv_start) + line_mask) & ~line_mask;
esp_cache_msync(inv_start, inv_len,
                ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                ESP_CACHE_MSYNC_FLAG_TYPE_INST);

// 4. Pipeline sync
__asm__ __volatile__ ("isync" ::: "memory");
```

同步顺序不能调换：
- 如果少了 D-cache writeback，PSRAM 物理内存可能还没有新代码，I-cache 重新取到旧内容。
- 如果少了 I-cache invalidate，CPU 可能继续执行同一 exec vaddr 上的旧 cache line。
- 如果少了 `isync`，流水线可能仍使用 invalidate 前已经预取的指令。

调试时如果要打印生成字节，必须打印 `pool_write` 侧内容：

```c
uint8_t *write_bytes = s_jit_pool_write + ((uint8_t *)block->host_code - s_jit_pool_exec);
esp_rom_printf("%02x %02x %02x\n", write_bytes[0], write_bytes[1], write_bytes[2]);
```

不能读取 `block->host_code[0]`。`block->host_code` 是 exec alias，作为数据读会在
ESP32-S3 上触发 `LoadStoreError`，典型异常为：

```
Guru Meditation Error: Core 0 panic'ed (LoadStoreError)
EXCCAUSE: 0x00000003
EXCVADDR: 0x42800000
```

### 地址映射示例

| 用途 | 虚拟地址 | 物理 | Bus |
|------|----------|------|-----|
| 写入（data） | `0x3C050000` | PSRAM page 0x50000 | DBUS / D-cache |
| 执行（exec） | `0x42800000` | PSRAM page 0x50000 | IBUS / I-cache |

### sdkconfig 关键配置

```ini
# PSRAM 基础
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y

# XIP-from-PSRAM（消除 flash 操作时的 cache-disable 窗口）
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y

# MMU 页大小（池分配对齐依赖此值）
CONFIG_MMU_PAGE_SIZE_64KB=y
CONFIG_MMU_PAGE_SIZE=0x10000

# Cache 行大小（I-cache invalidation 对齐依赖）
CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_32B=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE=32
CONFIG_ESP32S3_DATA_CACHE_LINE_32B=y
CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=32

# MEMPROT（阻止 IRAM 写入的根因，PSRAM 方案绕过此限制）
CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y
CONFIG_ESP_SYSTEM_MEMPROT_FEATURE_LOCK=y

# Cache writeback 支持（esp_cache_msync C2M 依赖）
# SOC_CACHE_WRITEBACK_SUPPORTED=1 (SoC 级，自动)
```

### CMakeLists.txt 依赖

```cmake
REQUIRES
    esp_mm    # esp_mmu_map, esp_mmu_vaddr_to_paddr, esp_cache_msync
```

### IDF API 依赖清单

| API | 头文件 | 用途 |
|-----|--------|------|
| `heap_caps_aligned_alloc` | `esp_heap_caps.h` | PSRAM 对齐分配 |
| `esp_mmu_vaddr_to_paddr` | `esp_mmu_map.h` | data vaddr → 物理地址 |
| `esp_mmu_map` | `esp_mmu_map.h` | 物理页 → exec vaddr 映射 |
| `esp_cache_msync` | `esp_cache.h` | D-cache writeback / I-cache invalidate |
| `ESP_MMU_MMAP_FLAG_PADDR_SHARED` | `esp_mmu_map.h` | 一物理多虚拟标志 |
| `MMU_TARGET_PSRAM0` | `hal/mmu_types.h` | PSRAM 目标标识 |

### esp_cache_msync 使用注意

| 方向 | 允许的 type | 允许 UNALIGNED? | 用途 |
|------|-------------|-----------------|------|
| C2M (writeback) | DATA only | ✅ 是 | D-cache → PSRAM |
| M2C (invalidate) | DATA 或 INST | ❌ 否（需 cache-line 对齐） | PSRAM → I-cache |

**不能**对 C2M 方向指定 `ESP_CACHE_MSYNC_FLAG_TYPE_INST`，否则返回错误。

本项目采用的规则：
- `C2M` 对 data alias 调用，带 `ESP_CACHE_MSYNC_FLAG_UNALIGNED`，长度使用真实 `host_len`。
- `M2C | TYPE_INST` 对 exec alias 调用，起止地址手动扩展到 32-byte cache line。
- 每次提交 block 都同步一次；池创建后也对整个 exec region 做一次预 invalidate，
  清理可能来自旧映射或复位前残留的 I-cache line。

注意：曾尝试使用 ROM `Cache_Invalidate_Addr()` 或手写 `IHI` 排查 cache，但最终证明
Task 1.1 的 `IllegalInstruction` 根因是 RRI8 指令编码，不是 `esp_cache_msync`
失效。最终代码保留 `esp_cache_msync` 路径。

### 为什么不再保留 IRAM/DIRAM fallback

保留 fallback 会制造两个问题：
- **误导调试方向**：当 PSRAM 初始化失败时，代码可能悄悄落回 IRAM/DIRAM，重新触发
  已知的 MEMPROT/CACHEERR 问题。
- **破坏单一路径验证**：当前 selftest 的 PASS 证明的是 PSRAM data/exec 双映射链路，
  不是 IRAM 写入链路。继续保留未验证路径会扩大状态空间。

因此代码策略是：
- `jit_acquire_pool()` 只尝试 PSRAM 分配 + exec alias 映射。
- `jit_pool_exec_for()` 只做 PSRAM data alias → exec alias 转换。
- `jit_commit_code()` 只提交到 PSRAM write alias 并同步 exec alias I-cache。
- `jit_pool_ready()` 只在 `s_jit_pool_psram && s_jit_pool_exec` 时返回 true。
- PSRAM 不可用时，不 panic、不尝试 IRAM，直接让解释器继续运行。

### 板上验证判据

PSRAM JIT 池本身是否可用，不能只看 `esp_mmu_map()` 返回成功；必须同时满足：

1. 串口打印 data/exec 双地址：
   ```
   [jit] pool write=0x3c050000 exec=0x42800000 psram=1
   ```
2. `jit_translate` 使用 write 地址写入，用 exec 地址作为 `host_code`：
   ```
   [jit] translate: raw=0x3c050000 write=0x3c050000 exec=0x42800000 tmp=... psram=1
   [jit] exec @0x42800000 cpu=...
   ```
3. PC 能进入 `0x428xxxxx`。如果 panic PC 在 `0x428xxxxx`，说明至少已经开始从
   PSRAM exec alias 取指。
4. selftest 通过：
   ```
   [jit_selftest] summary: 3/3 PASS
   ```

Task 1.1 结束时的最终状态满足以上全部条件。

### 常见故障与判读

| 现象 | 典型日志 | 判读 |
|------|----------|------|
| `CACHEERR` | `Cache disabled but cached memory region accessed` | 仍在写 IRAM 或 flash/cache-disable 窗口访问了 cached 区域 |
| `LoadStoreError` | `EXCVADDR=0x42800000` | 把 exec alias 当 data 指针读取，不代表取指失败 |
| `IllegalInstruction` at `0x42800000` | 第一条指令处 panic | 可能是入口 ABI/首条编码/cache 同步问题，先 dump write alias 字节 |
| `IllegalInstruction` at `0x42800003` | `ENTRY` 后第一条指令 panic | 首条 load/ALU 编码错误概率高；Task 1.1 中是 RRI8 byte layout 错 |
| `LoadProhibited EXCVADDR=0x1004` | prologue load 处 panic | 看起来像 cpu 指针错，实际可能是 `L32I` 字段布局错导致错误访存 |

排查顺序：
1. 确认 `write=0x3c... exec=0x428... psram=1`。
2. 只读取 write alias，打印前 8-16 字节。
3. 用 Xtensa assembler / objdump 反查目标指令字节。
4. 确认 `block->host_code` 是 exec alias，不是 write alias。
5. 确认 `C2M` 和 `M2C|TYPE_INST` 同步顺序完整。
6. 再分析 ABI 和 `CPUI386` 偏移。
7. 不要回退到 IRAM/DIRAM 路径；该路径已判定废弃。

### 性能考量

- PSRAM 取指比 IRAM 慢（cache miss 需走 OPI SPI 总线，约 80-120ns vs IRAM 单周期）
- 但 I-cache 命中时近似零开销（ESP32-S3 I-cache 16/32KB）
- JIT 块通常较小（< 256 bytes），cache locality 好
- 对比 IRAM 方案的优势：**无 MEMPROT 限制、无 CACHEERR 风险、池大小不受 IRAM 空间约束**

### 下一步

Task 1.1 已完成：PSRAM 执行池可用，prologue/epilogue 进入与返回路径通过板上
selftest。下一步进入 **Task 1.2**，验证块退出后的 `next_ip` / `ip` /
`ifetch` 一致性。

---

## 0.B  通用化边界说明（2026-07-01 更新）

### 当前结论

为避免后续 RISC-V 后端接入时继续把“x86 前端状态”和“LX7 发射细节”绑在一起，
JIT 头文件已经拆成两层：

- `jit_x86.h`：**架构无关的 x86 JIT 合约**。包含块缓存元数据、`JITState` /
  `JITBlock`、NOJIT/Exit/Bail 枚举、`jit_init()` / `jit_try_execute()` /
  `jit_translate()` 等公共 API，以及未来共享 decoder/executor 需要的 i386/i387
  命令枚举与函数声明。
- `jit_lx7.h`：**Xtensa LX7 后端头**。只保留 ESP32-S3/LX7 约束、x86 GPR →
  LX7 寄存器映射、PSRAM pool/selftest 辅助声明。
- `jit_lx7.c`：当前唯一实现。它仍包含 bring-up 阶段的本地 x86 扫描译码逻辑；
  这不是最终前端边界，后续稳定后可逐步迁移到 `jit_x86` 前端实现。

### 不要误读

- `jit_x86.h` 的 i386/i387 命令声明是**接口预留**，不是已经实现的通用 decoder。
- 目前没有 RISC-V 后端、没有 RISC-V 构建目标、也没有 RISC-V 验收标准。
- 本计划所有板上验收仍以 **ESP32-S3 / LX7 / COM20** 为准，不使用 QEMU。
- `JITState` 布局暂不随意扩张。`CPUI386` 里内嵌 `JITState`，且 `cc` 偏移已通过
  `_Static_assert` 锚定；需要新增跨后端字段时，要同步检查结构布局影响。

### 后续演进原则

1. 新的 x86/i387 语义分类、通用 action/command 名称，应优先放在 `jit_x86.h` 或后续
   `jit_x86.c`，不要继续塞进 `jit_lx7.h`。
2. 后端发射细节（寄存器分配、host 分支范围、cache 同步、可执行内存池）必须留在各自
   后端文件，例如 `jit_lx7.*`、未来 `jit_riscv.*`。
3. 当前任务优先级不变：先把 LX7 后端按 selftest 驱动跑稳，再抽公共 decoder；不要为了
   未来 RISC-V 提前大改已验证的 LX7 路径。

---

## 1. 总体阶段划分

| 阶段 | 主题 | 产出 | Task 数 | 估时 |
|------|------|------|---------|------|
| P0 | 可观测性与迭代环路 | 构建开关、板上差分自检、翻译/执行 trace | 5 | ~5 天 |
| P1 | ABI / 状态同步正确性 | prologue/epilogue、next_ip 一致性，放开 MOV_RI/MOV_RR/直线块 | 5 | ~5 天 |
| P2 | 惰性标志正确性 | ALU/逻辑/移位/NEG 的 `cc.*` 与解释器逐位一致，死标志消除验证 | 5 | ~5 天 |
| P3 | 控制流 | JMP、CMP/TEST+Jcc 融合、分支回填、块链接 | 4 | ~4 天 |
| P4 | 缓存与自修改代码 | 逐出/flush、CR3 切换、写内存失效、精确页内失效 | 4 | ~4 天 |
| P5 | 性能与扩面 | 基准、窄编码/最小存取、放开更多 opcode、内存操作数(长期) | 4 | ~4 天 |

总计 ≈ 27 个 task ≈ 27 个工作日。每个 task 控制在 2 小时内（含构建/板上验证），做不完就拆。

---

## 2. 贯穿始终的工作约定

**分级开关（P0 建立后全程使用）**
- `TINY386_ENABLE_JIT`：总开关，接入 `i386.c`。
- `TINY386_JIT_LEVEL`：0=全关，1=单指令块基线，2=直线块，3=控制流……逐级抬升。
- `jit_action_enabled()` 仍是"白名单闸门"，每放开一个动作必须先过自检。

**每个 task 的标准执行环（务必遵守顺序）**
1. 按本文件中该 task 的描述改代码。
2. 编译（`idf.py -C make/esp-ili9341 -B build_ili9341 -DBOARD=ili9341 build`），确保无编译错误。
3. 烧录到开发板（`idf.py -C make/esp-ili9341 -B build_ili9341 -p COM20 flash`）。
4. 串口抓取 selftest 输出（`python tools/serial_capture.py COM20 115200 --timeout 8`），差分自检必须全 PASS。
5. 只有自检通过，才进行 SeaBIOS/DOS 启动冒烟。
6. `git commit`，提交信息写明放开了什么、自检结果、风险。

**验收的黄金标准**：同一段 x86 字节，**JIT 块执行后的 8 个 GPR + EFLAGS + next_ip**，必须与**纯解释器执行**逐位相等。这是所有正确性 task 的统一判据。

---

## 3. 详细 Task 清单

### 阶段 P0：可观测性与迭代环路

#### Task 0.1 — 接入构建开关与 NOP 基线
- **目标**：让 JIT 真正进入固件，并以"只允许 NOP"的最低档复现已知可用基线。
- **范围(≈2h)**：在 `make/esp-ili9341/main/CMakeLists.txt` 为 `i386.c` 与 `jit_lx7.c` 增加
  `-DTINY386_ENABLE_JIT -DTINY386_JIT_LEVEL=1`（先只让 NOP 生效，确认 `jit_action_enabled` level 行为）。
- **涉及文件**：`make/esp-ili9341/main/CMakeLists.txt`。
- **Codex 提示词要点**：只改构建定义，不改逻辑；保证非 ESP32 主机构建仍不启用 JIT。
- **验收**：固件编译通过；板上启动到硬盘引导、VGA 出图，30s 无 WDT（复现历史 NOP 冒烟）。

#### Task 0.2 — 主机端单元测试工程
- **目标**：脱离硬件就能编译/测试 emitter 与译码器。
- **范围(≈2h)**：
  - 新建 `tests/`（主机 gcc）：把 `decode_x86_insn` / `emit_*` 以 `-DTINY386_JIT_HOST_TEST` 方式独立编译，写最小测试框架（断言 + 计数）。
- **涉及文件**：`tests/`（新增），`jit_lx7.c` 顶部少量 `#ifndef BUILD_ESP32` 暴露内部函数给测试。
- **验收**：`make -C tests` 跑出至少 1 条断言。
- **备注**：不依赖 QEMU。正确性验证全部在开发板上通过差分自检完成。

#### Task 0.3 — 板上差分自检（最重要的基础设施）✅ 已完成
- **目标**：开机时运行一组内置 x86 片段，**JIT 执行 vs 解释器执行**逐位比对，串口打印 PASS/FAIL。
- **已完成**：
  - ✅ `jit_selftest.c/.h` 框架已实现（NOP / MOV_RI 用例）
  - ✅ `esp_main.c` 调用 `jit_selftest_run()` 已接入
  - ✅ XIP-from-PSRAM 已开启
  - ✅ 构建编译通过
  - ✅ IRAM/DIRAM 写入路线已判定不可行并废弃
  - ✅ PSRAM data/exec 双映射 JIT 池已跑通
- **废弃路线**：不再尝试把 dst 转 DRAM alias 后写 IRAM；该路线会触发 MEMPROT/CACHEERR。
- **最终路线**：只使用 PSRAM write alias 写入，并通过 exec alias 取指执行。
- **验收**：COM20 板上 selftest `3/3 PASS`（NOP / MOV_EAX_imm32 / MOV_EBX_imm32_neg）。
- **依赖**：后续所有正确性 task 都向这个自检集合追加用例。

#### Task 0.4 — 翻译与执行 trace
- **目标**：可选地打印每个块的翻译信息与执行进出状态，便于定位 WDT/发散。
- **范围(≈2h)**：在 `jit_translate`/`jit_try_execute` 加 `#if TINY386_JIT_TRACE` 串口日志：
  guest paddr、x86 原始字节、动作序列、host 字节数、退出 next_ip、命中/未命中计数。
- **涉及文件**：`jit_lx7.c`；若 trace 开关需要跨后端共享，放入 `jit_x86.h`，LX7 私有开关放入 `jit_lx7.h`。
- **Codex 提示词要点**：trace 默认关闭、零开销；打开后限频/限量避免刷屏拖垮 WDT。
- **验收**：打开 trace 后能看到 NOP 块的翻译与执行日志，关闭后无任何额外开销。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。新增 `TINY386_JIT_TRACE`（默认 0）与 gated `JIT_TRACEF`，覆盖
  `try`、cache miss、sticky NOJIT、scan action、emit action、translate ok/bail、exec/done。构建通过；默认关闭 trace 的
  app 经 COM19 刷入后 selftest `4/4 PASS`。JTAG 验收因 USB-JTAG 未枚举连续失败 3 次跳过，改用 COM19 串口刷写完成。

#### Task 0.5 — NOJIT/Bail 统计面板
- **目标**：把 `JITState` 的 hits/misses/bailed/invalidations 以及各 `JITBailReason` 计数定期打印。
- **范围(≈2h)**：新增按 bail 原因的计数数组；在自检/运行中周期性 dump。
- **涉及文件**：`jit_x86.h`（通用统计字段/枚举），`jit_lx7.c`（当前 LX7 dump 实现）。
- **验收**：运行后能看到"哪种原因导致最多次 NOJIT"，为后续放开优先级提供数据。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。`JITState` 增加 `bail_counts[]` 与 `stats_ticks`；
  `jit_count_bail()` 统一统计 NOJIT/code16/paging 等失败原因；`jit_dump_stats()` 输出 hits/misses/bailed/
  invalidations/smc/pool/epoch 和非零 bail 分布。CMake 增加 `TINY386_JIT_STATS_PERIOD` cache 变量，默认 2048。

---

### 阶段 P1：ABI / 状态同步正确性（定位历史 WDT 根因）

> 假设：MOV_RR/JMP 失败大概率不是编码错，而是 **块退出后 CPU 状态与解释器不一致**
> （next_ip / ip / ifetch.paddr / GPR 回写 / CALL0 ABI）。本阶段用自检逐个证伪。

#### Task 1.1 — prologue/epilogue 与 CALL0 ABI 审计 ✅ 已完成
- **目标**：确认 `emit_prologue`/`emit_epilogue`、`entry32`/`retw`、GPR 偏移、`a2=cpu` 约定全部正确。
- **范围(≈2h)**：核对 `GPR_OFF`/`NEXT_IP_OFF` 与 `CPUI386` 实际布局（加 `_Static_assert`）；
  核对 CALL0/窗口 ABI 与 `entry32` 是否自洽（是否真的需要 ENTRY，还是纯 CALL0 不该用 ENTRY）。
- **涉及文件**：`jit_lx7.c`，`i386.c`（静态断言）。
- **验收**：新增的偏移静态断言编译通过；单 MOV_RI 块自检 PASS。

##### 调试记录（2026-06-30 / COM20）

起点：PSRAM JIT 池已经可执行，`CACHEERR` 已消除，串口显示：
```
[jit] pool write=0x3c050000 exec=0x42800000 psram=1
[jit_selftest] start (3 cases)
[jit] translate: raw=0x3c050000 write=0x3c050000 exec=0x42800000 tmp=0x3fcbf820 psram=1
PC=0x42800006  EXCCAUSE=0x1c (LoadProhibited)  EXCVADDR=0x00001004
```

初始假设是 CALL0/windowed ABI 或 `CPUI386` 偏移错误。审计后确认：
- `CPUI386.gprx[0].r32 == 0`
- `CPUI386.gprx[1].r32 == 4`
- `CPUI386.gprx[7].r32 == 28`
- `CPUI386.next_ip == 36`
- `cc` 相关偏移断言仍匹配 `jit_x86.h`

ABI 试错过程：
- 曾尝试移除 `ENTRY`、改用 `RET.N`，并通过 inline asm `CALLX0` 调用 JIT。
- 结果出现 `IllegalInstruction`，且寄存器窗口状态显示编译器仍按 ESP-IDF 默认
  windowed ABI 调用函数入口。
- 因此回滚 CALL0 方案，保留 `ENTRY a1,32` + `RETW.N` + `fn(cpu)`。

关键突破来自串口内存 dump：
```
Memory dump at 0x427ffffc: 00000002 22004136 42220032
PC      : 0x42800003
```

这表示第一条 `ENTRY` (`36 41 00`) 已经成功执行，真正崩溃点是随后的
`L32I`。当时 JIT 生成：
```
36 41 00 22 32 00 22 42 ...
```

使用 Xtensa 工具链反查编码：
```
entry a1, 32      -> 36 41 00
l32i  a3, a2, 0  -> 32 22 00
l32i  a4, a2, 4  -> 42 22 01
s32i  a3, a2, 64 -> 32 62 10
addi  a3, a2, 64 -> 32 c2 40
```

根因：`emit_l32i` 等 RRI8 宽格式 helper 把字段当成了：
```
byte0 = opbyte
byte1 = (t << 4) | s
byte2 = imm
```

正确格式应为：
```
byte0 = (t << 4) | op0
byte1 = (op_sub << 4) | s
byte2 = imm8
```

因此错误的 `22 32 00` 不是 `l32i a3,a2,0`，而是非法/错误格式；正确字节是
`32 22 00`。这解释了为什么最初像是 `a2` 或 `CPUI386` 偏移坏了，实际是
load 指令本身编码错误。

同步修复：
- `emit_addi`
- `emit_l32i`
- `emit_s32i`
- `emit_beq` / `emit_bne`
- `emit_blt` / `emit_bge`
- `emit_bltu` / `emit_bgeu`

RI 分支也用工具链核对，发现：
- `BNEZ` opcode 应为 `0x56`
- `BLTZ` opcode 应为 `0x96`
- `BGEZ` opcode 保持 `0xD6`
- `BEQZ` opcode 保持 `0x16`

分支回填同时修正为 Xtensa 3-byte branch 的 `PC+4` 基准：
```
br_off = taken_start - (branch_site + 4)
```

最终保留的 ABI 结论：
- ESP-IDF / ESP32-S3 固件默认 windowed ABI。
- JIT block 作为普通 C 函数指针调用时，入口必须使用 `ENTRY`，出口必须使用
  `RETW.N`。
- 不要混用 C 函数指针调用与手写 CALL0 ABI，除非整个调用链和编译选项都切到
  CALL0。

最终 COM20 验收：
```
idf.py -C make/esp-ili9341 -B build_ili9341 -DBOARD=ili9341 build
idf.py -C make/esp-ili9341 -B build_ili9341 -p COM20 flash
python tools/serial_capture.py COM20 115200 --timeout 8
```

串口结果：
```
[jit] pool write=0x3c050000 exec=0x42800000 psram=1
[jit_selftest] start (3 cases)
[jit] translate: raw=0x3c050000 write=0x3c050000 exec=0x42800000 tmp=0x3fcbf820 psram=1
[jit] exec @0x42800000 cpu=0x3fccaf3c
[jit_selftest] PASS NOP (interp=1 jit=1)
[jit_selftest] PASS MOV_EAX_imm32 (interp=1 jit=1)
[jit_selftest] PASS MOV_EBX_imm32_neg (interp=1 jit=1)
[jit_selftest] summary: 3/3 PASS
[boot] selftest finished, halting
```

Task 1.1 判定：通过。后续任务可以在这个 ABI/编码基线之上继续放开更多动作。

#### Task 1.2 — next_ip / ip / ifetch 一致性
- **目标**：保证块执行后解释器接管时状态一致（循环里已有 `cpu->ip=next_ip; ifetch.paddr=0`，需验证充分）。
- **范围(≈2h)**：在自检中，块执行后再让解释器继续执行 N 条，验证不发散；检查 `cycle` 计数语义。
- **涉及文件**：`jit_selftest.c`，必要时 `i386.c` 循环。
- **验收**：MOV_RI 块 + 后续解释器混合执行，状态与全解释器一致。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。`JITCpuSnapshot` 扩展比较 `ip` 与 `cycle`；
  新增 mixed 自检：`NOP -> MOV_RI` 与 `MOV_RI -> NOP -> MOV_RI`，路径为先 JIT 一条再解释器继续执行。
  COM19 板上结果：`MIXED_NOP_THEN_MOV_EAX_mixed PASS`、`MIXED_MOV_EAX_NOP_MOV_EBX_mixed PASS`，
  总结 `6/6 PASS`。主循环现有 `cpu->ip = cpu->next_ip; cpu->ifetch.paddr = 0` 交接路径验证通过。

#### Task 1.3 — 放开 MOV_RI（单指令块）
- **目标**：把 `ACT_MOV_RI` 纳入白名单并通过启动冒烟。
- **范围(≈2h)**：`jit_action_enabled` 放开 MOV_RI；自检覆盖正负/大小立即数；启动冒烟。
- **验收**：自检 PASS；SeaBIOS→DOS 启动无 WDT。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。`ACT_MOV_RI` 纳入 level 1 白名单，保留
  `TINY386_JIT_ENABLE_MOV_RI` 保险开关；bring-up 初期曾使用 `TINY386_JIT_SINGLE_INSN_BLOCK=1` 隔离验证，
  Task 1.5 已解除该限制。
  同时修正 `TINY386_JIT_SELFTEST_ONLY` / `TINY386_JIT_SELFTEST_AT_BOOT` 的预处理判断，让宏值 0 真正关闭。
  COM19 app-flash 成功；30 秒启动冒烟无 WDT/panic，SeaBIOS 完成 PCI/VGA 初始化，硬盘启动进入
  `Booting from 0000:7c00`，随后到达 `set VGA mode 1`。

#### Task 1.4 — 定位并修复 MOV_RR
- **目标**：解决历史最顽固的 MOV_RR WDT。
- **范围(≈2h)**：先开 trace 跑到 PCI/MPTABLE 区附近，抓首个发散块；用自检构造该字节序列复现；修复后放开。
- **涉及文件**：`jit_lx7.c`（`emit_mov` 当前使用 3 字节 `OR rd,rs,rs` 稳态路径），`jit_selftest.c`。
- **验收**：MOV_RR 自检 PASS；启动通过；记录根因到提交信息。
- **备注**：历史 `mov.n` 字节对照表已删除；先用 `OR rd,rs,rs`（3 字节）稳态版本验证 MOV_RR 状态同步与块缓存/逐出交互。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。新增 MOV_RR 差分自检：
  `MOV_EAX_EBX`（`89 d8`）与 `MOV_EDI_EAX`（`8b f8`）。旧 3 字节 `OR rd,rs,rs` 路径复现失败：
  先表现为 GPR 不一致，修正 RRR 字段后在 `or a3,a6,a6` 处 `IllegalInstruction`。最终改用 Xtensa density
  `MOV.N` 编码（如 `mov.n a3,a6 -> 3d 06`），MOV_RR 自检 `8/8 PASS`。运行时将
  `TINY386_JIT_ENABLE_MOV_RR` 打开；COM19 正常启动冒烟 30 秒无 WDT/panic，SeaBIOS 到硬盘启动并到达
  `set VGA mode 1`。

#### Task 1.5 — 多指令直线块
- **目标**：放开无分支直线块（MOV_RR/MOV_RI/XCHG/BSWAP/CWDE/CDQ 组合，`TINY386_JIT_LEVEL=2`）。
- **范围(≈2h)**：去掉 `*_SINGLE_BLOCK` 限制（仅对已验证动作）；自检加多指令片段；启动冒烟。
- **验收**：多指令片段自检 PASS；启动通过；命中率明显上升（看 P0.5 面板）。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成第一阶段。移除 ili9341 构建中的
  `TINY386_JIT_SINGLE_INSN_BLOCK=1`，先对已验证的 `MOV_RI`/`MOV_RR`/`NOP` 放开多指令直线块；
  新增 `BLOCK_MOV_CHAIN` 差分自检（`MOV EAX,imm32 -> MOV EBX,EAX -> NOP`），板上 selftest
  `9/9 PASS`。切回正常启动后经 COM19 app-flash，30 秒启动冒烟无 WDT/panic，SeaBIOS 到硬盘启动并到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。`XCHG/BSWAP/CWDE/CDQ` 仍保持后续 level 放开。

---

### 阶段 P2：惰性标志正确性

> 解释器是 **lazy flags**：写 `cc.op/dst/dst2/src1/src2/mask`，`get_CF/ZF/SF/OF/AF/PF` 时按需计算。
> JIT 必须写出**完全等价**的 `cc.*`，否则后续 Jcc/解释器读标志会发散。

#### Task 2.1 — ADD/SUB 标志等价
- **目标**：`ACT_ALU_RR/RI` 的 ADD/SUB 写出的 `cc.*` 与解释器逐位一致。
- **范围(≈2h)**：自检对 ADD/SUB 遍历边界值（0、-1、INT_MAX、进位/溢出/借位组合），比较 6 个标志位。
- **涉及文件**：`jit_selftest.c`，必要时修 `emit_action` 的 `cc` 写出。
- **验收**：ADD/SUB 全标志自检 PASS 后放开这两类。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成板上差分自检与主程序回刷。新增 14 个
  ADD/SUB 边界自检（RR/RI 覆盖 zero、carry/borrow、overflow、aux carry、negative imm），连同既有用例
  selftest-only 结果 `23/23 PASS`。修复两处底层发射问题：RRR ALU 字段顺序改为 assembler 对照的
  `t/op0, r/s, op2/op1` 布局；`MOVI` 3 字节编码修正，并将 `cc.mask` 的 `0x8d5/0x8c5`
  改走 `emit_movi32` literal 路径。`jit_action_enabled()` 已在 `TINY386_JIT_LEVEL >= 2` 对
  `ACT_ALU_RR/RI` 的 ADD/SUB 放开，其余 ALU/CMP/TEST/Jcc 仍保持关闭。切回正常主程序后 COM19
  app-flash 成功，30 秒启动冒烟无 WDT/panic，到达 `Booting from 0000:7c00` 与 `set VGA mode 1`。

#### Task 2.2 — AND/OR/XOR 逻辑标志
- **目标**：逻辑运算 `cc.mask=LOGIC`、CF/OF=0、SF/ZF/PF 正确。
- **范围(≈2h)**：自检遍历逻辑用例；放开 AND/OR/XOR。
- **验收**：自检 PASS；启动通过。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。沿用 ALU 差分框架新增 12 个
  AND/OR/XOR 边界用例（RR/RI 覆盖 zero、sign、parity），并把初始 EFLAGS 六个相关位全部置 1，
  验证 CF/OF 清零、SF/ZF/PF 更新、AF 保留。`jit_action_enabled()` 已在
  `TINY386_JIT_LEVEL >= 2` 对 `ACT_ALU_RR/RI` 的 AND/OR/XOR 放开。COM19 selftest-only
  结果 `35/35 PASS`；切回正常主程序后 app-flash 成功，30 秒启动冒烟无 WDT/panic，到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。

#### Task 2.3 — NEG/NOT/INC/DEC
- **目标**：NEG 用 `JIT_CC_NEG32`；NOT 不影响标志；**INC/DEC 必须保留 CF**（历史失败点）。
- **范围(≈2h)**：自检覆盖 INC/DEC 对 OF/SF/ZF/AF/PF 更新且 CF 不变；按需用专门 cc.op 或"标志可证死"才放开。
- **涉及文件**：`jit_lx7.c`，`jit_selftest.c`。
- **验收**：INC/DEC 自检 PASS（含 CF 保留）；启动通过。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成受限放开。新增 `JIT_CC_INC32`、
  `JIT_CC_DEC32` 和 `JIT_CC_MASK_ARITH_NO_CF` 静态校验；NEG 写 `JIT_CC_NEG32`，
  INC/DEC 写 `JIT_CC_INC32/DEC32` 且 `cc.mask` 不含 CF，从而保留已 materialized 的 CF。
  自检新增 NOT flags-preserve、NEG zero/min、INC/DEC CF set/clear 用例，COM19 selftest-only
  结果 `42/42 PASS`。运行时放开 NEG；INC/DEC 仅在 `flags_dead` 为真时放开，避免前序 lazy CF
  尚未物化时错误保留。切回正常主程序后 app-flash 成功，30 秒启动冒烟无 WDT/panic，到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。

#### Task 2.4 — 死标志消除（flags_dead）验证
- **目标**：验证 `jit_translate` 反向扫描的 `flags_dead` 推导正确（写标志后被覆盖、且中间无 Jcc/读标志）。
- **范围(≈2h)**：自检构造"ADD 后紧跟 ADD 覆盖标志、再 Jcc"等序列，比较最终标志。
- **验收**：死标志路径与非死路径结果一致；无错误省略。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。新增 `BLOCK_ADD_ADD_COVER_FLAGS`
  多指令块：第一条 `ADD EAX,EBX` 产生的 flags 被第二条 `ADD EAX,ECX` 覆盖，但第二条仍依赖第一条的
  EAX 数据结果，用于验证 flags-dead 只省略 flags 写出、不丢 GPR 数据流。修复自检 harness：每个 case
  setup 时清理测试代码窗口，避免短指令用例读到前一用例残留 bytes。COM19 selftest-only 结果
  `43/43 PASS`；切回正常主程序后 app-flash 成功，30 秒启动冒烟无 WDT/panic，到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。

#### Task 2.5 — 移位标志（SHL/SHR/SAR，imm 与 CL）
- **目标**：移位的 CF/OF/SF/ZF/PF 与解释器一致（移位 0 不改标志的特例）。
- **范围(≈2h)**：自检遍历移位量 0/1/中间/31；放开 `ACT_SHx_RI`/`ACT_SHx_CL`。
- **验收**：自检 PASS；启动通过。
- **备注**：当前移位 emitter 未写 `cc.*`——本 task 需补齐标志写出或在标志活时回退解释器。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成 imm 路径。新增 `JIT_CC_SHL/SHR/SAR`
  静态校验，`ACT_SHx_RI` 对移位量 0 保持 no-op、不改 flags；flags live 时写出
  `cc.dst/cc.dst2/cc.src1/cc.op/cc.mask`，flags dead 时只发实际移位。修正 Xtensa
  `SLLI/SRLI/SRAI` 立即移位编码以及 `SSL/SSR` SAR-load 编码；此前 COM19 上
  `SHL_RI_count1` 触发 `IllegalInstruction`，经 objdump 对照后已修复。自检新增
  SHL/SHR/SAR 的 count 0/1/4/31 共 12 例，COM19 selftest-only 结果 `55/55 PASS`。
  切回正常主程序后 app-flash 成功，30 秒启动冒烟无 WDT/panic，到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。`ACT_SHx_CL` 仍保持未放开，后续可单独补
  CL masked count 与 flags live 覆盖。

---

### 阶段 P3：控制流

#### Task 3.1 — 放开 JMP（块边界）
- **目标**：解决历史 JMP WDT（relocation 阶段）。
- **范围(≈2h)**：epilogue 写 `next_ip=target_eip` 返回 C 即可（无需 host J）；trace 首批 JMP，比对 target；放开 `ACT_JMP`，`TINY386_JIT_LEVEL=3`。
- **验收**：JMP 自检 PASS；relocation 阶段不再 WDT；启动通过。

#### Task 3.2 — CMP/TEST + Jcc 融合逐条件验证
- **目标**：16 个条件码（Z/NZ/B/NB/BE/NBE/L/NL/LE/NLE/S/NS/O/NO/P/NP）的融合分支正确。
- **范围(≈2h)**：自检对每个 cc 用 CMP_RR/CMP_RI/TEST_RR 三种来源各构造 taken/not-taken；比较 next_ip。
- **涉及文件**：`jit_selftest.c`，`jit_lx7.c`（补全/修正未支持的 cc，如 O/NO/P/NP 目前 `return false` 回退）。
- **验收**：所有支持的 cc 自检 PASS，不支持的安全回退解释器（不发散）。

#### Task 3.3 — 分支回填范围与降级
- **目标**：LX7 `BEQ/...` 偏移是有符号 8 位、`BEQZ/...` 是 12 位；epilogue 较大时可能溢出。
- **范围(≈2h)**：检测 `br_off` 超范围时改用"短分支跳过一个 J/长跳"或回退；自检构造大 epilogue 触发。
- **验收**：构造的超范围用例不产生坏跳转（PASS 或安全回退）。

#### Task 3.4 — 直接块链接（direct block linking）
- **目标**：fallthrough/taken 直接跳到已编译的下一块入口，减少回 C 与 prologue/epilogue 开销。
- **范围(≈2h)**：块记录出口 patch 点；命中目标块时回填 host 直跳；CR3/flush 时清链接。
- **涉及文件**：`jit_x86.h`（通用 `JITBlock` 链接元数据），`jit_lx7.c`（LX7 patch 点与回填实现）。
- **验收**：自检仍 PASS；P5 基准显示开销下降。
- **备注**：DOSBox-X dynrec 的链接思路可参考（仅设计参考，不拷贝代码）。

---

### 阶段 P4：缓存与自修改代码（SMC）

#### Task 4.1 — 缓存逐出/冲突/pool 满
- **目标**：哈希冲突逐出、`guest_paddr` 校验、pool 满 `jit_invalidate_all` 路径正确。
- **范围(≈2h)**：自检构造同槽不同地址、pool 逼近满；验证不执行到陌生块。
- **验收**：构造用例下无错误命中、无越界。

#### Task 4.2 — CR3 / 分页模式切换失效
- **目标**：写 CR3、切 CR0.PG、切换 code16/32 时 `jit_invalidate_all`。
- **范围(≈2h)**：在 `i386.c` 写 CR3/CR0 的位置调用失效；自检模拟切换后不复用旧块。
- **涉及文件**：`i386.c`（MOV CR 处），`jit_lx7.c`。
- **验收**：切换后旧块不被执行；启动/重启动稳定。

#### Task 4.3 — 写内存触发页失效
- **目标**：guest 写内存命中已翻译页时 `jit_invalidate_page`（SMC 正确性）。
- **范围(≈2h)**：找到 store 路径 hook（`store8/16/32` / IDE/DMA 写 RAM）；写命中翻译页时失效。
- **涉及文件**：`i386.c`（store 路径），`jit_lx7.c`。
- **验收**：自检：先翻译一块，再改写其字节，再执行 → 走新代码而非旧块。
- **风险**：每次写都查表会很慢，需用页位图/脏标记降开销（与 4.4 结合）。

#### Task 4.4 — 精确页内范围失效
- **目标**：只失效"源字节范围与写范围重叠"的块（利用 `guest_paddr/guest_end`）。
- **范围(≈2h)**：把 4.3 的整页失效细化为范围重叠；维护页→块的轻量索引（`JITPageTrack`）。
- **验收**：写未重叠区域不误失效；写重叠区域必失效。

---

### 阶段 P5：性能与扩面

#### Task 5.1 — 性能基准
- **目标**：量化 JIT on/off 的收益。
- **范围(≈2h)**：用 `tools/dosbench.asm` 或固定 DOS 工作负载，统计 cycle/帧率/耗时；记录到 `docs/`。
- **验收**：得到可复现的基准数字与命中率/bail 分布。

#### Task 5.2 — 最小寄存器存取与窄编码
- **目标**：prologue/epilogue 只 load/store 块内实际用到的 GPR；启用 `mov.n`/`addi.n` 等窄编码。
- **范围(≈2h)**：在扫描阶段统计读写寄存器集合；epilogue 只回写脏寄存器。
- **验收**：自检仍 PASS；基准较 5.1 改善。

#### Task 5.3 — 扩大 opcode 覆盖
- **目标**：按 P0.5 的 bail 统计，优先放开出现最多的安全 opcode（如更多 ALU/移位/`MOVZX/MOVSX` 寄存器形式）。
- **范围(≈2h)**：每次放开 1–2 个，先自检后冒烟。
- **验收**：命中率上升，启动/基准稳定。

#### Task 5.4 — 内存操作数（长期，谨慎）
- **目标**：支持寄存器寻址的内存读写（需 TLB/页走查快路径）。
- **范围(≈2h 起，可能需拆多 task)**：先支持最简单的 `[reg]`/`[reg+disp]` 直读直写命中 `phys_mem` 的快路径，未命中回退解释器。
- **涉及文件**：`jit_lx7.c`（新增内存 action），可能复用 `i386.c` 的地址翻译。
- **验收**：内存读写自检 PASS；不命中快路径时安全回退；启动稳定。
- **风险**：这是最高风险区，TLB/分页/MMIO/对齐都要正确，务必差分覆盖充分。

---

## 4. 每日执行模板（给 Codex 的统一指令骨架）

> 把下面这段当作每天那个 task 的"系统提示"，替换 `{TASK_ID}`：

```
你正在实现 tiny386hen JIT 开发计划中的 {TASK_ID}。
约束：
1. 只完成本 task 的范围，不顺手改其他动作的开关。
2. 任何"正确性"改动，必须在 jit_selftest 中新增/复用用例，判据是
   JIT 块执行后的 8×GPR + EFLAGS(6 位) + next_ip 与纯解释器逐位相等。
3. 非 ESP32 主机构建必须仍可编译且 JIT 关闭；不破坏现有启动路径。
4. 先跑 tests/ 主机单测与板上自检，全 PASS 再做启动冒烟。
5. 输出：改了哪些文件、放开了什么、自检结果、已知风险，便于写 commit。
```

---

## 5. 风险与原则

- **自检驱动**：自检未过不进入启动冒烟，所有验证直接在开发板上完成（不依赖 QEMU）。
- **白名单只增不滥**：`jit_action_enabled` 每次只放开已自检的动作。
- **标志是头号陷阱**：lazy flags 的 `cc.*` 必须与解释器逐位一致（P2 专门处理）。
- **能回退就回退**：任何不确定的 cc / 超范围分支 / 内存形式，宁可 `return false` 回解释器，保证不发散。
- **参考但不拷贝**：DOSBox-X dynrec 仅作设计参考（块出口分类、页跟踪、NOJIT 缓存），不复制其源码/字节。

---

## 6. 里程碑

- **M1（P0 完成）**：有差分自检 + trace，NOP/MOV_RI 自检 PASS（板上串口确认）。
- **M2（P1–P2 完成）**：直线块 + 全 ALU/移位 标志正确，DOS 启动稳定加速。
- **M3（P3 完成）**：JMP/Jcc/块链接可用，热路径基本被 JIT 覆盖。
- **M4（P4 完成）**：SMC/缓存健壮，长时间运行不发散。
- **M5（P5 完成）**：有性能基准与可量化收益，opcode 覆盖持续扩大。

---

## 2026-07-01 Execution Record (COM19, auto-run through Task 5.3)

- Task 3.1 done: enabled `ACT_JMP` with `TINY386_JIT_LEVEL=3` / `TINY386_JIT_ENABLE_JMP=1`; added rel8/rel32 JMP selftests; board selftest and normal firmware smoke passed.
- Task 3.2 done: enabled CMP_RR/CMP_RI/TEST_RR + Jcc fusion for supported cc set; added 32 taken/not-taken branch selftests; board selftest passed.
- Task 3.3 done: changed Jcc emission to range-safe inverted short branch plus long `J` to the taken epilogue; unsupported conditions bail before emitting partial branch bytes.
- Task 3.4 skipped for now: current JIT blocks are full Xtensa windowed ABI functions (`ENTRY` + `RETW.N`). Directly jumping into another block entry would nest/corrupt the call window and return chain. Revisit only after introducing a separate body-entry ABI or a trampoline/link stub model.
- Task 4.1 done: verified cache slot conflict and pool-full flush paths with board selftests (`CACHE_CONFLICT`, `CACHE_POOL_FULL`).
- Task 4.2 done: invalidates all JIT blocks on CR0 paging/WP/PE changes, CR3 writes, task-switch CR3 load, and CS code16/code32 transitions; selftest covers real interpreter `MOV CR3,EAX` invalidation.
- Task 4.3 done: guest RAM stores now call JIT SMC invalidation after `pstore8/16/32` on non-MMIO physical writes, including split writes.
- Task 4.4 done: added `jit_invalidate_range()` and a hashed translated-code page bitmap so normal data writes avoid scanning the JIT cache; selftests prove same-page non-overlap writes do not invalidate, overlap writes do invalidate and retranslate modified code.
- Task 5.1 done: captured COM19 perf baseline with JIT level3 and temporary level0. In this boot window both reached `set VGA mode 1`; representative level3 samples were `ips=550889` at about 5.9s, `ips=1044520` at about 10.9s, and `ips=875941` at about 20.9s. Level0 samples were effectively similar for this workload, indicating current opcode coverage still misses the dominant DOS hot path.
- Task 5.2 done: prologue/epilogue now use conservative per-block GPR masks; only read GPRs are loaded and only written GPRs are stored. This preserves the existing function ABI and avoids the Task 3.4 linking blocker.
- Task 5.3 done: enabled `XCHG EAX,r32` at level3 and added `XCHG_EAX_EBX` / `XCHG_EAX_EDI` differential selftests.
- Verification: final selftest-only firmware on COM19 reported `96/96 PASS`; final normal firmware build flashed over COM19 and reached `Booting from 0000:7c00` and `set VGA mode 1` within a 45s capture with no WDT or panic.
