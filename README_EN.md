# TwinView

[中文](README.md) | English

**Zero-config LAN screen casting / wireless second monitor**

A cross-platform C++ project: turn any computer on your LAN into a second monitor.

[![Version](https://img.shields.io/badge/version-0.8.1-blue)](https://github.com/wzy-777/TwinView)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#requirements)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#requirements)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## Table of Contents

- [TwinView](#twinview)
  - [Table of Contents](#table-of-contents)
  - [Introduction](#introduction)
  - [Features](#features)
  - [Architecture](#architecture)
    - [Directory Layout](#directory-layout)
    - [Executables](#executables)
    - [Network Ports](#network-ports)
  - [How It Works](#how-it-works)
    - [1 — Discovery (SSDP)](#1--discovery-ssdp)
    - [2 — Extend Handshake](#2--extend-handshake)
    - [3 — Virtual Display Lifecycle (Extend Mode)](#3--virtual-display-lifecycle-extend-mode)
    - [4 — Frame Streaming](#4--frame-streaming)
    - [5 — Sidecar Services](#5--sidecar-services)
  - [Display Modes](#display-modes)
  - [Requirements](#requirements)
  - [Installing Dependencies](#installing-dependencies)
  - [Building](#building)
    - [Windows Console Notes](#windows-console-notes)
    - [Packaging the Installer (Windows)](#packaging-the-installer-windows)
  - [Usage](#usage)
    - [GUI (recommended)](#gui-recommended)
    - [Receiver Keys](#receiver-keys)
    - [Sender Keys](#sender-keys)
  - [Windows Virtual Display Driver](#windows-virtual-display-driver)
    - [Installation (Administrator PowerShell)](#installation-administrator-powershell)
    - [Resolution Notes](#resolution-notes)
  - [Network Configuration](#network-configuration)
    - [Firewall Rules (Receiver)](#firewall-rules-receiver)
    - [Network Requirements](#network-requirements)
  - [Performance](#performance)
    - [Bandwidth (uncompressed RGB24)](#bandwidth-uncompressed-rgb24)
    - [Latency](#latency)
  - [Troubleshooting](#troubleshooting)
    - [Diagnostic Commands](#diagnostic-commands)
  - [Roadmap](#roadmap)
  - [Acknowledgements](#acknowledgements)
  - [License](#license)

---

## Introduction

TwinView is a lightweight, cross-platform **screen extension / casting** tool. The sender captures the local screen and streams it over the network to the receiver, which displays it fullscreen.

Two modes are provided:

1. **Mirror** — the receiver shows the sender's primary screen in full (demos and presentations).
2. **Extend** — the sender **automatically activates a virtual display**, creating a real desktop workspace; windows and the mouse can be dragged into this "invisible screen" and the receiver shows it in real time. The virtual display is detached when the session ends.

Extend mode requires a Windows indirect display driver (IddCx); see [installation below](#windows-virtual-display-driver). The driver only needs to be installed once — attaching/detaching the virtual display is managed automatically during the session.

---

## Features

| Category | Capability |
|----------|-----------|
| **Extend mode** | Virtual display auto-enabled and attached at session start; on every exit path (normal end / Ctrl+C / disconnect / console close / hard-kill fallback) it is detached and the device is disabled, so it vanishes from system settings completely |
| **Virtual display integration** | Activation/deactivation/positioning done in-process via the CCD API — no manual display-settings fiddling |
| **Visible cursor** | The pointer is drawn into the video frames; in extend mode this equals "pointer crossing" |
| **Direct connect** | `sender <IP>` skips discovery — works when SSDP multicast is blocked by network policy |
| **Discovery** | Zero-config SSDP discovery (UDP 1900 multicast + broadcast fallback) |
| **GUI** | SDL2_ttf interface: receive/send, automatic search, device list (cast/extend), manual IP entry |
| **Performance** | Up to 120 FPS streaming, 4 MB socket buffers, TCP_NODELAY |
| **CPU offload** | RLE frame compression and color conversion offloaded to the receiver's CPU (TCP 8082) |
| **Port inspector** | Remotely list/query/kill processes on any port of the receiver (TCP 8083) |
| **Cross-platform** | Linux (X11), Windows 10/11, macOS 10.15+ |

---

## Architecture

### Directory Layout

```
TwinView/
├── makefile                  # Cross-platform build (Linux/macOS/Windows MinGW)
├── src/
│   ├── app.cpp               # GUI launcher (SDL2 + SDL2_ttf): receive/send/search/cast
│   ├── sender.cpp            # Capture, handshake, streaming
│   ├── receiver.cpp          # Fullscreen render, SSDP broadcast, compute + port services
│   ├── discover.cpp/.h       # SSDP discovery engine
│   ├── gpu_accelerate.c/.h   # Remote CPU offload (RLE compress / color convert)
│   ├── ports.cpp/.h          # Remote port inspection service
│   └── vdd.cpp/.h            # Windows virtual-display lifecycle (shared by sender + GUI)
├── bin/                      # Executables + runtime DLLs (build output)
├── scripts/                  # Windows convenience launch scripts
├── installer/                # Inno Setup packaging script (TwinView.iss)
├── vdd/                      # Virtual display driver (signed MttVDD + install scripts)
├── test/                     # Local tests / diagnostics (not committed)
└── build/                    # Object files
```

### Executables

| Program | Responsibility |
|---------|----------------|
| `app` | GUI launcher: receive/send choice, SSDP search, device list (cast/extend), manual IP |
| `sender` | Captures the screen (GDI/X11/CoreGraphics), negotiates mode, streams; manages virtual-display activation and positioning in extend mode via the vdd module |
| `receiver` | SSDP broadcast, fullscreen rendering, hosts the two sidecar services |

### Network Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 1900 | UDP multicast | SSDP discovery (M-SEARCH / NOTIFY) |
| 8081 | TCP | Video frame stream |
| 8082 | TCP | CPU compute offload service |
| 8083 | TCP | Remote port inspection service |

---

## How It Works

### 1 — Discovery (SSDP)

```
Receiver  →  joins 239.255.255.250:1900, periodically sends NOTIFY, listens for M-SEARCH
Sender    →  sends M-SEARCH per NIC (multicast + broadcast fallback), collects 200 OK replies, lists receivers for the user
```

- The sender probes on every local NIC (multicast + 255.255.255.255 broadcast) — broadcast often still gets through on enterprise networks that drop multicast
- Replies are keyed on the **datagram source address** (LOCATION may name the wrong NIC on multi-homed receivers; the source address is always reachable)
- The receiver's LOCATION uses the NIC IP on the **same subnet** as the requester

If network policy blocks even broadcast, use direct connect:

```
bin/sender 192.168.1.105        # skip discovery, connect straight to 8081
bin/sender 192.168.1.105 8081   # or specify the port
bin/sender 192.168.1.105 --mode extend    # non-interactive: set mirror / extend (used by the GUI)
```

### 2 — Extend Handshake

After the receiver and display mode are chosen, the sender connects over TCP 8081 and exchanges a handshake:

```
Sender → Receiver   (16 bytes, network byte order)
  uint32  sender_width
  uint32  sender_height
  uint32  fps
  uint32  mode          0=mirror  1=extend

Receiver → Sender   (12 bytes)
  uint32  receiver_width
  uint32  receiver_height
  uint32  status        0=OK
```

### 3 — Virtual Display Lifecycle (Extend Mode)

```
Extend mode selected
  → vddStartup():
      enable the Root\MttVDD device (if disabled; SetupAPI, needs admin)
      → CCD SetDisplayConfig activates the virtual display (takes it over
        if left over from a previous session)
      → positioned right of the existing desktop, 1920x1080@60
      → locked as the capture source
  → handshake, stream (windows/mouse can cross into the virtual screen)
  → session end / Ctrl+C / peer disconnect / console window closed
  → vddShutdown(): detach the virtual display + DISABLE the Root\MttVDD
    device → the virtual screen vanishes from Settings > Display entirely
    (not merely detached from the desktop)
```

Implemented in [vdd.cpp](src/vdd.cpp) / [vdd.h](src/vdd.h) (shared by the sender and the GUI), built on the
Windows CCD API (`QueryDisplayConfig` / `SetDisplayConfig`),
`ChangeDisplaySettingsEx`, and SetupAPI device enable/disable (`DIF_PROPERTYCHANGE`).

> Note: a CCD detach alone is NOT enough — the PnP display device still
> shows up in Settings > Display and can still be extended to. Every
> session end therefore **disables the device** (the whole IddCx/UMDF
> stack goes down), and the next extend session re-enables it
> automatically. Matching is exact on hardware ID `Root\MttVDD`, so other
> vendors' virtual display adapters are untouched.

**The virtual display never lingers** – every exit path tears it down and disables it:

| Exit path | Handling |
|-----------|----------|
| Normal end / peer disconnect / send failure | `vddShutdown()` in `main` cleanup |
| Ctrl+C | Graceful exit, same cleanup |
| Sender console window closed (X) | Detached inside the `CTRL_CLOSE_EVENT` handler |
| Process `exit()` | `atexit(vddShutdown)` fallback |
| GUI "Stop streaming" | Ctrl+C graceful stop first (waits up to 8 s), then the GUI calls `vddForceDetach()` if the sender had to be hard-killed |

The streaming socket has a 2 s send timeout: when the peer disconnects or
goes silent the send fails quickly, the session ends, and the virtual
display is detached immediately instead of hanging.

### 4 — Frame Streaming

```
Sender    →  capture (extend mode = virtual screen; mirror = primary screen)
          →  draw the mouse cursor into the frame (GetCursorInfo + DrawIconEx)
          →  optionally RLE-compress via the compute offload service
          →  send [uint32 frame length][frame data]

Receiver  →  read frame header, read frame body
          →  RLE-decompress if frame length < raw size
          →  SDL_UpdateTexture → RenderCopy → RenderPresent
```

### 5 — Sidecar Services

- **Compute offload (8082)**: PING / COMPRESS (RLE) / COLORCONV (BGR→RGB), executed on the receiver's CPU with real timing measurements (`clock_gettime` on Linux/macOS, `QueryPerformanceCounter` on Windows). If the service is unavailable, the sender silently falls back to local uncompressed frames.
- **Port inspector (8083)**: LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT. Port data is collected natively per platform: Linux reads `/proc/net/*` + inode→PID mapping; macOS shells out to `lsof`; Windows uses `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`.

---

## Display Modes

| Mode | GUI button / CLI | Behavior |
|------|------------------|----------|
| Mirror | "Cast" / `--mode mirror` (default) | Receiver shows the sender's primary screen in a resizable window |
| Extend | "Extend" / `--mode extend` | Sender auto-activates a virtual display; receiver shows it borderless fullscreen as a **real desktop workspace** — windows can be dragged in, the mouse crosses over |

> Earlier "simulated extend" variants (extend-right/below/left: fullscreen mirror with a blue seam line) were removed — they never created a real desktop workspace. Extend is always mode 2.

---

## Requirements

**Linux**

- Ubuntu 18.04+ / Debian 10+ / Fedora 32+ / Arch
- GCC 8+ (C++17), make
- libX11-dev, libsdl2-dev, libsdl2-ttf-dev, pthread
- Any CJK font (`fonts-wqy-microhei` or Noto Sans CJK) — the GUI exits with an error if no font is found

**Windows**

- Windows 10 1903+ / Windows 11
- MinGW-w64 (MSYS2) + make — **MSVC is not supported** (the build only goes through g++/makefile)
- SDL2, SDL2_ttf (MSYS2 packages), Windows SDK (iphlpapi, needed by the port inspector)
- Extend mode: virtual display driver (see [below](#windows-virtual-display-driver))
- `sender` / `app` embed a `requireAdministrator` manifest — **they must run elevated**
  (one UAC prompt at launch). Extend mode needs admin rights to enable/disable the
  virtual display device so it disappears from Settings > Display after the session.
  `receiver` does not.

**macOS**

- macOS Catalina 10.15+, Clang 12+
- `brew install sdl2 sdl2_ttf`

> `SDL2_ttf` is a required dependency (localized GUI).
>
> **Note**: on Windows deployment the runtime DLLs must sit next to the exe (`bin/` already contains the full closure). After adding a dependency, resolve the closure with `ldd bin/app.exe`.

---

## Installing Dependencies

```bash
make install-deps          # Linux/macOS/MSYS2 automatic install
```

---

## Building

Build from source in three steps: install dependencies → make → run the binaries in `bin/`.

```bash
git clone https://github.com/wzy-777/TwinView.git
cd TwinView
make install-deps    # 1. install system dependencies (auto-detects the package manager)
make                 # 2. build everything (output goes to bin/)
./bin/app            # 3. run the GUI (or bin/sender, bin/receiver)
```

> Windows: run the same commands in an MSYS2 MinGW64 terminal; MSVC is not supported.

Individual targets and development helpers:

```bash
make              # everything (bin/sender, bin/receiver, bin/app)
make sender       # sender only
make receiver     # receiver only
make app          # GUI launcher only
make debug        # -g -O0 -DDEBUG debug build
make clean        # remove build output
make check        # check environment/sources
make run          # build and launch the GUI (also run-sender / run-receiver)
```

Builds use `-O3 -march=native -flto`; output goes to `bin/`.

### Windows Console Notes

- `sender` is linked as a console-subsystem binary — it shows logs and exits gracefully on Ctrl+C (including detaching the virtual display).
- `receiver` / `app` are linked with `-mwindows` (GUI subsystem), so stdout is discarded; to see receiver logs, relink receiver manually with `-mconsole`.

### Packaging the Installer (Windows)

Build the installer with Inno Setup:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\TwinView.iss
# Output: dist\TwinView-Setup-<version>.exe
```

The installer bundles `bin/` (exes + DLLs), `scripts/`, and the `vdd/` virtual display driver (the oversized vendor GUI is excluded), and creates Start-menu/desktop shortcuts.

---

## Usage

### GUI (recommended)

```
bin/app
```

Choose "Receive" or "Send":

- **Receive**: shows the local IP, ports and a "receiving" status while launching the receiver to wait for connections; "Stop" ends it
- **Send**: shows "searching"; once devices are found, each row offers "Cast" (mirror) or "Extend"; streaming can be stopped at any time
- **No devices found**: three options — "Keep searching", "Enter IP manually", "Exit"; a manually entered IP leads to the same cast/extend choice
- **ESC**: back from the manual-IP page; on other pages stop the current session and return home; on the home screen quits

On Windows you can also use the convenience scripts in `scripts/`: `start_receiver.bat` starts the receiver in one click; `启动投屏发送.bat <receiver-IP>` direct-connects and sends.

### Receiver Keys

| Key | Action |
|-----|--------|
| ESC / Q | Disconnect and quit |
| F11 | Toggle fullscreen (extend mode) |
| Close window | Stop receiving |

### Sender Keys

| Input | Action |
|-------|--------|
| Ctrl+C | Graceful exit (virtual display detached + device disabled) |
| Mode prompt 1/2 | Mirror / Extend (interactive CLI only) |

---

## Windows Virtual Display Driver

Extend mode relies on an indirect display driver (IddCx) to provide the virtual monitor. This project adapts **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)** (MIT license, properly signed via SignPath).

The driver **only needs to be installed once**. Afterwards no manual steps are needed — sender enables the device and attaches the virtual display when an extend session starts, and detaches + disables it when the session ends (so it disappears from Settings > Display completely).

### Installation (Administrator PowerShell)

```powershell
# 1. Download from GitHub Releases (e.g. VDD.Control.25.7.23.zip) and extract

# 2. Stage the driver
pnputil /add-driver MttVDD.inf /install
# (note: the driver in the x86 directory actually targets NTamd64)

# 3. Create the Root\MttVDD device
devcon.exe install MttVDD.inf "Root\MttVDD"

# 4. Copy the settings file (resolution/refresh-rate list lives there)
Copy-Item vdd_settings.xml C:\Windows\System32\drivers\UMDF\vdd_settings.xml

# 5. Rescan; "Generic Monitor (VDD by MTT)" appearing means success
pnputil /scan-devices
```

### Resolution Notes

Once activated, the virtual display is automatically configured to **1920x1080@60** and placed right of the existing desktop. The resolution can be chosen at session start (interactive mode) or defaults to 1920x1080; to change the default, edit the `vddConfigure(dev, x, 0, w, h, 60)` call in `vddStartup()` in vdd.cpp, or add modes to the driver's `vdd_settings.xml`.

---

## Network Configuration

### Firewall Rules (Receiver)

```powershell
# Windows (Administrator PowerShell)
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

### Network Requirements

- All devices on the same subnet
- Multicast depends on switch IGMP snooping; **if multicast is blocked, use direct connect** (`sender <IP>`)
- 1080p@60 over gigabit wired recommended; 5 GHz WiFi works at lower resolutions

---

## Performance

### Bandwidth (uncompressed RGB24)

| Resolution | 30 FPS | 60 FPS |
|------------|--------|--------|
| 1280×720 | ~125 MB/s | ~250 MB/s |
| 1920×1080 | ~280 MB/s | ~560 MB/s |
| 2560×1440 | ~500 MB/s | ~1 GB/s |

> RLE compression typically cuts 30–70% of bandwidth on desktop content (text/UI); video content compresses less.

### Latency

| Link | Typical |
|------|---------|
| Gigabit wired | < 5 ms |
| 5 GHz WiFi | 10–15 ms |
| 2.4 GHz WiFi | 20–35 ms |

The receiver writes statistics to `gpu_stats.json` every 60 seconds and prints a console summary every 30 seconds.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| No receivers found | UDP 1900 multicast and broadcast both blocked | Use direct connect `sender <IP>` |
| Exits instantly / no logs from receiver | `receiver`/`app` use the GUI subsystem (-mwindows), stdout discarded | sender logs to console natively; relink receiver/app with `-mconsole` to see output |
| Missing *.dll | Incomplete runtime DLL closure | Resolve with `ldd bin/app.exe` and copy missing DLLs into `bin/` (next to the exe) |
| Connection refused | Receiver not running | Check port 8081, restart the receiver |
| "Virtual display driver not found" | MttVDD driver not installed | Follow the [installation steps](#windows-virtual-display-driver) |
| "Virtual display did not come up" | Driver installed but activation failed | Check device status with `pnputil /enum-devices /class Monitor`, reboot and retry |
| Extend mode black screen / no video | Handshake mismatch between the two ends (mode 0=mirror 1=extend) | Use the same binary version on both ends |
| Virtual display stays in Settings > Display after disconnect | Older builds only did a CCD detach, or sender/app not run as admin | Fixed: the Root\MttVDD device is disabled at exit (sender/app must run elevated). If the log shows a disable failure, manually disable "Virtual Display Driver" in Device Manager |

### Diagnostic Commands

```bash
netstat -tulpn | grep -E '8081|8082|8083|1900'   # receiver's four ports
nc -zv <receiver-IP> 8081                          # TCP reachability
make check                                          # environment self-check
```

---

## Roadmap

- [x] Screen mirroring
- [x] Extend mode (virtual display auto attach/detach + per-monitor capture + pointer crossing, Windows)
- [x] Direct connect (bypass SSDP multicast)
- [x] Remote CPU offload (RLE compression + color conversion, real timing)
- [x] Extend handshake (resolution exchange)
- [x] GUI launcher (SDL2_ttf interface: receive/send/search/device list/manual IP)
- [x] macOS CoreGraphics capture
- [x] Remote port inspection service (TCP 8083)
- [x] Per-client statistics + JSON export
- [ ] Receiver input back-channel (real keyboard/mouse control from the receiver side)
- [ ] **Optional encoders: RLE / H.264 / H.265 / AV1 as a selectable option**
- [ ] GUI advanced settings (resolution choice, encoder choice, connection history)
- [ ] Audio capture and streaming
- [ ] TLS encryption
- [ ] Virtual display resolution adapts to the receiver (post-handshake negotiation)
- [ ] Adaptive frame rate
- [ ] Wayland support
- [ ] Virtual display solutions for macOS/Linux
- [ ] Screen stitching

---

## Acknowledgements

This project is developed from **[RGM (Ralefaso GlassMirror)](https://github.com/RR-Ralefaso/RGM)**.

The virtual display driver comes from **[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)**.

## License

[MIT](LICENSE) © 2026 wzy-777

---

<div align="center">

**Linux • Windows 10/11 • macOS — one codebase, every platform.**

</div>
