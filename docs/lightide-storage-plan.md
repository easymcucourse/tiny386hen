# LightIDE 存储路径完整方案（ESP32-S3 + Flash / SD 直读）

> 目标：在 `refs/tiny386/ide.c`（及 ESP 侧 BlockDevice 后端）上，为 **ESP32-S3** 建立 **介质直读、少拷贝、条带化 I/O** 的 IDE 硬盘/CD 路径，减少 PSRAM/中间缓冲与 CPU 搬运，提升 DOS/Windows 启动与顺序读吞吐。
>
> 本文档参照 [`lightvga-display-plan.md`](lightvga-display-plan.md) 的结构，汇总 LightIDE 功能范围、现状数据流、直读架构、条带缓冲、实施阶段与验收标准。**实施前以本文为准。**

---

## 0. 范围与约束

### 0.1 LightIDE 功能（相对 `ide.c` 全量）

| 项目 | 行为 |
|------|------|
| 支持设备 | IDE 主/从 HDD（`hda`–`hdd`）、CD-ROM（`cda`–`cdd`） |
| ATA 命令 | 保持现有：`WIN_READ` / `WIN_WRITE` / `WIN_MULTREAD` / `WIN_MULTWRITE` / `WIN_IDENTIFY` / `WIN_SETMULT` 等 |
| ATAPI | 保持现有 CD 命令集（`GPCMD_READ_10` 等）；CISO 压缩镜像可选（`IDE_ENABLE_CISO`） |
| 平台 | 仅 ESP32-S3 固件路径（`BUILD_ESP32`）；桌面参考实现不改 |
| 写路径 | HDD 可写（SD raw / flash 分区 RW）；CD/CISO 只读 |
| ini 介质 URI | **`flash:label`**、**`sdmmc:raw`** / **`sdspi:raw`**、**`sdmmc:raw@LBA`**；FAT 文件路径作兼容回退 |

### 0.2 不可删除的部分

**`IDEState.io_buffer` 与 PIO 状态机不能去掉**：Guest 通过 0x1F0 数据端口、`insw`/`outsw` 按字传输；BIOS/DOS 依赖 DRQ/IRQ 时序。可 **缩小常驻缓冲、改为外部条带懒分配、或对直读后端启用零拷贝视图**，不可删除 IDE 寄存器语义。

**不能把整盘镜像 mmap 进 Guest `phys_mem`**：磁盘容量可达数百 MB，必须 **按需扇区/条带** 映射或 DMA 读入，不能像 BIOS 那样一次性 `partition_mmap_copy` 到 RAM。

### 0.3 相关源文件

| 文件 | 角色 |
|------|------|
| `refs/tiny386/ide.c` | IDE 模拟、`BlockDevice`、PIO 传输、`io_buffer` |
| `refs/tiny386/ide.h` | 对外 API |
| `src/tiny386/pc.c` | IDE I/O 端口、`io_read_string` 挂接 |
| `src/tiny386/i386.c` | `rep insw/outsw` 字符串 I/O 快路径 |
| `src/esp/main/flash_stdio.c` | `flash:` 路径的 `fopen/fread` 拦截 |
| `src/esp/main/storage.c` | SD 初始化、`rawsd` 全局 |
| `src/esp/main/esp_main.c` | `partition_mmap_copy`、Flash MMU 读策略 |
| `make/esp-ili9341/tiny386.ini` | `hda = sdspi:raw` 等配置 |

---

## 1. 现状与问题

### 1.1 内存（每 IDE 通道 × 每盘）

| 缓冲 | 位置 | 大小 | 说明 |
|------|------|------|------|
| `IDEState.io_buffer` | `IDEState` 内（`pcmalloc`） | **2052 B** | `MAX_MULT_SECTORS(4)×512+4`；所有读写的唯一 staging |
| `BlockDeviceFile`（FAT 文件） | `FILE*` + 可选 `sector_table` | 0～整盘 | snapshot 模式按扇区 `pcmalloc` 复制 |
| `BlockDeviceCISO` | 堆 | **2176 B** `cmp_buf` | 每 2048B CD 扇区解压 |
| Flash stdio 写缓存 | 栈/结构体 | **4096 B** | 仅写路径；读走 `esp_partition_read` |

### 1.2 每扇区读数据流（现状）

#### A. Flash 分区镜像（`flash:disk.img` → `fopen` → `bf_read_async`）

```
Guest WIN_READ / MULTREAD
       │
ide_sector_read → bf_read_async
       │
fseeko + fread (flash_stdio: esp_partition_read)     512B～2KB 读 Flash
       │
写入 IDEState.io_buffer                             staging #1
       │
Guest insw / rep insw：
  慢路径：ide_data_readw × N                         逐 word 读 io_buffer
  快路径：ide_data_read_string → memcpy              io_buffer → phys_mem   staging #2
```

#### B. SD 裸分区（`BlockDeviceESPSD`，需 `/dev/mmcblk0`）

```
sdmmc_read_sectors(card, buf=io_buffer, ...)        SD → io_buffer
       │
ide_data_read_string / insw                          io_buffer → phys_mem
```

#### C. FAT 上的 `.img`（`/sdcard/disk.img`）

```
FAT + VFS → fread                                   额外文件系统层 + 可能非对齐
       │
io_buffer → phys_mem                                同上
```

### 1.3 `rep insw` 快路径（已有，未与直读联动）

`i386.c` 在 **页对齐、非 iomem、`io_read_string` 可用** 时，对端口 **0x1F0** 的 `rep insw` 一次 bulk 调用：

```c
cpu->cb.io_read_string(io, port, cpu->phys_mem + guest_paddr, 1, count);
```

对应 `ide_data_read_string` → `memcpy(buf, io_buffer + data_index, len)`。

