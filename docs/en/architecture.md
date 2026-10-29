# Architecture & Protocol

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/architecture.md)

This page covers Mivara's directory layout, executables, network ports, the full lifecycle of a session from discovery to teardown, and the protocol v3 and connection-authorization constants and codec options. See [Building & Packaging](building.md) for installation and build steps, [User Guide](usage.md) for day-to-day use, and [Troubleshooting](troubleshooting.md) for common issues.

## Directory Layout

```
Mivara/
├── makefile                  # Cross-platform build (Linux/macOS/Windows MinGW)
├── src/
│   ├── app.cpp               # GUI launcher (SDL2 + SDL2_ttf): left nav + Home/Settings (history embedded in Home)
│   ├── sender.cpp            # Capture, handshake, streaming
│   ├── receiver.cpp          # Fullscreen render, SSDP broadcast, compute + port services, stitch target
│   ├── proto.h               # Protocol v3 (handshake + authorization + control packets / codec enum)
│   ├── auth.cpp/.h           # SHA-256/HMAC/PBKDF2, password record, session token, failure throttle
│   ├── kvm.cpp/.h            # Stitch (software KVM) controller and target, TCP 8084
│   ├── tray.cpp/.h           # Windows tray icon + single instance (no-ops elsewhere)
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
| `app` | GUI launcher: left nav (Home/Settings), connect-device entry, low-weight receive status bar, connection history on Home, SSDP search, device list (cast/extend/stitch), manual IP, approval dialogs, tray + single instance, live reconfigure |
| `sender` | Captures the screen (GDI/X11/CoreGraphics), negotiates mode, streams; manages virtual-display activation and positioning in extend mode via the vdd module; with `--mode stitch` it runs the stitch controller (kvm module) and never captures |
| `receiver` | SSDP broadcast, fullscreen rendering, hosts the compute-offload, port-inspector and stitch-input sidecar services |

## Network Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 1900 | UDP multicast | SSDP discovery (M-SEARCH / NOTIFY) |
| 8081 | TCP | Video frame stream |
| 8082 | TCP | CPU compute offload service (session token required) |
| 8083 | TCP | Remote port inspection service (session token required) |
| 8084 | TCP | Stitch (software KVM) keyboard/mouse input channel (same receive policy as 8081) |
| 8085 | TCP | Stitch file drop (optional, off by default; guarded by a one-shot token from the active stitch session; copy-only) |

All of these ports must be allowed on the receiver; see [Network & Firewall](network.md) for firewall rules and network requirements. 8082/8083 are no longer unauthenticated: they only accept the 32-byte token issued by the current authorized 8081 session, and the token is cleared when the session ends.

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

The sender then connects over TCP 8081 and exchanges the real handshake (protocol v3, all fields network byte order):

```
Sender → Receiver   HsRequestV2 (48 bytes, 12 x uint32)
  magic          0x54564853 ('TVHS')
  version        3
  width, height  capture size (downscaling happens inside the encoder)
  fps            target frame rate
  mode           0=mirror  1=extend
  codec          0=raw 1=RLE 2=H.264 3=H.265 4=AV1
  codec_flags    bit0 = sender wants hardware encode
  bitrate_kbps   0 = quality (CRF) mode
  quality        0..51 (CRF-like; 0xFFFFFFFF = encoder-default sentinel, reserved/unused)
  gop_frames     IDR interval (0 = default 2*fps)
  reserved       0

Receiver → Sender   HsAuthStart (64 bytes; sent for every policy)
  magic          0x54564841 ('TVHA')  version 2
  mode           0=any 1=password 2=manual approval
  kdf_iter       PBKDF2 iterations (password mode only)
  salt[16]       password KDF salt (password mode only)
  nonce[32]      fresh for every connection

Sender → Receiver   HsRequestExtV3 (48 bytes)
  magic          0x54564858 ('TVHX')  version 2
  device[32]     sender hostname (shown in the approval dialog)
  flags, reserved

Sender → Receiver   HsAuthProof (40 bytes; password mode only)
  magic 0x54564841  version 2  proof[32]

