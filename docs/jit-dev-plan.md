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
| P6 | Benchmark 与调优 | 解释 JIT 拖慢 IPS 的原因，建立可复现实验矩阵与调优闭环 | 6 | ~6 天 |

总计 ≈ 33 个 task ≈ 33 个工作日。每个 task 控制在 2 小时内（含构建/板上验证），做不完就拆。

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
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。将 `TINY386_JIT_LEVEL` 和
  `TINY386_JIT_ENABLE_JMP` 改为 CMake cache 变量，默认 level3 且打开 JMP gate。新增
  `BLOCK_JMP_REL8` 与 `BLOCK_JMP_REL32` 自检，覆盖 rel8/rel32 块边界跳转，JIT 通过写
  `next_ip=target_eip` 返回 C 调度，不做 host 直跳。COM19 selftest-only 通过，切回正常
  主程序后 app-flash 成功，启动冒烟无 WDT/panic，到达 `Booting from 0000:7c00` 与
  `set VGA mode 1`。

#### Task 3.2 — CMP/TEST + Jcc 融合逐条件验证
- **目标**：16 个条件码（Z/NZ/B/NB/BE/NBE/L/NL/LE/NLE/S/NS/O/NO/P/NP）的融合分支正确。
- **范围(≈2h)**：自检对每个 cc 用 CMP_RR/CMP_RI/TEST_RR 三种来源各构造 taken/not-taken；比较 next_ip。
- **涉及文件**：`jit_selftest.c`，`jit_lx7.c`（补全/修正未支持的 cc，如 O/NO/P/NP 目前 `return false` 回退）。
- **验收**：所有支持的 cc 自检 PASS，不支持的安全回退解释器（不发散）。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成支持子集。新增 CMP_RR、CMP_RI、
  TEST_RR 到 Jcc 的分支差分自检 32 例，覆盖 Z/NZ/B/NB/BE/NBE/L/NL/LE/NLE/S/NS 的
  taken/not-taken 路径。`jit_action_enabled()` 在 level3 放开 `ACT_CMP_RR`、
  `ACT_CMP_RI`、`ACT_TEST_RR`、`ACT_JCC`；未支持或无法融合的条件仍 `return false`
  安全回退解释器。COM19 selftest-only 通过，正常主程序启动冒烟无 WDT/panic。

#### Task 3.3 — 分支回填范围与降级
- **目标**：LX7 `BEQ/...` 偏移是有符号 8 位、`BEQZ/...` 是 12 位；epilogue 较大时可能溢出。
- **范围(≈2h)**：检测 `br_off` 超范围时改用"短分支跳过一个 J/长跳"或回退；自检构造大 epilogue 触发。
- **验收**：构造的超范围用例不产生坏跳转（PASS 或安全回退）。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。Jcc emitter 改为“反向短分支跳过
  long `J`，再由 long `J` 跳到 taken epilogue”的结构，避免 epilogue 增长后直接短分支
  偏移溢出。补充了发射前 supported 条件检查， unsupported cc 在写入 placeholder 前即
  回退，避免生成半截坏块。原有 32 个分支自检保持 PASS，正常主程序启动冒烟无
  WDT/panic。

#### Task 3.4 — 直接块链接（direct block linking）
- **目标**：在不破坏 Xtensa windowed ABI 的前提下，让已缓存的 fallthrough 后继块可由源块出口直接串接，减少一次回 C 调度。
- **范围(≈2h)**：块记录链接目标元数据；命中已编译 fallthrough 目标块时发射 `CALL8` link-stub；SMC、CR3/flush、cache 冲突或目标块替换时清理链接源块。
- **涉及文件**：`jit_x86.h`（通用 `JITBlock` 链接元数据），`jit_lx7.c`（LX7 `CALL8` link-stub 与失效实现），`jit_selftest.c`（链接差分自检）。
- **验收**：板上 selftest 仍 PASS，新增 fallthrough 链接用例；正常主程序启动冒烟无 WDT/panic。
- **备注**：不能 raw `J` 到另一个 block 的函数入口；当前保守方案使用 `CALL8` 保持 windowed ABI 正确。DOSBox-X dynrec 的链接思路可参考（仅设计参考，不拷贝代码）。
- **执行结果（2026-07-01 / COM19 retry）**：✅ 已完成保守第一阶段。确认原始 `J` 到
  另一个 block 入口仍会破坏 Xtensa windowed ABI，因此改用 link-stub：fallthrough 目标块已在
  cache 中时，源块出口先回写 dirty GPR，将 `CPUI386*` 从当前窗口 `a2` 放入 outgoing arg0
  `a10`，再 `CALL8` 到目标 block，目标 `RETW.N` 返回源块后源块再 `RETW.N` 回 C。
  新增 `JIT_BLOCKF_LINKED_EXIT`、`link_slot/link_paddr/link_x86_insns` 元数据，并在目标块
  被 SMC/冲突替换失效时同步失效链接源块。新增 `LINK_FALLTHROUGH_MOV_EAX_NOP` 自检，
  COM19 selftest-only 结果 `97/97 PASS`；正常主程序回刷后 45 秒启动冒烟无 WDT/panic，
  到达 `Booting from 0000:7c00` 与 `set VGA mode 1`。taken Jcc 与未命中目标的后续
  patch 回填仍留作后续扩展。

---

### 阶段 P4：缓存与自修改代码（SMC）

#### Task 4.1 — 缓存逐出/冲突/pool 满
- **目标**：哈希冲突逐出、`guest_paddr` 校验、pool 满 `jit_invalidate_all` 路径正确。
- **范围(≈2h)**：自检构造同槽不同地址、pool 逼近满；验证不执行到陌生块。
- **验收**：构造用例下无错误命中、无越界。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。确认现有 `jit_translate()` 已在同槽
  不同 `guest_paddr` 时清空旧 slot，pool 空间不足时调用 `jit_invalidate_all()`。新增
  selftest-only wrapper 读取 `pool_epoch` 并强制设置 `pool_used`，构造 `CACHE_CONFLICT`
  与 `CACHE_POOL_FULL` 两个用例。COM19 selftest-only 中两项均 PASS，最终汇总纳入
  `96/96 PASS`。

#### Task 4.2 — CR3 / 分页模式切换失效
- **目标**：写 CR3、切 CR0.PG、切换 code16/32 时 `jit_invalidate_all`。
- **范围(≈2h)**：在 `i386.c` 写 CR3/CR0 的位置调用失效；自检模拟切换后不复用旧块。
- **涉及文件**：`i386.c`（MOV CR 处），`jit_lx7.c`。
- **验收**：切换后旧块不被执行；启动/重启动稳定。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。新增 `jit_state_change_invalidate()`
  helper，并在 `MOV CR0` 的 PG/WP/PE 变化、`MOV CR3`、task switch 读取新 CR3、以及
  `set_seg()` 中 CS 的 code16/code32 变化处调用 `jit_invalidate_all()`。自检新增
  `STATE_CR3_INVALIDATE`，先 JIT 生成旧块，再解释执行真实 `MOV CR3,EAX`，确认
  `pool_epoch` 前进。COM19 selftest-only PASS，正常主程序启动冒烟稳定。

#### Task 4.3 — 写内存触发页失效
- **目标**：guest 写内存命中已翻译页时 `jit_invalidate_page`（SMC 正确性）。
- **范围(≈2h)**：找到 store 路径 hook（`store8/16/32` / IDE/DMA 写 RAM）；写命中翻译页时失效。
- **涉及文件**：`i386.c`（store 路径），`jit_lx7.c`。
- **验收**：自检：先翻译一块，再改写其字节，再执行 → 走新代码而非旧块。
- **风险**：每次写都查表会很慢，需用页位图/脏标记降开销（与 4.4 结合）。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。新增 `jit_store_invalidate()`，在
  非 MMIO 的 `store8/16/32` 路径完成 `pstore*` 后按实际物理写入范围触发 JIT 失效；
  跨页/拆分写会分别对各段调用。MMIO 写仍交给设备 callback，不做 JIT SMC 处理。自检
  通过修改已翻译代码字节验证旧块被丢弃并重新翻译新代码。

