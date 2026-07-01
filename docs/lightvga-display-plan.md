# LightVGA 显示路径完整方案（ESP32-S3 + ILI9341 320×240）

> 目标：在 `src/esp/main/lightvga.c` 上，为 **ESP32-S3 + ILI9341 SPI 屏（逻辑 320×240）** 建立省 PSRAM、**vram 内 SRAM**、条带 DMA 自适应（SRAM/PSRAM）、图形可提速的显示架构。
>
> 本文档汇总 LightVGA 功能范围、缓冲与条带设计、硬件旋转 vs 软件 SWAPXY、实施阶段与验收标准。**实施前以本文为准。**

---

## 0. 范围与约束

### 0.1 LightVGA 功能（相对 `vga.c`）

| 项目 | 行为 |
|------|------|
| 支持模式 | 文本模式 + 320×200×8 图形（含 mode 13h 快速路径、VBE 钳制到 320×200×8） |
| VBE | `vbe_fixup_regs` / GETCAPS 仅暴露 320×200×8；其它分辨率写操作被静默钳制 |
| `force_8dm` | 默认 `1`（等同 `vga_force_8dm = 1`） |
| 平台 | 仅 ESP32-S3 + ILI9341，无 `#ifdef` 多板；RGB565 |
| 图形不匹配 | `vga_graphic_refresh_unmatched()` 占位（当前不刷新） |

### 0.2 不可删除的部分

**Guest VGA RAM（`pc->vga_mem` / `s->vga_ram`）不能去掉**：x86 通过 0xA0000 / PCI BAR 读写；BIOS/DOS 设模式、写文本、mode 13h、VBE LFB 均依赖此内存。可 **缩小至 64 KB** 并 **放入内 SRAM**（§4.4.1），不可删除。

**不能**把 `vga_ram` 与 LCD 像素缓冲合并为同一块线性 RGB 帧缓冲，除非改为非 VGA 设备模型（会破坏标准 BIOS 路径）。

### 0.3 相关源文件

| 文件 | 角色 |
|------|------|
| `src/esp/main/lightvga.c` | VGA 模拟、刷新、VBE |
| `src/esp/main/esp_main.c` | `Console`、`redraw`、`pc_new` 传 fb |
| `src/esp/main/lcd_ili9341.c` | ILI9341 初始化、`lcd_draw`、`max_transfer_sz` |
| `src/esp/main/board_ili9341.h` | `LCD_WIDTH/HEIGHT`、`SWAPXY`、屏参 |
| `src/esp/main/startup_splash.c` | 启动 logo 送屏（与坐标约定一致） |
| `src/esp/main/lightvga_bench.c`（可选） | §15 测速；`CONFIG_LIGHTVGA_BENCH` |
| `src/tiny386/pc.c` | `vga_mem` 分配、`vga_init` |

---

## 1. 现状与问题

### 1.1 内存（每 Console）

| 缓冲 | 分配 | 大小 | 说明 |
|------|------|------|------|
| `vga_mem` | `psmalloc`（PSRAM） | 256 KB | Guest 显存 |
| `fb` | `psmalloc`（PSRAM） | 153 600 B | lightvga 合成帧，**非 DMA** |
| `fb1` | `fbmalloc`（内 SRAM DMA） | 4 800 B | SPI 源，32 条之一 |

### 1.2 每帧显示数据流（现状）

```
Guest 写 vram (PSRAM)
       │
vga_refresh: CPU 读 vram → 写 fb (PSRAM)          ~153 KB 写 PSRAM
       │
redraw ×32: CPU 读 fb → memcpy → fb1 (内 SRAM)    ~153 KB 读 PSRAM + CPU 拷
       │
SPI DMA 读 fb1 → ILI9341                          ~153 KB（内 SRAM 源）
       │
usleep(900) × 32                                  ~29 ms/帧 固定等待
```

### 1.3 软件 SWAPXY（现状）

`board_ili9341.h` 定义 `SWAPXY`，lightvga 像素地址：

```c
offset = 2 * (y + x * 240);   // 列主序，非标准 y*320+x
```

`lcd_ili9341.c` 初始化：

```c
esp_lcd_panel_swap_xy(panel, false);
esp_lcd_panel_mirror(panel, true, true);
```

送屏窗口（`redraw` / splash）：

```c
lcd_draw(0, y_strip, LCD_HEIGHT, y_strip + h, buf);
// x: 0..240, y: 沿 320 方向切条
```

物理屏 240×320 竖屏 + 逻辑 320×240 横屏，在 **未开 `swap_xy`** 时用软件转置弥补。

### 1.4 瓶颈归纳

