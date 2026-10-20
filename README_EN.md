# Mivara

[中文](README.md) | English

**Zero-config LAN screen casting / wireless second monitor**

A cross-platform C++ project: turn any computer on your LAN into a second monitor.

[![Version](https://img.shields.io/badge/version-0.9.1-blue)](https://github.com/wzy-777/Mivara)
[![Download Latest Installer](https://img.shields.io/badge/download-latest%20installer-brightgreen?logo=github&logoColor=white)](https://github.com/wzy-777/Mivara/releases/latest/download/Mivara-Setup.exe)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#requirements)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#requirements)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

> Renamed: formerly **TwinView**, now **Mivara** (幻屏) as of v0.9.1; old GitHub links redirect automatically.

---

## Table of Contents

- [Mivara](#mivara)
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
  - [Codecs](#codecs)
  - [Settings & History](#settings--history)
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
    - [Windows SmartScreen Notice](#windows-smartscreen-notice)
    - [Diagnostic Commands](#diagnostic-commands)
  - [Roadmap](#roadmap)
  - [Acknowledgements](#acknowledgements)
  - [License](#license)

---

## Introduction

Mivara is a lightweight, cross-platform **screen extension / casting** tool. The sender captures the local screen and streams it over the network to the receiver, which displays it fullscreen.

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
| **Video codecs** | H.264 / H.265 / AV1 (FFmpeg), hardware-first (NVENC/QSV/AMF/VAAPI) with automatic software fallback (libx264/libx265/SVT-AV1); raw/RLE lossless options kept |
| **Live reconfigure** | Change codec/quality/bitrate/fps/scale/chroma mid-session — applied in seconds without dropping the stream (codec switches blip for a frame or two) |
| **Visible cursor** | The pointer is drawn into the video frames; in extend mode this equals "pointer crossing" |
| **Direct connect** | `sender <IP>` skips discovery — works when SSDP multicast is blocked by network policy |
| **Discovery** | Zero-config SSDP discovery (UDP 1900 multicast + broadcast fallback), shows hostnames |
| **Connection history** | Last 20 connections embedded on the Home screen — one-click direct connect, no search needed |
| **GUI** | SDL2_ttf interface: left nav (Home / Settings), receive/send, connection history, automatic search, device list (cast/extend), manual IP entry, live status panel, independent log window |
| **Auto-start / auto-receive** | Launch at logon (Windows Task Scheduler, no UAC prompt); auto-receive opens the app directly in receive mode when enabled (off by default) |
| **Update check** | A "Check for updates" button in Settings plus an automatic check at launch (on by default, at most once per 24 h per machine, can be turned off). A new release can be downloaded in-app (size + SHA-256 verified) and installed with one click, or opened on the release page for a manual download. Only the GitHub release source is contacted; no local data is uploaded |
| **Performance** | Up to 120 FPS streaming, 4 MB socket buffers, TCP_NODELAY |
| **CPU offload** | RLE frame compression and color conversion offloaded to the receiver's CPU (TCP 8082, RLE mode only) |
| **Port inspector** | Remotely list/query/kill processes on any port of the receiver (TCP 8083) |
| **Cross-platform** | Linux (X11), Windows 10/11, macOS 10.15+ |

---

## Architecture

### Directory Layout

```
Mivara/
├── makefile                  # Cross-platform build (Linux/macOS/Windows MinGW)
├── src/
│   ├── app.cpp               # GUI launcher (SDL2 + SDL2_ttf): left nav + Home/Settings (history embedded in Home)
│   ├── sender.cpp            # Capture, handshake, streaming
│   ├── receiver.cpp          # Fullscreen render, SSDP broadcast, compute + port services
│   ├── proto.h               # Protocol v2 (handshake / control packets / codec enum)
│   ├── codec.cpp/.h          # FFmpeg encode/decode (H.264/H.265/AV1, sender + receiver)
│   ├── config.cpp/.h         # GUI settings persistence (config.ini)
│   ├── update.cpp/.h         # Update check + in-app download/install (GUI only)
│   ├── version.h             # Single version source (shared by C++ and Inno Setup)
│   ├── history.cpp/.h        # Connection history (history.ini)
│   ├── discover.cpp/.h       # SSDP discovery engine
│   ├── gpu_accelerate.c/.h   # Remote CPU offload (RLE compress / color convert)
│   ├── ports.cpp/.h          # Remote port inspection service
│   └── vdd.cpp/.h            # Windows virtual-display lifecycle (shared by sender + GUI)
├── bin/                      # Executables + runtime DLLs (build output)
├── scripts/                  # Windows convenience launch scripts
├── installer/                # Inno Setup packaging script (Mivara.iss)
├── vdd/                      # Virtual display driver (signed MttVDD + install scripts)
├── winget/                   # winget-pkgs submission manifests (updated per release)
├── test/                     # Local tests / diagnostics (not committed)
└── build/                    # Object files
```

### Executables

| Program | Responsibility |
|---------|----------------|
| `app` | GUI launcher: left nav (Home/Settings), receive/send choice, connection history on Home, SSDP search, device list (cast/extend), manual IP, live reconfigure |
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

After the receiver and display mode are chosen, the sender connects over TCP 8081 and exchanges a handshake (protocol v2, all fields network byte order):

```
Sender → Receiver   HsRequestV2 (48 bytes, 12 x uint32)
  magic          0x54564853 ('TVHS')
  version        2
  width, height  capture size (downscaling happens inside the encoder)
  fps            target frame rate
  mode           0=mirror  1=extend
  codec          0=raw 1=RLE 2=H.264 3=H.265 4=AV1
  codec_flags    bit0 = sender wants hardware encode
  bitrate_kbps   0 = quality (CRF) mode
  quality        0..51 (CRF-like; 0xFFFFFFFF = encoder-default sentinel, reserved/unused)
  gop_frames     IDR interval (0 = default 2*fps)
  reserved       0

Receiver → Sender   HsResponseV2 (36 bytes)
  magic, version, recv_width, recv_height
  status         0=OK 1=version 2=codec 3=dims 4=decoder 5=legacy
  codec          accepted codec
  codec_flags, reserved[2]
```

Both ends must run the same version. A legacy peer (old 16-byte handshake) is detected and reported explicitly — no silent black screen.

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
          →  encode per the negotiated codec:
               H.264/H.265/AV1  → FFmpeg encode (hardware-first, low-latency tuned) → send AU
               RLE              → optionally compress via the compute offload service (8082)
               raw              → send RGB24 as-is
          →  send [uint32 frame length][frame data]

Receiver  →  read frame header, read frame body
          →  coded → FFmpeg decode → YUV/NV12 texture render
          →  RLE → decompress → RGB24 texture
          →  SDL_UpdateTexture/YUVTexture → RenderCopy → RenderPresent
```

Control packets: `0xFFFFFFFF` + `ctrl_type` + body. `CTRL_RESIZE` (extend-mode resolution change) and `CTRL_RECONFIG` (runtime parameter change) both carry a full `HsRequestV2`; the receiver re-inits its decoder and skips to the next keyframe.

### 5 — Sidecar Services

- **Compute offload (8082)**: PING / COMPRESS (RLE) / COLORCONV (BGR→RGB), executed on the receiver's CPU with real timing measurements (`clock_gettime` on Linux/macOS, `QueryPerformanceCounter` on Windows). If the service is unavailable, the sender silently falls back to local uncompressed frames.
- **Port inspector (8083)**: LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT. Port data is collected natively per platform: Linux reads `/proc/net/*` + inode→PID mapping; macOS shells out to `lsof`; Windows uses `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`.

---

## Codecs

| Codec | Notes | Default |
|-------|-------|---------|
| **H.264** | Balanced quality/latency; hardware (NVENC/QSV/AMF/VAAPI) first, falls back to libx264 | ✅ |
| **H.265** | Better quality per bitrate; hardware first, falls back to libx265 | |
| **AV1** | Best compression; hardware first, falls back to SVT-AV1 | |
| **Raw** | Lossless RGB24, highest bandwidth | |
| **RLE** | Lossless compression (can offload to the receiver's CPU) | |

Low-latency tuning: no B-frames, `zerolatency`, GOP = 2×fps, immediate IDR after resize. For desktop text, enable 4:4:4 chroma in Settings (software encoders only).

## Settings & History

The GUI Settings screen (persisted to `%LOCALAPPDATA%\Mivara\config.ini`):

| Setting | Default | Description |
|---------|---------|-------------|
| Appearance | follow system | Light / Dark / Follow system; the header switcher applies instantly and writes `theme` to `config.ini`, so it needs no OK |
| Codec | H.264 | auto/H.264/H.265/AV1/raw/RLE |
| Hardware accel | on | Prefer GPU encoding |
| Quality / Bitrate | 23 / 0 | Bitrate > 0 uses bitrate mode, otherwise CRF |
| FPS / Scale | 60 / 100% | Sender cap and pre-downscale |
| Chroma | 4:2:0 | 4:4:4 = sharper text (software encode) |
| Status console | off | Shows the classic cmd window |
| Log window | off | Auto-open the independent log window |
| Auto-start | off | Registers a Windows Task Scheduler job (onlogon, no UAC at logon); cleaned up on uninstall |
| Auto-receive | off | Enter receive mode as soon as the app opens |
| Auto update check | on | Checks for a new release ~3 s after launch (at most once per 24 h per machine); when off, only the manual button goes online |

> "Follow system" reads the OS light/dark preference. Windows reads the `AppsUseLightTheme` registry value and re-checks it every 2 s, so flipping the OS theme is followed immediately; macOS (`AppleInterfaceStyle`) and Linux (GNOME `color-scheme`) are sampled once at startup, so an OS theme change needs a restart of the app there. A failed probe always falls back to light.

**Live reconfigure**: while streaming, change any transfer parameter in Settings and press OK — it takes effect within seconds without dropping the connection (codec/scale switches blip for a frame or two). In classic console mode there is no stdin channel, so the session is restarted with the new parameters instead.

Transfer status is shown live in the GUI status panel (FPS / bitrate / encoder); the independent log window shows child-process output. The cmd console is hidden by default and can be re-enabled in Settings.

The connection-history list (`history.ini`, last 20 entries) sits under the receive/send cards on the Home screen — click to connect directly (cast/extend) without searching; individual entries can be deleted or cleared.

---

## Display Modes

| Mode | GUI button / CLI | Behavior |
|------|------------------|----------|
| Mirror | "Cast" / `--mode mirror` (default) | Receiver shows the sender's primary screen in a resizable window |
| Extend | "Extend" / `--mode extend` | Sender auto-activates a virtual display; receiver shows it borderless fullscreen as a **real desktop workspace** — windows can be dragged in, the mouse crosses over |

> Earlier "simulated extend" variants (extend-right/below/left: fullscreen mirror with a blue seam line) were removed — they never created a real desktop workspace. Extend always uses `--mode extend` (protocol mode 1).

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

| Component | Purpose |
|-----------|---------|
| g++ / make | C++17 build |
| libX11-dev | Linux screen capture |
| SDL2 / SDL2_ttf | Receiver rendering + GUI (CJK fonts) |
| **FFmpeg** (libavcodec/libavutil/libswscale) | H.264/H.265/AV1 encode/decode (MSYS2: `mingw-w64-x86_64-ffmpeg`) |

---

## Building

Build from source in three steps: install dependencies → make → run the binaries in `bin/`.

```bash
git clone https://github.com/wzy-777/Mivara.git
cd Mivara
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
& "D:\Program Files\Inno Setup 6\ISCC.exe" installer\Mivara.iss
# Output: dist\Mivara-Setup-<version>.exe
```

A release must also carry the fixed-name copy `dist/Mivara-Setup.exe` (the in-app "Download update" and the README download badge both use that fixed URL) **and** `SHA256SUMS.txt` (the checksum anchor the in-app install verifies; without it only the browser download is offered). CI produces all three; for a local build copy it by hand: `copy dist\Mivara-Setup-<version>.exe dist\Mivara-Setup.exe`.

The installer bundles `bin/` (exes + DLLs), `scripts/`, and the `vdd/` virtual display driver (the oversized vendor GUI is excluded), and creates Start-menu/desktop shortcuts. The "Launch Mivara" checkbox on the finish page starts the elevated GUI directly (the installer launches it with its elevated token — no second UAC prompt).

---

## Usage

### GUI (recommended)

```
bin/app
```

The top-right of the header holds the **appearance switcher** — three icons: sun = Light, moon = Dark, monitor = Follow system. The left side holds the nav bar: **Home** (receive/send + connection history), **Settings**, and "Quit" at the bottom:

- **Appearance**: three icons in the header (sun = Light, moon = Dark, monitor = Follow system); one click recolours the window and writes `theme` to `config.ini`, with no OK needed. The same setting appears as a labelled three-way switch under Settings · Appearance, for anyone who does not read the icons. See the platform note above for "Follow system"
- **Home · Receive**: shows the local IP, ports and a "receiving" status while launching the receiver to wait for connections; "Stop" ends it
- **Home · Send**: shows "searching"; once devices are found (with hostnames), each row offers "Cast" (mirror) or "Extend"; streaming can be stopped at any time
- **Home · Connection history**: the last 20 connections are listed under the receive/send cards — click to connect directly (no search); delete single entries or clear all; mouse wheel scrolls
- **Home · In session**: while casting/receiving you can switch to Settings (the session keeps running); "Back to session" or ESC in Settings returns to the live status, while "Home" goes to the Home screen (shows the session running, with Stop)
- **Settings**: the left column is "Quality & codec" (codec auto/H.264/H.265/AV1/raw/RLE, quality/FPS/bitrate/scale/chroma); the right column is "Connection & startup" (hardware accel / status console / log window / auto-start / auto-receive / auto update check) and "Appearance"; the "Software update" card sits below. **Changed transfer parameters apply live when you press OK — the stream is not dropped**
- **Settings · Update**: the bottom "Update" row shows the current version and the check result; a new release offers "Download update" (with progress) → "Install now" (Windows: after verification the app quits and starts the installer; Linux/macOS: opens the release page). You can "Ignore this version"; a new release also shows a banner at the top of Home and a dot on the Settings tab
- **No devices found**: three options — "Keep searching", "Enter IP manually", "Exit"; a manually entered IP leads to the same cast/extend choice
- **Streaming status**: live FPS / bitrate / encoder in the GUI status panel; the "Log" button opens the independent log window (child-process output). The cmd console is hidden by default — re-enable it in Settings
- **ESC**: discard Settings edits and return to the session; back from the manual-IP page; stop the session on cast/receive screens and go home; quits from home

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
| "Check for updates" fails (DNS / timeout / secure connection failed) | The check only contacts `github.com`; a flaky direct connection (corporate or mainland-China networks) makes it fail — expected degradation | Use "Open download page" and download in your browser (system proxy/VPN); quit Mivara in-app before running the new installer so the virtual display is detached cleanly |
| "Download update" reports a missing checksum file | That release has no `SHA256SUMS.txt` (in-app install requires a hash anchor, so it is not offered without one) | Use "Open download page" and install manually |
| SmartScreen prompts on download/run ("isn't commonly downloaded" / "Unknown publisher") | Installer is not code-signed and the new file has no download reputation (a normal reputation prompt, not a malware verdict) | Verify the SHA-256, then continue per the prompt — see [Windows SmartScreen Notice](#windows-smartscreen-notice) |

### Windows SmartScreen Notice

Installing straight from GitHub may show the following prompts on some Windows 10/11 machines — these are **SmartScreen reputation prompts (unsigned file, newly released, low download count), not malware verdicts**:

| Prompt | Where | Cause |
|--------|-------|-------|
| "Mivara-Setup.exe isn't commonly downloaded. Make sure you trust…" | Browser download bar | The file has no download reputation yet (standard Microsoft Defender SmartScreen check) |
| "Windows protected your PC" | On launch (may appear) | The installer is not code-signed |
| "Publisher: Unknown" | UAC elevation prompt | Same (unsigned) |

Verify the file first (must match the SHA-256 in the corresponding release notes):

```powershell
Get-FileHash "$HOME\Downloads\Mivara-Setup.exe" -Algorithm SHA256
```

If it matches, continue per the normal flow:

1. Browser download bar → "…" → "Keep";
2. Double-click the installer → "More info" → "Run anyway".

> If in doubt: Properties → General → "Unblock" (or PowerShell `Unblock-File`) is Windows' official "I trust this file" action — run it after the hash check.

(The installer is currently not code-signed — common for open-source projects.)

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
- [x] Optional encoders: RLE / H.264 / H.265 / AV1 as a selectable option
- [x] GUI advanced settings (encoder picker, connection history)
- [x] In-app update check + one-click download/install (Windows, SHA-256 verified, admin-only staging directory)
- [ ] GUI advanced settings: extend-resolution choice (currently fixed at the 1920x1080 default in the GUI)
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

Windows installer code signing (application in progress): provided free of charge by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org) — see the [Code signing policy](CODE_SIGNING_POLICY.md).

## License

[MIT](LICENSE) © 2026 wzy-777

---

<div align="center">

**Linux • Windows 10/11 • macOS — one codebase, every platform.**

</div>