#### Task 4.4 — 精确页内范围失效
- **目标**：只失效"源字节范围与写范围重叠"的块（利用 `guest_paddr/guest_end`）。
- **范围(≈2h)**：把 4.3 的整页失效细化为范围重叠；维护页→块的轻量索引（`JITPageTrack`）。
- **验收**：写未重叠区域不误失效；写重叠区域必失效。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成。新增 `jit_invalidate_range()`，用
  `guest_paddr/guest_end` 判断写入范围是否与 block 源字节范围重叠；保留
  `jit_invalidate_page()` 作为整页 wrapper。最初每次 guest RAM 写都扫描 cache，导致正常
  启动明显变慢；随后增加 1024-bit hashed translated-code page bitmap，只有写入可能含有
  JIT 代码的页时才扫描 block。自检新增 `SMC_RANGE_NONOVERLAP` 与 `SMC_RANGE_OVERLAP`：
  同页非重叠写不增加 invalidations，重叠写必失效并执行修改后的新代码。COM19
  selftest-only PASS；正常主程序恢复到 45 秒内到达 `set VGA mode 1`。

---

### 阶段 P5：性能与扩面

#### Task 5.1 — 性能基准
- **目标**：量化 JIT on/off 的收益。
- **范围(≈2h)**：用 `tools/dosbench.asm` 或固定 DOS 工作负载，统计 cycle/帧率/耗时；记录到 `docs/`。
- **验收**：得到可复现的基准数字与命中率/bail 分布。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成启动窗口基线。分别构建并刷入 level3
  与临时 level0 固件，均抓取 45 秒启动串口日志，两者都到达 `set VGA mode 1`。level3
  代表性 perf 样本为约 5.9s `ips=550889`、约 10.9s `ips=1044520`、约 20.9s
  `ips=875941`；level0 在该启动工作负载中数值接近，说明当前 opcode 覆盖尚未命中 DOS
  后段主热路径。备注：本次是启动窗口基线，后续仍需专门 DOS benchmark 或 bail/hot-path
  统计来指导扩面。

#### Task 5.2 — 最小寄存器存取与窄编码
- **目标**：prologue/epilogue 只 load/store 块内实际用到的 GPR；启用 `mov.n`/`addi.n` 等窄编码。
- **范围(≈2h)**：在扫描阶段统计读写寄存器集合；epilogue 只回写脏寄存器。
- **验收**：自检仍 PASS；基准较 5.1 改善。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成保守版。扫描 action 后计算每个 block 的
  GPR read/write mask；prologue 只 load 会被读取的寄存器，epilogue 只 store 会被写入的
  寄存器，避免 NOP/JMP/CMP-only 等块无意义地保存全部 8 个 GPR。此实现保持现有完整
  Xtensa windowed 函数 ABI，不触碰 Task 3.4 的直接块链接阻塞点。COM19 selftest-only
  结果 `96/96 PASS`，正常主程序 app-flash 后 45 秒启动冒烟无 WDT/panic，到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。

#### Task 5.3 — 扩大 opcode 覆盖
- **目标**：按 P0.5 的 bail 统计，优先放开出现最多的安全 opcode（如更多 ALU/移位/`MOVZX/MOVSX` 寄存器形式）。
- **范围(≈2h)**：每次放开 1–2 个，先自检后冒烟。
- **验收**：命中率上升，启动/基准稳定。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成第一批扩面。放开已有 decoder/emitter
  但尚未进入 runtime gate 的 `XCHG EAX,r32`，在 `TINY386_JIT_LEVEL >= 3` 下启用。
  自检新增 `XCHG_EAX_EBX` 与 `XCHG_EAX_EDI` 两个差分用例，同时覆盖 5.2 的 GPR mask
  不漏 load/store。COM19 selftest-only 结果 `96/96 PASS`；切回正常主程序后 app-flash
  成功，45 秒启动冒烟无 WDT/panic，到达 `Booting from 0000:7c00` 与 `set VGA mode 1`。
  备注：下一批 opcode 应优先依据 bail/hot-path 统计选择，不再靠猜测扩面。

#### Task 5.4 — 内存操作数（长期，谨慎）
- **目标**：支持寄存器寻址的内存读写（需 TLB/页走查快路径）。
- **范围(≈2h 起，可能需拆多 task)**：先支持最简单的 `[reg]`/`[reg+disp]` 直读直写命中 `phys_mem` 的快路径，未命中回退解释器。
- **涉及文件**：`jit_lx7.c`（新增内存 action），可能复用 `i386.c` 的地址翻译。
- **验收**：内存读写自检 PASS；不命中快路径时安全回退；启动稳定。
- **风险**：这是最高风险区，TLB/分页/MMIO/对齐都要正确，务必差分覆盖充分。
- **执行结果（2026-07-01 / COM19）**：✅ 已完成第一片保守实现。新增 `ACT_MOV_RM32`
  与 `ACT_MOV_MR32`，先只放开 `MOV r32,[base+disp]` 和 `MOV [base+disp],r32`；
  decoder 明确拒绝 prefix、SIB、`[disp32]`、复杂段覆盖和非 MOV 内存形式。执行路径不直接
  内联页表/TLB，而是在调用 helper 前把 JIT GPR 同步回 `CPUI386`，通过现有 `cpu_load32` /
  `cpu_store32` 复用解释器侧地址翻译、MMIO 边界和 SMC 失效逻辑，helper 返回后再重新 load
  需要的 GPR。新增 `MOV_EAX_mem_EBX_disp8` 与 `MOV_mem_EBX_disp8_EAX` 两个差分用例；
  COM19 selftest-only 结果 `99/99 PASS`，正常主程序 45 秒启动冒烟无 WDT/panic，到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。
  备注：这是正确性优先的 helper-call 路径，不是最终高性能快路径；SIB、段覆盖、直接
  `[disp32]`、8/16-bit memory forms、memory ALU/CMP/TEST、内联 TLB/page-walk 仍留作后续
  拆分任务。
- **追加执行结果（2026-07-01 / COM19）**：✅ 已完成第二片保守扩面。`decode_simple_memop()`
  放开 `index=none` 的 SIB 形式，覆盖 `[esp]` / `[esp+disp8]` / `[esp+disp32]`；同时支持
  direct `[disp32]` 与 `A1/A3` moffs32。direct 内存操作用 `mem_base=-1` 表示无 base，
  emitter 仍走现有 `cpu_load32` / `cpu_store32` helper，不引入新的分页/MMIO/SMC 风险。
  自检新增 `MOV_EAX_mem_ESP_disp8`、`MOV_mem_ESP_disp8_EAX`、`MOV_EAX_mem_disp32`、
  `MOV_mem_disp32_EAX`、`MOV_EAX_moffs32`、`MOV_moffs32_EAX`。COM19 selftest-only 结果
  `105/105 PASS`；切回正常主程序后 45 秒启动冒烟无 WDT/panic，到达
  `Booting from 0000:7c00` 与 `set VGA mode 1`。45 秒末尾统计显示
  `bail unsupported_opcode 1580`，下一步应优先做 unsupported opcode 热点统计，而不是继续猜测扩面。

