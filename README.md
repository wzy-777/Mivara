# TwinView (幻屏)

中文 | [English](README_EN.md)

**零配置局域网投屏 / 无线扩展屏**

一个跨平台的 C++ 项目：把局域网内的任何一台电脑变成你的第二台显示器。

[![Version](https://img.shields.io/badge/version-0.8.1-blue)](https://github.com/wzy-777/TwinView)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#环境要求)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#环境要求)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## 目录

- [TwinView (幻屏)](#twinview-幻屏)
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
    - [诊断命令](#诊断命令)
  - [路线图](#路线图)
  - [致谢](#致谢)
  - [许可证](#许可证)

---

## 项目简介

TwinView（幻屏）是一个轻量级、跨平台的**屏幕扩展/投屏**工具。发送端（sender）采集本机画面，通过网络推流到接收端（receiver），接收端全屏显示。

项目提供两种模式：

1. **镜像投屏** —— 接收端显示发送端主屏的完整画面（演示/展示场景）
2. **扩展屏** —— 发送端**自动激活虚拟显示器**，创建一块真实的桌面空间；窗口和鼠标可以直接拖入/移入这块"看不见的屏幕"，接收端实时显示。会话结束时虚拟显示器自动关闭。

扩展模式需要 Windows 间接显示驱动（IddCx）支持，安装方法见[下文](#windows-虚拟显示器驱动)。驱动只需安装一次；虚拟显示器的开/关由程序在会话期间自动管理。

---

## 功能特性

| 类别 | 能力 |
|------|------|
| **扩展屏** | 会话开始自动附加虚拟显示器，会话结束（含 Ctrl+C）自动分离 |
| **虚拟显示器集成** | 程序内完成激活/停用/定位（CCD API），无需手动操作显示设置 |
| **鼠标可见** | 光标直接绘制进视频帧；扩展模式下即"指针穿越" |
| **直连模式** | `sender <IP>` 跳过发现阶段，SSDP 多播被网络策略拦截时仍可连接 |
| **发现** | SSDP（UDP 1900 多播 + 广播兜底）零配置自动发现接收端 |
| **图形界面** | SDL2_ttf 中文 GUI：接收/发送、自动搜索、机器列表（投屏/扩展）、手动输入 IP |
| **性能** | 120 FPS 推流、4 MB 流 socket 缓冲、TCP_NODELAY 优化 |
| **CPU 卸载** | RLE 帧压缩与色彩修正可卸载到接收端 CPU（TCP 8082） |
| **端口检查器** | 远程列出/查询/终止接收端任意端口进程（TCP 8083） |
| **跨平台** | Linux（X11）、Windows 10/11、macOS 10.15+ |

---

## 架构

### 目录结构

```
TwinView/
├── makefile                  # 跨平台构建（Linux/macOS/Windows MinGW）
├── src/
│   ├── app.cpp               # 图形启动器（SDL2 + SDL2_ttf）：接收/发送/搜索/投屏
│   ├── sender.cpp            # 采集、握手、推流、虚拟显示器生命周期管理
│   ├── receiver.cpp          # 全屏显示、SSDP 广播、计算服务、端口服务
│   ├── discover.cpp/.h       # SSDP 发现引擎
│   ├── gpu_accelerate.c/.h   # 远程 CPU 卸载（RLE 压缩 / 色彩修正）
│   └── ports.cpp/.h          # 远程端口检查服务
├── bin/                      # 可执行程序 + 运行时 DLL（构建输出）
├── scripts/                  # Windows 便捷启动脚本
├── installer/                # Inno Setup 打包脚本（TwinView.iss）
├── vdd/                      # 虚拟显示器驱动（MttVDD 签名驱动 + 安装脚本）
├── test/                     # 本地测试 / 诊断代码（不入库）
└── build/                    # 目标文件
```

### 可执行程序

| 程序 | 职责 |
|------|------|
| `app` | 图形启动器：接收/发送选择、SSDP 搜索、机器列表（投屏/扩展）、手动 IP |
| `sender` | 采集画面（GDI/X11/CoreGraphics），协商模式，推流；扩展模式下管理虚拟显示器的激活与定位 |
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

选定接收端与显示模式后，发送端连接 TCP 8081 交换握手包：

```
发送端 → 接收端   (16 字节，网络字节序)
  uint32  sender_width
  uint32  sender_height
  uint32  fps
  uint32  mode          0=镜像  1=扩展

接收端 → 发送端   (12 字节)
  uint32  receiver_width
  uint32  receiver_height
  uint32  status        0=OK
```

### 3 — 虚拟显示器生命周期（扩展模式）

```
选择扩展模式
  → vddStartup()：
      CCD SetDisplayConfig 激活虚拟显示器（若未激活）
      → 定位到现有桌面右侧，1920x1080@60
      → 自动锁定该显示器为采集源
  → 握手、推流（窗口/鼠标可穿越到虚拟屏）
  → 会话结束 / Ctrl+C
  → vddShutdown()：分离虚拟显示器（仅当它由本程序激活时）
```

实现于 [sender.cpp](src/sender.cpp) 的 `vddStartup` / `vddShutdown`，基于
Windows CCD API（`QueryDisplayConfig` / `SetDisplayConfig`）与
`ChangeDisplaySettingsEx`。

### 4 — 帧流

```
发送端   →  采集画面（扩展模式=虚拟屏；镜像=主屏）
        →  将鼠标光标绘制进帧（GetCursorInfo + DrawIconEx）
        →  可选经计算卸载服务 RLE 压缩
        →  发送 [uint32 帧长][帧数据]

接收端   →  读帧头、读帧体
        →  若帧长 < 原始大小则 RLE 解压
        →  SDL_UpdateTexture → RenderCopy → RenderPresent
```

### 5 — 附加服务

- **计算卸载（8082）**：PING / COMPRESS（RLE 压缩）/ COLORCONV（BGR→RGB），处理发生在接收端 CPU，含真实耗时测量（Linux/macOS 用 `clock_gettime`，Windows 用 `QueryPerformanceCounter`）。服务不可用时发送端静默回退为本地未压缩帧。
- **端口检查（8083）**：LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT。端口数据按平台原生采集：Linux 读 `/proc/net/*` + inode→PID 映射；macOS 调 `lsof`；Windows 用 `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`。

---

## 显示模式

| 模式 | GUI 按钮 / 命令行 | 行为 |
|------|------------------|------|
| 镜像 | 「投屏」/ `--mode mirror`（默认） | 接收端以可缩放窗口显示发送端主屏画面 |
| 扩展 | 「扩展」/ `--mode extend` | 发送端自动激活虚拟显示器；接收端无边框全屏显示这块**真实桌面空间**——窗口可拖入、鼠标可穿越 |

> 早期版本的"模拟扩展"（extend-right/below/left：接收端全屏显示主屏画面并画一条蓝色拼接线）已移除——它们并不创建真实桌面空间。扩展一律使用模式 2。

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

---

## 编译

从源码编译三步：装依赖 → make → 运行 `bin/` 下的产物。

```bash
git clone https://github.com/wzy-777/TwinView.git
cd TwinView
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
& "D:\Program Files\Inno Setup 6\ISCC.exe" installer\TwinView.iss
# 产物：dist\TwinView-Setup-<版本>.exe
```

安装包内容：`bin/`（exe + DLL）、`scripts/`、`vdd/` 虚拟显示器驱动（不含体积超限的厂商 GUI），安装后在开始菜单/桌面创建快捷方式。

---

## 使用指南

### 图形界面（推荐）

```
bin/app
```

打开后选择「接收」或「发送」：

- **接收**：显示本机 IP、端口与"正在接收"状态，同时启动接收端等待连接；「停止接收」结束
- **发送**：显示"搜索中"，搜到设备后列出机器清单，每台可选「投屏」（镜像）或「扩展」；推流中可「停止」
- **未搜到设备**：提供「继续搜索」「手动输入ip」「退出」三个选项；手动输入 IP 后同样进入投屏/扩展选择
- **ESC**：手动输入页返回；其他页面停止当前会话并回首页；首页退出

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
| Ctrl+C | 优雅退出（含虚拟显示器分离） |
| 模式提示处 1/2 | 镜像 / 扩展（仅命令行交互模式） |

---

## Windows 虚拟显示器驱动

扩展模式依赖间接显示驱动（IddCx）提供虚拟显示器。本项目适配 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**（MIT 协议，SignPath 正规签名）。

驱动**只需安装一次**。装好后无需任何手动操作——sender 会在扩展会话开始时自动激活虚拟显示器、结束时自动分离。

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

虚拟显示器激活后由 sender 自动配置为 **1920x1080@60** 并置于现有桌面右侧。如需修改默认分辨率，编辑 sender.cpp 中 `vddStartup()` 里的 `vddConfigure(dev, x, 0, 1920, 1080, 60)` 调用，或编辑驱动的 `vdd_settings.xml` 添加模式后重新编译。

---

## 网络配置

### 防火墙规则（接收端）

```powershell
# Windows（管理员 PowerShell）
New-NetFirewallRule -DisplayName "TwinView SSDP"    -Direction Inbound -Protocol UDP -LocalPort 1900 -Action Allow
New-NetFirewallRule -DisplayName "TwinView Stream"  -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow
New-NetFirewallRule -DisplayName "TwinView Compute" -Direction Inbound -Protocol TCP -LocalPort 8082 -Action Allow
New-NetFirewallRule -DisplayName "TwinView Ports"   -Direction Inbound -Protocol TCP -LocalPort 8083 -Action Allow
```

```bash
# Linux (UFW)
sudo ufw allow 1900/udp comment 'TwinView SSDP'
sudo ufw allow 8081/tcp comment 'TwinView Video Stream'
sudo ufw allow 8082/tcp comment 'TwinView Compute Offload'
sudo ufw allow 8083/tcp comment 'TwinView Port Inspector'
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
| Ctrl+C 后虚拟屏仍在 | 旧版本无优雅退出 | 已修复；确认使用新版 sender |

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
- [ ] **可选编码方案：RLE / H.264 / H.265 / AV1 四种方式作为可配置项**
- [ ] GUI 高级设置（分辨率选择、编码方式、连接历史管理）
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

## 许可证

[MIT](LICENSE) © 2026 wzy-777

---

<div align="center">

**Linux • Windows 10/11 • macOS —— 一套代码，全平台运行。**

</div>