**瓶颈**：数据必须先进入 `io_buffer`；无法 **SD/Flash → Guest phys_mem 一次拷贝**。

### 1.4 ini 与实现对齐缺口

| ini 配置 | 代码实际 |
|----------|----------|
| `hda = sdspi:raw` | `ide_attach` 仅识别 **`/dev/mmcblk0`**，**未识别 `sdspi:raw`** |
| `hda = flash:disk.img` | 走 **`BlockDeviceFile` + fread**，非 mmap 直读 |
| `mult_sectors = 4` | 单次最多 **4 扇区（2 KB）**，启动时 IRQ/命令次数偏多 |

### 1.5 瓶颈归纳

1. **Flash 读双拷贝**：`esp_partition_read` → `io_buffer` → `phys_mem`（`rep insw` 时）
2. **`io_buffer` 仅 2 KB**：`MAX_MULT_SECTORS=4`，顺序读 IRQ 与命令开销大
3. **FAT 文件镜像多一层**：簇链、缓存、与 SD 争用
4. **无后端能力协商**：所有后端统一 `read_async(..., buf, n)`，无法表达「本段已在 mmap 中」
5. **Flash 直接读与多核 D-cache**：`esp_main.c` 已说明 `esp_partition_read` 可能与其他核 PSRAM 争用；**磁盘热路径应优先 `esp_partition_mmap` 映射读**（与 BIOS 策略一致）
6. **`sdspi:raw` 未接线**：配置意图是 SD 裸 LBA，代码未实现

---

## 2. 目标架构

### 2.1 原则

| 原则 | 说明 |
|------|------|
| **介质直读** | Flash 分区 **mmap**；SD **sdmmc_read_sectors 直写目标缓冲**；避免 FAT 热路径 |
| **单次拷贝上限** | 顺序读：**介质 → Guest phys_mem** 或 **介质 → 条带 → phys_mem**，去掉多余 staging |
| **条带缓冲（路径 C）** | 大条带（8～32 扇区）替代固定 2 KB `io_buffer`；**SRAM 优先、PSRAM 回退** |
| **零拷贝视图（Flash）** | mmap 后端对命中扇区返回 **指针**，PIO/`read_string` 从映射地址读，**不 memcpy 进条带** |
| **保留 PIO 语义** | DRQ/IRQ/`insw` 逐字路径不变；优化 **数据源**，不改 Guest 可见行为 |
| **写路径单独策略** | 写必须 staging（Guest → 条带 → 介质）；读优化为主 |

### 2.2 目标数据流

```
                    ┌──────────────────────────────────────────┐
  Guest             │  phys_mem（PSRAM，8MB 等）               │
  rep insw / movsw  │  ← 目标：DOS 缓冲区 / FAT 驱动 buffer     │
                    └──────────────▲───────────────────────────┘
                                   │ ① 理想：单次 memcpy 或零拷贝
                    ┌──────────────┴───────────────────────────┐
                    │  LightIDE 传输层                          │
                    │  · 零拷贝：data_ptr = mmap + sector*512   │
                    │  · 条带：stripe[n*512] → read_string      │
                    │  · 小命令：io_buffer 2KB（IDENTIFY 等）   │
                    └──────────────▲───────────────────────────┘
                                   │
          ┌────────────────────────┼────────────────────────┐
          │                        │                        │
   ┌──────▼──────┐         ┌───────▼───────┐        ┌───────▼───────┐
   │ Flash mmap  │         │ SD raw        │        │ FAT .img      │
   │ esp_partition_mmap      sdmmc_read_sectors   │ fread (回退)   │
   │ 只读、按 VA 读          │ 直写 stripe/phys     │ 兼容 ini      │
   └─────────────┘         └───────────────┘        └───────────────┘

  删除/避免：Flash esp_partition_read  per-sector、FAT 热路径、整盘 RAM 镜像
```

---

## 3. 介质 URI 与后端选择

### 3.1 推荐 ini 语法

```ini
[pc]
; 只读 Flash 分区（整分区视为 RAW 512B 扇区镜像）
hda = flash:diskimg

; SD 卡整盘 LBA0 起（SDMMC 或 SDSPI，由 storage.c 决定）
hda = sdmmc:raw
hda = sdspi:raw

; SD 卡分区偏移（MBR 后第一个分区等）
hda = sdmmc:raw@2048

; 兼容：FAT 文件（不推荐热路径）
;hda = /sdcard/dos.img
```

### 3.2 后端优先级

| URI | 后端 | 读策略 | 写 |
|-----|------|--------|-----|
| `flash:label` | **BlockDeviceFlashMmap** | mmap 常驻；零拷贝读 | 可选 RW（`flash_stdio` 扇区擦写，慢） |
| `sdmmc:raw` / `sdspi:raw` | **BlockDeviceESPSD** | `sdmmc_read_sectors` → 目标 buf | `sdmmc_write_sectors` |
| `sdmmc:raw@LBA` | ESPSD + `start_sector` | 同上 | 同上 |
| 路径 `/dev/mmcblk0` | ESPSD | 同上（别名） | 同上 |
| `/sdcard/...` | BlockDeviceFile | fread（回退） | fwrite |
| CD + CISO | BlockDeviceCISO | 解压 → 条带（无法零拷贝） | — |

### 3.3 `ide_attach` 路由（目标）

```c
#ifdef BUILD_ESP32
if (strncmp(filename, "flash:", 6) == 0)
    bs = block_device_init_flash_mmap(filename + 6);
else if (is_sd_raw_uri(filename, &start, &count))  /* sdmmc:raw, sdspi:raw, @offset */
    bs = block_device_init_espsd(start, count);
else if (strcmp(filename, "/dev/mmcblk0") == 0)
    bs = block_device_init_espsd(0, -1);
else
    bs = block_device_init(filename, BF_MODE_RW);
#endif
```

---