#### Task 5.5 — unsupported opcode 热点直方图
- **目标**：把当前 `JIT_BAIL_UNSUPPORTED_OPCODE` 从单一计数拆成可行动的 opcode 热点列表，决定下一批扩面顺序。
- **范围(≈2h)**：在 decode/scan 失败路径记录首个 unsupported opcode（含 `0F xx` 扩展 opcode、必要时记录 ModRM reg/opcode group）；
  增加轻量直方图与周期性 dump，默认低频输出，避免串口拖慢启动。
- **涉及文件**：`jit_x86.h`（统计字段上限/开关，如需通用化），`jit_lx7.c`（scan 失败记录与 dump），`make/esp-ili9341/main/CMakeLists.txt`（可选 cache 开关）。
- **验收**：45 秒正常启动日志能列出 top unsupported opcode；不启用详细统计时默认性能不明显退化；selftest-only 仍 PASS。
- **风险**：串口输出会扰动 perf，必须支持关闭或低频采样；不要把 unsupported 统计误当作 NOJIT sticky 语义的一部分。
- **执行结果（2026-07-02 / COM19）**：✅ 已完成。新增 `TINY386_JIT_UNSUPPORTED_HIST` 与
  `unsupported_opcode_counts[]`/`unsupported_opcode_total`，scan 首个失败 opcode 时记录单字节 opcode 或
  `0F xx` 扩展 opcode，并在现有低频 `jit_stats` dump 中输出 top unsupported 列表。COM19 selftest-only
  结果 `119/119 PASS`；正常主程序回刷后 45 秒启动冒烟无 WDT/panic，到达 `Booting from 0000:7c00`
  与 `set VGA mode 3`。45 秒统计显示 `unsupported_opcode_total 1579`，热点为 `6a` 1550 次，
  其次是 `4a`、`02`、`40`、`61`、`c3`、`c6`、`35`。统计只作为诊断数据，不参与 sticky NOJIT 语义。

#### Task 5.6 — memory CMP/TEST helper-call 形式
- **目标**：若 5.5 证明 `CMP r/m32,r32`、`CMP r32,r/m32`、`TEST r/m32,r32` 是热点，先用保守 helper-call 路径支持内存比较/测试。
- **范围(≈2h 起)**：复用 5.4 的内存地址解码与 `cpu_load32` helper；只支持 32-bit、无 prefix、无段覆盖的已验证寻址形式；
  JIT 只加载内存值并写出与现有 `CMP_RR` / `TEST_RR` 等价的 lazy flags，不写内存。
- **涉及文件**：`jit_lx7.c`、`jit_selftest.c`。
- **验收**：新增差分自检覆盖 memory CMP/TEST taken/not-taken Jcc；COM19 selftest PASS；正常启动 45 秒无 WDT/panic。
- **风险**：lazy flags 是高风险区，必须比对 EFLAGS 与后续 Jcc；对未验证宽度、段覆盖、复杂 SIB 均回退解释器。
- **执行结果（2026-07-02 / COM19）**：✅ 已完成保守 helper-call 版本。新增 `ACT_CMP_RM32`、
  `ACT_CMP_MR32` 与 `ACT_TEST_MR32`，复用 5.4 的内存地址解码和 `cpu_load32` helper；helper 调用前
  同步必要 GPR 到 `CPUI386`，返回后重载块内需要的 GPR，并写出与 `CMP_RR`/`TEST_RR` 等价的 lazy flags
  状态以继续使用现有 Jcc fusion。自检新增 `CMP_RM32_JZ_taken/not`、`CMP_MR32_JB_taken/not`、
  `TEST_MR32_JZ_taken/not`，COM19 selftest-only `119/119 PASS`；正常启动 45 秒无 WDT/panic。

#### Task 5.7 — 8/16-bit memory MOV 保守扩面
- **目标**：若 5.5 显示 8/16-bit memory MOV 是热点，支持 `MOV r8/r16,[mem]` 与 `MOV [mem],r8/r16` 的 helper-call 版本。
- **范围(≈2h 起)**：先覆盖 8-bit 与 16-bit MOV 的无 prefix/必要 prefix 组合，明确处理 partial register 写入（AL/CL/DL/BL/AH/CH/DH/BH）；
  store 复用 `cpu_store8/16`，load 复用 `cpu_load8/16`。
- **涉及文件**：`jit_lx7.c`、`jit_selftest.c`，必要时增加小 helper。
- **验收**：差分自检覆盖低/高 8-bit 寄存器、16-bit 子寄存器、内存 store 后探针值；启动冒烟稳定。
- **风险**：partial register 合并容易发散；16-bit operand-size prefix 与当前 prefix 拒绝策略冲突，必须单独白名单而不是放开全部 prefix。
- **执行结果（2026-07-02 / COM19）**：✅ 已完成保守扩面。新增 `ACT_MOV_RM8`、`ACT_MOV_MR8`、
  `ACT_MOV_RM16` 与 `ACT_MOV_MR16`，load 复用 `cpu_load8/16`，store 复用 `cpu_store8/16`；仅对白名单
  `0x66 8B/89` 放开 operand-size prefix，其它 prefix 仍回退解释器。JIT emitter 负责 AL/CL/DL/BL 与
  AH/CH/DH/BH 的 partial-register 合并，以及 AX/CX/DX/BX 等 16-bit 子寄存器合并。自检新增低 8 位、
  高 8 位、16 位 load/store 用例，COM19 selftest-only `119/119 PASS`；正常启动冒烟稳定。

#### Task 5.8 — complex SIB 地址建模
- **目标**：支持 `[base + index*scale + disp]` 的常见 SIB 寻址，为后续内存热点 opcode 扩面铺路。
- **范围(≈2h 起)**：扩展 `MemOp` 记录 base/index/scale/disp；先仍只服务 MOV/CMP/TEST helper-call 路径；
  继续拒绝段覆盖和 address-size override。
- **涉及文件**：`jit_lx7.c`、`jit_selftest.c`。
- **验收**：自检覆盖 scale=1/2/4/8、无 base、无 index、disp8/disp32；复杂 SIB 未命中时安全回退。
- **风险**：`index=ESP` 在 x86 SIB 中表示 no-index，`base=EBP && mod=0` 表示 no-base + disp32，必须逐例覆盖。
- **执行结果（2026-07-02 / COM19）**：✅ 已完成当前 helper-call 消费者需要的地址建模。`MemOp`
  扩展为 base/index/scale/disp 结构，正确处理 `index=ESP` 的 no-index 语义，以及
  `base=EBP && mod=0` 的 no-base + disp32 语义；目前仅服务 MOV/CMP/TEST helper-call 路径，段覆盖和
  address-size override 继续回退解释器。自检新增 `MOV_EAX_mem_SIB_scale4` 与
  `MOV_mem_SIB_scale4_EAX`，并保留既有 `[esp+disp]`、direct `[disp32]`、moffs32 覆盖；COM19
  selftest-only `119/119 PASS`，正常启动冒烟稳定。

#### Task 5.9 — memory helper-call 到 inline fast path 的过渡
- **目标**：在 correctness 覆盖足够后，减少 5.4/5.6/5.7 helper-call 的函数调用成本。
- **范围(>2h，拆分实施)**：先识别最安全的 flat physical RAM 命中路径；只在 paging/MMIO/边界/未对齐条件都满足时内联 load/store，
  否则回退现有 helper-call；保留 helper-call 作为语义基线。
