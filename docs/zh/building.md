# 构建与打包

> [文档索引](../README.md) · [返回 README](../../README.md) · [English](../en/building.md)

本页说明如何在 Linux、Windows 与 macOS 上从源码构建 Mivara、各构建目标的用途，以及在 Windows 上打包安装程序。扩展模式所需的虚拟显示器驱动安装见[虚拟显示器驱动](virtual-display.md)。

## 环境要求

**Linux**

- Ubuntu 18.04+ / Debian 10+ / Fedora 32+ / Arch
- GCC 8+（C++17）、make
- libX11-dev、libsdl2-dev、libsdl2-ttf-dev、pthread
- 任一系统中文字体（`fonts-wqy-microhei` 或 Noto Sans CJK）——GUI 缺字体会直接报错退出

**Windows**

- Windows 10 1903+ / Windows 11
- MinGW-w64（MSYS2）+ make——**不支持 MSVC**（构建仅走 g++/makefile）
- SDL2、SDL2_ttf（MSYS2 包），Windows SDK（iphlpapi，端口检查器需要）
- 扩展模式：虚拟显示器驱动（见[虚拟显示器驱动](virtual-display.md)）
- `sender` / `app` 内嵌 `requireAdministrator` 清单——**需以管理员运行**（启动时
  UAC 确认一次）。扩展模式要用管理员权限启用/禁用虚拟显示设备，使其会话结束后
  从「设置 → 屏幕」彻底消失。`receiver` 不需要。

**macOS**

- macOS Catalina 10.15+，Clang 12+
- `brew install sdl2 sdl2_ttf`

> `SDL2_ttf` 为必需依赖（GUI 中文界面）。
>
> **注意**：Windows 部署时运行时 DLL 需与 exe 同目录（`bin/` 已包含完整闭包）。新增依赖后可用 `ldd bin/app.exe` 解析闭包补齐。

## 安装依赖

```bash
make install-deps          # Linux/macOS/MSYS2 自动安装
```

| 组件 | 用途 |
|------|------|
| g++ / make | C++17 构建 |
| libX11-dev | Linux 屏幕采集 |
| SDL2 / SDL2_ttf | 接收端渲染 + GUI（中文字体） |
| **FFmpeg**（libavcodec/libavutil/libswscale） | H.264/H.265/AV1 编解码（MSYS2: `mingw-w64-x86_64-ffmpeg`） |

## 从源码构建

从源码编译三步：装依赖 → make → 运行 `bin/` 下的产物。

```bash
git clone https://github.com/wzy-777/Mivara.git
cd Mivara
make install-deps    # 1. 安装系统依赖（Linux/macOS/MSYS2 自动识别包管理器）
make                 # 2. 编译全部程序（产物输出到 bin/）
./bin/app            # 3. 运行图形界面（或 bin/sender、bin/receiver）
```

> Windows：在 MSYS2 MinGW64 终端中执行同样的命令；不支持 MSVC。

## 构建目标

可单独构建或使用开发辅助目标：

```bash
make              # 全部构建（bin/sender、bin/receiver、bin/app）
make sender       # 仅发送端
make receiver     # 仅接收端
make app          # 仅图形启动器
make debug        # -g -O0 -DDEBUG 调试构建
make clean        # 清理产物
make check        # 检查环境/源码
make run          # 构建并启动图形启动器（另有 run-sender / run-receiver）
```

构建使用 `-O3 -march=native -flto` 优化，产物输出到 `bin/`。

## Windows 控制台说明

- `sender` 以控制台子系统链接——有日志输出，Ctrl+C 优雅退出（含虚拟显示器分离）。
- `receiver` / `app` 以 GUI 子系统链接（`-mwindows`），无控制台输出；如需接收端日志，可手动用 `-mconsole` 重新链接 receiver。

## 打包安装程序

用 Inno Setup 编译安装包：

```powershell
& "D:\Program Files\Inno Setup 6\ISCC.exe" installer\Mivara.iss
# 产物：dist\Mivara-Setup-<版本>.exe
```

发布时需**同时**上传固定名副本 `dist\Mivara-Setup.exe`（应用内「下载更新」与 README 的下载徽章都用这个固定名地址），以及 `SHA256SUMS.txt`（应用内安装的校验依据，缺失时只提供浏览器下载）。CI 打包会自动生成这三个文件；本地打包可手动复制：`copy dist\Mivara-Setup-<版本>.exe dist\Mivara-Setup.exe`。

安装包内容：`bin/`（exe + DLL）、`scripts/`、`vdd/` 虚拟显示器驱动（不含体积超限的厂商 GUI），以及离线文档（`README.md`、`README_EN.md`、`LICENSE`、`CODE_SIGNING_POLICY.md`、`docs/`），安装后 README 里的相对链接仍可打开；安装后在开始菜单/桌面创建快捷方式。完成页的「启动 Mivara」勾选项直接拉起管理员权限的 GUI（安装器以提权令牌启动，无二次 UAC 弹窗）。