## 4. BlockDevice 扩展：直读与零拷贝

### 4.1 现状 API

```c
int (*read_async)(BlockDevice *bs, uint64_t sector_num,
                  uint8_t *buf, int n, BlockDeviceCompletionFunc *cb, void *opaque);
/* 返回：-1 错，0 同步完成，>0 异步 */
```

所有读都 **写入调用者提供的 buf**；无法返回「已在 mmap 中」。

### 4.2 扩展：`BlockDeviceReadResult`

```c
typedef enum {
    BD_READ_COPIED,     /* 数据已在 buf */
    BD_READ_ZERO_COPY,  /* 数据在 result.ptr，生命周期到下次 read 或 unmap */
} bd_read_kind_t;

typedef struct {
    bd_read_kind_t kind;
    uint8_t *ptr;       /* ZERO_COPY 时有效 */
    int sectors;        /* 连续 512B 扇区数 */
} BlockDeviceReadView;

/* 新接口（可选实现，NULL 则回退 read_async） */
int (*read_view)(BlockDevice *bs, uint64_t sector_num, int max_sectors,
                 BlockDeviceReadView *out);
```

**Flash mmap** 实现 `read_view`：

```c
out->kind = BD_READ_ZERO_COPY;
out->ptr = mmap_base + sector_num * 512;
out->sectors = min(max_sectors, nb_sectors - sector_num, 连续至映射末尾);
return 0;
```

**SD raw** 实现 `read_view` = NULL，始终 `read_async` 到条带/Guest。

### 4.3 Flash mmap 后端（`BlockDeviceFlashMmap`）

与 `esp_main.c` 中 `partition_mmap_copy` **同源策略**，但 **不 copy 到 RAM**：

```c
typedef struct {
    const esp_partition_t *part;
    const uint8_t *mmap_base;
    esp_partition_mmap_handle_t handle;
    int64_t nb_sectors;
} BlockDeviceFlashMmap;

static int flash_mmap_read_view(..., BlockDeviceReadView *out)
{
    if (sector_num >= bf->nb_sectors) return -1;
    out->kind = BD_READ_ZERO_COPY;
    out->ptr = bf->mmap_base + sector_num * 512;
    out->sectors = min(max_sectors, bf->nb_sectors - sector_num);
    return 0;
}
```

| 项 | 约定 |
|----|------|
| 映射时机 | `ide_attach` 时一次 `esp_partition_mmap(..., ESP_PARTITION_MMAP_DATA, ...)` |
| 释放 | `ide` 析构或换盘时 `esp_partition_munmap`（BIOS 映射可保留不 unmap 的经验同样适用，但磁盘更大需支持 unmap） |
| 对齐 | 512B 扇区对齐由分区大小保证 |
| 多核 | **只读 VA 访问**，不走 `esp_partition_read` 停缓存路径 |

---

## 5. 条带缓冲设计（路径 C，对照 LightVGA §4）

### 5.1 为何仍需要条带（SD / 写 / 非对齐）

| 路径 | 数据流 | 问题 |
|------|--------|------|
| 现状 | 介质 → 2KB io_buffer → phys_mem | 条带太小、次数多 |
| Flash 零拷贝 | mmap ptr → phys_mem（`read_string`） | **SD 无 mmap** |
| SD 直写条带 | SD DMA → 32KB stripe → phys_mem | **单次 memcpy**，无 io_buffer 中转 |
| SD 直写 Guest | SD DMA → phys_mem | **理想**；需 Guest 缓冲 DMA 安全且对齐 |

ESP32-S3：`phys_mem` 在 PSRAM，`sdmmc_read_sectors` **可 DMA 到 PSRAM**（需 buffer 对齐）。**优先直写 Guest**；不可用时直写条带。

### 5.2 条带几何

条带 = 连续 **N 扇区 × 512 B**。

| N 扇区 | 条带大小 | 用途 |
|--------|----------|------|
| **4** | 2 KB | 与现 `MAX_MULT_SECTORS` 一致；IDENTIFY、小传输 |
| **16** | 8 KB | **默认读条带**（省 SRAM） |
| **32** | 16 KB | 顺序读推荐 |
| **64** | 32 KB | 启动/大文件复制；需测 `MALLOC_CAP_DMA` |

与 VGA 相同：**启动时探测堆，SRAM DMA 优先，不足则 PSRAM**。

```c
typedef enum { STRIPE_SRAM, STRIPE_PSRAM } stripe_mem_t;

static void *stripe_alloc(size_t size, stripe_mem_t *where)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (p) { *where = STRIPE_SRAM; return p; }
    p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (p) { *where = STRIPE_PSRAM; return p; }
    return NULL;
}
```

### 5.3 常驻 vs 懒分配

```text
常驻：  io_buffer[2052]        — IDENTIFY、ATAPI 包、≤4 扇区 PIO
懒分配： stripe_read[N*512]   — HDD 顺序读；不用时释放
可选：  stripe_write[N*512]   — 写路径乒乓（P1）
```

### 5.4 IDE 传输状态扩展

```c
struct IDEState {
    /* 现有字段 … */
    uint8_t io_buffer[MAX_MULT_SECTORS * 512 + 4];

    /* LightIDE 扩展 */
    uint8_t *data_ptr;          /* 当前传输数据源（io_buffer 或 mmap 或 stripe） */
    int      data_zero_copy;    /* 1 = 勿 free，指向前端映射 */
    uint64_t data_sector;       /* 调试用 / prefetch */
};
```

`ide_transfer_start` 改为接受 **外部 ptr**：

```c
static void ide_transfer_start_ptr(IDEState *s, uint8_t *ptr, int size,
                                   EndTransferFunc *end, int zero_copy);
```

`ide_data_read_string`：

```c
memcpy(buf, s->data_ptr + s->data_index, len);  /* 零拷贝时 ptr 即 mmap */
```