- **涉及文件**：`jit_lx7.c`，可能需要从 `i386.c` 暴露只读 fast-path helper 或共享 TLB 查询接口。
- **验收**：同一 workload 下 helper-call 与 inline fast path 差分一致；perf 相比 5.4/5.6 有可量化提升；SMC 自检仍 PASS。
- **风险**：这是 P5 最高风险优化，容易绕过 MMIO/分页/SMC/对齐语义；没有足够统计和自检前不要启动。
- **执行结果（2026-07-02 / COM19）**：⏸️ 本轮明确不启用。Task 5.5 的真实启动窗口统计显示
  最大 unsupported 热点是 `6A` (`PUSH imm8`) 1550 次，而不是 5.4/5.6/5.7 的 memory helper-call 成本；
  因此继续保留 helper-call 作为正确性基线，不引入 inline RAM fast path，避免在缺少收益证据时绕过
  paging/MMIO/SMC/边界语义。下一步更适合按 histogram 优先做栈类 opcode 扩面。

---

### 阶段 P6：Benchmark 与调优闭环

> 背景：2026-07-02 之后观察到若干窗口中 JIT 会拖慢 `ips`。这不能只用“opcode 覆盖不足”
> 一句话解释，因为当前 JIT 还叠加了 PSRAM 取指、C helper-call、block 进入/返回、translation/cache
> miss、SMC invalidation、串口 stats 输出、以及混合 JIT/解释器调度等成本。P6 的目标是先把这些成本分解成
> 可复现的数字，再决定是继续扩 opcode、优化 dispatch/linking、inline memory fast path，还是对某些块主动
> NOJIT。

#### P6.0 — 性能判据与实验纪律

- **主判据**：不要只看瞬时 `ips`。每个实验必须同时记录 wall time、guest cycles、`pc_steps`、
  `step_count`、JIT hits/misses/bailed、host buffer full、pool epoch、SMC flush、unsupported top opcode。
- **对照组**：至少保留 `JIT_LEVEL=0`、当前默认 level3、以及“只开一个候选优化/只关一个可疑功能”的 A/B 组。
- **窗口固定**：启动 benchmark 必须用固定时间点/事件点切片，例如：
  - `SeaBIOS start -> set VGA mode`
  - `set VGA mode -> Booting from 0000:7c00`
  - `Booting from 0000:7c00 -> DOS prompt/固定程序入口`
  - DOS 内部 microbench 固定循环
- **串口扰动控制**：stats dump 默认低频；做精确 benchmark 时只允许事件边界输出摘要，禁止逐块 trace。
- **结论门槛**：小于 5% 的单次差异只记录为噪声；需要 3 次重复同方向才允许写成“改善/退化”。
- **回滚规则**：任何优化若 selftest PASS 但固定 benchmark 退化超过 10%，默认 gate 关闭，留下可复现实验记录。

#### Task 6.1 — benchmark harness 与日志格式

- **目标**：建立板上可重复的 benchmark harness，让不同 JIT gate 的结果可比。
- **范围(≈2h)**：新增统一 perf snapshot 输出，记录 phase id、ms、ips、cycles、pc_steps、JIT stats、top unsupported；
  支持通过 CMake cache 标记 benchmark profile（normal boot / selftest / DOS microbench）。
- **涉及文件**：`jit_x86.h`、`jit_lx7.c`、`esp_main.c` 或现有 perf 输出点，必要时新增 `tools/bench_capture.py`。
- **验收**：同一固件连续 3 次 45 秒 capture 可自动提取 phase 表；不开 benchmark profile 时正常日志不明显变多。
- **风险**：串口本身会拖慢系统；输出必须是低频摘要，不允许每个 block 打印。

#### Task 6.2 — JIT 成本分解计数器

- **目标**：区分“JIT 执行慢”与“JIT 没命中/翻译/失效/回退导致慢”。
- **范围(≈2h)**：增加计数器：translate attempts、successful translations、cache hits、cache misses、sticky nojit hits、
  emitted bytes、x86 bytes、linked exits、helper-call actions、host_buffer_full、pool flush、SMC invalidations。
- **涉及文件**：`jit_x86.h`、`jit_lx7.c`。
- **验收**：45 秒启动日志能看出每个 phase 的 JIT 有效执行比例，例如 `jit_guest_insns / total_guest_insns`，
  并能定位退化来自 miss/bailed 还是来自 valid block 执行成本。
- **风险**：计数器更新本身也有开销；高频字段使用整数累加，避免格式化输出进入热路径。

#### Task 6.3 — 固定 DOS microbench 套件

- **目标**：用可控 guest 程序隔离不同类型的 JIT 收益/退化，不再只依赖启动窗口。
- **范围(≈2h 起)**：扩展 `tools/dosbench.asm` 或新增 DOS `.COM`，至少包含：
  - register ALU tight loop
  - branch taken/not-taken loop
  - stack `PUSH/POP/CALL/RET` loop
  - memory MOV/CMP/TEST loop
  - SMC/写代码压力小用例
- **涉及文件**：`tools/dosbench.asm`、DOS 镜像打包脚本/说明、`docs/` benchmark 记录。
- **验收**：每个 microbench 能输出固定迭代数耗时或 cycle；JIT level0/level3 至少跑 3 次并记录均值。
- **风险**：DOS 程序的计时源可能受模拟器自身影响；优先用 emulator 内部 cycle/perf 计数交叉验证。

#### Task 6.4 — gate 矩阵与二分定位

- **目标**：找出拖慢 IPS 的具体 gate，而不是把“JIT”当成一个整体开关。
- **范围(≈2h)**：把高风险 gate 拆成 CMake cache 开关，至少包括：block linking、memory helper-call actions、
  CMP/TEST+Jcc fusion、JMP、unsupported histogram、SMC bitmap/range path、future stack ops。
- **涉及文件**：`make/esp-ili9341/main/CMakeLists.txt`、`jit_lx7.c`。
- **验收**：能生成一张 A/B 表：每行一个 gate，列出 selftest、45 秒 boot phase、DOS microbench 的相对变化。
- **风险**：gate 组合爆炸；每轮只做单变量差异，禁止混合多个未知改动得出结论。

#### Task 6.5 — 热块与冷块策略

- **目标**：避免把只执行一次或频繁 bail 的冷块编译进 JIT，减少 translation/cache 成本。
- **范围(≈2h 起)**：记录每个 paddr 的解释器命中次数或 sticky nojit 次数；尝试热度阈值，例如同一 paddr 第 N 次执行才翻译；
  对 host_buffer_full/pool churn 明显的块主动 NOJIT 或缩短扫描。
- **涉及文件**：`jit_x86.h`、`jit_lx7.c`、可能涉及 `i386.c` 调度路径。
- **验收**：启动 benchmark 中 miss/translate 次数下降；wall time/ips 不低于默认策略；selftest PASS。
- **风险**：热度阈值可能延迟真正热路径加速；需要按 phase 分析，不能只看总数。

#### Task 6.6 — 调优决策表与下一步路线

- **目标**：把 P6 数据转成明确工程决策。
- **范围(≈2h)**：维护 `docs/jit-benchmark-results.md` 或本文件附录表，按收益/风险排序：
  - 继续扩 opcode（例如 5.5 已显示 `6A PUSH imm8` 热）
  - 优化 block dispatch/linking
  - inline memory fast path
  - 调低/关闭某些 gate
  - 改变块扫描长度或热度阈值
- **涉及文件**：`docs/`。
- **验收**：每个候选优化都有“证据、预计收益、风险、回退开关、验证用例”；没有数据的优化不得进入默认 gate。
- **风险**：benchmark 过拟合单一启动路径；必须同时保留 DOS microbench 与真实启动 smoke 两类证据。

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

## 2026-07-01 Execution Record (COM19, auto-run through Task 5.4)