1. PSRAM 全帧 `fb` + `redraw` 整帧回读 → **~306 KB/帧 CPU 触达 PSRAM**（仅显示）
2. DMA 缓冲仅 4.8 KB → **32 次** SPI 事务 + **32 次** `memcpy`
3. **`usleep(900)×32`** → 约 29 ms/帧与显示无关的等待
4. 软件 SWAPXY → 条带按「列」切，缓存不友好，流式实现复杂

---

## 2. 目标架构

### 2.1 原则

| 原则 | 说明 |
|------|------|
| 去掉 PSRAM `fb` | 不为合成帧保留 150KB PSRAM |
| SPI 只读 **DMA 缓冲** | 条带 **优先内 SRAM DMA**；不足时 **PSRAM DMA**（§4.4） |
| 流式条带（路径 C） | 读 vram → 转换 → **条带缓冲** → GDMA → SPI |
| **vram 64 KB** | **固定内 SRAM**（`MALLOC_CAP_INTERNAL`），Guest 读写与刷新均受益 |
| 条带 / 乒乓 | **按空闲内存选 SRAM 或 PSRAM DMA**（见 §4.4） |
| 文本 | **不提速**（全帧重画、无脏矩形）；条带优先占最少内 SRAM |
| 图形 | 加大条带、双缓冲乒乓、去掉 sleep、可选脏矩形 |
| 旋转 | 优先 **ILI9341 MADCTL**（`swap_xy` + `mirror`），去掉软件 SWAPXY |

### 2.2 目标数据流

```
                    ┌─────────────────────────────────────┐
  Guest CPU         │  vram 64KB (内 SRAM)                 │
  写 0xA0000 ──────►│  文本: char/attr + 字模              │
                    │  图形: 8bpp 索引                     │
                    └──────────────┬──────────────────────┘
                                   │ vga_refresh（按条带）
                    ┌──────────────▼──────────────────────┐
                    │  lightvga：读 vram → RGB565          │
                    │  写入当前条带（SRAM 或 PSRAM DMA）    │
                    └──────────────┬──────────────────────┘
                                   │ GDMA（无 CPU 整帧 memcpy）
                    ┌──────────────▼──────────────────────┐
                    │  SPI2 → ILI9341（MADCTL 横屏 320×240）│
                    └─────────────────────────────────────┘

  删除：PSRAM fb、memcpy(fb→fb1)、usleep(900)、软件 SWAPXY（硬件定向成功后）
  vram 不再占用 PSRAM 池；条带 DMA 缓冲按 §4.4 自适应放置
```

---

## 3. LCD 旋转：硬件 MADCTL vs 软件 SWAPXY

### 3.1 控制器能做什么

ILI9341 通过 **MADCTL**（ESP-IDF：`esp_lcd_panel_swap_xy` / `esp_lcd_panel_mirror`）设置：

| 能力 | API | 效果 |
|------|-----|------|
| X/Y 对调 | `esp_lcd_panel_swap_xy(true)` | GRAM 行列扫描对调 |
| 镜像 | `esp_lcd_panel_mirror(mx, my)` | 配合排线方向 |
| RGB/BGR | `rgb_ele_order` / BGR bit | 与轴交换无关 |

`esp_lcd_panel_draw_bitmap` 要求 `color_data` 为窗口内 **行主序**：

```text
len = (x_end - x_start) * (y_end - y_start) * 2
```

硬件定向正确后，软件使用 **标准布局**：

```c
offset = 2 * (y * LCD_WIDTH + x);   // 0 ≤ x < 320, 0 ≤ y < 240
stride = LCD_WIDTH * 2;
lcd_draw(0, y0, LCD_WIDTH, y1, strip);   // 横条切分
```

### 3.2 不能指望硬件完成的

- RGB565 **字节序**（高低字节）可能仍需 `fb_pack_color` / 面板 BGR 设置，需实测
- 旋转只有 **swap + mirror 组合**，需色条 / splash **目测校准**，不能假设 `swap_xy(true)` 一次就对

### 3.3 改造时需对齐的三处

1. `lcd_ili9341.c` — `swap_xy` / `mirror` 组合  
2. `startup_splash.c` — `lcd_draw` 与 logo 资源读入步长  
3. `lightvga.c` / `esp_main.c` — 条带刷新坐标与像素写入公式  

### 3.4 建议验证顺序

1. 仅改 `lcd_ili9341.c` MADCTL + 棋盘格 / 色条测试  
2. 方向正确后改 splash  
3. 再去掉 `SWAPXY`、统一 lightvga 与流式条带为 **横条（按 y 切）**  

同仓库参考：`refs/tiny386/esp/main/board_elecrow7s3.h` 注释 **No SWAPXY — landscape matches VGA directly**（RGB 屏思路相同）。

---

## 4. 流式条带设计（路径 C 增强）

### 4.1 为何不用「整帧 PSRAM lcd_buf + SPI 直读」