---

## 6. 读路径：三种模式

### 6.1 模式 Z — Flash 零拷贝（只读 mmap）

```text
ide_sector_read:
  if bs->read_view && view.kind == ZERO_COPY:
      data_ptr = view.ptr
      ide_transfer_start_ptr(..., zero_copy=1)
      ide_set_irq
  else:
      走模式 S
```

Guest `rep insw`：`read_string` 从 **mmap VA** 一次 memcpy 到 phys_mem（**仅 1 次拷贝**，无 io_buffer）。

### 6.2 模式 S — SD 条带直读（推荐默认）

```text
ide_sector_read:
  n = min(req_nb_sectors, stripe_sectors, 剩余 nsector)
  if guest 即将 read_string 且 phys 对齐且 n 大:
      sdmmc_read_sectors(..., phys_mem + guest_off, n)   /* 直写 Guest，模式 G */
  else:
      sdmmc_read_sectors(..., stripe_read, n)
      data_ptr = stripe_read
  ide_transfer_start_ptr(...)
```

### 6.3 模式 G — Guest 直写（`rep insw` 协同）

**条件**（与 `i386.c` INS_helper2 一致）：

- 端口 0x1F0 / 0x170
- `rep insw`，`dir > 0`
- Guest 物理地址 **字对齐** 且 **4KB 页内连续**
- 目标在 `phys_mem` 内、非 iomem
- 当前 IDE 传输 **整段扇区对齐**，且 **data 尚未灌入 io_buffer**

**实现要点**：在 `ide_data_read_string` **首次**被调用且 `data_index==0` 时，若满足条件且后端为 SD，**撤销已读入 stripe 的计划**，改为：

```c
sdmmc_read_sectors(card, cpu->phys_mem + guest_paddr, sector, n);
s->data_index = n * 512;
s->end_transfer_func(s);
```

或更简单：**在 `ide_sector_read` 前向注册「下一段 Guest 目标」**（P2，需 CPU 侧钩子）。

**首版 P0**：条带 → `read_string` 单次 memcpy（已比 word 循环快）。**P1**：SD 直写 Guest。

### 6.4 模式 W — 写（不变更语义）

```text
Guest outsw → io_buffer / stripe_write → sdmmc_write_sectors / flash_stdio
```

写路径 **不零拷贝**；可选条带减少 `fwrite` 次数。

---

## 7. `MAX_MULT_SECTORS` 与 SET MULTIPLE

| 项 | 现状 | 目标 |
|----|------|------|
| `MAX_MULT_SECTORS` | 4 | **16**（`io_buffer` 扩至 8KB+）或保持 4，大读走条带 |
| `WIN_SETMULT` | 支持 | Guest 设 16 时与条带 N 对齐 |
| `ide_identify` word 47 | `0x8000 \| 4` | `0x8000 \| 16`（若条带 16） |

**注意**：`io_buffer` 扩大会增大 **每 IDEState** 结构；CD ATAPI 包仍可用前 512B。可选 **io_buffer 保持 2KB**，大读仅用外部 `stripe_read`。

---

## 8. 各场景行为

### 8.1 DOS / BIOS 启动（顺序读）

- 预期：`rep insw` 从 0x1F0 读 MBR、IO.SYS
- 目标：Flash **`flash:boot`** 或 SD **`sdspi:raw`** + 条带 16～32 扇区
- 验收：启动时间、串口 optional bench（§15）

### 8.2 Windows 9x 安装（混合随机/顺序）

- 保持 PIO 正确性；随机小读仍走 ≤4 扇区 `io_buffer`
- 条带过大对随机读无益；**按命令 nsector 自适应**，不强制灌满条带

### 8.3 CD-ROM / CISO

- **无法 Flash 式零拷贝**（解压）；维持 `cmp_buf` + 2048B `io_buffer` 段
- 可选：解压直写条带，减少 `io_buffer` 内 memcpy

### 8.4 换盘 / `ide_change_cd`

- Flash mmap：`munmap` + 重映射
- SD raw：仅改 `nb_sectors` / 偏移

---

## 9. PSRAM、Flash 与多核

| 配置项 | 现状 | 目标 |
|--------|------|------|
| HDD 镜像位置 | FAT 文件或 fread | **Flash 分区 mmap** 或 **SD LBA raw** |
| 热路径 Flash API | `esp_partition_read`（stdio） | **`esp_partition_mmap` + VA 读** |
| `phys_mem` | PSRAM | 不变；SD DMA 可直达 |
| 与 VGA 争用 | Flash 读 vs PSRAM 写 | 磁盘 mmap **读 VA**；避免 Core 间 `esp_partition_read` 停 D-cache（见 `esp_main.c` 注释） |
| `rawsd` 初始化 | `storage_init` | `ide_attach` 前必须完成 |

**注意**：SD 与 LCD SPI 若同总线需硬件分板评估；与 LightVGA 条带 **共享 PSRAM 带宽** 时，测速阶段记录并发。

---

## 10. 性能优化优先级

| 优先级 | 措施 | 预期 |
|--------|------|------|
| **P0** | 实现 **`sdspi:raw` / `sdmmc:raw` URI** | ini 与行为一致 |
| **P0** | **Flash mmap 后端** + 零拷贝读 | Flash 镜像 −1 次拷贝/扇区 |
| **P0** | **条带 16 扇区（8 KB）** + `read_string` | 顺序读 IRQ↓、bulk memcpy |
| **P1** | SD **直写 Guest phys**（模式 G） | 再 −1 次拷贝 |
| **P1** | 读 **prefetch 下一条带**（异步 SD） | 与 Guest 消费重叠 |
| **P2** | `MAX_MULT_SECTORS` / SET MULT → 16 | 减少 MULT 命令次数 |
| **P2** | 写路径条带聚合 | 写性能 |
| **不做** | 整盘载入 RAM | 内存不可行 |
| **不做** | UDMA 仿真 | 现 IDENTIFY 无 DMA；复杂度高 |