Receiver → Sender   HsResponseV2 (36 bytes)
  magic, version, recv_width, recv_height
  status         0=OK 1=version 2=codec 3=dims 4=decoder 5=legacy
                 6=busy 7=bad password/throttled 8=approval timeout 9=denied
                 10=receiver authorization record corrupt
  codec          accepted codec
  codec_flags, reserved[2]

Receiver → Sender   HsResponseExtV3 (44 bytes; status=OK only)
  magic 0x54564852 ('TVHR')  version 2  token[32]  flags
```

Authorization is enforced **on the receiver**: only a local PBKDF2 record exists there, and the proof is purpose-separated:

```
purpose_key = HMAC-SHA256(PBKDF2(fold(password), salt, iter), "Mivara/v1/auth-key" ‖ purpose)
proof       = HMAC-SHA256(purpose_key, "Mivara/v1/auth-proof" ‖ purpose
                          ‖ nonce ‖ HsRequestV2 ‖ HsRequestExtV3)
```

`purpose=1` for 8081 and `2` for 8084, so a recorded proof is not interchangeable between the two. `fold` is ASCII-only and locale-independent: `A`–`Z` become `a`–`z`, every other byte is unchanged, so passwords are case-insensitive. Record creation, the 8081 sender and the 8084 stitch controller must all fold before PBKDF2; the receiver only holds the record and never folds it. New passwords use 300,000 PBKDF2 iterations (existing records keep their count). The password itself never crosses the wire. `token` is a random 32-byte per-session token that 8082/8083 must re-check **before every request**. Manual approval is a receiver-side dialog (default deny, 30 s timeout, one prompt at a time; another device is answered "busy"). 8081 and 8084 share one active-session slot: while either has a session, the other answers "busy". Every policy still receives `HsAuthStart` (salt/iter are empty in any/manual mode); a missing or corrupt password record answers `status=10` and is never downgraded to open.

> Purpose separation and the higher iteration count reduce cross-port replay and raw-record reuse, but they do **not** remove offline guessing or the credential-equivalence of a leaked `config.ini` — see [SECURITY.md](../../SECURITY.md).

Both ends must run the same version. A v2 sender (the old 48/36-byte handshake) receives `status=1` **before** the authorization exchange, so an old client cannot bypass the policy; a legacy peer (old 16-byte handshake) is detected and reported explicitly — no silent black screen.

### 3 — Virtual Display Lifecycle (Extend Mode)

```
Extend mode selected
  → vddStartup():
      save the current non-MTT paths, sources, clone groups and modes
      → enable the Root\MttVDD device (if disabled; SetupAPI, needs admin)
      → the driver reads vdd_settings.xml only at start-up; if the target
        size is missing the exact mode is appended by default
        (`vdd_mode_write=never` opts out; backup to .mivara.bak, read-back
        verified) and the device is restarted once when needed
      → remove automatic MTT clone paths and restore the saved layout
      → select an unused source from actual CCD paths with its own clone group
      → positioned right of the existing desktop, at the receiver's
        resolution when available, otherwise the closest aspect ratio
      → read back the independent topology, actual GDI name and original
        layout before locking the capture source
  → handshake, stream (windows/mouse can cross into the virtual screen)
  → session end / Ctrl+C / peer disconnect / console window closed
  → vddShutdown(): detach only MTT paths + DISABLE the Root\MttVDD device
    + restore the original layout → the virtual screen vanishes from
    Settings > Display entirely (not merely detached from the desktop)