| 路径 | 数据流 | 问题 |
|------|--------|------|
| 现状 memcpy | PSRAM fb → CPU → 内 SRAM → SPI | 整帧 CPU 读 PSRAM |
| PSRAM DMA 直读 | CPU 写 PSRAM + DMA 读 PSRAM → SPI | 无 memcpy，但 PSRAM **写+读** 仍争用 |
| **流式条带（SRAM 优先）** | CPU 读 vram（**内 SRAM**）→ 条带 → SPI | 无显示用 PSRAM fb；vram 与 Guest 同在内 SRAM |
| **流式条带（PSRAM 回退）** | 同上，条带在 PSRAM DMA | 内 SRAM 不够放乒乓时启用；SPI 仍 DMA，略慢 |

**vram 固定内 SRAM** 后，刷新侧不再读 PSRAM vram；Guest 写显存延迟也显著降低。  
条带缓冲：**优先内 SRAM DMA**（SPI 最快）；若 `MALLOC_CAP_DMA` 不足则 **回退 PSRAM DMA**（ESP32-S3 支持，`CONFIG_SOC_PSRAM_DMA_CAPABLE`），与 `phys_mem` 争用但总好过放不下或单缓冲无乒乓。

### 4.2 条带几何（硬件横屏、标准行主序后）

逻辑分辨率 `LCD_WIDTH=320`，`LCD_HEIGHT=240`。

条带 `i`（共 `NN` 条，`i = 0 .. NN-1`）覆盖：

```text
y ∈ [i * (240/NN), (i+1) * (240/NN) - 1],  x ∈ [0, 319]
条带字节数 = (240/NN) * 320 * 2
```

| NN | 条高（行） | 条带大小 | 用途 |
|----|------------|----------|------|
| **16** | 15 | **9 600 B** | **文本**（省 SRAM） |
| 8 | 30 | 19 200 B | 图形备选 |
| **4** | 60 | **38 400 B** | **图形推荐** |
| 2 | 120 | 76 800 B | 图形激进（需实测 `MALLOC_CAP_DMA` 空闲） |
| **1** | 240 | **153 600 B** | 整帧一次 SPI（对照组；**仅测速**，见 §15） |

> 若实施前仍保留软件 SWAPXY，条带按 **列** 切（与现 `redraw` 一致）；**MADCTL 改完后必须改为按行切**，上表才适用。  
> **NN 与条带字节数最终取值以 §15 测速结果为准**；上表为默认推荐起点。

### 4.3 条带缓冲策略（文本 / 图形）

#### 文本模式（不提速、省 SRAM）

| 项 | 策略 |
|----|------|
| `NN_TEXT` | **16**（9.6 KB/条） |
| 缓冲 | **1 块**（无乒乓） |
| 放置 | **优先内 SRAM DMA**；失败则 **PSRAM DMA**（§4.4） |
| 脏矩形 | **不做** |
| 栈 `rowbuf` | 保留 **640 B**（单字符行） |
| 流程 | 每帧 16 次：栅格条带 → `lcd_draw` → `lcd_wait_flush_done()` |

**文本模式 DMA 峰值：9.6 KB**（单块，无乒乓）。

#### 图形模式（提速）

| 项 | 策略 |
|----|------|
| `NN_GFX` | **4**（38.4 KB/条），不足时降为 **8**（19.2 KB/条） |
| 缓冲 | **2 块乒乓** |
| 放置 | **按 §4.4 自适应**：两缓冲同域（全 SRAM 或全 PSRAM） |
| 脏矩形 | 可选 P2；首版可全帧 4 条 |
| 流程 | 画条带 k 的同时 SPI 发条带 k-1（首末条注意 wait） |

**图形模式 DMA 峰值**（懒分配，与文本互斥）：

| `NN_GFX` | 乒乓合计 | 优先域 | 回退域 |
|----------|----------|--------|--------|
| 4 | **76.8 KB** | 内 SRAM DMA | PSRAM DMA |
| 8 | **38.4 KB** | 内 SRAM DMA | PSRAM DMA |

#### 模式切换

```text
常驻：     vram 64 KB（内 SRAM，不释放）
文本运行：  strip_text 9.6 KB（SRAM 或 PSRAM，单块）
图形运行：  strip_gfx 乒乓（同上，2 块）
blank：     可释放 gfx / text 条带，vram 保留
```

---

### 4.4 内存放置：vram 固定 SRAM，条带自适应

#### 4.4.1 vram（64 KB）— 固定内 SRAM

| 项 | 约定 |
|----|------|
| 大小 | **64 KB**（`vga_mem_size`） |
| 分配 | **不走 `bigmalloc` / PSRAM 池** |
| API | `heap_caps_malloc(64*1024, MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT)` |
| 映射 | 仍挂 `pc->vga_mem` → `vga_init` → 0xA0000 / PCI BAR |
| 释放 | 随 `pc` 析构；模式切换 **不释放** |

