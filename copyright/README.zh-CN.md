# 版权和许可证说明

本项目自身代码以 MIT License 发布，详见仓库根目录的 `LICENSE`。

本仓库同时包含或引用若干第三方/参考项目。第三方项目仍然遵循其各自的原始
许可证；本项目的 MIT License 不会改变这些第三方项目的授权条款。

## 本项目

- 项目：tiny386hen
- 许可证：MIT License
- 许可证文件：`../LICENSE`

## 参考项目

| 项目 | 路径 | 许可证 | 原始许可证副本 |
| --- | --- | --- | --- |
| Tiny386 | `refs/tiny386` | BSD-3-Clause | `TINY386-LICENSE.txt` |
| SeaBIOS | `refs/seabios` | GNU LGPL-3.0 | `SEABIOS-COPYING.LESSER.txt` |
| SeaBIOS GPL 文本 | `refs/seabios` | GNU GPL-3.0 | `SEABIOS-COPYING.txt` |
| FreeDOS kernel | `refs/fdos-kernel` | GNU GPL-2.0 | `FDOS-KERNEL-COPYING.txt` |
| emu8950 | `refs/emu8950` | MIT License | `EMU8950-LICENSE.txt` |

## 注意事项

- `release/bios.bin` 和 `release/vgabios.bin` 来源于 SeaBIOS 构建产物，
  需要遵循 SeaBIOS 的 LGPL-3.0 授权要求。
- `refs/tiny386` 中的 Tiny386 参考源码遵循 BSD-3-Clause。
- `refs/fdos-kernel` 中的 FreeDOS kernel 参考源码遵循 GPL-2.0。
- ESP overlay 中的 Adlib/OPL2 后端使用 `refs/emu8950`，不再编译
  Tiny386 参考源码中的 `fmopl.c`。
- 如果发布包含第三方二进制或源码的包，请一并保留本目录中的对应许可证文本。