- Task 3.1 done: enabled `ACT_JMP` with `TINY386_JIT_LEVEL=3` / `TINY386_JIT_ENABLE_JMP=1`; added rel8/rel32 JMP selftests; board selftest and normal firmware smoke passed.
- Task 3.2 done: enabled CMP_RR/CMP_RI/TEST_RR + Jcc fusion for supported cc set; added 32 taken/not-taken branch selftests; board selftest passed.
- Task 3.3 done: changed Jcc emission to range-safe inverted short branch plus long `J` to the taken epilogue; unsupported conditions bail before emitting partial branch bytes.
- Task 3.4 retry done: implemented a conservative fallthrough link-stub for already-cached successor blocks. Raw `J` between block entries remains unsafe with Xtensa windowed ABI, so the emitted linked exit uses `CALL8` with `CPUI386*` moved into outgoing arg0 (`a10`), then returns to C after the successor returns. Target invalidation also invalidates linked sources. Board selftest added `LINK_FALLTHROUGH_MOV_EAX_NOP` and passed `97/97`.
- Task 4.1 done: verified cache slot conflict and pool-full flush paths with board selftests (`CACHE_CONFLICT`, `CACHE_POOL_FULL`).
- Task 4.2 done: invalidates all JIT blocks on CR0 paging/WP/PE changes, CR3 writes, task-switch CR3 load, and CS code16/code32 transitions; selftest covers real interpreter `MOV CR3,EAX` invalidation.
- Task 4.3 done: guest RAM stores now call JIT SMC invalidation after `pstore8/16/32` on non-MMIO physical writes, including split writes.
- Task 4.4 done: added `jit_invalidate_range()` and a hashed translated-code page bitmap so normal data writes avoid scanning the JIT cache; selftests prove same-page non-overlap writes do not invalidate, overlap writes do invalidate and retranslate modified code.
- Task 5.1 done: captured COM19 perf baseline with JIT level3 and temporary level0. In this boot window both reached `set VGA mode 1`; representative level3 samples were `ips=550889` at about 5.9s, `ips=1044520` at about 10.9s, and `ips=875941` at about 20.9s. Level0 samples were effectively similar for this workload, indicating current opcode coverage still misses the dominant DOS hot path.
- Task 5.2 done: prologue/epilogue now use conservative per-block GPR masks; only read GPRs are loaded and only written GPRs are stored. This preserves the existing function ABI and remains compatible with the Task 3.4 `CALL8` link-stub.
- Task 5.3 done: enabled `XCHG EAX,r32` at level3 and added `XCHG_EAX_EBX` / `XCHG_EAX_EDI` differential selftests.
- Task 5.4 first slice done: enabled simple `MOV r32,[base+disp]` and `MOV [base+disp],r32` through conservative C helpers that reuse `cpu_load32` / `cpu_store32`; added `MOV_EAX_mem_EBX_disp8` and `MOV_mem_EBX_disp8_EAX` differential selftests.
- Verification: final selftest-only firmware on COM19 reported `99/99 PASS`; final normal firmware build flashed over COM19 and reached `Booting from 0000:7c00` and `set VGA mode 1` within a 45s capture with no WDT or panic.

### Per-task Results And Notes

| Task | Result | Validation | Notes |
| --- | --- | --- | --- |
| 3.1 JMP | Done | Added `BLOCK_JMP_REL8` and `BLOCK_JMP_REL32`; board selftest passed; normal firmware reached DOS boot path. | `TINY386_JIT_LEVEL` and `TINY386_JIT_ENABLE_JMP` are now CMake cache variables so level/gate changes can be tested without editing source. |
| 3.2 CMP/TEST + Jcc | Done | Added 32 branch differential cases covering CMP_RR, CMP_RI, TEST_RR with taken/not-taken outcomes; board selftest passed. | Enabled supported cc set only. Unsupported or unfused conditions still fall back to interpreter rather than emitting unsafe native branches. |
| 3.3 Branch range fallback | Done | Existing branch selftests stayed green after emitter rewrite. | Jcc now emits inverted short branch over a long `J` to the taken epilogue, avoiding oversized direct branch offsets when epilogues grow. |
| 3.4 Direct block linking | Done, conservative first slice | Added `LINK_FALLTHROUGH_MOV_EAX_NOP`; final selftest `97/97 PASS`; normal firmware reached `Booting from 0000:7c00` and `set VGA mode 1`. | Uses `CALL8` link-stub for already-cached fallthrough successors instead of raw `J` between block entries; target invalidation clears linked sources. Taken Jcc and unresolved-exit patching remain future work. |
| 4.1 Cache conflict / pool full | Done | Added `CACHE_CONFLICT` and `CACHE_POOL_FULL`; final selftest included both in `96/96 PASS`. | Selftest wrappers expose pool epoch and forced pool usage only for ESP32 JIT test builds. |
| 4.2 CR3 / paging / code-size invalidation | Done | Added `STATE_CR3_INVALIDATE`; final selftest included it in `96/96 PASS`. | Hooks invalidate all JIT blocks on CR0 paging/WP/PE changes, CR3 writes, task-switch CR3 load, and CS code16/code32 changes. |
| 4.3 Store-triggered SMC invalidation | Done | Covered by SMC overlap selftest; final selftest included it in `96/96 PASS`. | Store hooks run after non-MMIO guest RAM `pstore8/16/32`, including split writes. MMIO writes are left to device callbacks. |
| 4.4 Precise range invalidation | Done | Added `SMC_RANGE_NONOVERLAP` and `SMC_RANGE_OVERLAP`; final selftest included both in `96/96 PASS`. | Added `jit_invalidate_range()` plus a hashed translated-code page bitmap. First naive store-scan path slowed boot badly; bitmap filter restored normal boot speed. |
| 5.1 Performance baseline | Done | Captured COM19 level3 and temporary level0 45s boot windows; both reached `set VGA mode 1`. | Representative level3 samples: about `550889 ips` at 5.9s, `1044520 ips` at 10.9s, `875941 ips` at 20.9s. Level0 was similar in this workload, so current coverage is not yet hitting the dominant DOS hot path. |
| 5.2 Minimal GPR load/store | Done | Final selftest `96/96 PASS`; final normal firmware reached `set VGA mode 1`. | Prologue loads only read GPRs; epilogue stores only written GPRs. This keeps the existing function ABI and is compatible with the Task 3.4 `CALL8` link-stub. |
| 5.3 Opcode coverage | Done | Added `XCHG_EAX_EBX` and `XCHG_EAX_EDI`; final selftest `96/96 PASS`; final normal firmware reached `set VGA mode 1`. | Runtime gate now enables `XCHG EAX,r32` at level3. Next useful candidates should come from bail/hot-path stats rather than guessing. |
| 5.4 Memory operands | First slice done | Added `MOV_EAX_mem_EBX_disp8` and `MOV_mem_EBX_disp8_EAX`; final selftest `99/99 PASS`; final normal firmware reached `set VGA mode 1`. | Supports only simple 32-bit MOV `[base+disp]` forms through helper calls. Complex addressing, narrower widths, memory ALU/CMP/TEST, and inline TLB/page-walk remain future slices. |

## 2026-07-02 Execution Record (COM19, Task 5.5 through conservative 5.8)

