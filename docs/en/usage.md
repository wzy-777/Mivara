# User Guide

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/usage.md)

Mivara is a lightweight, cross-platform **screen extension / casting** tool. The sender captures the local screen and streams it over the network to the receiver, which displays it fullscreen.

Two display modes are provided, plus a "stitch" mode that sends no picture at all:

1. **Mirror** — the receiver shows the sender's primary screen in full (demos and presentations).
2. **Extend** — the sender **automatically activates a virtual display**, creating a real desktop workspace; windows and the mouse can be dragged into this "invisible screen" and the receiver shows it in real time. The virtual display is detached when the session ends.
3. **Stitch (software KVM)** — both PCs keep their own monitors and **share one keyboard and mouse**: choose which side of this PC the peer sits on (left / right / top / bottom) when starting, push the cursor out of that edge to take over the peer, and move back out of the peer's facing edge (or press Ctrl+Alt+Shift+Q) to hand control back. The side can be switched at any time during the session. No capture, no encoder, no virtual display; Windows ↔ Windows only.

Extend mode requires a Windows indirect display driver (IddCx); see [Virtual Display Driver](virtual-display.md) for installation. The driver only needs to be installed once — attaching/detaching the virtual display is managed automatically during the session.

This page covers day-to-day use: the interface and capabilities first, then connecting by display mode. For discovery, handshakes and streaming internals, see [Architecture & Protocol](architecture.md).

## Screenshots

The GUI is Chinese-only; the screenshots below show the Home screen (light/dark), device discovery, and an active stream:

| Home (light) | Home (dark) |
|:---:|:---:|
| ![Home (light)](../assets/home-light.png) | ![Home (dark)](../assets/home-dark.png) |

| Device discovery | Streaming |
|:---:|:---:|
| ![Device discovery](../assets/device-list.png) | ![Streaming](../assets/streaming.png) |

## Features

| Category | Capability |
|----------|-----------|
| **Extend mode** | Virtual display auto-enabled and attached at session start; on every exit path (normal end / Ctrl+C / disconnect / console close / hard-kill fallback) it is detached and the device is disabled, so it vanishes from system settings completely |
| **Virtual display integration** | Activation/deactivation/positioning done in-process via the CCD API — no manual display-settings fiddling |
| **Video codecs** | H.264 / H.265 / AV1 (FFmpeg), hardware-first (NVENC/QSV/AMF/VAAPI) with automatic software fallback (libx264/libx265/SVT-AV1); raw/RLE lossless options kept |
| **Live reconfigure** | Change codec/quality/bitrate/fps/scale/chroma mid-session — applied in seconds without dropping the stream (codec switches blip for a frame or two) |
| **Visible cursor** | The pointer is drawn into the video frames; in extend mode this equals "pointer crossing" |
| **Direct connect** | `sender <IP>` skips discovery — works when SSDP multicast is blocked by network policy |
| **Discovery** | Zero-config SSDP (UDP 1900 multicast + broadcast fallback); concurrent NIC listeners, a total deadline and cancellation; the first device appears immediately and later devices are added incrementally |
| **Connection history** | Last 20 connections embedded on the Home screen — one-click direct connect, no search needed |
| **Stitch (software KVM)** | One keyboard/mouse drives two Windows PCs: low-level input hooks + Raw Input, forwarded over TCP 8084 and injected on the peer with `SendInput`; the peer must confirm every session in a local dialog, and a refusal suppresses further prompts for 60 s (Windows only) |
| **GUI** | SDL2_ttf interface: left nav (Home / Settings), receive/send, connection history, automatic search, device list (cast/extend/stitch), manual IP entry, live status panel, independent log window |
| **Auto-start / auto-receive** | Launch at logon (Windows Task Scheduler, no UAC prompt); auto-receive opens the app directly in receive mode when enabled (off by default) |
| **Update check** | A "Check for updates" button in Settings plus an automatic check at launch (on by default, at most once per 24 h per machine, can be turned off). A new release can be downloaded in-app (size + SHA-256 verified) and installed with one click, or opened on the release page for a manual download. Only the GitHub release source is contacted; no local data is uploaded |
| **Performance** | Up to 120 FPS streaming, 4 MB socket buffers, TCP_NODELAY |
| **CPU offload** | RLE frame compression and color conversion offloaded to the receiver's CPU (TCP 8082, RLE mode only) |
| **Port inspector** | Remotely list/query/kill processes on any port of the receiver (TCP 8083) |
| **Cross-platform** | Linux (X11), Windows 10/11, macOS 10.15+ |

