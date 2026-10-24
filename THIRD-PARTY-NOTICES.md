# Third-party notices / 第三方组件声明

Mivara (幻屏) source code is released under the MIT License — see [LICENSE](LICENSE). This file lists the third-party components bundled with or linked into Mivara's binaries and installer. It does not change, replace or extend the licenses of those components; each component remains under its own terms.

Mivara（幻屏）的源码以 MIT 许可证发布，详见 [LICENSE](LICENSE)。本文件列出随 Mivara 二进制或安装包分发、或与其链接的第三方组件。本文件不改变、不替代也不扩展这些组件自身的许可条款，各组件仍适用其各自的许可证。

## Virtual Display Driver (MttVDD) / 虚拟显示驱动（MttVDD）

- **License / 许可证**: MIT License, Copyright (c) 2024 Virtual Display.
- **Source / 源码**: <https://github.com/VirtualDrivers/Virtual-Display-Driver>
- **Full notice / 完整声明**: [`vdd/LICENSE`](vdd/LICENSE).
- The installer packages both the x86/AMD64 and ARM64 driver packages; `install_vdd.ps1` picks the directory that matches the machine. / 安装包同时包含 x86/AMD64 与 ARM64 驱动包，`install_vdd.ps1` 会按当前机器架构选择目录。
- The driver is required for extend mode. / 扩展模式需要该驱动。

## SDL2 and SDL2_ttf / SDL2 与 SDL2_ttf

- **License / 许可证**: zlib license.
- **Source / 源码**: <https://www.libsdl.org/>
- SDL2 is used by the GUI and receiver for windowing, rendering and text; SDL2_ttf provides TrueType font rendering. / SDL2 用于 GUI 与接收端的窗口、渲染和文本显示；SDL2_ttf 提供 TrueType 字体渲染。
- Windows runtime DLLs are bundled in the installer. / Windows 运行时 DLL 随安装包一并分发。

## FFmpeg (libavcodec / libavutil / libswscale)

- **License / 许可证**: the release build links the FFmpeg libraries supplied by the build environment. The CI build uses MSYS2's `mingw-w64-x86_64-ffmpeg`, which is configured with `--enable-gpl --enable-version3` and is licensed **GPL-3.0-or-later**; the combined release binary must therefore be distributed under GPL-3.0-or-later terms. The Mivara source itself remains MIT.
- **许可证**：发布构建链接构建环境提供的 FFmpeg 库。CI 使用 MSYS2 的 `mingw-w64-x86_64-ffmpeg`，其 configure 参数包含 `--enable-gpl --enable-version3`，许可证为 **GPL-3.0-or-later**；因此合并后的发布二进制需按 GPL-3.0-or-later 分发。Mivara 源码本身仍为 MIT。
- **License text / 许可证全文**: [`LICENSES/GPL-3.0-or-later`](LICENSES/GPL-3.0-or-later) (shipped with the installer). / [`LICENSES/GPL-3.0-or-later`](LICENSES/GPL-3.0-or-later)（随安装包分发）。
- **Corresponding source / 对应源码**: the project does not modify FFmpeg and does not vendor its source. The exact source used by a release is the MSYS2 source package matching the `mingw-w64-x86_64-ffmpeg` version installed by CI: source archives are published at <https://repo.msys2.org/mingw/sources/> and the package recipe at <https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-ffmpeg>; upstream source is at <https://ffmpeg.org/download.html>. Mivara's own corresponding source is this repository. / 本项目不修改 FFmpeg，也不内置其源码。某次发布对应的源码就是与 CI 所装 `mingw-w64-x86_64-ffmpeg` 版本一致的 MSYS2 源码包：源码归档发布在 <https://repo.msys2.org/mingw/sources/>，包配方在 <https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-ffmpeg>，上游源码在 <https://ffmpeg.org/download.html>。Mivara 自身的对应源码即本仓库。
- If an MIT/LGPL-only binary is required, link an LGPL FFmpeg build instead of the distro default. / 若需要仅受 MIT/LGPL 约束的二进制，请改为链接 LGPL 构建的 FFmpeg，而不是发行版默认包。

## devcon.exe

- A Microsoft Windows Driver Kit tool used by `vdd/install_vdd.ps1` and `vdd/install_vdd2.ps1` to create the root-enumerated device. / 这是 Microsoft Windows Driver Kit 提供的工具，由 `vdd/install_vdd.ps1` 与 `vdd/install_vdd2.ps1` 用于创建根枚举设备。
- Redistribution is governed by the Microsoft WDK license; verify before redistributing. / 其再分发受 Microsoft WDK 许可证约束；再分发前请自行核实。

## Windows runtime DLLs / Windows 运行时 DLL

- `make copy-dlls` copies the MinGW-w64 runtime closure into `bin/`, and the installer ships it. Depending on the build machine this includes the GCC runtime libraries (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` — the GCC runtime libraries carry the GCC Runtime Library Exception) and SDL2_ttf's transitive dependencies (typically FreeType, HarfBuzz, Graphite2, libpng, zlib, bzip2, Brotli, GLib, gettext/libintl and libiconv). Each remains under its own license.
- `make copy-dlls` 会把 MinGW-w64 运行时闭包复制到 `bin/`，安装包会一并分发。具体清单取决于构建机，通常包括 GCC 运行时库（`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`，其中 GCC 运行时库带有 GCC Runtime Library Exception）以及 SDL2_ttf 的传递依赖（常见为 FreeType、HarfBuzz、Graphite2、libpng、zlib、bzip2、Brotli、GLib、gettext/libintl、libiconv）。各组件仍适用其各自的许可证。
- Regenerate the exact closure with `ldd bin/*.exe` when preparing a release and ship the matching notices. / 发布前请用 `ldd bin/*.exe` 重新确认实际清单，并随附相应的许可证声明。

## Corrections / 更正

Questions or corrections: please open a GitHub [issue](https://github.com/wzy-777/Mivara/issues).

如有疑问或需要更正，请提交 GitHub [issue](https://github.com/wzy-777/Mivara/issues)。