- Task 5.5 done: added a lightweight unsupported-opcode histogram keyed by one-byte opcodes and `0F xx` extended opcodes. It is gated by `TINY386_JIT_UNSUPPORTED_HIST` and dumped through the existing low-frequency stats path. A 45s normal boot capture reported `unsupported_opcode_total 1579`, with top entries led by `6a` (`1550` hits), then `4a`, `02`, `40`, `61`, `c3`, `c6`, and `35`.
- Task 5.6 done: added conservative helper-call `CMP r32,[mem]`, `CMP [mem],r32`, and `TEST [mem],r32` actions using `cpu_load32`, with lazy flags and existing native Jcc fusion preserved. Board selftests cover taken/not-taken Jcc cases.
- Task 5.7 done: added helper-call 8-bit and 16-bit memory MOV load/store forms, including low/high 8-bit registers and 16-bit operand-size prefix forms. Store uses `cpu_store8/16`; load uses `cpu_load8/16` and JIT-side partial-register merge.
- Task 5.8 done: expanded `MemOp` to model base/index/scale/disp SIB addressing, including `index=ESP` as no-index and `base=EBP && mod=0` as no-base + disp32. Current consumers are the conservative memory MOV/CMP/TEST helper-call paths.
- Task 5.9 decision: not enabled. The new 5.5 histogram shows the dominant miss is `PUSH imm8` (`6A`), not memory helper-call overhead, so inline memory fast paths remain deferred to avoid bypassing paging/MMIO/SMC semantics without evidence.
- Verification: COM19 selftest-only firmware reported `119/119 PASS`. Normal firmware was rebuilt and app-flashed back to COM19; a 45s capture reached SeaBIOS, `set VGA mode 3`, `Booting from 0000:7c00`, and periodic perf/stats output with no WDT or panic.

## 2026-07-02 Execution Record (P6 benchmark and tuning loop)

- Task 6.1 done: added a low-frequency `[bench]` perf snapshot path, gated by `TINY386_BENCH_PROFILE`. The snapshot includes phase id, wall time, ips, guest cycle delta, `pc_steps`, `step_count`, JIT hit/miss counters, translation counters, SMC/pool counters, helper/link counters, emitted bytes, and unsupported total. Normal profile `0` keeps the existing log volume.
- Task 6.2 done: split JIT costs into counters for translate attempts, successful translations, cache misses, sticky NOJIT hits, JIT guest instructions, emitted x86/host bytes, linked exits, helper-call actions, host-buffer-full, and pool flushes.
- Task 6.3 done: expanded `tools/dosbench.asm` into fixed ALU, branch, stack, memory, and SMC cases with stable `BENCH_CASE` output. NASM assembled it locally to a 987-byte `.COM`.
- Task 6.4 done: added CMake cache A/B gates for block linking, memory helper actions, CMP/TEST+Jcc fusion, SMC bitmap prefilter, benchmark profile selection, and unsupported histogram.
- Task 6.5 done as a measurement hook rather than default policy change: the new counters can show miss/translate churn and sticky NOJIT pressure; no hot/cold threshold is enabled by default until repeated captures prove it helps.
- Task 6.6 done: added `docs/jit-benchmark-results.md` with protocol, capture commands, gate matrix, current optimization decisions, and an empty results matrix for repeated runs.
- Tooling: added `tools/bench_capture.py`, which can capture serial or parse existing logs and emit a CSV phase table. It understands both new `[bench]` snapshots and older `[perf]` + `[jit_stats]` output.
- Verification: default ILI9341 firmware builds successfully with ESP-IDF 5.5.1. A separate benchmark build also succeeds with `-DTINY386_BENCH_PROFILE=2 -DTINY386_JIT_ENABLE_LINKING=0`, proving the profile/gate chain works. Local parser sample and DOS bench NASM assembly passed.
- Board note: this workstation currently enumerates COM101 and Bluetooth COM11, not the historical COM19 port, so no app-flash or 45-second board capture was run in this slice. The P6 harness is ready for the next board capture once the active board port is confirmed.
- COM19 follow-up: COM19 reappeared as `USB-SERIAL CH340K`. The first app-flash/capture hit an early `debugcon` task stack overflow before BIOS; `DEBUGCON_TASK_STACK` was raised from 2048 to 4096 and the benchmark firmware was rebuilt/flashed. A 45s COM19 capture then reached SeaBIOS, `set VGA mode 3`, `Booting from 0000:7c00`, and emitted `[bench]` snapshots with no panic/WDT. Capture artifacts: `serial_COM19_p6_bench_20260702_090304.log` and `.csv`.
- COM19 P6 sample result (`TINY386_BENCH_PROFILE=2`, `TINY386_JIT_ENABLE_LINKING=0`): at about 40.0s, `ips=10665`, `cycles=53326`, `pc_steps=304320`, `jit_hits=189`, `jit_misses=1859`, `translate_attempts=1613`, `translated=25`, `bailed=1588`, `sticky_nojit=271`, `host_buffer_full=8`, `smc_flushes=430`, `invalidations=25`, `helper_actions=14`, `unsupported_opcode_total=1442` in the snapshot; the later periodic stats dump showed `unsupported_opcode_total=1580`, led by `6a` with 1551 hits. This reinforces the P5.9 decision that `PUSH imm8` is the next evidence-backed opcode target, while inline memory fast path remains deferred.

### P6 Per-task Results And Notes

| Task | Result | Validation | Notes |
| --- | --- | --- | --- |
| 6.1 Benchmark harness / log format | Done | Added `[bench]` snapshots gated by `TINY386_BENCH_PROFILE`; COM19 45s capture produced parseable CSV rows. | Normal profile `0` preserves the old low-noise boot path; benchmark profile `2` labels snapshots as `dosmicro`. |
| 6.2 JIT cost counters | Done | COM19 snapshot showed hits/misses, translate attempts, translated blocks, bails, sticky NOJIT, SMC, helper actions, emitted bytes, and unsupported total in one line. | Counters make the current slowdown explainable: late-window execution is dominated by unsupported `6A` churn, not valid-block execution volume. |
| 6.3 DOS microbench suite | Done, source-level suite | `tools/dosbench.asm` assembles locally with NASM to a 987-byte `.COM`. | Includes ALU, branch, stack, memory, and SMC cases. It still needs packaging into the DOS image for repeatable in-guest case timing. |
| 6.4 Gate matrix / bisection | Done | Separate build succeeded with `-DTINY386_BENCH_PROFILE=2 -DTINY386_JIT_ENABLE_LINKING=0`; COM19 app-flash and capture were successful. | Added gates for linking, memory helpers, CMP/TEST+Jcc, SMC bitmap, benchmark profile, and unsupported histogram. Each future comparison should change one gate at a time. |
| 6.5 Hot/cold block strategy | Measurement hooks done, policy deferred | Capture exposes miss/translate/bail/sticky-NOJIT pressure; no hotness threshold was enabled. | Current evidence points first to stack opcode coverage (`PUSH imm8`) rather than a hot/cold translation policy change. |
| 6.6 Tuning decision table | Done | Added `docs/jit-benchmark-results.md` and recorded the COM19 result row. | Decision table keeps unsupported `6A PUSH imm8` as the next evidence-backed opcode target; inline memory fast path remains gated off until helper cost dominates a repeated benchmark. |

## P7 Stabilization And Evidence-First Optimization Plan (2026-07-02)

### P0 - Decide Whether JIT Is Negative During DOS

#### Task 7.0 - Fixed-phase level0 vs level3 A/B

- **Goal**: Answer whether JIT is a net loss in the DOS phase before adding more opcode coverage.
- **Scope**: Use event slices instead of wall-clock points: `set VGA mode` -> `Booting from 0000:7c00` -> DOS prompt or the closest stable DOS marker currently available. Run level0 and level3 three times each, then compare the DOS-phase mean wall time.
- **Dependencies**: Keep the board port stable with automatic active COM detection, and keep the `debugcon` stack fix in the default configuration.
- **Acceptance**: A small table with 3x level0 and 3x level3 captures, per-phase wall time, final mean, and the decision: opcode coverage first vs churn reduction first.
- **Risk**: Time-window captures can mislead; phase markers are the source of truth.

### P1 - Stop Wasting Translation Work

#### Task 7.1 - Persistent NOJIT and hotness threshold