**理由**：Guest `vga_mem_read/write` 与 `vga_refresh` 读 vram 均为热点；64 KB 可放进内 SRAM，且 **不再占用 `PSRAM_ALLOC_LEN`**（可还给 `phys_mem` 或缩小池）。

**注意**：vram **不需要** `MALLOC_CAP_DMA`；与条带缓冲分离，避免 Guest 随机写破坏正在 SPI 的像素。

#### 4.4.2 条带 / 乒乓 — 按空闲块选择 SRAM 或 PSRAM

启动或切入图形/文本模式时探测堆，**同一组条带的两块乒乓必须落在同一域**（便于统一 `lcd_draw` 与性能预期）。

```c
typedef enum { STRIP_SRAM, STRIP_PSRAM } strip_mem_t;

static void *strip_alloc(size_t size, strip_mem_t *where)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_DMA);
    if (p) { *where = STRIP_SRAM; return p; }
    p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (p) { *where = STRIP_PSRAM; return p; }
    return NULL;
}

/* 图形乒乓：两块必须同域 */
static int strip_gfx_alloc(size_t strip_bytes, void **a, void **b, strip_mem_t *mem)
{
    *a = strip_alloc(strip_bytes, mem);
    if (!*a) return -1;
    void *p = (*mem == STRIP_SRAM)
        ? heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA)
        : heap_caps_malloc(strip_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!p) { heap_caps_free(*a); *a = NULL; return -1; }
    *b = p;
    return 0;
}
```

**决策顺序**（图形 `NN_GFX=4`，乒乓 76.8 KB）：

```text
1. heap_caps_get_largest_free_block(MALLOC_CAP_DMA) ≥ 76800 ?
     → strip_gfx[0..1] 内 SRAM DMA
2. 否则 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA) ≥ 76800 ?
     → strip_gfx[0..1] PSRAM DMA（乒乓仍有效，SPI 略慢）
3. 否则 NN_GFX 降为 8（乒乓 38.4 KB），重复 1–2
4. 仍失败 → 单缓冲图形（无乒乓，P0 功能降级）+ 日志
```

文本 `strip_text`（9.6 KB）用同一 `strip_alloc`；通常内 SRAM 足够。

#### 4.4.3 内 SRAM 预算（典型）

| 区域 | 大小 | 域 |
|------|------|-----|
| `vga_mem` | **64 KB** | 内 SRAM（固定） |
| `strip_text` | 9.6 KB | SRAM 或 PSRAM DMA |
| `strip_gfx` 乒乓 | ≤76.8 KB | SRAM 或 PSRAM DMA |
| `rowbuf` | 640 B | 栈 |

**峰值内 SRAM（全走 SRAM 条带）**：64 + 76.8 + 0.64 ≈ **141 KB** + IDF/WiFi/栈/JIT。  
若 `MALLOC_CAP_DMA` 紧张，**vram 仍占内 SRAM**（已定），条带 **自动落到 PSRAM**，总线分工：Guest/刷新读 **内 vram**，SPI DMA 读 **PSRAM 条带**。

实施前打印（建议 `esp_main` 启动日志）：

```c
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
```

---

## 5. 各模式刷新行为

### 5.1 文本（`graphic_mode == 1`）

```text
for i in 0 .. NN_TEXT-1:
    memset 条带或条内黑底
    对条带内相交字符格：读 vram → rowbuf → 写入条带
    lcd_draw(0, y0, 320, y1, strip_text)
    lcd_wait_flush_done()
```

- 宽 > 40 列：裁左侧 40 列（与现 lightvga 一致）  
- 非法参数：不刷新，保留上一帧  

### 5.2 图形 mode13 / VBE 8bpp

```text
for i in 0 .. NN_GFX-1:
    栅格条带（y 0..199 居中，上下黑边）
    乒乓：SPI 条带 k-1 ‖ 画条带 k
lcd_wait_flush_done()
```

### 5.3 图形 unmatched

- 首版：**不刷新**（与现 `vga_graphic_refresh_unmatched` 空实现一致）  
- 可选：用 **文本小条带** 循环清屏（不另分配缓冲）  

### 5.4 Blank

- 不刷新；切入新模式时 `simplefb_clear` 改为 **按条带清屏 + SPI**  

### 5.5 VBE 寄存器（Guest 侧）

- 写非 320×200×8：`vbe_fixup_regs` **静默钳制**  
- GETCAPS：仅报 320 / 200 / 8  
- Guest 不收到错误码  

---

## 6. PSRAM 与配置

