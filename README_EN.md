# Mivara

[中文](README.md) | English

**Zero-config LAN casting · Wireless second monitor**

*For you who work across two computers and two screens.*

Turn any computer on your LAN into a second screen. The sender captures and streams its display; the receiver shows it fullscreen. In extend mode the system gains a real virtual display, so windows and the mouse can move straight onto it.

[![Version](https://img.shields.io/badge/version-0.11.3-blue)](https://github.com/wzy-777/Mivara)
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
- **Stitch file drop (optional, copy-only)** — when enabled on both ends, drag files or folders across the screen edge and release over an Explorer window or the desktop on the peer to copy them into that folder. The source is never moved or deleted, name collisions are renamed, and both ends default to off.
- **Zero-config discovery** — SSDP finds receivers on the LAN and shows their hostnames; direct IP connection works when multicast is blocked by network policy.
- **High quality, low latency** — H.264 / H.265 / AV1 with hardware encoding first (NVENC / QSV / AMF / VAAPI) and automatic software fallback; up to 120 FPS.
- **Live reconfigure** — change codec, quality, bitrate, frame rate, scale or chroma mid-session; applied within seconds without dropping the stream.
- **Connection authorization** — three mutually exclusive policies: allow any connection (risk warning, never the default), password only (4–8 characters, letters are case-insensitive; only a PBKDF2 record is stored locally and the password never crosses the wire), or manual approval for every connection (allow once / deny within 30 s, timeout auto-denies, duplicate requests merge). New installs default to manual approval; casting, extending and stitching share the same policy.
- **Chinese-language GUI** — Home / Settings, connection history, device list, live status and log window; light / dark / follow-system themes.
- **Close without interrupting** — on Windows the close button hides the window to the notification area and the session keeps running; click the icon to restore, right-click for Show / Quit (quitting with a live session asks first); launching again wakes the running instance.
- **Ready out of the box** — launch at logon, auto-receive, in-app update check and one-click install (SHA-256 verified).
- **Cross-platform** — Windows 10/11, Linux (X11), macOS 10.15+.

## Quick Start

### Windows (recommended)

1. Download [Mivara-Setup.exe](https://github.com/wzy-777/Mivara/releases/latest/download/Mivara-Setup.exe) (or pick a version from [Releases](https://github.com/wzy-777/Mivara/releases)).
2. Run the installer. It is not code-signed, so SmartScreen may prompt — see [below](#windows-smartscreen-notice).
3. On the receiver, open Mivara and click "Start receiving" in the Home receive status bar; on the sender click "Connect device", pick a device, then choose "Cast", "Extend" or "Stitch" (stitch needs Windows on both ends — see the [User Guide](docs/en/usage.md#stitch-software-kvm)). New installs default to manual approval, so the receiver asks you to "Allow once" or "Deny"; change this under Settings → Receiving & Security to "Password only" or "Allow any connection".

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
| [Architecture & Protocol](docs/en/architecture.md) | Directory layout, data flow, protocol v3 and connection authorization, codecs and sidecar services |
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

## Security

Mivara is designed for **trusted LANs**. The receiver can require manual approval, a password, or allow any connection; 8081 (video and authorization) and 8084 (stitch keyboard/mouse) obey the same policy, 8085 (optional stitch file drop) is gated by a one-shot token from the active stitch session, and 8082 (compute offload) and 8083 (port inspector) only accept the token of the current authorized session. Still note:

- **Authorization is not encryption.** The password is only an admission check (a PBKDF2 record is stored locally, the password never crosses the wire and is verified with an HMAC challenge); **the video stream and stitch events are not encrypted**, so other devices on the link can still capture them.
- "Allow any connection" lets any device that can reach the port in directly — use it only on a network you fully control.
- Never port-forward these ports to the public internet. See [SECURITY.md](SECURITY.md) for the threat model and how to report a vulnerability.

## Acknowledgements

The virtual display driver comes from **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**.

Third-party components and their licenses are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## License

The **source code** is released under the [MIT License](LICENSE) © 2026 wzy-777. The released `sender` / `receiver` binaries link FFmpeg under GPL-3.0-or-later, so **those two binaries are distributed under GPL-3.0-or-later** (full text: [LICENSES/GPL-3.0-or-later](LICENSES/GPL-3.0-or-later)); `app` does not link FFmpeg. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the component list and how to obtain FFmpeg's corresponding source.

---

<div align="center">

**Linux • Windows 10/11 • macOS — one codebase, every platform.**

</div>