- **Goal**: Eliminate repeated scans for blocks that are already known to fail translation.
- **Scope**: Investigate why the P6 sample had `1588` bails but only `271` sticky NOJIT hits. Make unsupported-opcode entry paddr values immediately and persistently NOJIT even when the direct-mapped block cache slot is later reused. Add a configurable hotness threshold so cold/one-shot paddr values are interpreted until their Nth hit.
- **Implementation note**: The first slice adds a separate NOJIT table and `TINY386_JIT_HOT_THRESHOLD` cache variable, defaulting to `2`. Selftest-only and selftest-at-boot builds bypass the threshold so differential tests still exercise JIT on their first step.
- **Acceptance**: Same workload shows clearly lower `translate_attempts` and `bailed`, `sticky_nojit` rises relative to unsupported repeats, IPS does not regress, and selftest passes.
- **Risk**: A stale NOJIT entry can suppress future translation after SMC/code replacement. Correctness is preserved because execution falls back to the interpreter, but performance evidence must be interpreted with this in mind.

#### Task 7.2 - SMC flush attribution

- **Goal**: Separate SMC bitmap false positives from true translated-code overlap.
- **Scope**: Count bitmap misses, bitmap-hit scans, scans with no overlap, and scans that actually invalidate translated blocks.
- **Acceptance**: Benchmark snapshots explain whether the P6 `smc_flushes=430` sample was mostly harmless page/hash noise or real self-modifying-code churn.
- **Risk**: More counters are not a policy by themselves. If true overlap dominates, the next task should mark high-frequency self-modifying pages NOJIT instead of repeatedly translating them.

### P2 - Evidence-backed Stack Opcode Coverage

#### Task 7.3 - `PUSH imm8` first

- **Goal**: Cover the strongest unsupported-opcode signal: `6A PUSH imm8`, which represented about 98% of unsupported hits in the P6 sample.
- **Scope**: Start with a conservative helper-call path: sign-extend imm8, decrement ESP by 4, store through the existing CPU memory helper path, and update ESP explicitly.
- **Acceptance**: Differential selftests cover ESP update, sign extension, SS/stack edge behavior, and stack writes that cross pages; A/B captures must show benefit before enabling by default.
- **Risk**: Stack writes can interact with SMC invalidation when stack and code pages alias or are reused by DOS code.

#### Task 7.4 - `PUSH r32` / `POP r32`

- **Goal**: Extend stack coverage only after `PUSH imm8` is proven useful.
- **Scope**: Use the same conservative helper-call model as Task 7.3.
- **Acceptance**: Differential selftests for all GPRs, especially ESP, and a repeated DOS stack microbench result.

#### Task 7.5 - `CALL rel` / `RET`

- **Goal**: Cover control-flow stack operations after simple stack data movement is stable.
- **Scope**: Treat as a control-flow/block-exit task, not just a stack store/load task. Keep block linking interactions explicit and conservative.
- **Acceptance**: Differential selftests for next_ip, return target, ESP, and nested call/ret smoke cases.
- **Risk**: This touches block boundaries and C/JIT dispatch semantics, so it should not be bundled with PUSH/POP.

### P3 - Structural Optimizations After Evidence

#### Task 7.6 - Expanded block linking

- **Goal**: Add taken-Jcc linking and miss patch-back only if A/B data shows C dispatch overhead is a bottleneck.
- **Acceptance**: Gate-controlled comparison proves benefit without destabilizing invalidation.

#### Task 7.7 - Inline memory fast path remains deferred

- **Goal**: Keep Task 5.9 deferred until helper-call cost dominates repeated captures.
- **Rationale**: The P6 sample had only `helper_actions=14`; bypassing paging/MMIO/SMC semantics is not justified by current evidence.

## 2026-07-02 Execution Record (P7 first slice)

- P7 plan added: P0 fixed-phase A/B, P1 translation-churn stop-loss, P2 stack opcode coverage, and P3 deferred structural optimizations are now recorded as Tasks 7.0 through 7.7.
- Task 7.1 first slice done: added an independent NOJIT table so unsupported paddr entries survive direct-mapped block-cache slot reuse; added `TINY386_JIT_HOT_THRESHOLD`, default `2`, to skip translating one-shot cold blocks. Selftest-only and selftest-at-boot builds bypass the threshold so differential tests still JIT on their first step.
- Task 7.2 first slice done: SMC invalidation now counts bitmap misses, bitmap-hit scans, scans with no actual overlap, and scans that really invalidated translated code.
- Tooling updated: `tools/bench_capture.py` now keeps P7 counters in stable CSV columns, parses `[123 ms] [perf] ...` lines, and auto-detects UTF-16 logs produced by PowerShell `Tee-Object`.
- Correctness fix found during P7 board selftest: `ACT_MOV_RM8` and `ACT_MOV_RM16` now include the destination GPR in the read mask before partial-register merge. Without this, `MOV AH,[mem]` could clear AL when the old EAX value was not loaded.
- Verification: default ILI9341 firmware builds successfully. Selftest-only firmware builds, app-flashes on COM19, and reports `119/119 PASS`.
- Normal smoke: default firmware was app-flashed back to COM19. A 45s capture reached SeaBIOS, `Booting from 0000:7c00`, and `set VGA mode 1` with no WDT/panic. Artifacts: `serial_COM19_p7_selftest_20260702.log`, `serial_COM19_p7_smoke_20260702.log`, and `serial_COM19_p7_smoke_20260702.csv`.
- Task 7.0 done: built and flashed dedicated benchmark-profile firmware for level3 and level0, then captured three 45s COM19 runs for each. Level3 marker timings were VGA3 -> boot sector `7279`, `7280`, `7280` ms and did not reach `set VGA mode 1` in the 45s window. Level0 marker timings were VGA3 -> boot sector `7278`, `7278`, `7278` ms and VGA3 -> VGA1 `19905`, `19906`, `19906` ms.
- P7.0 decision: boot-sector timing is effectively tied, but the DOS post-boot phase is still negative for current level3 because level0 reaches `set VGA mode 1` reliably while level3 does not. P7.1 did solve the translation-churn symptom: final level3 benchmark snapshots had only `26` translate attempts, `16` translations, and `10` bails, versus the P6 sample's `1613` attempts and `1588` bails. The next useful work remains P2 stack opcode coverage, starting with `6A PUSH imm8`, rather than structural linking or inline memory fast paths.
- SMC attribution from the A/B run: level3 final snapshots had `302` SMC scans and all were false positives, with `0` true overlap invalidations; level0 had no JIT SMC scans. This argues for keeping the bitmap prefilter and avoiding an SMC policy change until a workload shows real overlap churn.
- P7.0 artifacts: `serial_COM19_p7_l3_run1_20260702.log/.csv/.phase.csv` through `run3`, and `serial_COM19_p7_l0_run1_20260702.log/.csv/.phase.csv` through `run3`.

## 2026-07-02 Execution Record (P7.3 PUSH imm8)