| 配置项 | 现状 | 目标 |
|--------|------|------|
| `vga_mem_size` | 256 KB | **64 KB** |
| `vga_mem` 物理位置 | PSRAM（`bigmalloc`） | **内 SRAM**（`heap_caps`，§4.4.1） |
| PSRAM `fb` | 153 KB | **0** |
| 条带 DMA | 内 SRAM 4.8 KB | **9.6～76.8 KB**，SRAM 优先、PSRAM 回退（§4.4.2） |
| `PSRAM_ALLOC_LEN` | 5 MB 量级 | 可减 **~256 KB**（vram 迁出 + 无 fb）后微调 |
| `LCD_WIDTH` × `LCD_HEIGHT` | 320 × 240 | 不变 |
| PCI VGA BAR | 随 `vga_mem_size` | 与 64 KB 一致 |

---

## 7. 性能优化（图形向；文本不适用）

| 优先级 | 措施 | 预期 |
|--------|------|------|
| **P0** | 删除 `usleep(900)×NN` | ~29 ms/帧（现 32 条时） |
| **P0** | 删除 PSRAM `fb` + `memcpy` | −153 KB PSRAM 读/帧 |
| **P0** | `NN_GFX`: 32→**4** | SPI 事务 −87.5% |
| **P1** | **`vga_mem` 64 KB → 内 SRAM** | Guest 写显存 + 刷新读 vram 延迟降低 |
| **P1** | 图形双条带乒乓 + `lcd_wait_flush_done` | SPI 与栅格重叠 |
| **P1** | `max_transfer_sz` = 条带字节数（最大 38400 或整帧 153600） | 单次 DMA 打满 |
| **P2** | 条带 SRAM 优先 / PSRAM 回退（§4.4） | 内 SRAM 紧时仍保乒乓 |
| **P2** | 图形脏矩形（仅 320×200） | 静态画面少画条带 |
| **P3** | `vga_task` 与 i386 分核 + 队列 | 墙钟再降 |
| **不做** | 文本脏矩形、文本乒乓、减小文本 NN「为了快」 | 文本以 **省 SRAM** 为准 |

### 7.1 帧时间粗算（图形，NN_GFX=4，无 sleep）

| 环节 | 量级 |
|------|------|
| CPU 栅格（320×200） | 8～15 ms |
| SPI 4 次 | 8～15 ms |
| 乒乓重叠后墙钟 | **约 15～25 ms → 40～65 FPS 上限** |

实际仍受 `pc_vga_step` / `redraw` 调用频率限制。

---

## 8. 内存预算汇总

| 区域 | 现状 | 目标 |
|------|------|------|
| `vga_mem` | PSRAM 256 KB | **内 SRAM 64 KB**（固定） |
| PSRAM `fb` | 153 KB | **0** |
| 条带 DMA（文本） | 4.8 KB 内 SRAM | **9.6 KB**（SRAM 或 PSRAM） |
| 条带 DMA（图形乒乓） | 同上 | **≤76.8 KB**（SRAM 或 PSRAM，懒分配） |
| 栈 `rowbuf` | 640 B | 640 B |
| **PSRAM 显示相关** | ~409 KB | **0～76.8 KB**（仅条带回退 PSRAM 时） |
| **内 SRAM 显示峰值** | 4.8 KB | **64 KB + 9.6～76.8 KB**（条带全 SRAM 时） |

每帧内存流量（显示路径）：

| | 现状 | 目标 |
|--|------|------|
| PSRAM CPU 触达 | 写 fb + 读 fb ≈ **306 KB** | **0**（vram 已迁出；条带若 PSRAM 仅 DMA 读条带） |
| 内 SRAM 触达 | memcpy 153 KB | 读 **vram 64 KB** + 写条带（同域或跨域） |

---

## 9. 模块改动清单

### 9.1 `esp_main.c`

```text
删除：fb (PSRAM), fb1 固定 4.8KB, NN=32, memcpy, usleep(900)

新增：
  strip_alloc() / strip_gfx_alloc()     // §4.4.2，记录 strip_mem_t
  strip_text[9600]                      // 文本：单块，SRAM 或 PSRAM DMA
  strip_gfx[2][38400]                   // 图形：乒乓，懒分配，同域
  DisplaySink 记录条带所在域（日志 / 可选统计）

console_init / pc_new：不再分配 PSRAM fb；vram 在 pc.c 内 SRAM 分配
```

### 9.2 `lcd_ili9341.c`

```c
max_transfer_sz = max(38400, 9600);   // 或 153600 若整帧一次 SPI
trans_queue_depth = 4~8;
// MADCTL：swap_xy + mirror 按 §3 校准
```

### 9.3 `lightvga.c`

| 项 | 改动 |
|----|------|
| `vga_init` | 无 PSRAM `fb`；注册条带上下文 |
| `fb_put_px` | 标准 `y*320+x` 或 `strip_put_px` |
| `vga_text_refresh` | `NN_TEXT` 条带循环 → `strip_text` |
| 图形刷新 | `NN_GFX` 条带 → `strip_gfx[ping]` |
| `FBDevice` | `fb_data` 可改为条带上下文（当前 x0,y0,y1, buf） |
| `simplefb_clear` | 按条带 memset + SPI |

