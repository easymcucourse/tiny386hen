# tiny386hen

[English](README.md)

本仓库整理 Tiny386 在 ESP32-S3 + ILI9341 屏幕上的参考源码、本地 ESP 适配代码、构建配置和固件产物。

## 许可证

本项目以 MIT License 发布，详见 [LICENSE](LICENSE)。
第三方参考项目保留原始许可证，详见 [copyright/README.zh-CN.md](copyright/README.zh-CN.md)。

## 目录结构

- `refs/tiny386`：上游 Tiny386 源码，包含 SeaBIOS patch/config 和 `tiny386.ini`
- `refs/seabios`：上游 SeaBIOS 源码
- `refs/fdos-kernel`：FreeDOS kernel 参考源码
- `refs/emu8950`：MIT 许可的 OPL2/YM3812 模拟器参考源码
- `src/esp/main`：本项目 ESP32-S3/ILI9341 代码，只放代码
- `make/esp-ili9341`：ESP-IDF 工程、CMake、`sdkconfig`、分区表和 linker fragment
- `script`：构建辅助脚本
- `release`：生成的固件二进制文件

## 初始化子模块

克隆仓库后，先初始化参考源码：

```sh
git submodule update --init --recursive
```

## 编译 SeaBIOS

SeaBIOS 从 `refs/seabios` 编译，并使用 `refs/tiny386/seabios` 中的 Tiny386 patch 和配置。
辅助脚本会自动应用 patch、复制配置、编译 SeaBIOS，并把最终二进制复制到 `release`。

### Windows

先安装以下任一环境：

- WSL，并安装 `make`、`gcc`、`python3`、`iasl`
- MSYS2，安装路径为 `C:\msys64`，并安装 `make`、`gcc`、`python3`、`iasl`

然后在仓库根目录运行：

```powershell
.\script\build-seabios.ps1
```

常用参数：

```powershell
.\script\build-seabios.ps1 -SkipFetch
.\script\build-seabios.ps1 -SkipClean
.\script\build-seabios.ps1 -BuildEnv Wsl
.\script\build-seabios.ps1 -BuildEnv Msys2
```

### Linux / WSL / MSYS2 shell

安装 `make`、`gcc`、`python3`、`iasl` 后运行：

```sh
./script/build-seabios.sh
```

常用参数：

```sh
./script/build-seabios.sh --skip-fetch
./script/build-seabios.sh --skip-clean
```

## 编译 ESP32-S3 ILI9341 固件

安装 ESP-IDF 5.5 或更新版本，并确保当前 shell 能找到 `idf.py`。
下面的 Windows 示例假设 ESP-IDF 安装在 `C:\Espressif\frameworks\esp-idf-v5.5.1`；
如果安装路径不同，请自行替换。

### Windows PowerShell

```powershell
. 'C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1'
.\script\build-ili9341.ps1
```

清理后重新编译：

```powershell
. 'C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1'
.\script\build-ili9341.ps1 -Clean
```

只编译，不生成合并烧录镜像：

```powershell
.\script\build-ili9341.ps1 -NoMerge
```

### Linux / WSL / MSYS2 shell

```sh
. /path/to/esp-idf/export.sh
./script/build-ili9341.sh
```

清理后重新编译：

```sh
. /path/to/esp-idf/export.sh
CLEAN=1 ./script/build-ili9341.sh
```

只编译，不生成合并烧录镜像：

```sh
NO_MERGE=1 ./script/build-ili9341.sh
```

## 编译产物

SeaBIOS 和 ILI9341 编译成功后，主要产物如下：

```text
release/bios.bin
release/vgabios.bin
release/esp/bootloader.bin
release/esp/partition-table.bin
release/esp/tiny386hen_ili9341.bin
release/esp/bios.bin
release/esp/vgabios.bin
release/esp/tiny386.ini
release/esp/flash_image_ILI9341.bin
```

`release/esp/flash_image_ILI9341.bin` 是已经合并好的整包镜像，可以从 `0x0` 直接烧录。

## SD 卡建议

建议使用可靠品牌的 8 GB 到 32 GB microSD 卡，并格式化为单个 FAT32 MBR 分区。
Class 10 或 A1 卡通常更适合 ESP32 的 SDMMC/SDSPI 接线；容量很大的 exFAT 卡、
来路不明的卡和老旧低速卡更容易在挂载或模拟器读写时出问题。

为了提高兼容性，格式化时建议使用 16 KB 或 32 KB 分配单元，格式化后再复制 DOS
镜像和支持文件，并在插入 ESP32-S3 开发板前正常弹出 SD 卡。如果 SD 卡初始化失败，
先尝试缩短接线、检查 SD 信号线的上拉电阻，并优先换一张较小容量的 FAT32 卡测试，
再考虑修改固件配置。

ESP-IDF 生成目录已被 git 忽略：

```text
make/esp-ili9341/build_*/
make/esp-ili9341/managed_components/
```

## 烧录固件

先让 ESP32-S3 开发板进入 bootloader 模式，然后选择以下任一方式。

### 烧录合并镜像

这是最简单的 release 烧录方式：

```powershell
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 .\release\esp\flash_image_ILI9341.bin
```

POSIX shell：

```sh
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 release/esp/flash_image_ILI9341.bin
```

如果电脑上有多个串口，在 `write_flash` 前增加 `-p COMx` 或 `-p /dev/ttyUSBx` / `-p /dev/ttyACMx`。

### 分文件烧录

```powershell
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 .\release\esp\bootloader.bin 0x8000 .\release\esp\partition-table.bin 0x10000 .\release\esp\tiny386hen_ili9341.bin 0x1d0000 .\release\esp\bios.bin 0x1f0000 .\release\esp\vgabios.bin 0x200000 .\release\esp\tiny386.ini
```

POSIX shell：

```sh
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 release/esp/bootloader.bin 0x8000 release/esp/partition-table.bin 0x10000 release/esp/tiny386hen_ili9341.bin 0x1d0000 release/esp/bios.bin 0x1f0000 release/esp/vgabios.bin 0x200000 release/esp/tiny386.ini
```

### 使用 ESP-IDF 烧录

进入 ESP-IDF 环境后，也可以直接烧录工程构建产物：

```powershell
idf.py -C .\make\esp-ili9341 -B build_ili9341 -p COMx flash
```

POSIX shell：

```sh
idf.py -C make/esp-ili9341 -B build_ili9341 -p /dev/ttyUSB0 flash
```

这种方式只烧录 ESP-IDF app、bootloader 和 partition table。
如果还要一起烧录 `bios.bin`、`vgabios.bin`、`tiny386.ini`，请使用上面的合并镜像或分文件烧录命令。

## 手动编译 SeaBIOS

调试时也可以手动执行同等步骤：

```sh
cd refs/seabios
git apply --ignore-space-change ../tiny386/seabios/patch
cp ../tiny386/seabios/config .config
make PYTHON=python3 olddefconfig
make PYTHON=python3
```

预期生成的固件镜像为 `out/bios.bin` 和 `out/vgabios.bin`。