- Task 7.3 implementation done: added `ACT_PUSH_IMM8` for opcode `6A`, sign-extending imm8 and writing a 32-bit value through an SS stack helper. The helper decrements ESP by 4, stores with `cpu_store32(..., SEG_SS, ...)`, then commits ESP only if the store succeeds.
- Added `TINY386_JIT_ENABLE_PUSH_IMM8`, default `0`, and a CMake cache variable of the same name. Selftest whitelisting can still exercise the action regardless of the default runtime gate.
- Differential selftest: selftest-only firmware built, app-flashed on COM19, and reported `122/122 PASS`. New cases cover positive imm8, negative imm8 sign extension, and an unaligned ESP stack write. Artifact: `serial_COM19_p7_push_selftest_20260702.log`.
- Benchmark gate result: with `TINY386_BENCH_PROFILE=2`, level3, and `TINY386_JIT_ENABLE_PUSH_IMM8=1`, three 45s runs all reached boot sector at VGA3 -> boot `7280 ms` and none reached `set VGA mode 1`. Final snapshot means were `translate_attempts=1236`, `translated=1228`, `bailed=8`, `helper_actions=1218`, `smc_flushes=2110`, and `unsupported_total=6`; `6A` disappeared from the unsupported list.
- P7.3 decision: keep `PUSH imm8` disabled by default. The opcode implementation is correct in isolation, but enabling it alone converts the old unsupported `6A` hotspot into helper-call stack translation churn without improving the fixed-phase result. The next stack work should not simply turn on more helper-call stack opcodes by default; it needs either a broader stack-op bundle behind a gate, a stack-specific hotness/NOJIT policy, or lower-cost stack memory handling.
- P7.3 artifacts: `serial_COM19_p7_push_l3_run1_20260702.log/.csv/.phase.csv` through `run3`.

## 2026-07-02 Execution Record (P7 fixed-phase gate bisect)

- Level ladder, 3 runs each, fixed markers VGA3 -> boot sector -> VGA1: level0/1/2 all reached VGA1 with means `19906.0`, `19905.3`, and `19912.0 ms`; level3 reached boot at mean `7280.0 ms` but missed VGA1 in all three 45s captures. This localizes the DOS-stage net negative to level3 gates rather than the generic JIT prestep entry.
- Counter split: level2 ended around `5` translated blocks and `87` SMC scans while still reaching VGA1; level3 ended around `15-16` translated blocks, `5-6` helper actions, and `302` SMC scans and failed to reach VGA1.
- Single-variable gate: `TINY386_JIT_ENABLE_MEM_HELPERS=0` with level3 restored VGA1 in all 3 runs. Mean VGA3 -> boot was `7282.0 ms`; mean VGA3 -> VGA1 was `19875.7 ms`; final snapshots were stable at `166` JIT hits, `1328-1331` misses, `24/11/13` translate/trans/bail, `0` helper actions, and `293` SMC scans.
- Decision: helper-call memory actions are the prestep/cold-translation tax for the DOS phase. Change the default gate to `TINY386_JIT_ENABLE_MEM_HELPERS=0`, preserving the code behind an explicit experiment switch. This cuts cold helper translation cost to zero in the default firmware while keeping CMP/TEST+Jcc and other level3 non-helper coverage available.
- Default verification: a fresh benchmark build with no explicit mem-helper override matched the gated run in 3/3 captures: VGA3 -> boot `7282.0 ms`, VGA3 -> VGA1 `19876.0 ms`, final `24/11/13` translate/trans/bail, `helper_actions=0`, and `smc_flushes=293`.
- Normal-profile restore/smoke: flashed `TINY386_BENCH_PROFILE=0` default firmware back to COM19 and captured 45s. It reached boot at `8308 ms`, VGA1 at `20981 ms`, and had no panic/Guru/WDT.
- Artifacts: `serial_COM19_p7_bisect_l0_run1_20260702.log/.csv/.phase.csv` through level3 run3, plus `serial_COM19_p7_gate_mem0_run1_20260702.log/.csv/.phase.csv` through run3.

## 2026-07-02 Execution Record (P7 DOS image microbench and high-risk gates)

- DOS image packaging done: added `tools/inject_dosbench.py`, assembled `tools/dosbench.asm` with NASM to a 987-byte `DOSBENCH.COM`, injected it into both `release/dos.img` and `release/esp/dos.img`, and patched `FDAUTO.BAT` to auto-run it when present. The injector is MBR/FAT16 aware and keeps `.bak` image backups outside the committed set.

- Bench capture updated: `tools/bench_capture.py` now parses `BENCH_START`, `BENCH_CASE <name> <hex_ticks>`, and `BENCH_END`, and keeps the new P7 counters in stable CSV columns: block entry/exit counts, interpreter exits, lookup/translate/exec cycle buckets, and guest PSRAM pointer/scan buckets.

- Fixed-phase 3-run DOSBENCH result with JIT disabled (`TINY386_JIT_LEVEL=0`): VGA3->VGA1 was `19906`, `19906`, `19904 ms`; DOSBENCH ticks were stable at ALU `322-323`, BRANCH `289-290`, STACK `40-41`, MEM `2-3`, SMC `0-1`.

- Fixed-phase 3-run level3 default result with helper memory still disabled: VGA3->VGA1 was `19876`, `19880`, `19877 ms`; DOSBENCH ticks matched the interpreter within noise. Final snapshots showed only about `169-171` block executions over the capture and about `5900` interpreter exits. `lookup_cycles` dominated at about `20.9M`, while `exec_cycles` was only about `8-9K`; guest PSRAM address setup and scan were tiny (`guest_ptr_cycles` below `400`, `guest_scan_cycles` below `47K`). Conclusion: current JIT does not materially beat the interpreter inside DOSBENCH because coverage is too low; the remaining tax is mostly block lookup / fallback, not PSRAM guest-byte fetch.

- Experimental `TINY386_JIT_ENABLE_INLINE_MEM=1` was added for direct 32-bit `MOV/CMP/TEST` guest-memory operations. Inline-only booted and ran DOSBENCH once: VGA3->VGA1 `19882 ms`, ticks ALU `323`, BRANCH `289`, STACK `40`, MEM `3`, SMC `0`. This proves the direct 32-bit path can be exercised, but it did not move the DOSBENCH needle.

- Experimental `TINY386_JIT_ENABLE_STACK_FASTPATH=1` was added for direct `PUSH imm8` stack stores. Stack-fastpath-only failed the DOS phase twice: captures reached the boot sector but not VGA1 or DOSBENCH, while repeatedly translating and invalidating blocks (`hits/translations/invalidations` climbed into the hundreds of thousands). The unsafe shortcut skips SS base, stack mask/limit, page/MMIO behavior, and SMC store semantics. It remains default-off and should not be used as the next performance path without a safer segmented stack address model.

- P7 decision after high-risk gate bisect: keep default firmware conservative (`MEM_HELPERS=0`, `INLINE_MEM=0`, `STACK_FASTPATH=0`, `PUSH_IMM8=0`). Inline memory is a usable experiment switch but not yet a win. Stack fast path is the current correctness hazard. The next useful optimization is reducing lookup/fallback tax and increasing translated coverage with safe stack semantics, not bypassing memory helpers globally.

## 2026-07-02 Execution Record (P7 IRAM emitter/macro probe)

- Implementation probe: converted the LX7 byte-emitter hot helpers from inline functions to macro expansion and moved translation/emission hot functions into IRAM, including `decode_x86_insn`, `emit_action`, `jit_action_enabled`, `emit_movi32`, addressing helpers, and GPR save/load emitters. `jit_translate` was already in IRAM.

- Build and board result: `build_dosbench_l3_iram_macro` built and flashed on COM19, then ran three 90s DOSBENCH captures without panic/WDT. The phase result was effectively unchanged: VGA3->VGA1 `19877`, `19880`, `19878 ms` versus the previous default level3 mean `19877.7 ms`. DOSBENCH ticks stayed in noise range: ALU `323`, BRANCH `289`, STACK `40-41`, MEM `2-3`, SMC `0-1`.

- Counter result: this does reduce translation-side cost, but not enough to affect the workload. Mean `translate_cycles` dropped from about `114750` to `65031`, and `guest_scan_cycles` from about `43382` to `8208`; however `lookup_cycles` stayed around `21M` per capture and dominates the remaining JIT tax. Conclusion: keep the change as a low-risk translation-cost cleanup, but the next performance work still needs lower lookup/fallback pressure or more useful translated coverage.
