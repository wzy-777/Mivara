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
| `app` | GUI launcher: left nav (Home/Settings), receive/send choice, connection history on Home, SSDP search, device list (cast/extend/stitch), manual IP, live reconfigure |
| `sender` | Captures the screen (GDI/X11/CoreGraphics), negotiates mode, streams; manages virtual-display activation and positioning in extend mode via the vdd module; with `--mode stitch` it runs the stitch controller (kvm module) and never captures |
| `receiver` | SSDP broadcast, fullscreen rendering, hosts the compute-offload, port-inspector and stitch-input sidecar services |

## Network Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 1900 | UDP multicast | SSDP discovery (M-SEARCH / NOTIFY) |
| 8081 | TCP | Video frame stream |
| 8082 | TCP | CPU compute offload service |
| 8083 | TCP | Remote port inspection service |
| 8084 | TCP | Stitch (software KVM) keyboard/mouse input channel |

All of these ports must be allowed on the receiver; see [Network & Firewall](network.md) for firewall rules and network requirements.

## How It Works

A full session runs in order: discovery (or direct connect) → handshake → virtual-display activation in extend mode → frame streaming → teardown. The sections below follow that order.

### 1 — Discovery (SSDP)

```
Receiver  →  joins 239.255.255.250:1900, periodically sends NOTIFY, listens for M-SEARCH
Sender    →  sends M-SEARCH per NIC and listens concurrently; the GUI shows the first reply immediately, then adds later devices
```

- All NIC sockets wait non-blockingly, with three retransmits 100 ms apart. Five seconds is the total scan budget, not a per-NIC/per-packet timeout, and cancellation interrupts the wait.
- Each interface sends multicast, 255.255.255.255 and its own directed broadcast. Replies are deduplicated by **datagram source address + port** before any device is reported; repeated replies no longer cause serial TCP probes, and the actual connection validates reachability.
- The receiver's LOCATION uses the NIC IP on the **same subnet** as the requester.

If network policy blocks even broadcast, use direct connect:

```
bin/sender 192.168.1.105        # skip discovery, connect straight to 8081
bin/sender 192.168.1.105 8081   # or specify the port
bin/sender 192.168.1.105 --mode extend    # non-interactive: set mirror / extend (used by the GUI)
bin/sender 192.168.1.105 --mode stitch    # stitch: only the 8084 input channel, no video (used by the GUI)
```

For more on multicast, subnets and firewalls, see [Network & Firewall](network.md).

### 2 — Extend Handshake

After the receiver and display mode are chosen, **extend mode** first sends an `HsRequestV2` probe with `width = height = 0`: the receiver answers with its current display resolution in `HsResponseV2.recv_width / recv_height` and `status = 3` (bad dimensions), then closes the connection before creating a SDL window or opening a decoder — so nothing flashes on screen. The sender uses this to size the virtual display before starting it. This is a valid request under the existing protocol; the protocol version does not change.

The sender then connects over TCP 8081 and exchanges the real handshake (protocol v2, all fields network byte order):

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
      → the driver reads vdd_settings.xml only at start-up; if the target
        size is missing the exact mode is appended by default
        (`vdd_mode_write=never` opts out; backup to .mivara.bak, read-back
        verified) and the device is restarted once when needed
      → CCD SetDisplayConfig activates the virtual display (takes it over
        if left over from a previous session)
      → positioned right of the existing desktop, at the receiver's
        resolution when available, otherwise the closest aspect ratio
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
          →  aspect-fit the capture size into the window, centred with black bars (never stretched)
          →  SDL_UpdateTexture/YUVTexture → RenderCopy → RenderPresent
