# Mivara (幻屏)

中文 | [English](README_EN.md)

**零配置局域网投屏 / 无线扩展屏**

一个跨平台的 C++ 项目：把局域网内的任何一台电脑变成你的第二台显示器。

[![Version](https://img.shields.io/badge/version-0.9.2-blue)](https://github.com/wzy-777/Mivara)
[![下载最新安装包](https://img.shields.io/badge/download-latest%20installer-brightgreen?logo=github&logoColor=white)](https://github.com/wzy-777/Mivara/releases/latest/download/Mivara-Setup.exe)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#环境要求)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#环境要求)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

> 更名说明：本项目原名 **TwinView**，自 v0.9.1 起更名为 **Mivara**（幻屏）；旧的 GitHub 链接会自动跳转到新地址。

---

## 目录

- [Mivara (幻屏)](#mivara-幻屏)
  - [目录](#目录)
  - [项目简介](#项目简介)
  - [功能特性](#功能特性)
  - [架构](#架构)
    - [目录结构](#目录结构)
    - [可执行程序](#可执行程序)
    - [网络端口](#网络端口)
  - [工作原理](#工作原理)
    - [1 — 发现（SSDP）](#1--发现ssdp)
    - [2 — 扩展握手](#2--扩展握手)
    - [3 — 虚拟显示器生命周期（扩展模式）](#3--虚拟显示器生命周期扩展模式)
    - [4 — 帧流](#4--帧流)
    - [5 — 附加服务](#5--附加服务)
  - [编解码](#编解码)
  - [设置与历史](#设置与历史)
  - [显示模式](#显示模式)
  - [环境要求](#环境要求)
  - [安装依赖](#安装依赖)
  - [编译](#编译)
    - [Windows 控制台说明](#windows-控制台说明)
    - [打包安装程序（Windows）](#打包安装程序windows)
  - [使用指南](#使用指南)
    - [图形界面（推荐）](#图形界面推荐)
    - [接收端按键](#接收端按键)
    - [发送端按键](#发送端按键)
  - [Windows 虚拟显示器驱动](#windows-虚拟显示器驱动)
    - [安装步骤（管理员 PowerShell）](#安装步骤管理员-powershell)
    - [分辨率说明](#分辨率说明)
  - [网络配置](#网络配置)
    - [防火墙规则（接收端）](#防火墙规则接收端)
    - [网络要求](#网络要求)
  - [性能](#性能)
    - [带宽（未压缩 RGB24）](#带宽未压缩-rgb24)
    - [延迟](#延迟)
  - [故障排除](#故障排除)
    - [Windows SmartScreen 提示说明](#windows-smartscreen-提示说明)
    - [诊断命令](#诊断命令)
  - [路线图](#路线图)
  - [致谢](#致谢)
  - [许可证](#许可证)

---

## 项目简介

Mivara（幻屏）是一个轻量级、跨平台的**屏幕扩展/投屏**工具。发送端（sender）采集本机画面，通过网络推流到接收端（receiver），接收端全屏显示。

项目提供两种模式：

1. **镜像投屏** —— 接收端显示发送端主屏的完整画面（演示/展示场景）
2. **扩展屏** —— 发送端**自动激活虚拟显示器**，创建一块真实的桌面空间；窗口和鼠标可以直接拖入/移入这块"看不见的屏幕"，接收端实时显示。会话结束时虚拟显示器自动关闭。

扩展模式需要 Windows 间接显示驱动（IddCx）支持，安装方法见[下文](#windows-虚拟显示器驱动)。驱动只需安装一次；虚拟显示器的开/关由程序在会话期间自动管理。

---

## 功能特性

| 类别 | 能力 |
|------|------|
| **扩展屏** | 会话开始自动启用并挂载虚拟显示器，任何退出路径（正常结束 / Ctrl+C / 断开 / 关窗 / 强杀兜底）都分离并禁用设备，彻底从系统设置消失 |
| **虚拟显示器集成** | 程序内完成激活/停用/定位（CCD API），无需手动操作显示设置 |
| **视频编码** | H.264 / H.265 / AV1（FFmpeg），硬件优先（NVENC/QSV/AMF/VAAPI）自动回退软件（libx264/libx265/SVT-AV1）；保留原始/RLE 无损选项 |
| **运行时调参** | 传输中随时改编码/画质/码率/帧率/缩放/色度，秒级热生效不断流（换编码瞬断一两帧后恢复） |
| **鼠标可见** | 光标直接绘制进视频帧；扩展模式下即"指针穿越" |
| **直连模式** | `sender <IP>` 跳过发现阶段，SSDP 多播被网络策略拦截时仍可连接 |
| **发现** | SSDP（UDP 1900 多播 + 广播兜底）零配置自动发现接收端，显示主机名 |
| **连接历史** | 主页内嵌最近 20 条连接，一键直连免搜索 |
| **图形界面** | SDL2_ttf 中文 GUI：左侧导航（主页/设置）、接收/发送、连接历史、自动搜索、机器列表（投屏/扩展）、手动输入 IP、实时状态面板、独立日志窗口 |
| **自启动 / 自动接收** | 开机自启动（Windows 计划任务，登录免 UAC）；自动接收开启后打开软件直接进入接收模式（默认关） |
| **检查更新** | 设置页「检查更新」按钮 + 启动自动检查（默认开，同一台机器每 24 小时至多一次，可在设置中关闭）；发现新版本可应用内下载安装包（校验大小与 SHA-256）后一键安装，或打开发布页手动下载。只访问 GitHub 发布源，不上传任何本地数据 |
| **性能** | 120 FPS 推流、4 MB 流 socket 缓冲、TCP_NODELAY 优化 |
| **CPU 卸载** | RLE 帧压缩与色彩修正可卸载到接收端 CPU（TCP 8082，仅 RLE 模式） |
| **端口检查器** | 远程列出/查询/终止接收端任意端口进程（TCP 8083） |
| **跨平台** | Linux（X11）、Windows 10/11、macOS 10.15+ |

---

## 架构

### 目录结构

```
Mivara/
├── makefile                  # 跨平台构建（Linux/macOS/Windows MinGW）
├── src/
│   ├── app.cpp               # 图形启动器（SDL2 + SDL2_ttf）：左侧导航 + 主页/设置（历史内嵌主页）
│   ├── sender.cpp            # 采集、握手、推流
│   ├── receiver.cpp          # 全屏显示、SSDP 广播、计算服务、端口服务
│   ├── proto.h               # 协议 v2（握手/控制包/编码枚举）
│   ├── codec.cpp/.h          # FFmpeg 编解码（H.264/H.265/AV1，sender + receiver）
│   ├── config.cpp/.h         # GUI 设置持久化（config.ini）
│   ├── update.cpp/.h         # 更新检查 + 应用内下载安装（GUI 专用）
│   ├── version.h             # 版本号唯一来源（C++ 与 Inno Setup 共用）
│   ├── history.cpp/.h        # 连接历史（history.ini）
│   ├── discover.cpp/.h       # SSDP 发现引擎
│   ├── gpu_accelerate.c/.h   # 远程 CPU 卸载（RLE 压缩 / 色彩修正）
│   ├── ports.cpp/.h          # 远程端口检查服务
│   └── vdd.cpp/.h            # Windows 虚拟显示器生命周期（sender + GUI 共用）
├── bin/                      # 可执行程序 + 运行时 DLL（构建输出）
├── scripts/                  # Windows 便捷启动脚本
├── installer/                # Inno Setup 打包脚本（Mivara.iss）
├── vdd/                      # 虚拟显示器驱动（MttVDD 签名驱动 + 安装脚本）
├── winget/                   # winget-pkgs 上架清单（每次发布更新）
├── test/                     # 本地测试 / 诊断代码（不入库）
└── build/                    # 目标文件
```

### 可执行程序

| 程序 | 职责 |
|------|------|
| `app` | 图形启动器：左侧导航（主页/设置）、接收/发送选择、主页内嵌连接历史、SSDP 搜索、机器列表（投屏/扩展）、手动 IP、运行时调参 |
| `sender` | 采集画面（GDI/X11/CoreGraphics），协商模式，推流；扩展模式下经 vdd 模块管理虚拟显示器的激活与定位 |
| `receiver` | SSDP 广播，全屏渲染，承载计算卸载与端口检查两个附加服务 |

### 网络端口

| 端口 | 协议 | 用途 |
|------|------|------|
| 1900 | UDP 多播 | SSDP 发现（M-SEARCH / NOTIFY） |
| 8081 | TCP | 视频帧流 |
| 8082 | TCP | CPU 计算卸载服务 |
| 8083 | TCP | 远程端口检查服务 |

---

## 工作原理

### 1 — 发现（SSDP）

```
接收端  →  加入 239.255.255.250:1900，周期 NOTIFY 广播，监听 M-SEARCH
发送端  →  逐网卡发送 M-SEARCH（多播 + 广播兜底），收集 200 OK 应答，列出接收端供用户选择
```

- 发送端在每块本地网卡上分别发探测（多播 + 255.255.255.255 广播），企业网丢多播时广播往往仍可达
- 应答以**数据包源地址**为准（LOCATION 多网卡机器可能写错网卡，源地址必可达）
- 接收端应答的 LOCATION 使用与请求方**同子网**的网卡 IP

若网络策略连广播也拦截，可使用直连模式：

```
bin/sender 192.168.1.105        # 跳过发现，直接连接 8081
bin/sender 192.168.1.105 8081   # 也可指定端口
bin/sender 192.168.1.105 --mode extend    # 非交互：直接指定 mirror / extend（GUI 使用）
```

### 2 — 扩展握手

选定接收端与显示模式后，发送端连接 TCP 8081 交换握手包（协议 v2，全部为网络字节序）：

```
发送端 → 接收端   HsRequestV2（48 字节，uint32 x 12）
  magic          0x54564853 ('TVHS')
  version        2
  width, height  采集尺寸（缩放由编码器内部完成）
  fps            目标帧率
  mode           0=镜像  1=扩展
  codec          0=原始 1=RLE 2=H.264 3=H.265 4=AV1
  codec_flags    bit0=希望硬件编码
  bitrate_kbps   0=质量（CRF）模式
  quality        0..51（CRF 类；0xFFFFFFFF = 编码器默认哨兵，保留未用）
  gop_frames     IDR 间隔（0=默认 2*fps）
  reserved       0

接收端 → 发送端   HsResponseV2（36 字节）
  magic, version, recv_width, recv_height
  status         0=OK 1=版本 2=编码 3=尺寸 4=解码器 5=旧协议
  codec          接受的编码
  codec_flags, reserved[2]
```

两端必须为同一版本。旧版（16 字节握手）会被识别为 legacy peer 并给出明确报错，不会再黑屏。

### 3 — 虚拟显示器生命周期（扩展模式）

```
选择扩展模式
  → vddStartup()：
      启用 Root\MttVDD 设备（若被禁用；SetupAPI，需管理员）
      → CCD SetDisplayConfig 激活虚拟显示器（若上次会话遗留则直接接管）
      → 定位到现有桌面右侧，1920x1080@60
      → 自动锁定该显示器为采集源
  → 握手、推流（窗口/鼠标可穿越到虚拟屏）
  → 会话结束 / Ctrl+C / 断开连接 / 关闭控制台窗口
  → vddShutdown()：分离虚拟显示器 + 禁用 Root\MttVDD 设备
      → 虚拟屏从「设置 → 屏幕」彻底消失（不是仅从桌面分离）
```

实现于 [vdd.cpp](src/vdd.cpp) / [vdd.h](src/vdd.h)（sender 与 GUI 共用），基于
Windows CCD API（`QueryDisplayConfig` / `SetDisplayConfig`）、
`ChangeDisplaySettingsEx` 与 SetupAPI 设备启停（`DIF_PROPERTYCHANGE`）。

> 注意：仅做 CCD 分离是不够的——PnP 显示设备仍会出现在「设置 → 屏幕」里并可被
> 扩展。所以每次会话结束会**禁用设备**（整个 IddCx/UMDF 栈下线），下次扩展会话
> 开始时自动重新启用。按硬件 ID `Root\MttVDD` 精确匹配，不影响其它厂商的虚拟
> 显示适配器。

**虚拟显示器不会残留**——所有退出路径都会拆除并禁用它：

| 退出路径 | 处理方式 |
|----------|----------|
| 正常结束 / 对端断开 / 发送失败 | `main` 收尾时 `vddShutdown()` |
| Ctrl+C | 优雅退出，同样走收尾 |
| 关闭 sender 控制台窗口（X） | 在 `CTRL_CLOSE_EVENT` 处理函数内直接拆除 |
| 进程 `exit()` | `atexit(vddShutdown)` 兜底 |
| GUI「停止推流」 | 先发 Ctrl+C 优雅停止（最长等 8 秒），若被强杀则 GUI 调 `vddForceDetach()` 兜底 |

推流套接字带 2 秒发送超时：对端断开或假死时发送很快失败，会话立即收尾拆除虚拟显示器，不会卡死。

### 4 — 帧流

```
发送端   →  采集画面（扩展模式=虚拟屏；镜像=主屏）
        →  将鼠标光标绘制进帧（GetCursorInfo + DrawIconEx）
        →  按协商编码处理：
             H.264/H.265/AV1  → FFmpeg 编码（硬件优先，低延迟调优）→ 发送访问单元
             RLE              → 可选经计算卸载服务压缩（8082）
             原始             → 直接发送 RGB24
        →  发送 [uint32 帧长][帧数据]

接收端   →  读帧头、读帧体
        →  编码流 → FFmpeg 解码 → YUV/NV12 纹理渲染
        →  RLE → 解压 → RGB24 纹理
        →  SDL_UpdateTexture/YUVTexture → RenderCopy → RenderPresent
```

控制包：`0xFFFFFFFF` + `ctrl_type` + 负载。`CTRL_RESIZE`（扩展模式分辨率变化）与 `CTRL_RECONFIG`（运行时参数变化）都携带完整 `HsRequestV2`，接收端据此重建解码器并跳到下一个关键帧。

### 5 — 附加服务

- **计算卸载（8082）**：PING / COMPRESS（RLE 压缩）/ COLORCONV（BGR→RGB），处理发生在接收端 CPU，含真实耗时测量（Linux/macOS 用 `clock_gettime`，Windows 用 `QueryPerformanceCounter`）。服务不可用时发送端静默回退为本地未压缩帧。
- **端口检查（8083）**：LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT。端口数据按平台原生采集：Linux 读 `/proc/net/*` + inode→PID 映射；macOS 调 `lsof`；Windows 用 `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`。

---

## 编解码

| 编码 | 说明 | 默认 |
|------|------|------|
| **H.264** | 画质/延迟均衡；硬件（NVENC/QSV/AMF/VAAPI）优先，回退 libx264 | ✅ |
| **H.265** | 同码率画质更好；硬件优先，回退 libx265 | |
| **AV1** | 压缩率最高；硬件优先，回退 SVT-AV1 | |
| **原始** | 无损 RGB24，带宽最大 | |
| **RLE** | 无损压缩（可远程卸载到接收端 CPU） | |

低延迟调优：无 B 帧、`zerolatency`、GOP=2×fps、resize 后立即 IDR。桌面文字建议在设置中开启 4:4:4 色度（仅软件编码）。

## 设置与历史

GUI「设置」页可配置（保存在 `%LOCALAPPDATA%\Mivara\config.ini`）：

| 设置 | 默认 | 说明 |
|------|------|------|
| 外观 | 跟随系统 | 亮色 / 暗色 / 跟随系统；顶栏右侧切换即时生效并写入 `config.ini` 的 `theme`，无需点「确定」 |
| 编码方式 | H.264 | 自动/H.264/H.265/AV1/原始/RLE |
| 硬件加速 | 开 | 优先使用 GPU 编码 |
| 画质 / 码率 | 23 / 0 | 码率>0 时按码率，否则 CRF |
| 帧率 / 缩放 | 60 / 100% | 发送端上限与预缩小 |
| 色度 | 4:2:0 | 4:4:4 文字更锐（软件编码） |
| 状态控制台 | 关 | 开启可显示经典 cmd 窗口 |
| 日志窗口 | 关 | 启动时自动打开独立日志窗 |
| 开机自启动 | 关 | 注册 Windows 计划任务（onlogon，登录免 UAC 弹窗）；卸载时自动清理 |
| 自动接收 | 关 | 打开软件后直接进入接收模式 |
| 自动检查更新 | 开 | 启动约 3 秒后检查新版本（同一台机器每 24 小时至多一次）；关闭后仅手动点「检查更新」时联网 |

> 「跟随系统」读取操作系统的浅色/深色偏好：Windows 读注册表 `AppsUseLightTheme` 并每 2 秒复查一次，系统切换即时跟随；macOS（`AppleInterfaceStyle`）与 Linux（GNOME `color-scheme`）只在启动时读取一次，改系统主题需重启软件生效。检测失败一律回退亮色。

**运行时调参**：传输过程中打开「设置」修改参数并点「确定」，参数秒级热生效、连接不断（编码/缩放切换会瞬断一两帧）。经典控制台模式下无 stdin 通道，将自动按新参数重启会话。

传输状态默认显示在 GUI 状态面板（实时 FPS/码率/编码器），可另开独立日志窗口查看子进程输出。

「主页」的接收/发送按钮下方即连接历史（`history.ini`，最近 20 条），点击即可直连（投屏/扩展），无需重新搜索；支持删除单条与清空。

---

## 显示模式

| 模式 | GUI 按钮 / 命令行 | 行为 |
|------|------------------|------|
| 镜像 | 「投屏」/ `--mode mirror`（默认） | 接收端以可缩放窗口显示发送端主屏画面 |
| 扩展 | 「扩展」/ `--mode extend` | 发送端自动激活虚拟显示器；接收端无边框全屏显示这块**真实桌面空间**——窗口可拖入、鼠标可穿越 |

> 早期版本的"模拟扩展"（extend-right/below/left：接收端全屏显示主屏画面并画一条蓝色拼接线）已移除——它们并不创建真实桌面空间。扩展一律使用 `--mode extend`（协议 mode 1）。

---

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
- 扩展模式：虚拟显示器驱动（见[下文](#windows-虚拟显示器驱动)）
- `sender` / `app` 内嵌 `requireAdministrator` 清单——**需以管理员运行**（启动时
  UAC 确认一次）。扩展模式要用管理员权限启用/禁用虚拟显示设备，使其会话结束后
  从「设置 → 屏幕」彻底消失。`receiver` 不需要。

**macOS**

- macOS Catalina 10.15+，Clang 12+
- `brew install sdl2 sdl2_ttf`

> `SDL2_ttf` 为必需依赖（GUI 中文界面）。
>
> **注意**：Windows 部署时运行时 DLL 需与 exe 同目录（`bin/` 已包含完整闭包）。新增依赖后可用 `ldd bin/app.exe` 解析闭包补齐。

---

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

---

## 编译

从源码编译三步：装依赖 → make → 运行 `bin/` 下的产物。

```bash
git clone https://github.com/wzy-777/Mivara.git
cd Mivara
make install-deps    # 1. 安装系统依赖（Linux/macOS/MSYS2 自动识别包管理器）
make                 # 2. 编译全部程序（产物输出到 bin/）
./bin/app            # 3. 运行图形界面（或 bin/sender、bin/receiver）
```

> Windows：在 MSYS2 MinGW64 终端中执行同样的命令；不支持 MSVC。

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

### Windows 控制台说明

- `sender` 以控制台子系统链接——有日志输出，Ctrl+C 优雅退出（含虚拟显示器分离）。
- `receiver` / `app` 以 GUI 子系统链接（`-mwindows`），无控制台输出；如需接收端日志，可手动用 `-mconsole` 重新链接 receiver。

### 打包安装程序（Windows）

用 Inno Setup 编译安装包：

```powershell
& "D:\Program Files\Inno Setup 6\ISCC.exe" installer\Mivara.iss
# 产物：dist\Mivara-Setup-<版本>.exe
```

发布时需**同时**上传固定名副本 `dist\Mivara-Setup.exe`（应用内「下载更新」与 README 的下载徽章都用这个固定名地址），以及 `SHA256SUMS.txt`（应用内安装的校验依据，缺失时只提供浏览器下载）。CI 打包会自动生成这三个文件；本地打包可手动复制：`copy dist\Mivara-Setup-<版本>.exe dist\Mivara-Setup.exe`。

安装包内容：`bin/`（exe + DLL）、`scripts/`、`vdd/` 虚拟显示器驱动（不含体积超限的厂商 GUI），安装后在开始菜单/桌面创建快捷方式。完成页的「启动 Mivara」勾选项直接拉起管理员权限的 GUI（安装器以提权令牌启动，无二次 UAC 弹窗）。

---

## 使用指南

### 图形界面（推荐）

```
bin/app
```

打开后顶栏右侧是**外观切换**（三个图标：太阳＝亮色、月亮＝暗色、显示器＝跟随系统），左侧为导航栏：**主页**（接收/发送 + 连接历史）、**设置**，栏底「退出」：

- **外观**：顶栏右侧三个图标（太阳＝亮色、月亮＝暗色、显示器＝跟随系统），点一下立即换色并写入 `config.ini`，不需要点「确定」；同一设置在「设置 · 外观」里是带文字的三档切换，认不准图标时到那里看。「跟随系统」见上节的平台差异说明
- **主页 · 接收**：显示本机 IP、端口与"正在接收"状态，同时启动接收端等待连接；「停止接收」结束
- **主页 · 发送**：显示"搜索中"，搜到设备后列出机器清单（含主机名），每台可选「投屏」（镜像）或「扩展」；推流中可「停止」
- **主页 · 连接历史**：接收/发送按钮下方列出最近 20 条连接，点击直连（免搜索），可删除单条或清空，滚轮翻页
- **主页 · 会话中**：投屏/接收进行时可随时切到「设置」（会话不停）；「返回会话」或设置页按 ESC 回到实时状态；点「主页」回到主页（显示运行中，可停止）
- **设置**：左列「画质与编码」（编码方式 自动/H.264/H.265/AV1/原始/RLE、画质/帧率/码率/缩放/色度），右列「连接与启动」（硬件加速/状态控制台/日志窗口/开机自启动/自动接收/自动检查更新）与「外观」，底部「软件更新」。**传输中改参数点「确定」即热生效不断流**
- **设置 · 更新**：底部「更新」行显示当前版本与检查结果；发现新版本 → 「下载更新」（带进度）→「立即安装」（Windows：校验通过后退出应用并启动安装程序；Linux/macOS：打开下载页手动安装）。可「忽略此版本」，发现新版本时主页顶部横幅与设置红点会提示
- **未搜到设备**：提供「继续搜索」「手动输入ip」「退出」三个选项；手动输入 IP 后同样进入投屏/扩展选择
- **推流状态**：默认在 GUI 状态面板实时显示 FPS / 码率 / 编码器；「日志」按钮打开独立日志窗口（子进程输出）。默认不弹出 cmd 控制台，可在设置中开启
- **ESC**：设置丢弃编辑并返回会话；手动输入页返回；投屏/接收页停止会话回主页；主页退出

Windows 下也可用 `scripts/` 中的快捷脚本：`start_receiver.bat` 一键启动接收端；`启动投屏发送.bat <接收端IP>` 直连发送。

### 接收端按键

| 按键 | 作用 |
|------|------|
| ESC / Q | 断开并退出 |
| F11 | 切换全屏（扩展模式） |
| 关闭窗口 | 停止接收 |

### 发送端按键

| 输入 | 作用 |
|------|------|
| Ctrl+C | 优雅退出（含虚拟显示器分离 + 设备禁用） |
| 模式提示处 1/2 | 镜像 / 扩展（仅命令行交互模式） |

---

## Windows 虚拟显示器驱动

扩展模式依赖间接显示驱动（IddCx）提供虚拟显示器。本项目适配 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**（MIT 协议，SignPath 正规签名）。

驱动**只需安装一次**。装好后无需任何手动操作——sender 会在扩展会话开始时自动启用设备并激活虚拟显示器、结束时分离并禁用设备（彻底从「设置 → 屏幕」消失）。

### 安装步骤（管理员 PowerShell）

```powershell
# 1. 从 GitHub Releases 下载（如 VDD.Control.25.7.23.zip）并解压

# 2. 驱动入库
pnputil /add-driver MttVDD.inf /install
# （注意：x86 目录内的驱动实际面向 NTamd64）

# 3. 创建 Root\MttVDD 设备
devcon.exe install MttVDD.inf "Root\MttVDD"

# 4. 拷贝设置文件（分辨率/刷新率列表在此配置）
Copy-Item vdd_settings.xml C:\Windows\System32\drivers\UMDF\vdd_settings.xml

# 5. 扫描设备，出现 "Generic Monitor (VDD by MTT)" 即成功
pnputil /scan-devices
```

### 分辨率说明

虚拟显示器激活后由 sender 自动配置为 **1920x1080@60** 并置于现有桌面右侧。分辨率可在会话开始时选择（交互模式）或默认 1920x1080；如需修改默认值，编辑 vdd.cpp 中 `vddStartup()` 里的 `vddConfigure(dev, x, 0, w, h, 60)` 调用，或编辑驱动的 `vdd_settings.xml` 添加模式后重新编译。

---

## 网络配置

### 防火墙规则（接收端）

```powershell
# Windows（管理员 PowerShell）
New-NetFirewallRule -DisplayName "Mivara SSDP"    -Direction Inbound -Protocol UDP -LocalPort 1900 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Stream"  -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Compute" -Direction Inbound -Protocol TCP -LocalPort 8082 -Action Allow
New-NetFirewallRule -DisplayName "Mivara Ports"   -Direction Inbound -Protocol TCP -LocalPort 8083 -Action Allow
```

```bash
# Linux (UFW)
sudo ufw allow 1900/udp comment 'Mivara SSDP'
sudo ufw allow 8081/tcp comment 'Mivara Video Stream'
sudo ufw allow 8082/tcp comment 'Mivara Compute Offload'
sudo ufw allow 8083/tcp comment 'Mivara Port Inspector'
```

### 网络要求

- 所有设备同一子网
- 多播依赖交换机 IGMP snooping 配置；**多播被拦截时用直连模式**（`sender <IP>`）
- 1080p@60 建议千兆有线；5 GHz WiFi 可用于较低分辨率

---

## 性能

### 带宽（未压缩 RGB24）

| 分辨率 | 30 FPS | 60 FPS |
|--------|--------|--------|
| 1280×720 | ~125 MB/s | ~250 MB/s |
| 1920×1080 | ~280 MB/s | ~560 MB/s |
| 2560×1440 | ~500 MB/s | ~1 GB/s |

> RLE 压缩对桌面内容（文本/UI）通常可降低 30–70% 带宽；视频内容压缩率较低。

### 延迟

| 连接 | 典型值 |
|------|--------|
| 千兆有线 | < 5 ms |
| 5 GHz WiFi | 10–15 ms |
| 2.4 GHz WiFi | 20–35 ms |

接收端每 60 秒将统计写入 `gpu_stats.json`，每 30 秒输出控制台摘要。

---

## 故障排除

| 现象 | 原因 | 解决 |
|------|------|------|
| No receivers found | UDP 1900 多播/广播均被拦 | 用直连模式 `sender <IP>` |
| 程序秒退 / 接收端无日志 | `receiver`/`app` 是 GUI 子系统（-mwindows），stdout 丢弃 | sender 自带控制台日志；receiver/app 需手动用 `-mconsole` 重新链接看输出 |
| 找不到 *.dll | 运行时 DLL 闭包不全 | 用 `ldd bin/app.exe` 解析闭包，把缺失 DLL 拷到 `bin/`（与 exe 同目录） |
| Connection refused | 接收端未运行 | 检查 8081 端口，重启接收端 |
| "Virtual display driver not found" | MttVDD 驱动未安装 | 按[安装步骤](#windows-虚拟显示器驱动)安装驱动 |
| "Virtual display did not come up" | 驱动安装但激活失败 | `pnputil /enum-devices /class Monitor` 检查设备状态，重启后再试 |
| 扩展模式黑屏 / 无画面 | 两端握手不一致（模式值 0=镜像 1=扩展） | 接收端与发送端使用同版本二进制 |
| 断开后虚拟屏仍在「设置 → 屏幕」 | 旧版本只做 CCD 分离，或未以管理员运行 | 已修复：退出时禁用 Root\MttVDD 设备（需管理员权限运行 sender/app）。若日志出现 disable 失败，可在设备管理器手动禁用「Virtual Display Driver」 |
| 「检查更新」失败（DNS 解析失败 / 请求超时 / 安全连接失败） | 检查只访问 `github.com`，直连不稳定时（企业网络或大陆网络常见）会失败，属预期降级 | 点设置页「打开下载页」用浏览器（走系统代理/VPN）手动下载安装；安装新版本前建议先在应用内「退出」，确保虚拟显示器正确卸载 |
| 点「下载更新」提示「未找到校验文件」 | 该 Release 未附带 `SHA256SUMS.txt`（应用内安装要求有哈希锚点，缺失时不做应用内安装） | 用「打开下载页」手动下载安装即可 |
| 下载/运行时 SmartScreen 提示（「通常不会下载」「未知发布者」） | 安装包未做代码签名 + 新文件无下载信誉（正常信誉提示，非报毒） | 校验 SHA-256 后按提示继续，详见 [Windows SmartScreen 提示说明](#windows-smartscreen-提示说明) |

### Windows SmartScreen 提示说明

从 GitHub 直接下载的安装包，在部分 Windows 10/11 上会遇到以下提示——都是 **SmartScreen 的信誉提示（文件未签名 + 新发布、下载量少），不是报毒**：

| 提示 | 出现位置 | 原因 |
|------|----------|------|
| 「Mivara-Setup.exe 通常不会下载。打开前请确保信任…」 | 浏览器下载栏 | 该文件尚无下载信誉（Microsoft Defender SmartScreen 常规检查） |
| 「Windows 已保护你的电脑」 | 双击运行时（可能） | 安装包未做代码签名 |
| 「发布者：未知」 | UAC 提权窗口 | 同上（未签名） |

先校验文件完整性（结果应与对应 Release 说明中的 SHA-256 一致）：

```powershell
Get-FileHash "$HOME\Downloads\Mivara-Setup.exe" -Algorithm SHA256
```

校验一致后按正常流程继续：

1. 浏览器下载条目旁「…」→「保留」；
2. 双击安装包 →「更多信息」→「仍要运行」。

> 若仍有顾虑：`属性 → 常规 → 解除锁定`（或 PowerShell `Unblock-File`）是 Windows 官方的「我已确认信任此文件」操作，可先校验哈希再执行。

（本安装包目前未做代码签名，这是开源项目的常见现状。）

### 诊断命令

```bash
netstat -tulpn | grep -E '8081|8082|8083|1900'   # 接收端四端口
nc -zv <接收端IP> 8081                            # TCP 可达性
make check                                        # 环境自检
```

---

## 路线图

- [x] 屏幕镜像
- [x] 扩展屏（虚拟显示器自动激活/分离 + 按显示器采集 + 鼠标穿越，Windows）
- [x] 直连模式（绕过 SSDP 多播）
- [x] 远程 CPU 卸载（RLE 压缩 + 色彩修正，真实耗时测量）
- [x] 扩展握手（分辨率交换）
- [x] 图形启动器（SDL2_ttf 中文界面：接收/发送/搜索/机器列表/手动 IP）
- [x] macOS CoreGraphics 采集
- [x] 远程端口检查服务（TCP 8083）
- [x] 每客户端统计 + JSON 导出
- [ ] 接收端输入回传（键鼠控制在接收端侧真实生效）
- [x] 可选编码方案：RLE / H.264 / H.265 / AV1 四种方式作为可配置项
- [x] GUI 高级设置（编码方式、连接历史管理）
- [x] 应用内检查更新 + 一键下载安装（Windows，SHA-256 校验 + 管理员专属下载目录）
- [ ] GUI 高级设置：扩展分辨率选择（当前 GUI 固定默认 1920x1080）
- [ ] 音频采集与传输
- [ ] TLS 加密
- [ ] 虚拟显示器分辨率自适应接收端（握手后协商）
- [ ] 自适应帧率
- [ ] Wayland 支持
- [ ] macOS/Linux 虚拟显示器方案
- [ ] 屏幕拼接

---

## 致谢

本项目基于 **[RGM (Ralefaso GlassMirror)](https://github.com/RR-Ralefaso/RGM)** 项目开发。

虚拟显示器驱动来自 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**。

Windows 安装包代码签名（申请中）：由 [SignPath.io](https://signpath.io) 免费提供、证书由 [SignPath Foundation](https://signpath.org) 持有——详见 [Code signing policy](CODE_SIGNING_POLICY.md)。

## 许可证

[MIT](LICENSE) © 2026 wzy-777

---

<div align="center">

**Linux • Windows 10/11 • macOS —— 一套代码，全平台运行。**

</div>