### 10.1 粗算（顺序读 64 KB，16 扇区条带）

| 环节 | 现状（4 扇区） | 目标（16 扇区 + mmap/直写） |
|------|----------------|----------------------------|
| 命令/IRQ 轮数 | ~32 次 / 64KB | ~8 次 |
| 拷贝次数 | 2（介质→buf→guest） | 0～1 |
| Flash CPU 读 | partition_read × 128 | mmap VA × 1 memcpy |

---

## 11. 内存预算汇总

| 区域 | 现状 | 目标 |
|------|------|------|
| `IDEState.io_buffer` ×2 通道 | ~4 KB | ~4 KB（或 8KB×2 若扩 MULT） |
| `stripe_read` | 0 | **8～32 KB**（懒分配，SRAM 或 PSRAM DMA） |
| Flash mmap | 0 | **0 RAM**（MMU 映射；仅页表） |
| CISO `cmp_buf` | 2 KB / CD | 不变 |
| **PSRAM 磁盘缓存** | 0（无整盘） | **0** |

每 64KB 顺序读流量：

| | 现状 | 目标 |
|--|------|------|
| 中间缓冲 | 64 KB 写 io_buffer + 64 KB 读 | 0～64 KB（直写 Guest 时 0） |
| Flash | 128 KB read API | 64 KB memcpy（mmap→guest） |

---

## 12. 模块改动清单

### 12.1 `ide.c`

```text
新增：
  block_device_init_flash_mmap(label)
  block_device_init_espsd 扩展 URI 解析（sdmmc:raw@LBA）
  BlockDevice.read_view（可选）
  ide_transfer_start_ptr / data_ptr / data_zero_copy
  stripe_alloc / stripe_read 懒分配
  ide_attach 路由 flash: / sd*:raw

修改：
  ide_sector_read   — 优先 read_view 零拷贝；否则 stripe 或 io_buffer
  ide_data_read_string — 从 data_ptr 拷贝
  ide_sector_write  — 可选 stripe_write

删除/避免：
  Flash 镜像走 BlockDeviceFile+fread（仅作回退）
```

### 12.2 `ide.h`

- 文档注释 URI 约定；不强制导出 `BlockDevice` 细节。

### 12.3 `flash_stdio.c`

- **`flash:` 读镜像**：IDE 不再依赖 `fopen`；保留给 **其它只读文件** 或 **Flash RW 写路径**。

### 12.4 `storage.c`

- 无硬改；保证 `rawsd` 在 `pc_new` 前有效。
- 可选：导出 `sdmmc_card_t *storage_get_rawsd(void)`。

### 12.5 `esp_main.c`

- 可选：启动日志打印 `heap_caps_get_largest_free_block(DMA/PSRAM)`（与 LightVGA 一致）。
- **不**在 `load_rom` 路径混用磁盘 mmap。

### 12.6 `tiny386.ini`

```ini
hda = flash:diskimg    ; 内置 Flash 分区 RAW
; 或
hda = sdspi:raw        ; TF 卡整盘
```

---

## 13. 同步与异步

| 场景 | 策略 |
|------|------|
| `read_async` 返回 0（同步） | 保持现状：读完成后设 DRQ、IRQ |
| SD 异步（P1） | 读下一条带时 Guest 消费上一条；`BUSY_STAT` 期间拒绝新命令 |
| Flash mmap | 始终同步（VA 读） |
| 与 Guest 重入 | 单线程 i386 步进；无额外锁（P0） |

---

## 14. 实施阶段

| 阶段 | 内容 | 验收 |
|------|------|------|
| **0** | **`sdspi:raw` / `sdmmc:raw` 接线**；`/dev/mmcblk0` 别名 | SD 裸盘 `FDISK` / `DIR C:` |
| **1** | **Flash mmap 后端** + 零拷贝 `read_view` | `hda=flash:diskimg` 启动 DOS |
| **2** | **stripe_read 16 扇区** + `data_ptr` 传输 | 顺序读 bench 提升 |
| **3** | **`read_string` / 条带协同**；可选 SD 直写 Guest | `rep insw` 路径 CORRECTNESS |
| **4**（可选） | prefetch、写条带、MULT=16 | 安装 Win9x 稳定性 |
| **B**（调试） | §15 测速 | CSV + 选定默认 N |

---

## 15. 附录：存储路径调速测速（调试方案）

> **目的**：在真机上扫 **后端类型（Flash mmap / SD raw / FAT）× 条带扇区数 N × 缓冲域（SRAM/PSRAM）× Guest 传输路径 × 直写 Guest**，量化介质读、拷贝、IDE 命令开销与端到端吞吐，为 §5.2 条带缺省值、§6 读模式选型、§10 优化优先级提供数据。  
> **范围**：仅 ESP32-S3 固件；**不改变 Guest 可见 IDE 语义**；测速代码用编译开关或 ini 启用，发布版默认关闭。

### 15.1 待扫参数（自变量）

