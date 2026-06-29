# tiny386hen

[Chinese](README.zh-CN.md)

tiny386hen is a third-party DOS / 386 PC emulator firmware adaptation for ESP32-S3. It uses Tiny386 reference code together with an ESP32-S3 adaptation layer, display and storage drivers, SeaBIOS build flow, and ready-to-flash firmware outputs.

This project is an independent third-party adaptation. It is not affiliated with, maintained by, endorsed by, or an official release of the upstream Tiny386 project.

The goal is to run a DOS environment on an ESP32-S3 board with PSRAM, an LCD, and an SD card. Peripheral parameters are configured through `tiny386.ini`, so common hardware wiring and boot options can be changed in the ini file without modifying the firmware source for every display, SD interface, or audio setting change.

## Features

- Targets ESP32-S3 with 8 MB or more PSRAM.
- Default LCD resolution is 320x240.
- LCD drivers currently support ST7789 and ILI9341.
- Supports I2S audio output.
- Supports both SD SPI and SDMMC SD-card wiring.
- Uses `tiny386.ini` to configure peripherals, boot images, and emulator parameters.
- Collects reference sources and license information for SeaBIOS, FreeDOS kernel, OPL2/YM3812 emulation, and related components.
- Provides build scripts for Windows PowerShell and Linux / WSL / MSYS2 shell.

## Hardware Requirements

- ESP32-S3 module or development board.
- PSRAM: 8 MB or more.
- Flash: 16 MB recommended.
- LCD: 320x240 resolution, currently ST7789 / ILI9341.
- SD card: SD SPI or SDMMC wiring.
- Audio: I2S DAC / codec / amplifier module.
- Recommended microSD card: 8 GB to 32 GB, formatted as a FAT32 MBR partition.

## Configuration

The firmware reads `tiny386.ini` to decide emulator and peripheral settings. The build scripts prefer:

```text
make/esp-ili9341/tiny386.ini
```

If that file does not exist, they use the default configuration from the reference source:

```text
refs/tiny386/esp/tiny386.ini
```

During build and flashing, `tiny386.ini` is written to the flash `ini` partition. At runtime, the firmware also tries to read a configuration file from the SD card. This keeps display, SD, audio, and boot-image parameters in an ini file and reduces the need to edit source code and rebuild firmware.

## Repository Layout

- `refs/tiny386`: upstream Tiny386 source, including SeaBIOS patch/config and `tiny386.ini`
- `refs/seabios`: upstream SeaBIOS source
- `refs/fdos-kernel`: FreeDOS kernel reference source
- `refs/emu8950`: MIT-licensed OPL2/YM3812 emulator reference source
- `src/esp/main`: local ESP32-S3 adaptation code
- `make/esp-ili9341`: ESP-IDF project, CMake files, `sdkconfig`, partition table, and linker fragments
- `script`: build helper scripts
- `release`: generated firmware binaries
- `copyright`: third-party reference project license notes

## Prepare Submodules

After cloning the repository, initialize the reference source trees:

```sh
git submodule update --init --recursive
```

## Build SeaBIOS

SeaBIOS is built from `refs/seabios` with the Tiny386 patch and config from `refs/tiny386/seabios`. The helper scripts apply the patch, copy the config, build SeaBIOS, and copy the final binaries into `release`.

### Windows

Install either:

- WSL with `make`, `gcc`, `python3`, and `iasl`
- MSYS2 installed at `C:\msys64` with `make`, `gcc`, `python3`, and `iasl`

Then run from the repository root:

```powershell
.\script\build-seabios.ps1
```

Useful options:

```powershell
.\script\build-seabios.ps1 -SkipFetch
.\script\build-seabios.ps1 -SkipClean
.\script\build-seabios.ps1 -BuildEnv Wsl
.\script\build-seabios.ps1 -BuildEnv Msys2
```

### Linux / WSL / MSYS2 Shell

Install `make`, `gcc`, `python3`, and `iasl`, then run:

```sh
./script/build-seabios.sh
```

Useful options:

```sh
./script/build-seabios.sh --skip-fetch
./script/build-seabios.sh --skip-clean
```

## Build ESP32-S3 Firmware