For the protocol and implementation behind each capability, see [Architecture & Protocol](architecture.md).

## Display Modes

| Mode | GUI button / CLI | Behavior |
|------|------------------|----------|
| Mirror | "Cast" / `--mode mirror` (default) | Receiver shows the sender's primary screen in a resizable window |
| Extend | "Extend" / `--mode extend` | Sender auto-activates a virtual display; receiver shows it borderless fullscreen as a **real desktop workspace** — windows can be dragged in, the mouse crosses over |
| Stitch | "Stitch" / `--mode stitch` | No picture is sent: both PCs use their own monitors and share one keyboard/mouse (Windows only) |

> Earlier "simulated extend" variants (extend-right/below/left: fullscreen mirror with a blue seam line) were removed — they never created a real desktop workspace. Extend always uses `--mode extend` (protocol mode 1).

Extend mode relies on the Windows virtual display driver, which the installer offers by default; installation and resolution configuration are covered in [Virtual Display Driver](virtual-display.md). Before connecting, the sender probes the receiver's current resolution and matches the virtual display to it. If the driver's mode list lacks that size, it is appended by default (`vdd_mode_write=never` disables this); if writes are disabled or fail, the receiver centres the picture with black bars instead of stretching it.

### Stitch (software KVM)

Stitch turns two Windows PCs sitting side by side into one workspace **without duplicating any picture**: machine A's desktop stays on A's monitor and B's on B's monitor — only the keyboard and mouse cross over.

- **Entry point**: click "Stitch" on a device row or in the connection history — a dialog first asks which side of this PC the peer sits on (left / right / top / bottom, preselected from the last answer); the CLI equivalent is `bin/sender <IP> --mode stitch --stitch-side left|right|top|bottom` (port 8084 — never point it at the video port).
- **Requirements**: both ends must be Windows and run the same Mivara version; the target must be serving (click "Receive" in the GUI or run `bin/receiver`).
- **Consent**: on every connection the target shows a topmost Yes/No dialog (default No, auto-declined after 30 s); its text names the edge the controller will enter through for the chosen side. After a refusal, the same source is rejected outright for 60 s instead of prompting again.
- **Choosing a side**: the side means **where the peer sits relative to this PC**, and it decides both the local edge the cursor leaves through and the peer's edge it enters from. Right: leave the local right edge, enter at the peer's left edge, return by leaving its left edge. Left: leave the local left edge, enter at the peer's right edge, return through its right edge. Top: leave the local top edge, enter at the peer's bottom edge, return through its bottom edge. Bottom: leave the local bottom edge, enter at the peer's top edge, return through its top edge. `Ctrl+Alt+Shift+Q` always hands control back immediately.
- **Switching mid-session**: the status panel's "Direction" row offers the four sides; a click takes effect immediately. If control is currently on the peer it is handed back first, and the new side becomes the default for the next session. With "Status console" enabled there is no stdin channel, so a switch restarts the sender and reconnects — the peer has to confirm the new session.
- **Stopping**: "Stop" in the status panel ends the session; closing the receiver window (or ESC/Q) disconnects immediately and releases every held key. Stopping receive mode from the GUI ends the receiver process directly, so a key or mouse button held at that instant may stay down on that PC until tapped once. The controller notices a normal peer exit or crash through its socket read side. With current builds on both ends, a network outage or power loss is detected by a heartbeat every 3 seconds with an approximately 9-second deadline, followed by release of held input; older peers fall back to TCP keepalive.
- **Permissions**: a receiver launched from the GUI inherits the GUI's administrator token (the GUI needs elevation for extend mode), so stitch input can reach elevated windows; a standalone `bin/receiver` runs unelevated and cannot type into administrator windows.
- **Security**: the stitch channel is unencrypted and intended for a trusted LAN only; either side can hand control back at any time, and the target never accepts a connection without a local confirmation.

## GUI

Launching the GUI is recommended:

```
bin/app
```

The top-right of the header holds the **appearance switcher** — three icons: sun = Light, moon = Dark, monitor = Follow system. The left side holds the nav bar: **Home** (receive/send + connection history) and **Settings**:

