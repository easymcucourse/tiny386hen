# tiny386hen

[English](README.md)

本仓库用于整理 Tiny386 相关固件资产的参考源码和本地构建脚本。

## 许可证

本项目以 MIT License 发布，详见 [LICENSE](LICENSE)。

第三方参考项目保留其原始许可证，详见
[copyright/README.zh-CN.md](copyright/README.zh-CN.md)。

## 目录结构

- `refs/tiny386`：上游 Tiny386 源码，包含 SeaBIOS 补丁和配置文件
- `refs/seabios`：上游 SeaBIOS 源码
- `refs/fdos-kernel`：FreeDOS kernel 参考源码
- `refs/emu8950`：MIT 许可证的 OPL2/YM3812 模拟器参考源码
- `script`：本地构建辅助脚本
- `release`：生成的固件二进制文件

## 初始化子模块

克隆仓库后，先初始化参考源码目录：

```sh
git submodule update --init --recursive
```

## 编译 SeaBIOS

SeaBIOS 从 `refs/seabios` 编译，并使用 `refs/tiny386/seabios` 中的 Tiny386
补丁和配置。辅助脚本会自动完成应用补丁、复制配置、编译 SeaBIOS，并把最终
二进制文件复制到 `release` 目录。

### Windows

请先安装以下任一环境：

- WSL，并安装 `make`、`gcc`、`python3`
- MSYS2，安装路径为 `C:\msys64`，并安装 `make`、`gcc`、`python3`

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

安装 `make`、`gcc`、`python3` 后运行：

```sh
./script/build-seabios.sh
```

常用参数：

```sh
./script/build-seabios.sh --skip-fetch
./script/build-seabios.sh --skip-clean
```

## 编译输出

编译成功后，SeaBIOS 输出文件会复制到：

```text
release/bios.bin
release/vgabios.bin
```

原始编译输出也会保留在：

```text
refs/seabios/out/bios.bin
refs/seabios/out/vgabios.bin
```

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
