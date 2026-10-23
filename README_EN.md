# Mivara

[中文](README.md) | English

**Zero-config LAN casting · Wireless second monitor**

Turn any computer on your LAN into a second screen. The sender captures and streams its display; the receiver shows it fullscreen. In extend mode the system gains a real virtual display, so windows and the mouse can move straight onto it.

[![Version](https://img.shields.io/badge/version-0.9.2-blue)](https://github.com/wzy-777/Mivara)
[![Download Latest Installer](https://img.shields.io/badge/download-latest%20installer-brightgreen?logo=github&logoColor=white)](https://github.com/wzy-777/Mivara/releases/latest/download/Mivara-Setup.exe)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#requirements)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#requirements)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

| Home (light) | Home (dark) |
|:---:|:---:|
| ![Mivara home, light theme](docs/assets/home-light.png) | ![Mivara home, dark theme](docs/assets/home-dark.png) |

## Highlights

- **Extended desktop** — an extend session automatically enables a Windows virtual display that adds real desktop space; windows can be dragged onto it and the pointer crosses over. Every exit path detaches and disables the device, so nothing lingers in Settings > Display.
- **Stitching (software KVM)** — two Windows PCs keep their own monitors and share one keyboard/mouse: choose which side of this PC the peer sits on (left / right / top / bottom) when starting, push the pointer out of that edge to take over the peer, and move back out of its facing edge (or press Ctrl+Alt+Shift+Q) to hand control back. The side can be switched mid-session and no picture is sent.
- **Zero-config discovery** — SSDP finds receivers on the LAN and shows their hostnames; direct IP connection works when multicast is blocked by network policy.
- **High quality, low latency** — H.264 / H.265 / AV1 with hardware encoding first (NVENC / QSV / AMF / VAAPI) and automatic software fallback; up to 120 FPS.
- **Live reconfigure** — change codec, quality, bitrate, frame rate, scale or chroma mid-session; applied within seconds without dropping the stream.
- **Chinese-language GUI** — Home / Settings, connection history, device list, live status and log window; light / dark / follow-system themes.
- **Ready out of the box** — launch at logon, auto-receive, in-app update check and one-click install (SHA-256 verified).
- **Cross-platform** — Windows 10/11, Linux (X11), macOS 10.15+.

## Quick Start

### Windows (recommended)

1. Download [Mivara-Setup.exe](https://github.com/wzy-777/Mivara/releases/latest/download/Mivara-Setup.exe) (or pick a version from [Releases](https://github.com/wzy-777/Mivara/releases)).
2. Run the installer. It is not code-signed, so SmartScreen may prompt — see [below](#windows-smartscreen-notice).
3. On the receiver, open Mivara and click "Receive"; on the sender click "Send", pick a device, then choose "Cast", "Extend" or "Stitch" (stitch needs Windows on both ends — see the [User Guide](docs/en/usage.md#stitch-software-kvm)).

> Extend mode requires the virtual display driver to be installed once — see [Virtual Display Driver](docs/en/virtual-display.md).

### Build from source

```bash
git clone https://github.com/wzy-777/Mivara.git
cd Mivara
make install-deps    # install system dependencies
make                 # build bin/app, bin/sender, bin/receiver
./bin/app            # launch the GUI
```

Other platforms and packaging: [Building & Packaging](docs/en/building.md).

## Requirements

| Platform | Version | Dependencies |
|----------|---------|--------------|
| Windows | Windows 10 1903+ / Windows 11 | Installer bundles the runtime DLLs; building from source needs MinGW-w64 (MSYS2), MSVC is not supported |
| Linux | Ubuntu 18.04+ / Debian 10+ / Fedora 32+ / Arch | X11, GCC 8+, libX11, SDL2, SDL2_ttf, any system CJK font |
| macOS | Catalina 10.15+ | Clang 12+, SDL2, SDL2_ttf (`brew install sdl2 sdl2_ttf`) |

- `sender` / `app` embed a `requireAdministrator` manifest and need one UAC confirmation at launch; `receiver` does not run elevated.
- Extend mode relies on a Windows indirect display driver, installed once — see [Virtual Display Driver](docs/en/virtual-display.md).

## Documentation

| Document | Contents |
|----------|----------|
| [User Guide](docs/en/usage.md) | Display modes, GUI flow, shortcuts, settings and history |
| [Architecture & Protocol](docs/en/architecture.md) | Directory layout, data flow, protocol v2, codecs and sidecar services |
| [Building & Packaging](docs/en/building.md) | Requirements, dependencies, build targets, Windows installer |
| [Virtual Display Driver](docs/en/virtual-display.md) | MttVDD installation and resolution configuration |
| [Network & Firewall](docs/en/network.md) | Ports, firewall rules, network requirements |
| [Performance](docs/en/performance.md) | Bandwidth, latency and statistics |
| [Troubleshooting](docs/en/troubleshooting.md) | Common issues, SmartScreen notice, diagnostic commands |
| [Roadmap](docs/en/roadmap.md) | Shipped and planned features |

中文文档：[docs/zh](docs/zh/) · [README.md](README.md)

## Windows SmartScreen Notice

The installer is currently not code-signed, so the first download or launch may show "isn't commonly downloaded", "Windows protected your PC" or "Publisher: Unknown" — these are SmartScreen reputation prompts, not malware verdicts. Verify the SHA-256 first (see the release notes and `SHA256SUMS.txt`):

```powershell
Get-FileHash "$HOME\Downloads\Mivara-Setup.exe" -Algorithm SHA256
```

If it matches: browser download bar → "…" → "Keep"; then double-click the installer → "More info" → "Run anyway". Full details: [Troubleshooting · SmartScreen](docs/en/troubleshooting.md#windows-smartscreen-notice).

## Acknowledgements

The virtual display driver comes from **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**.

Windows installer code signing (application in progress): provided free of charge by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org) — see the [Code signing policy](CODE_SIGNING_POLICY.md).

## License

[MIT](LICENSE) © 2026 wzy-777

---

<div align="center">

**Linux • Windows 10/11 • macOS — one codebase, every platform.**

</div>