| 轴 | 取值 | 说明 |
|----|------|------|
| **模式** | `micro` / `block` / `ide` / `boot` | 见 §15.3 |
| **后端** | `flash_mmap` / `sd_raw` / `fat_file` / `legacy` | `legacy` = 现 `fread`+2KB io_buffer 对照 |
| **条带扇区 N** | 4, 8, 16, 32, 64 | `stripe_bytes = N × 512` |
| **缓冲域** | `sram` / `psram` / `auto` | 覆盖 §5.2；测速时可 **强制** 单域 |
| **零拷贝** | `off` / `on` | 仅 `flash_mmap`；`read_view` → `data_ptr` |
| **直写 Guest** | `off` / `on` | 模式 G：SD DMA → `phys_mem`（§6.3） |
| **Guest 路径** | `read_string` / `insw_word` / `insd_dword` | 对应 `rep insw` / 逐 word / 32bit 端口 |
| **MULT 扇区** | 1, 4, 16 | `WIN_MULTREAD` 与 `MAX_MULT_SECTORS` 组合 |
| **读总量** | 256, 1024, 4096 扇区 | 128 KB / 512 KB / 2 MB；可 ini 配置 |
| **起始 LBA** | 0, 2048, … | 避开 MBR / 测分区偏移 |
| **轮数** | 预热 10 + 统计 30 | 顺序读可加大；boot 模式 3 次取中位数 |

**条带字节与 IDE 命令次数对照**（读 `total_sectors=1024`，按条带切分）：

| N 扇区 | stripe_bytes | 条带次数/1024扇区 | MULT=16 时命令数上限 |
|--------|--------------|-------------------|----------------------|
| 4 | 2 048 | 256 | 64 |
| 8 | 4 096 | 128 | 64 |
| 16 | 8 192 | 64 | 64 |
| 32 | 16 384 | 32 | 64 |
| 64 | 32 768 | 16 | 64 |

### 15.2 计时指标（因变量）

用 `esp_timer_get_time()`（µs），在 **单次读块 / 单条 IDE 命令 / 整段 benchmark** 三级打点：

| 指标 | 符号 | 含义 |
|------|------|------|
| 介质读 | `t_media` | `sdmmc_read_sectors` / mmap 命中 / `fread` 完成 |
| 条带拷贝 | `t_stripe` | 介质 → `stripe_read`（零拷贝时为 0） |
| Guest 交付 | `t_guest` | `read_string` memcpy 或 `insw` 循环 → `phys_mem` |
| IDE 命令 | `t_cmd` | `WIN_READ`/`MULTREAD` 发令 → DRQ+IRQ 完成（含状态机） |
| 条带合计 | `t_chunk` | `t_media + t_stripe + t_guest`（单条带） |
| 整段 | `t_total` | 读完 `total_sectors` 墙钟 |
| 吞吐 | `kbps` | `total_sectors × 512 × 1000 / t_total`（KB/s，整数） |
| 命令开销 | `t_cmd_overhead` | `t_total − Σ t_media`（IDE+拷贝纯开销） |

每条带 / 每命令额外记录（排障用）：

- `stripe_bytes` = `N × 512`
- `sectors_this_chunk`
- `data_zero_copy`（0/1）
- `direct_guest`（0/1）
- `heap_largest_dma` / `heap_largest_psram_dma`（分配后可选）
- `sd_freq_khz` / `cpu_mhz`（CSV 注释行）

**输出统计**：对统计轮取 **min / avg / max**（avg 用整数 µs）。

### 15.3 四种测试场景

#### A. `micro` — 纯 BlockDevice（无 IDE 状态机）

隔离 Guest 与 IDE 寄存器，只测 **后端 × 条带 N × 域 × 直写目标**：

```text
for each backend, N, mem, direct_guest:
    alloc stripe (if needed)
    warmup 10 × read total_sectors
    for run in 0..29:
        t0 = now()
        for off in 0 .. total_sectors step N:
            t_m0 = now()
            if backend == flash_mmap && zero_copy:
                ptr = mmap_base + off*512          // 无 t_media 写
            elif direct_guest:
                sdmmc_read_sectors(..., phys_mem + guest_off, N)
            else:
                sdmmc_read_sectors(..., stripe, N)
            t_media = now() - t_m0
            if !zero_copy && !direct_guest:
                t_s0 = now()
                memcpy(guest_buf, stripe, N*512)   // 模拟 read_string
                t_guest = now() - t_s0
        t_total = now() - t0
    print CSV row
```

用于回答：

- **Flash mmap 零拷贝** 相对 `fread` 省多少 `t_media`？
- **SD 直写 Guest** 相对「SD → 条带 → memcpy」省多少？
- **N=16 vs N=32** 在固定 512KB 下谁 `kbps` 更高？

#### B. `block` — BlockDevice + 模拟 PIO 交付

在 `micro` 基础上增加 **固定 Guest 缓冲** 上的 `read_string` 批量交付（仍不走完整 IDE）：

```text
guest_buf = phys_mem + 0x100000   // 页对齐测试地址
for each N, guest_path:
    if guest_path == read_string:
        ide_bench_deliver_string(guest_buf, data_ptr, bytes)
    else if guest_path == insw_word:
        ide_bench_deliver_insw(port_buf_sim, data_ptr, bytes)
```

用于回答：**Guest 交付路径** 占 `t_total` 比例；优化介质读后是否值得改 `insw` 循环。

#### C. `ide` — 完整 IDE 路径（推荐主扫）

- 挂载真实 `IDEState`，对 **primary 0x1F0** 发 **`WIN_MULTREAD`** 或 **`WIN_READ` 循环**。
- 条带 / 零拷贝 / 直写 Guest 走 §6 生产逻辑。
- Guest CPU **可选暂停**（bench 线程直接调 `ide_exec_cmd` + `ide_data_read_string`）或 **联跑**（`pc_step` 内 DOS 未启动）。

```text
for each backend, N, mem, mult, guest_path, zero_copy, direct_guest:
    ide_attach_bench(backend)
    ide_set_mult(mult)
    warmup 10
    for run in 0..29:
        reset_drive()
        t0 = now()
        for remaining sectors:
            issue READ/MULTREAD
            wait DRQ
            consume via read_string or insw
            wait IRQ
        t_total = now() - t0
    print row
```

扫参矩阵建议：