Install ESP-IDF 5.5 or newer and make sure `idf.py` is available in the current shell. The Windows examples below use an installed ESP-IDF at `C:\Espressif\frameworks\esp-idf-v5.5.1`; adjust the path if your ESP-IDF is installed elsewhere.

### Windows PowerShell

```powershell
. 'C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1'
.\script\build-ili9341.ps1
```

Clean rebuild:

```powershell
. 'C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1'
.\script\build-ili9341.ps1 -Clean
```

Build without generating the merged flash image:

```powershell
.\script\build-ili9341.ps1 -NoMerge
```

### Linux / WSL / MSYS2 Shell

```sh
. /path/to/esp-idf/export.sh
./script/build-ili9341.sh
```

Clean rebuild:

```sh
. /path/to/esp-idf/export.sh
CLEAN=1 ./script/build-ili9341.sh
```

Build without generating the merged flash image:

```sh
NO_MERGE=1 ./script/build-ili9341.sh
```

## Build Outputs

After successful SeaBIOS and ESP32-S3 firmware builds, the useful outputs are:

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

`release/esp/flash_image_ILI9341.bin` is a merged image ready to flash at offset `0x0`.

Generated ESP-IDF build directories are ignored by git:

```text
make/esp-ili9341/build_*/
make/esp-ili9341/managed_components/
```

## SD Card Recommendations

Use a reliable 8 GB to 32 GB microSD card formatted as a single FAT32 MBR partition. Class 10 or A1 cards from major brands tend to work best with ESP32 SDMMC / SDSPI wiring; very large exFAT-formatted cards, counterfeit cards, and old slow cards are more likely to fail during mount or emulator I/O.

For best compatibility, format the card with a 16 KB or 32 KB allocation unit. Copy DOS images and support files after formatting, and eject the card cleanly before inserting it into the ESP32-S3 board. If the card fails to initialize, try a shorter wiring path, check pull-ups on the SD lines, and test a smaller FAT32 card first.

## Flash Firmware

Connect the ESP32-S3 board in bootloader mode, then choose one of the following methods.

### Flash the Merged Image

This is the simplest release flashing command:

```powershell
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 .\release\esp\flash_image_ILI9341.bin
```

The same command from a POSIX shell:

```sh
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 release/esp/flash_image_ILI9341.bin
```

If multiple serial ports are present, add `-p COMx` on Windows or `-p /dev/ttyUSBx` / `-p /dev/ttyACMx` on Linux before `write_flash`.

### Flash Individual Images

```powershell
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 .\release\esp\bootloader.bin 0x8000 .\release\esp\partition-table.bin 0x10000 .\release\esp\tiny386hen_ili9341.bin 0x1d0000 .\release\esp\bios.bin 0x1f0000 .\release\esp\vgabios.bin 0x200000 .\release\esp\tiny386.ini
```

POSIX shell:

```sh
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 release/esp/bootloader.bin 0x8000 release/esp/partition-table.bin 0x10000 release/esp/tiny386hen_ili9341.bin 0x1d0000 release/esp/bios.bin 0x1f0000 release/esp/vgabios.bin 0x200000 release/esp/tiny386.ini
```

### Flash with ESP-IDF

When the ESP-IDF shell is active, you can also flash the project build directly:

```powershell
idf.py -C .\make\esp-ili9341 -B build_ili9341 -p COMx flash
```

POSIX shell:

```sh
idf.py -C make/esp-ili9341 -B build_ili9341 -p /dev/ttyUSB0 flash
```

This flashes the ESP-IDF app, bootloader, and partition table. Use the merged or individual-image commands above when you also want to flash `bios.bin`, `vgabios.bin`, and `tiny386.ini`.

## Manual SeaBIOS Build

For debugging, the same SeaBIOS build can be run manually:

```sh
cd refs/seabios
git apply --ignore-space-change ../tiny386/seabios/patch
cp ../tiny386/seabios/config .config
make PYTHON=python3 olddefconfig
make PYTHON=python3
```

The expected firmware images are `out/bios.bin` and `out/vgabios.bin`.

## License

This project is released under the BSD-3-Clause License. See [LICENSE](LICENSE). Third-party reference projects keep their original licenses. This independent adaptation is not affiliated with the upstream Tiny386 project. See [copyright/README.zh-CN.md](copyright/README.zh-CN.md) for details.