```

Implemented in [vdd.cpp](https://github.com/wzy-777/Mivara/blob/main/src/vdd.cpp) / [vdd.h](https://github.com/wzy-777/Mivara/blob/main/src/vdd.h) (shared by the sender and the GUI), built on the
Windows CCD API (`QueryDisplayConfig` / `SetDisplayConfig`),
`ChangeDisplaySettingsEx`, and SetupAPI device enable/disable (`DIF_PROPERTYCHANGE`).
Enabling the driver can automatically clone an MTT target onto a physical display source; that is not an independent extension. The code allocates an unused source and a separate clone group from actual CCD paths, never applies a global `SDC_TOPOLOGY_EXTEND`, and never accepts a physical-source/MTT clone as the capture target. It reads back the result after sizing and positioning. Normal shutdown preserves the non-MTT layout at that time, while failed startup restores the pre-start baseline; these temporary changes are not saved to the display configuration database.

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
| GUI "Stop streaming" | `STOP|mivara` over stdin for graceful stop (waits up to 8 s), or Ctrl+Break for a visible console without a pipe; the GUI calls `vddForceDetach()` if the sender had to be hard-killed |

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

> Security note: 8082 and 8083 require the 32-byte token of the current authorized 8081 session. It is presented on connect and **re-checked before every request** (including before 8083's port listing and kill). The gate is cleared when the session ends, so an already-open sidecar connection loses access on its next request and a new connection with the old token is refused. They are still **unencrypted**, so a device sniffing the link can still read query results or kill requests. Keep them LAN-only.

### 6 — Stitch Channel (software KVM, 8084)

Stitch is **not a display mode**: both ends keep their own physical monitor and desktop, and only keyboard/mouse events travel over TCP 8084, so capture, encoding, the virtual display and the 8081 video protocol are not involved (`HsRequestV2.mode` still only accepts 0/1; a stitch value never appears on 8081). The implementation lives in `src/kvm.cpp` / `src/kvm.h`.

**Handshake (all network-byte-order `uint32`)**

```
controller → target   KvmHello (16 bytes: magic 'MVK1', version 2, reserved; reserved[0] = peer side)

then, depending on the receiver's Receiving & Security policy:

  open     target → controller   KvmReply (32 bytes: magic, version, status, target virtual-desktop x/y/w/h, reserved)

  password / manual
           target → controller   HsAuthStart (64 bytes, shared with 8081)
           controller → target   HsRequestExtV3 (48 bytes, sender hostname)
           controller → target   HsAuthProof (40 bytes, password mode only)
           target → controller   HsResponseV2 (36 bytes, status below)
           target → controller   KvmReply (32 bytes, HS_OK only, followed by events)