```

Control packets: `0xFFFFFFFF` + `ctrl_type` + body. `CTRL_RESIZE` (extend-mode resolution change) and `CTRL_RECONFIG` (runtime parameter change) both carry a full `HsRequestV2`; the receiver re-inits its decoder and skips to the next keyframe.

Actual bandwidth use per codec is covered in [Performance](performance.md).

### 5 — Sidecar Services

- **Compute offload (8082)**: PING / COMPRESS (RLE) / COLORCONV (BGR→RGB), executed on the receiver's CPU with real timing measurements (`clock_gettime` on Linux/macOS, `QueryPerformanceCounter` on Windows). If the service is unavailable, the sender silently falls back to local uncompressed frames.
- **Port inspector (8083)**: LIST_TCP / LIST_UDP / LIST_ALL / GET_PORT / KILL_PORT. Port data is collected natively per platform: Linux reads `/proc/net/*` + inode→PID mapping; macOS shells out to `lsof`; Windows uses `GetExtendedTcpTable`/`GetExtendedUdpTable` + `CreateToolhelp32Snapshot`.

> Security note: TCP 8083 has **no authentication whatsoever** — any LAN peer that can reach the port can list, query and even kill processes on the receiver. Keep it LAN-only.

### 6 — Stitch Channel (software KVM, 8084)

Stitch is **not a display mode**: both ends keep their own physical monitor and desktop, and only keyboard/mouse events travel over TCP 8084, so capture, encoding, the virtual display and the 8081 video protocol are not involved (`HsRequestV2.mode` still only accepts 0/1; a stitch value never appears on 8081). The implementation lives in `src/kvm.cpp` / `src/kvm.h`.

**Handshake (all network-byte-order `uint32`)**

```
controller → target   KvmHello (16 bytes: magic 'MVK1', version, reserved; reserved[0] = peer side)
target → controller   KvmReply (32 bytes: magic, version, status, target virtual-desktop x/y/w/h, reserved)
controller → target   KvmEvent (16 bytes: type, a, b, c) × N
```

Status values: `0`=OK, `1`=version mismatch, `2`=declined, `3`=busy. Event types: `1`=move (target virtual-desktop px), `2`=button (1/2/3/4/5, b=down), `3`=wheel (a=signed delta, b=horizontal), `4`=key (a=down, b=VK, c=scan | extended<<16), `5`=release-all.

`KvmHello.reserved[0]` carries **where the target sits from the controller's point of view** (1=left, 2=right, 3=top, 4=bottom; 0 or any other value means unspecified and falls back to right) purely so the target's consent dialog can name the entry edge; an older target ignores it and behaves as the default right side, with no protocol version bump. The side itself is controller-only state — the target just injects the absolute coordinates it receives — so switching mid-session needs no protocol message at all.

**Controller side (sender --mode stitch)**

- `WH_MOUSE_LL` / `WH_KEYBOARD_LL` capture local input. The hook callbacks **never do network I/O** (overrunning the system's `LowLevelHooksTimeout` silently removes the hook); they only append to a lock-free SPSC ring drained by a dedicated thread.
- Once Windows clamps the cursor at the virtual-desktop edge, the hook's coordinates stop changing; `RIDEV_INPUTSINK` Raw Input deltas are used instead to detect "still pushing toward the chosen side" and to drive the remote cursor. Three pixels of overshoot enter the remote state and local input is swallowed; motion in any other direction resets the accumulator.
- While remote, moving the peer's cursor back out of **the edge it entered from**, or `Ctrl+Alt+Shift+Q`, hands control back. Enter/leave re-syncs physically held modifiers and mouse buttons so the peer never holds a stuck key. The four layouts: right → leave the local right edge, enter/leave at the peer's left edge; left → local left edge, the peer's right edge; top → local top edge, the peer's bottom edge; bottom → local bottom edge, the peer's top edge.
- The side is set by `--stitch-side left|right|top|bottom` (default `right`); the GUI asks before launching and stores the answer as `stitch_side` in `config.ini`, which preselects the next dialog. A live switch travels over the GUI→sender stdin channel as `STITCH|side=…`: the stdin thread calls `kvmSetControllerSide()`, which queues the request, and the message loop applies it on its 50 ms timer — handing control back and releasing held keys first if the peer currently has it.
- Connection/handshake failure, refusal and disconnect each emit their own `STAT|event=error`; state changes emit `STAT|event=stitch|state=local|remote|waiting|…|side=…`, which the GUI status panel renders and turns into the four-way switch.
- Non-Windows platforms return an error immediately (stitch is Windows-only).

**Target side (receiver's 8084 service)**

- A dedicated thread listens on 8084, independent of the video listener; it polls the running flag while idle and during a session, and is joined on every exit path.
- The session runs on its own thread so the listener stays responsive: while one is active a second controller receives `KVM_ERR_BUSY` immediately (its hello is read first — closing a socket with unread data turns the close into an RST). The controller drain thread also watches for EOF/errors, so an idle session cannot miss a peer exit indefinitely. Both ends negotiate `KVM_FEATURE_HEARTBEAT` through `KvmHello.reserved[1]` / `KvmReply.reserved`; when supported, bidirectional PING/PONG runs every 3 seconds and roughly 9 seconds without input ends the session and releases held keys. Older peers do not negotiate it and retain TCP keepalive (15 s idle, 3 s probes). Local stop uses `shutdown` to wake the sender; the target releases input on EOF, with no second blocking send.
- After a valid `KvmHello` it shows a **topmost Yes/No dialog** (default No, auto-declined after 30 s, closed automatically on shutdown); its text names the target edge the controller will enter through, taken from `reserved[0]`. Only on Yes does it return the target virtual-desktop metrics and start injecting; after an explicit "No" the same source is rejected outright for 60 s to prevent prompt flooding (leaving the box unanswered for 30 s is a timeout, not a refusal, and arms no cooldown). While the box is up the watcher also `select`s the connection: if the controller has already stopped or disconnected, the dialog is closed and the session slot freed immediately instead of after the timeout. A non-blocking FIN check runs before the box is created, and a peer that cancels during the prompt is not treated as a refusal but delays the next prompt by 3 s (recorded per source in a bounded 8-entry table **and** in one global tick, so a rotating source address bypasses only the per-source half), with at least 1.5 s between any two dialogs regardless of source — so connect-then-close cannot flicker the topmost dialog. Only an actual "No" arms the cooldown: `MessageBoxW` returning 0 (no interactive window station, out of resources) is a failed prompt — the request is still refused, but no cooldown is armed. The backoff wait keeps polling the socket, so a peer that has already disconnected releases the session slot at once.
- Events are injected with `SendInput`: absolute mouse coordinates + `MOUSEEVENTF_VIRTUALDESK` (normalised 0..65535), keyboard prefers scan codes (`KEYEVENTF_SCANCODE`, plus the extended flag when needed). On session end, disconnect or a clean shutdown every key/button recorded as held is released (a force-terminated process cannot run this — a known limit of "Stop" in receive mode under the GUI's default hidden-console launch).
- Injection is serial on the session thread. `SendInput` cannot cross UIPI into **higher**-integrity windows and cannot target the secure desktop (so Ctrl+Alt+Del cannot be synthesised). A standalone `receiver` is unelevated, so elevated windows receive no stitch input; a receiver launched by the GUI inherits the GUI's administrator token and can type into them.

> Security note: the 8084 channel is **unencrypted** and does not authenticate the peer — security rests entirely on the target's per-session human confirmation. As with 8083, keep it LAN-only.

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
| `mode` | `0`=mirror　`1`=extend (stitch does not use this protocol; see the [8084 channel](#6--stitch-channel-software-kvm-8084)) |
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