### 9.4 `board_ili9341.h`

- MADCTL 成功后 **删除 `SWAPXY`**  
- 可删 `SCALE_CROP` 等旧显示宏（lightvga 已不依赖）  

### 9.5 `pc.c` / `vga.h`

```c
/* 不再：pc->vga_mem = bigmalloc(pc->vga_mem_size); */
pc->vga_mem_size = 64 * 1024;   /* 或 conf->vga_mem_size，上限 64KB */
pc->vga_mem = heap_caps_malloc(pc->vga_mem_size,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
/* 失败：启动报错，不回退 PSRAM（vram 策略为固定内 SRAM） */
```

- `vga_init(vga_mem, 64*1024, ...)` → 去掉 PSRAM `fb` 或改为 `DisplaySink`  
- `vga_refresh` 签名可扩展 `strip_flush` 回调  

### 9.6 `startup_splash.c`

- 与 §3 新坐标一致：`lcd_draw(0, y, 320, y+rows, strip)`，logo 资源行主序 320 宽  

---

## 10. 同步与撕裂

| 模式 | 策略 |
|------|------|
| 文本单缓冲 | 每条条带：画完 → SPI → `lcd_wait_flush_done()` |
| 图形双缓冲 | 条带 k SPI 时画 k+1 |
| Guest 写 vram vs 刷新 | 与现况相同；极端情况可 P3 加锁 |

---

## 11. 实施阶段

| 阶段 | 内容 | 验收 |
|------|------|------|
| **0** | MADCTL 校准 + 色条；去掉 `SWAPXY` 意图验证 | 横屏 320×240 方向、颜色正确 |
| **1** | `strip_text` 9.6KB；文本 16 条流式；删 PSRAM `fb`、memcpy、usleep | BIOS/DOS 文本 |
| **2** | 图形 `strip_gfx` 乒乓 NN=4；`strip_alloc` SRAM/PSRAM；mode13/VBE | 320×200 图形 |
| **3** | `vga_mem` 64KB **内 SRAM**（`pc.c`）；`unmatched` 行为定稿 | 启动日志 heap；模式切换 |
| **4**（可选） | 图形脏矩形、分核队列 | 帧率与静态场景 |
| **B**（调试） | **§15 条带测速**：扫 NN / 域 / 乒乓，定稿 `NN_TEXT`/`NN_GFX` | 串口 CSV 表 + 选定默认值 |

---

## 12. 风险与回退

| 风险 | 缓解 |
|------|------|
| MADCTL 组合不对 | 阶段 0 独立验证；保留 git 可回退 `swap_xy(false)+SWAPXY` |
| 内 SRAM DMA 不够 76.8 KB 乒乓 | `strip_alloc` **回退 PSRAM DMA**；或 `NN_GFX=8`；或单缓冲 |
| 内 SRAM 连 64KB vram 都不够 | 启动失败并日志；需减 JIT/栈或缩 `NN_GFX`（**vram 不回退 PSRAM**） |
| splash 与 VGA 坐标不一致 | 与阶段 0/1 同改 |
| 64KB vram 不够某 Guest | 回退 `vga_mem_size` 至 128KB（仍建议内 SRAM；256KB 不可行） |

---

## 13. 附录：不支持模式处理（当前 lightvga 行为）

| 类型 | Guest | 屏幕 |
|------|-------|------|
| VBE 非 320×200×8 | 写成功，寄存器被 fixup | 按 320×200×8 显示 |
| 非 mode13 / 非 VBE 8bpp 图形 | 显存可写 | `unmatched`，不刷新，保留上一帧 |
| 80 列文本 | 正常 | 左 40 列，右侧裁切 |
| 非法文本参数 | 正常 | 不刷新 |
| 模式切换 | 正常 | `simplefb_clear` 清屏 |

---

## 15. 附录：条带缓冲测速（调试方案）

> **目的**：在真机上扫 **不同条带高度（NN）**、**缓冲域（SRAM / PSRAM）**、**单缓冲 / 乒乓**，量化栅格、SPI、整帧耗时，为 §4.2 的 `NN_TEXT` / `NN_GFX` 缺省值提供数据。  
> **范围**：仅 ESP32-S3 + ILI9341 固件；**不改变 Guest 行为**；测速代码用编译开关或 ini 启用，发布版默认关闭。

### 15.1 待扫参数（自变量）

