# tiny386hen

[Chinese](README.zh-CN.md)

This repository collects the reference sources, ESP overlay code, build
configuration, and release firmware assets used for Tiny386 on ESP32-S3 with an
ILI9341 display.

## License

This project is released under the MIT License. See [LICENSE](LICENSE).
Third-party reference projects keep their original licenses. See
[copyright/README.zh-CN.md](copyright/README.zh-CN.md) for details.

## Repository Layout

- `refs/tiny386`: upstream Tiny386 source, including SeaBIOS patch/config and `tiny386.ini`
- `refs/seabios`: upstream SeaBIOS source
- `refs/fdos-kernel`: FreeDOS kernel reference source
- `refs/emu8950`: MIT-licensed OPL2/YM3812 emulator reference source
- `src/esp/main`: local ESP32-S3/ILI9341 code only
- `make/esp-ili9341`: ESP-IDF project, CMake files, `sdkconfig`, partition table, and linker fragments
- `script`: build helper scripts
- `release`: generated firmware binaries

## Prepare Submodules

After cloning the repository, initialize the reference source trees:

```sh
git submodule update --init --recursive
```

## Build SeaBIOS

SeaBIOS is built from `refs/seabios` with the Tiny386 patch and config from
`refs/tiny386/seabios`. The helper scripts apply the patch, copy the config,
build SeaBIOS, and copy the final binaries into `release`.

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

## Build ESP32-S3 ILI9341 Firmware

Install ESP-IDF 5.5 or newer and make sure `idf.py` is available in the current
shell. The Windows examples below use an installed ESP-IDF at
`C:\Espressif\frameworks\esp-idf-v5.5.1`; adjust the path if your ESP-IDF is
installed elsewhere.

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

After successful SeaBIOS and ILI9341 builds, the useful outputs are:

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

`release/esp/flash_image_ILI9341.bin` is a merged image ready to flash at offset
`0x0`.

## SD Card Recommendations

Use a reliable 8 GB to 32 GB microSD card formatted as a single FAT32 MBR
partition. Class 10 or A1 cards from major brands tend to work best with ESP32
SDMMC/SDSPI wiring; very large exFAT-formatted cards, counterfeit cards, and
old slow cards are more likely to fail during mount or under emulator I/O.

For best compatibility, format the card with a 16 KB or 32 KB allocation unit,
copy DOS images and support files after formatting, and eject the card cleanly
before inserting it into the ESP32-S3 board. If the card fails to initialize,
try a shorter wiring path, check pull-ups on the SD lines, and test a smaller
FAT32 card before changing firmware settings.

Generated ESP-IDF build directories are ignored by git:

```text
make/esp-ili9341/build_*/
make/esp-ili9341/managed_components/
```

## Flash Firmware

Connect the ESP32-S3 board in bootloader mode, then choose one of the following
methods.

### Flash the Merged Image

This is the simplest release flashing command:

```powershell
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 .\release\esp\flash_image_ILI9341.bin
```

The same command from a POSIX shell:

```sh
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash 0x0 release/esp/flash_image_ILI9341.bin
```

If multiple serial ports are present, add `-p COMx` on Windows or
`-p /dev/ttyUSBx` / `-p /dev/ttyACMx` on Linux before `write_flash`.

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

This flashes the ESP-IDF app, bootloader, and partition table. Use the merged or
individual-image commands above when you also want to flash `bios.bin`,
`vgabios.bin`, and `tiny386.ini`.

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