controller → target   KvmEvent (16 bytes: type, a, b, c) × N
```

`KvmReply` status: `0`=OK, `1`=version mismatch, `2`=declined, `3`=busy. `HsResponseV2` reuses the 8081 codes (`0`=OK, `7`=bad password/throttled, `8`=approval timeout, `9`=denied, `10`=peer's password record is corrupt). Event types: `1`=move (target virtual-desktop px), `2`=button (1/2/3/4/5, b=down), `3`=wheel (a=signed delta, b=horizontal), `4`=key (a=down, b=VK, c=scan | extended<<16), `5`=release-all.

The proof binds this connection with the stitch purpose (`purpose=2`): `HMAC-SHA256(purpose_key, "Mivara/v1/auth-proof" ‖ 2 ‖ nonce ‖ KvmHello ‖ HsRequestExtV3)`, and the nonce is fresh every time, so a recorded answer cannot be replayed. 8081 and 8084 share one active-session slot, so while either has a session the other is answered `HS_ERR_BUSY` before authorization. Manual approval is serialized across both ports, and only an explicit refusal arms the 60 s cooldown — a timeout, disconnect or prompt failure releases the slot immediately. Hello / identity / proof are bounded to 3 / 10 / 125 s (140 s total, matching the controller's 120 s password prompt), so an unfinished handshake cannot pin the port. The controller reads the first four bytes to distinguish a direct reply (`MVK1`), an authorization challenge (`TVHA`) and a pre-challenge refusal (`TVHS`). A version-1 controller receives `KVM_ERR_VERSION` and cannot bypass the policy. `KvmHello.reserved[0]` carries **where the target sits from the controller's point of view** (1=left, 2=right, 3=top, 4=bottom; 0 or any other value means unspecified and falls back to right) purely so the approval dialog can name the entry edge. The side itself is controller-only state — the target just injects the absolute coordinates it receives — so switching mid-session needs no protocol message at all.

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
- After a valid `KvmHello` it **enforces the receive policy** (there is no separate stitch-only confirmation): `open` proceeds; `password` verifies the HMAC proof (with the per-source failure throttle); `manual` reuses the 8081 approval path — in GUI mode the app shows the dialog (device name, IP and "Request: stitch control", default deny, 30 s timeout, duplicate requests merged), and a standalone `bin/receiver` falls back to a topmost native Yes/No dialog (default No, auto-declined after 30 s, closed on shutdown, `select`ing the connection while waiting so a vanished controller frees the slot at once). Only after approval does it return the target virtual-desktop metrics and start injecting; an explicit refusal suppresses prompts from that source for 60 s. A missing or corrupt password record answers `HS_ERR_AUTH_CONFIG` (status 10) and is never downgraded to open. A version-1 controller is refused `KVM_ERR_VERSION`.
- Events are injected with `SendInput`: absolute mouse coordinates + `MOUSEEVENTF_VIRTUALDESK` (normalised 0..65535), keyboard prefers scan codes (`KEYEVENTF_SCANCODE`, plus the extended flag when needed). On session end, disconnect or a clean shutdown every key/button recorded as held is released (a force-terminated process cannot run this — a known limit of "Stop" in receive mode under the GUI's default hidden-console launch).
- Injection is serial on the session thread. `SendInput` cannot cross UIPI into **higher**-integrity windows and cannot target the secure desktop (so Ctrl+Alt+Del cannot be synthesised). A standalone `receiver` is unelevated, so elevated windows receive no stitch input; a receiver launched by the GUI inherits the GUI's administrator token and can type into them.

> Security note: 8084 uses the same authorization policy as 8081 (any / password / manual), but the channel is **unencrypted** — authorization decides who may connect and does not protect the input events. Keep it LAN-only.

### 7 — Stitch File Drop (8085, optional)

File drop is a companion channel of a stitch session — **not a display mode and not a new session**: it does not claim the 8081/8084 active-session slot, adds no second approval prompt and is off by default on both ends. When the controller enables `--file-drop 1` and the Medium-integrity `stitchcap.exe` helper is ready, it sets `KVM_FEATURE_FILE_DROP` in `KvmHello.reserved[1]`; the target echoes it only when `file_drop_accept=1`, the 8085 listener is up and authorization succeeded, then sends one marker plus eight fixed `KvmEvent` frames carrying a 32-byte one-shot token after `KvmReply`. The controller reads this fixed sequence before starting `drainLoop()`, so the 16-byte event stream never desynchronises. 8085 is a separate TCP service: `DropHello` carries the token, transfer id, release point and item/byte limits; manifest, chunks, per-file SHA-256 and a result frame follow. The target stages under `%LOCALAPPDATA%\Mivara\Drop\<transfer>`, verifies every file, then copies the top-level items into the Explorer/Desktop folder under the release point; copy-only, never overwrite, move or delete the source. Source-side edge capture is done by `stitchcap.exe`: an asInvoker process launched with the shell token (Medium integrity) that owns a 3-pixel topmost strip on the shared edge, reads `CF_HDROP` in `DragEnter`, reports the paths over a named pipe and returns `DROPEFFECT_NONE`, so nothing is copied locally. Medium is mandatory: a High window cannot receive a Medium Explorer's OLE drop (UIPI).

## Protocol Reference

| Item | Value |
|------|-------|
| Handshake magic | `0x54564853` (`'TVHS'`) |
| Protocol version | `3` |
| Handshake request `HsRequestV2` | 48 bytes (12 × `uint32`) |
| Authorization challenge `HsAuthStart` | 64 bytes (mode, KDF params, salt, nonce) |
| Identity extension `HsRequestExtV3` | 48 bytes (sender hostname) |
| Password proof `HsAuthProof` | 40 bytes (HMAC-SHA256) |
| Handshake response `HsResponseV2` | 36 bytes |
| Session token `HsResponseExtV3` | 44 bytes (32-byte token, OK only) |
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
| `status` | `0`=OK　`1`=version　`2`=codec　`3`=dims　`4`=decoder　`5`=legacy　`6`=busy　`7`=password/throttled　`8`=approval timeout　`9`=denied　`10`=auth record corrupt |
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