| 轴 | 取值 | 说明 |
|----|------|------|
| **模式** | `micro` / `gfx` / `text` | 见 §15.3 |
| **`NN`** | 1, 2, 4, 8, 16, 32 | 条数；`strip_bytes = (240/NN)*320*2` |
| **缓冲域** | `sram` / `psram` / `auto` | 覆盖 §4.4.2；测速时可 **强制** 单域便于对比 |
| **乒乓** | `off` / `on` | 图形专用；文本固定 `off` |
| **vram** | `sram`（默认）/ `psram`（对照） | 对照「vram 内 SRAM」收益；发布仅用 `sram` |
| **帧数** | 预热 30 + 统计 120 | 可 ini 配置 |

**条带字节与 SPI 次数对照**（`NN` 必须整除 240）：

| NN | strip_bytes | 乒乓×2 | SPI 次/帧 |
|----|-------------|--------|-----------|
| 32 | 4 800 | 9.6 KB | 32 |
| 16 | 9 600 | 19.2 KB | 16 |
| 8 | 19 200 | 38.4 KB | 8 |
| 4 | 38 400 | 76.8 KB | 4 |
| 2 | 76 800 | 153.6 KB | 2 |
| 1 | 153 600 | 307.2 KB | 1 |

### 15.2 计时指标（因变量）

用 `esp_timer_get_time()`（µs），在 **条带循环内** 与 **整帧** 两级打点：

| 指标 | 符号 | 含义 |
|------|------|------|
| 栅格 | `t_raster` | 读 vram（或合成图案）→ 写入条带 |
| SPI 提交 | `t_spi_submit` | `lcd_draw()` 返回前 |
| SPI 完成 | `t_spi_wait` | `lcd_wait_flush_done()` |
| 条带合计 | `t_strip` | `t_raster + t_spi_submit + t_spi_wait` |
| 整帧 | `t_frame` | 所有条带之和（乒乓时含流水线重叠） |
| 墙钟 FPS | `fps` | `1e6 / t_frame`（或按 `pc_vga_step` 调用间隔） |

每条带额外记录（用于发现瓶颈）：

- `spi_bytes` = `strip_bytes`
- `heap_largest_dma` / `heap_largest_psram_dma`（该次分配后，可选）

**输出统计**：对 120 帧取 **min / avg / max**（avg 用整数 µs 或 0.1 ms）。

### 15.3 三种测试场景

#### A. `micro` — 纯缓冲 + SPI（无 vga_refresh）

隔离 Guest，只测 **条带大小 × DMA 域 × SPI 事务数**：

```text
for each NN, mem_domain, pingpong:
    alloc strip(s)
    warmup 30 frames
    for frame in 0..119:
        for each strip i:
            memset(strip, pattern, strip_bytes)     // 或固定棋盘，避免编译器优化掉
            t0 = now()
            lcd_draw(0, y0, 320, y1, strip)
            t_spi_submit = now() - t0
            lcd_wait_flush_done()
            t_spi_wait = now() - t_spi_submit
        // 乒乓 on：与 §5.2 相同流水线
    print row
```

用于回答：**同样 153600 B/帧，NN=32 vs NN=1 谁更快？SRAM 条带比 PSRAM 快多少？**

#### B. `gfx` — mode 13h / VBE 8bpp 实路径

- Guest 跑固定画面：**全屏调色板滚动** 或 **竖条移动**（保证每帧全刷新、无脏矩形）。
- 走完整 `vga_refresh` → 条带栅格 → SPI。
- 扫：`NN ∈ {32,16,8,4,2}` × `mem ∈ {sram,psram}` × `ping ∈ {off,on}`。

用于回答：**乒乓在何种 NN 下墙钟优于单缓冲？推荐 `NN_GFX` 取 4 还是 8？**

#### C. `text` — BIOS/DOS 文本全帧

- 固定文本屏（如 `DIR` 列表或静态菜单），每帧 `vga_text_refresh` 扫 16/8/32 条。
- **不测乒乓**（策略已定单缓冲）；只扫 `NN_TEXT` 与条带域。

用于确认：文本 **9.6 KB（NN=16）** 是否足够，更大条带是否因 SPI 次数减少而值得多占 SRAM。

### 15.4 实现要点（代码结构）

#### 15.4.1 编译 / 配置开关

```c
/* sdkconfig 或 board_ili9341.h */
#define CONFIG_LIGHTVGA_BENCH 0   /* 1 = 编入测速；发布必须为 0 */

/* tiny386.ini 示例（bench 专用段，解析在 esp_main） */
[lightvga_bench]
enable = 1
mode = gfx          ; micro | gfx | text
nn = 4,8,16         ; 逗号列表或 sweep 脚本由主机串口发
mem = auto,sram,psram
ping = on,off
vram = sram
warmup = 30
frames = 120
```

测速运行时 **暂停** 正常 `redraw` 节流（若有），避免与 i386 争用时可临时 **降频 Guest**（`micro` 模式可停 `pc_vga_step`）。

#### 15.4.2 建议文件与钩子