- `backend ∈ {legacy, flash_mmap, sd_raw, fat_file}`
- `N ∈ {4, 8, 16, 32, 64}`
- `mem ∈ {auto, sram, psram}`
- `mult ∈ {1, 4, 16}`
- `guest_path ∈ {read_string, insw_word}`
- `zero_copy` 仅 flash；`direct_guest` 仅 sd_raw

用于回答：**生产 IDE 栈** 下推荐 **`stripe_N=16` 还是 32**？Flash 是否必须 mmap？

#### D. `boot` — 端到端 DOS 启动墙钟

- 正常 `pc_new` + 固定 ini（`hda = flash:…` 或 `sdspi:raw`）。
- 记录 **`load_bios_and_reset` 完成 → 首屏 `C:\>` 或 boot sector 后 N 秒** 的时间戳。
- **不扫全矩阵**；仅在 §15.5 收敛后，用 **2～3 组候选配置** 各跑 3 次取中位数。

可选 GPIO / 串口标记：

```text
[lightide_bench]
boot_marker = serial     ; 在 fixed DOS 批处理里 echo BENCH_DONE
```

用于回答：调速优化对 **用户可感知启动** 的提升是否 ≥10%。

### 15.4 实现要点（代码结构）

#### 15.4.1 编译 / 配置开关

```c
/* sdkconfig 或 board_ili9341.h */
#define CONFIG_LIGHTIDE_BENCH 0   /* 1 = 编入测速；发布必须为 0 */

/* tiny386.ini 示例（bench 专用段，esp_main 或 pc_main 解析） */
[lightide_bench]
enable = 1
mode = ide              ; micro | block | ide | boot
backend = sd_raw        ; flash_mmap | sd_raw | fat_file | legacy
stripe_n = 4,8,16,32    ; 逗号列表 sweep
mem = auto,sram,psram
zero_copy = on,off
direct_guest = on,off
guest_path = read_string,insw_word
mult = 1,4,16
total_sectors = 1024
start_lba = 0
warmup = 10
runs = 30
guest_buf = 0x100000    ; phys_mem 内偏移，页对齐
pause_guest = 1         ; ide/block 模式是否停 pc_step
```

测速运行时：

- **`micro`/`block`**：停 `pc_step` / `vga_task` 刷帧（若仍运行），减少 PSRAM 争用。
- **`ide` 联跑**：保留 VGA，CSV 注释 `vga_on=1` 作对照组。
- **`boot`**：正常 Guest，仅记录墙钟。

#### 15.4.2 建议文件与钩子

| 位置 | 内容 |
|------|------|
| `src/esp/main/lightide_bench.c` | `lightide_bench_run()`、`bench_sweep()`、CSV 打印 |
| `refs/tiny386/ide.c` | 可选 `IdeChunkTimings *` 回调；或 bench 包装 `ide_sector_read` |
| `esp_main.c` | `if (bench_enable) lightide_bench_run(); else pc_main(...)` |
| `ide.c` / 新后端 | `block_device_init_flash_mmap` bench 可强制 mmap |

强制缓冲域（测速专用）：

```c
void *bench_stripe_alloc(size_t n, stripe_mem_t force)
{
    if (force == STRIPE_SRAM)
        return heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (force == STRIPE_PSRAM)
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    return stripe_alloc(n, &force);
}
```

Bench 钩子示例（IDE 内，仅 `CONFIG_LIGHTIDE_BENCH`）：

```c
typedef struct {
    int64_t t_media_us;
    int64_t t_guest_us;
    int     sectors;
    int     zero_copy;
    int     direct_guest;
} IdeChunkTimings;

void lightide_bench_chunk_report(const IdeChunkTimings *t);
```

#### 15.4.3 串口输出格式（CSV）

```text
# lightide_bench mode=ide cpu_mhz=240 sd_freq_khz=40000 warmup=10 runs=30 total_sectors=1024
mode,backend,stripe_N,mem,mult,guest_path,zero_copy,direct_guest,stripe_B,t_media_avg_us,t_guest_avg_us,t_cmd_avg_us,t_total_avg_us,kbps_avg,cmd_count,stripes_ok
ide,sd_raw,16,sram,16,read_string,0,0,8192,xxxx,xxxx,xxxx,xxxx,xxxx,64,1
ide,sd_raw,16,sram,16,read_string,0,1,8192,xxxx,xxxx,xxxx,xxxx,xxxx,64,1
ide,flash_mmap,16,sram,16,read_string,1,0,8192,xxxx,xxxx,xxxx,xxxx,xxxx,64,1
ide,legacy,4,sram,4,insw_word,0,0,2048,xxxx,xxxx,xxxx,xxxx,xxxx,256,1
...
# RECOMMEND ide stripe_N=16 mem=auto mult=16 guest_path=read_string direct_guest=on (sd_raw)
```

- `stripes_ok=0`：条带分配失败或读错误，跳过或降 N。
- `cmd_count`：本次 `total_sectors` 下 IDE 命令次数（验证 MULT 生效）。

**boot 模式** 单独格式：

```text
# lightide_bench mode=boot backend=flash_mmap stripe_N=16
mode,backend,stripe_N,mem,boot_to_c_prompt_ms,run_idx
boot,flash_mmap,16,auto,45230,0
```

#### 15.4.4 扫参顺序（推荐）

```text
1. micro + sd_raw + sram only     → SD 基线（无 IDE、无 FAT）
2. micro + sd_raw + direct_guest on/off → 量化直写 Guest 收益
3. micro + flash_mmap + zero_copy on/off vs legacy fread
4. ide + sd_raw 全矩阵 N × mem × mult × guest_path
5. ide + flash_mmap（zero_copy=on）× N × mem
6. ide + fat_file（对照）× N=4,16
7. boot × 候选 top-3 配置（各 3 次）
8. 写回 §5.2 缺省 N、§10 优先级、sdkconfig 注释
```

### 15.5 判定与收敛标准

