# Mivara (幻屏)

中文 | [English](README_EN.md)

**零配置局域网投屏 · 无线扩展屏**

把局域网里的任意一台电脑变成你的第二块屏幕。发送端采集画面并推流，接收端全屏显示；扩展模式下，系统里会多出一块真实的虚拟显示器，窗口和鼠标可以直接拖过去。

[![Version](https://img.shields.io/badge/version-0.10.0-blue)](https://github.com/wzy-777/Mivara)
[![下载最新安装包](https://img.shields.io/badge/download-latest%20installer-brightgreen?logo=github&logoColor=white)](https://github.com/wzy-777/Mivara/releases/latest/download/Mivara-Setup.exe)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#环境要求)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#环境要求)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

| 主页（亮色） | 主页（暗色） |
|:---:|:---:|
| ![Mivara 主页（亮色）](docs/assets/home-light.png) | ![Mivara 主页（暗色）](docs/assets/home-dark.png) |

## 核心特性

- **扩展桌面** —— 会话开始自动启用 Windows 虚拟显示器，创建真实桌面空间；窗口可拖入、鼠标可穿越，任何退出路径都会分离并禁用设备，不会残留在「设置 → 屏幕」。
- **拼接（软件 KVM）** —— 两台 Windows 电脑各用各的显示器，只共享一套键鼠：启动时选择对方在本机的哪一侧（左/右/上/下），从该侧移出光标接管对方，移出对方屏幕的对应边缘或按 Ctrl+Alt+Shift+Q 交还；会话中可随时切换方向，不传画面。
- **零配置发现** —— SSDP 自动找到局域网内的接收端并显示主机名；多播被网络策略拦截时可直连 IP。
- **高画质低延迟** —— H.264 / H.265 / AV1，硬件编码优先（NVENC / QSV / AMF / VAAPI），自动回退软件编码；最高 120 FPS。
- **运行时调参** —— 传输中修改编码、画质、码率、帧率、缩放、色度，秒级生效不断流。
- **中文图形界面** —— 主页 / 设置、连接历史、设备列表、实时状态与日志窗口；亮色 / 暗色 / 跟随系统。
- **开箱即用** —— 开机自启动、自动接收、应用内检查更新与一键安装（SHA-256 校验）。
- **跨平台** —— Windows 10/11、Linux（X11）、macOS 10.15+。

## 快速开始

### Windows（推荐）

1. 下载 [Mivara-Setup.exe](https://github.com/wzy-777/Mivara/releases/latest/download/Mivara-Setup.exe)（也可以到 [Releases](https://github.com/wzy-777/Mivara/releases) 选择版本）。
2. 双击安装。安装包未做代码签名，可能出现 SmartScreen 提示，见[下文](#windows-smartscreen-提示说明)。
3. 接收端打开 Mivara 点「接收」；发送端点「发送」，选择设备后点「投屏」「扩展」或「拼接」（拼接需要两端都是 Windows，详见[使用指南](docs/zh/usage.md#拼接软件-kvm)）。

> 扩展模式需要安装一次虚拟显示器驱动，详见[虚拟显示器驱动](docs/zh/virtual-display.md)。

### 从源码运行

```bash
git clone https://github.com/wzy-777/Mivara.git
cd Mivara
make install-deps    # 安装系统依赖
make                 # 构建 bin/app、bin/sender、bin/receiver
./bin/app            # 启动图形界面
```

其他平台与打包步骤见[构建与打包](docs/zh/building.md)。

## 环境要求

| 平台 | 版本 | 依赖 |
|------|------|------|
| Windows | Windows 10 1903+ / Windows 11 | 安装包已含运行时 DLL；从源码构建需 MinGW-w64（MSYS2），不支持 MSVC |
| Linux | Ubuntu 18.04+ / Debian 10+ / Fedora 32+ / Arch | X11、GCC 8+、libX11、SDL2、SDL2_ttf、任一系统中文字体 |
| macOS | Catalina 10.15+ | Clang 12+、SDL2、SDL2_ttf（`brew install sdl2 sdl2_ttf`） |

- `sender` / `app` 内嵌 `requireAdministrator` 清单，启动时需确认一次 UAC；`receiver` 不需要管理员权限。
- 扩展模式依赖 Windows 间接显示驱动，只需安装一次，见[虚拟显示器驱动](docs/zh/virtual-display.md)。

## 文档

| 文档 | 内容 |
|------|------|
| [使用指南](docs/zh/usage.md) | 显示模式、界面流程、快捷键、设置与连接历史 |
| [架构与协议](docs/zh/architecture.md) | 目录结构、工作流程、协议 v2、编解码与附加服务 |
| [构建与打包](docs/zh/building.md) | 环境要求、依赖安装、编译、Windows 安装包 |
| [虚拟显示器驱动](docs/zh/virtual-display.md) | MttVDD 驱动安装与分辨率配置 |
| [网络与防火墙](docs/zh/network.md) | 端口、防火墙规则、网络要求 |
| [性能](docs/zh/performance.md) | 带宽、延迟与统计输出 |
| [故障排除](docs/zh/troubleshooting.md) | 常见问题、SmartScreen 提示、诊断命令 |
| [路线图](docs/zh/roadmap.md) | 已完成与计划中的功能 |

English documentation: [docs/en](docs/en/) · [README_EN.md](README_EN.md)

## Windows SmartScreen 提示说明

安装包目前未做代码签名，首次下载或运行时可能出现「通常不会下载」「Windows 已保护你的电脑」或「发布者：未知」——这是 SmartScreen 的信誉提示，不是报毒。建议先校验 SHA-256（见对应 Release 说明与 `SHA256SUMS.txt`）：

```powershell
Get-FileHash "$HOME\Downloads\Mivara-Setup.exe" -Algorithm SHA256
```

校验一致后：浏览器下载栏「…」→「保留」；双击安装包 →「更多信息」→「仍要运行」。完整说明见[故障排除 · SmartScreen](docs/zh/troubleshooting.md#windows-smartscreen-提示说明)。

## 致谢

虚拟显示器驱动来自 **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**。

Windows 安装包代码签名（申请中）：由 [SignPath.io](https://signpath.io) 免费提供、证书由 [SignPath Foundation](https://signpath.org) 持有——详见 [Code signing policy](CODE_SIGNING_POLICY.md)。

## 许可证

[MIT](LICENSE) © 2026 wzy-777

---

<div align="center">

**Linux • Windows 10/11 • macOS —— 一套代码，全平台运行。**

</div>