| 位置 | 内容 |
|------|------|
| `src/esp/main/lightvga_bench.c` | `lightvga_bench_run()`、`bench_sweep()`、CSV 打印 |
| `lightvga.c` | 可选回调 `StripTimings *`；或 bench 包装现有 refresh |
| `esp_main.c` | 启动后 `if (bench_enable) lightvga_bench_run(); else` 正常 `pc_new` |
| `lcd_ili9341.c` | 已有 `esp_timer`；bench 不改编驱动，仅调用 `lcd_draw` / `lcd_wait_flush_done` |

强制缓冲域（测速专用，绕过 `strip_alloc` 的 auto）：

```c
void *bench_strip_alloc(size_t n, strip_mem_t force)
{
    if (force == STRIP_SRAM)
        return heap_caps_malloc(n, MALLOC_CAP_DMA);
    if (force == STRIP_PSRAM)
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    return strip_alloc(n, &force);  /* auto */
}
```

#### 15.4.3 串口输出格式（CSV）

便于拷贝到表格对比：

```text
# lightvga_bench mode=gfx warmup=30 frames=120 vram=sram
mode,nn,mem,ping,strip_B,ping_total_B,spi_per_frame,t_raster_avg_us,t_spi_avg_us,t_frame_avg_us,fps_avg,strip_mem_ok
gfx,32,sram,on,4800,9600,32,xxxx,xxxx,xxxx,xx.x,1
gfx,16,sram,on,9600,19200,16,...
gfx,4,sram,on,38400,76800,4,...
gfx,4,psram,on,38400,76800,4,...
...
# RECOMMEND gfx NN=4 mem=auto ping=on  (人工或脚本标注)
```

每行一次 **完整 sweep 组合**；失败分配记 `strip_mem_ok=0` 并跳过或降 NN。

#### 15.4.4 扫参顺序（推荐）

```text
1. micro + sram only     → 建立 SPI/NN 基线（无 vga、无 PSRAM 争用）
2. micro + psram         → 条带 PSRAM DMA 惩罚量
3. gfx + vram=sram       → 全矩阵 NN × mem × ping
4. gfx + vram=psram      → 对照 vram 迁 SRAM 收益（可选）
5. text + NN ∈ {32,16,8} → 定 NN_TEXT
6. 根据 3–5 写回 §4.2 缺省值 + sdkconfig 注释
```

### 15.5 判定与收敛标准

| 场景 | 优选条件 |
|------|----------|
| **图形 `NN_GFX`** | 在 `strip_mem_ok=1` 前提下 **t_frame_avg 最小**；若 NN=4 与 NN=2 差距 <5%，取 **NN=4**（省 SRAM） |
| **图形乒乓** | `ping=on` 的 `t_frame` 明显低于 `off`（预期 ≥10%）；否则保留单缓冲并降 NN |
| **条带域** | 同 NN 下 `sram` 优于 `psram` 则 `auto` 默认 SRAM；PSRAM 仅作回退 |
| **文本 `NN_TEXT`** | 不追求 FPS；在 **avg 帧时可接受** 前提下取 **最小 strip_bytes**（默认 16） |
| **整帧 NN=1** | 仅作对照；若 SPI 队列/PSRAM 争用导致更慢，**不采用** |

记录结论到本文 **§14 修订** 一行，例如：`gfx NN_GFX=4 ping=on mem=auto 实测 fps=52`。

### 15.6 注意项

| 项 | 说明 |
|----|------|
| 预热 | 前 30 帧丢弃，避免首帧 alloc / cache cold |
| CPU 频率 | 测速固定 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`（如 240），记录到 CSV 注释 |
| SPI 时钟 | 记录 `LCD_SPI_CLOCK`（或 ini），变更后重跑 |
| 与 Guest 并行 | `gfx`/`text` 测速时记录是否 **绑核**、是否 **独占 vga_task**；对比时分两组 |
| `usleep` | 新路径 **不得** 在 bench 中保留旧 `usleep(900)` |
| 发布构建 | `CONFIG_LIGHTVGA_BENCH=0` 时 **零开销**（`#if` 剥掉 bench 对象文件） |

### 15.7 主机侧辅助（可选）

串口脚本（Python / PowerShell）发 ini 段、收集 CSV，自动生成 pivot 表：

```text
列：NN  行：mem+ping  值：t_frame_avg_us 或 fps_avg
```

便于一眼看出 **NN=4 sram+on** 是否最优。

---

## 14. 文档修订

| 日期 | 说明 |
|------|------|
| 2026-06-30 | 初版：汇总 LightVGA 范围、流式条带、MADCTL、内存与实施阶段 |
| 2026-06-30 | **vram 64KB 固定内 SRAM**；条带/乒乓 **SRAM 优先、PSRAM 回退**（§4.4） |
| 2026-06-30 | **§15 条带缓冲测速**：NN / 域 / 乒乓 sweep、CSV 输出、阶段 B 验收 |