| 场景 | 优选条件 |
|------|----------|
| **条带 N（SD）** | `stripes_ok=1` 且 **`kbps_avg` 最大**；N=16 与 N=32 差距 <5% 取 **N=16**（省 SRAM） |
| **条带域** | 同 N 下 `sram` 优于 `psram` ≥8% 则 `auto` 默认 SRAM；否则记录 PSRAM 回退阈值 |
| **Flash 后端** | `flash_mmap+zero_copy` 的 `t_total` 比 `legacy` **≥15%** 则 P0 必须上 mmap |
| **直写 Guest** | `direct_guest=on` 比 off **≥10%** 且 `boot` 无回归则 P1 默认开启（条件满足时） |
| **guest_path** | `read_string` 优于 `insw_word` 则 DOS 路径已受益；`insw_word` 仍测作回归 |
| **MULT** | `mult=16` 且 `cmd_count` 下降、`kbps` 不降则对齐 §7 |
| **fat_file** | 仅兼容；`kbps` 低于 `sd_raw` 预期值 30% 以上则在文档标注「勿作热路径」 |
| **boot** | 相对 `legacy` 墙钟 **≥10%** 或绝对值达到产品目标（如 <45s） |

记录结论到 **§18 修订**，例如：`ide stripe_N=16 mem=auto sd_raw direct_guest=on 实测 kbps=890 boot=41s`。

### 15.6 注意项

| 项 | 说明 |
|----|------|
| 预热 | 前 `warmup` 轮丢弃，避免 SD 初始化 / cache cold / 首 mmap |
| CPU 频率 | 固定 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`（如 240），写入 CSV 注释 |
| SD 时钟 | 记录 `host.max_freq_khz`（`storage.c`）；变更后全重跑 |
| Flash 并发 | **`micro`** 可与 VGA 并行测；**mmap** 路径勿与 `esp_partition_read` 混测作对照 |
| 多核 | 记录 i386 / vga 绑核情况；`pause_guest=1` 与 `0` 各一组 |
| 对齐 | `guest_buf` 测 **0x100000（对齐）** 与 **+2（故意不对齐）** 验证直写 Guest 回退 |
| 数据正确性 | 每轮抽样 1 扇区与 **golden CRC32**（预计算 LBA0..N）比对；失败则 `stripes_ok=0` |
| 发布构建 | `CONFIG_LIGHTIDE_BENCH=0` 时 **零开销**（`#if` 剥掉 bench 对象文件） |

### 15.7 对照组与基线（必跑）

| 标签 | 配置 | 用途 |
|------|------|------|
| **B0** | 现网 `ide.c` + FAT 或 fread + N=4 + io_buffer | 回归基线 |
| **B1** | `sdspi:raw` + legacy（未 mmap、未条带） | SD 接线验证 |
| **B2** | `flash_mmap` + zero_copy + N=16 | 目标 Flash 配置 |
| **B3** | `sd_raw` + N=16 + direct_guest + mult=16 | 目标 SD 配置 |

每版固件 **至少跑 B0 + B3**（`ide` 模式 512KB）；Flash 产品加 **B2**。

### 15.8 主机侧辅助（可选）

串口脚本（Python / PowerShell）发 `[lightide_bench]` 段、收集 CSV，生成 pivot 表：

```text
列：stripe_N  行：backend+mem+direct_guest  值：kbps_avg 或 t_total_avg_us
```

示例筛选命令（PowerShell）：

```powershell
Import-Csv lightide.csv | Where-Object { $_.mode -eq 'ide' -and $_.stripes_ok -eq '1' } |
  Sort-Object { [int]$_.kbps_avg } -Descending | Select-Object -First 10
```

便于一眼看出 **`sd_raw N=16 sram direct_guest=on`** 是否最优。

### 15.9 与实施阶段 B 的验收关系

| 阶段 B 门槛 | 测速要求 |
|-------------|----------|
| 阶段 2 完成 | `ide` 模式 B3 **`kbps_avg` ≥ B1 × 1.25** |
| 阶段 1 完成 | B2 **`t_media_avg` ≤ legacy × 0.5**（Flash 介质段） |
| 发布前 | `boot` 中位数 **≤ 产品 ini 目标**；`stripes_ok=1` 全矩阵无 CRC 失败 |

---

## 16. 风险与回退

| 风险 | 缓解 |
|------|------|
| mmap 分区非 512 对齐 | attach 时检查 `part->size % 512 == 0` |
| Flash 分区过大占 MMU | 仅映射整分区；ESP32 MMU 窗口足够典型 <16MB 镜像 |
| SD 直写 Guest 未对齐 | 不满足条件时回退条带 |
| `sdspi:raw` 与 FAT 同卡 | raw 绕开 VFS；文档说明分区布局 |
| CISO 解压 CPU 占用 | 保持独立路径，不强行条带 |
| 写 Flash 慢 | 文档标注 `flash:` RW 仅适合 small overlay |

---

## 17. 附录：不支持 / 回退行为

| 类型 | Guest | 行为 |
|------|-------|------|
| LBA48 / DMA ATA | 可能发命令 | 现状 abort；LightIDE 不扩展 |
| CD 2352 字节扇区 | READ CD | 维持现有 |
| snapshot `BF_MODE_SNAPSHOT` | 桌面 | ESP 不用 |
| FAT 路径 | 任意 | 回退 `BlockDeviceFile`，功能正确但慢 |

---

## 18. 文档修订

| 日期 | 说明 |
|------|------|
| 2026-06-30 | 初版：参照 LightVGA 结构，IDE 直读、Flash mmap、SD raw URI、条带缓冲、实施阶段 |
| 2026-06-30 | **§15 调速测速方案**：micro/block/ide/boot 四模式、sweep 矩阵、CSV、基线 B0–B3、阶段 B 验收 |
