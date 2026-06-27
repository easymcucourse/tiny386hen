# tiny386hen

[中文说明](README.zh-CN.md)

This repository collects the reference sources and helper scripts used to build
Tiny386-related firmware assets.

## License

This project is released under the MIT License. See [LICENSE](LICENSE).
Third-party reference projects keep their original licenses. See
[copyright/README.zh-CN.md](copyright/README.zh-CN.md) for details.

## Repository Layout

- `refs/tiny386`: upstream Tiny386 source, including the SeaBIOS patch and config
- `refs/seabios`: upstream SeaBIOS source
- `refs/fdos-kernel`: FreeDOS kernel reference source
- `refs/emu8950`: MIT-licensed OPL2/YM3812 emulator reference source
- `script`: local build helper scripts
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

- WSL with `make`, `gcc`, and `python3`
- MSYS2 installed at `C:\msys64` with `make`, `gcc`, and `python3`

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

### Linux / WSL / MSYS2 shell

Install `make`, `gcc`, and `python3`, then run:

```sh
./script/build-seabios.sh
```

Useful options:

```sh
./script/build-seabios.sh --skip-fetch
./script/build-seabios.sh --skip-clean
```

## Build Outputs

After a successful build, the SeaBIOS outputs are copied to:

```text
release/bios.bin
release/vgabios.bin
```

The original build outputs also remain in:

```text
refs/seabios/out/bios.bin
refs/seabios/out/vgabios.bin
```

## Manual SeaBIOS Build

For debugging, the same build can be run manually:

```sh
cd refs/seabios
git apply --ignore-space-change ../tiny386/seabios/patch
cp ../tiny386/seabios/config .config
make PYTHON=python3 olddefconfig
make PYTHON=python3
```

The expected firmware images are `out/bios.bin` and `out/vgabios.bin`.
