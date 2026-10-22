# Architecture & Protocol

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/architecture.md)

This page covers Mivara's directory layout, executables, network ports, the full lifecycle of a session from discovery to teardown, and the protocol v2 constants and codec options. See [Building & Packaging](building.md) for installation and build steps, [User Guide](usage.md) for day-to-day use, and [Troubleshooting](troubleshooting.md) for common issues.

## Directory Layout

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
├── docs/                     # Chinese/English user docs (index: docs/README.md, screenshots)
├── installer/                # Inno Setup packaging script (Mivara.iss)
├── vdd/                      # Virtual display driver (signed MttVDD + install scripts)
├── winget/                   # winget-pkgs submission manifests (updated per release)
├── test/                     # Local tests / diagnostics (not committed)
└── build/                    # Object files
```

## Executables

| Program | Responsibility |
|---------|----------------|
| `app` | GUI launcher: left nav (Home/Settings), receive/send choice, connection history on Home, SSDP search, device list (cast/extend), manual IP, live reconfigure |
| `sender` | Captures the screen (GDI/X11/CoreGraphics), negotiates mode, streams; manages virtual-display activation and positioning in extend mode via the vdd module |
| `receiver` | SSDP broadcast, fullscreen rendering, hosts the two sidecar services |

## Network Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 1900 | UDP multicast | SSDP discovery (M-SEARCH / NOTIFY) |
| 8081 | TCP | Video frame stream |
| 8082 | TCP | CPU compute offload service |
| 8083 | TCP | Remote port inspection service |

All four ports must be allowed on the receiver; see [Network & Firewall](network.md) for firewall rules and network requirements.

## How It Works

A full session runs in order: discovery (or direct connect) → handshake → virtual-display activation in extend mode → frame streaming → teardown. The sections below follow that order.

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

For more on multicast, subnets and firewalls, see [Network & Firewall](network.md).

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

Implemented in [vdd.cpp](https://github.com/wzy-777/Mivara/blob/main/src/vdd.cpp) / [vdd.h](https://github.com/wzy-777/Mivara/blob/main/src/vdd.h) (shared by the sender and the GUI), built on the
Windows CCD API (`QueryDisplayConfig` / `SetDisplayConfig`),
`ChangeDisplaySettingsEx`, and SetupAPI device enable/disable (`DIF_PROPERTYCHANGE`).

> Note: a CCD detach alone is NOT enough — the PnP display device still
> shows up in Settings > Display and can still be extended to. Every
> session end therefore **disables the device** (the whole IddCx/UMDF
> stack goes down), and the next extend session re-enables it
> automatically. Matching is exact on hardware ID `Root\MttVDD`, so other
> vendors' virtual display adapters are untouched.

Driver installation and resolution configuration are covered in [Virtual Display Driver](virtual-display.md).

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

Actual bandwidth use per codec is covered in [Performance](performance.md).

### 5 — Sidecar Services

- **Compute offload (8082)**: PING / COMPRESS (RLE) / COLORCONV (BGR→RGB), executed on the receiver's CPU with real timing measurements (`clock_gettime` on Linux/macOS, `QueryPerformanceCounter` on Windows). If the service is unavailable, the sender silently falls back to local uncompressed frames.
- **Port inspector (8083)**: LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT. Port data is collected natively per platform: Linux reads `/proc/net/*` + inode→PID mapping; macOS shells out to `lsof`; Windows uses `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`.

> Security note: TCP 8083 has **no authentication whatsoever** — any LAN peer that can reach the port can list, query and even kill processes on the receiver. Keep it LAN-only.

## Protocol Reference

| Item | Value |
|------|-------|
| Handshake magic | `0x54564853` (`'TVHS'`) |
| Protocol version | `2` |
| Handshake request `HsRequestV2` | 48 bytes (12 × `uint32`) |
| Handshake response `HsResponseV2` | 36 bytes |
| Byte order | Network byte order (all `uint32`) |
| Frame packet | `[uint32 frame length][frame data]` |
| Control packet | `uint32 0xFFFFFFFF` + `uint32 ctrl_type` + body |
| Resolution change | Control `CTRL_RESIZE`, body = full `HsRequestV2` |
| Runtime reconfigure | Control `CTRL_RECONFIG`, body = full `HsRequestV2` |

| Field | Values |
|-------|--------|
| `mode` | `0`=mirror　`1`=extend |
| `codec` | `0`=raw　`1`=RLE　`2`=H.264　`3`=H.265　`4`=AV1 |
| `codec_flags` | `bit0` = sender wants hardware encode |
| `status` | `0`=OK　`1`=version　`2`=codec　`3`=dims　`4`=decoder　`5`=legacy |
| `bitrate_kbps` | `0` = quality (CRF) mode |
| `gop_frames` | `0` = default `2*fps` |

Field-level layout is in “[2 — Extend Handshake](#2--extend-handshake)” above.

## Codecs

| Codec | Notes | Default |
|-------|-------|---------|
| **H.264** | Balanced quality/latency; hardware (NVENC/QSV/AMF/VAAPI) first, falls back to libx264 | ✅ |
| **H.265** | Better quality per bitrate; hardware first, falls back to libx265 | |
| **AV1** | Best compression; hardware first, falls back to SVT-AV1 | |
| **Raw** | Lossless RGB24, highest bandwidth | |
| **RLE** | Lossless compression (can offload to the receiver's CPU) | |

Low-latency tuning: no B-frames, `zerolatency`, GOP = 2×fps, immediate IDR after resize. For desktop text, enable 4:4:4 chroma in Settings (software encoders only).

Bandwidth and latency figures are in [Performance](performance.md).