- **Appearance**: three icons in the header (sun = Light, moon = Dark, monitor = Follow system); one click recolours the window and writes `theme` to `config.ini`, with no OK needed. The same setting appears as a labelled three-way switch under Settings · Appearance, for anyone who does not read the icons. For "Follow system", see the platform note in [Settings & History](#settings--history)
- **Home · Receive**: shows the local IP, ports and a "receiving" status while launching the receiver to wait for connections; "Stop" ends it
- **Home · Send**: shows "searching"; once devices are found (with hostnames), each row offers "Cast" (mirror), "Extend" or "Stitch" (stitch first asks which side the peer is on); streaming can be stopped at any time
- **Home · Connection history**: the last 20 connections are listed under the receive/send cards — click to connect directly (no search, all three modes); delete single entries or clear all; mouse wheel scrolls
- **Home · In session**: while casting/receiving you can switch to Settings (the session keeps running); "Back to session" or ESC in Settings returns to the live status, while "Home" goes to the Home screen (shows the session running, with Stop)
- **Settings**: the left column is "Quality & codec" (codec auto/H.264/H.265/AV1/raw/RLE, quality/FPS/bitrate/scale/chroma); the right column is "Connection & startup" (hardware accel / status console / log window / auto-start / auto-receive / auto update check) and "Appearance"; the "Software update" card sits below. **Changed transfer parameters apply live when you press OK — the stream is not dropped**
- **Settings · Update**: the bottom "Update" row shows the current version and the check result; a new release offers "Download update" (with progress) → "Install now" (Windows: after verification the app quits and starts the installer; Linux/macOS: opens the release page). You can "Ignore this version"; a new release also shows a banner at the top of Home and a dot on the Settings tab
- **No devices found**: three options — "Keep searching", "Enter IP manually", "Exit"; a manually entered IP leads to the same cast/extend choice. If a connection fails or no receiver is ever found, see [Troubleshooting](troubleshooting.md)
- **Streaming status**: live FPS / bitrate / encoder in the GUI status panel; the "Log" button opens the independent log window (child-process output). The cmd console is hidden by default — re-enable it in Settings
- **ESC**: discard Settings edits and return to the session; back from the manual-IP page; stop the session on cast/receive screens and go home; quits from home

On Windows you can also use the convenience scripts in `scripts/`: `start_receiver.bat` starts the receiver in one click; `启动投屏发送.bat <receiver-IP>` direct-connects and sends.

## Receiver Keys

| Key | Action |
|-----|--------|
| ESC / Q | Disconnect and quit |
| F11 | Toggle fullscreen (extend mode) |
| Close window | Stop receiving |

## Sender Keys

| Input | Action |
|-------|--------|
| Ctrl+C | Graceful exit (virtual display detached + device disabled) |
| Mode prompt 1/2/3 | Mirror / Extend / Stitch (interactive CLI only) |
| Ctrl+Alt+Shift+Q | Stitch mode: hand the keyboard/mouse back immediately |

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

A few options exist only in `config.ini`: `extend_res` (`auto` or `WxH`, default `auto`) forces the virtual-display size; `vdd_mode_write` (`always` / `never`, default `always`) controls whether the receiver's resolution may be added to `vdd_settings.xml` (no prompt any more; an old `ask` value is treated as `always`); and `stitch_side` (`left` / `right` / `top` / `bottom`, default `right`) stores the stitch direction, rewritten whenever it is chosen in the launch dialog or switched mid-session.

> "Follow system" reads the OS light/dark preference. Windows reads the `AppsUseLightTheme` registry value and re-checks it every 2 s, so flipping the OS theme is followed immediately; macOS (`AppleInterfaceStyle`) and Linux (GNOME `color-scheme`) are sampled once at startup, so an OS theme change needs a restart of the app there. A failed probe always falls back to light.

**Live reconfigure**: while streaming, change any transfer parameter in Settings and press OK — it takes effect within seconds without dropping the connection (codec/scale switches blip for a frame or two). In classic console mode there is no stdin channel, so the session is restarted with the new parameters instead.

Transfer status is shown live in the GUI status panel (FPS / bitrate / encoder); the independent log window shows child-process output. The cmd console is hidden by default and can be re-enabled in Settings.

The connection-history list (`history.ini`, last 20 entries) sits under the receive/send cards on the Home screen — click to connect directly (cast/extend) without searching; individual entries can be deleted or cleared.
