# AGENTS.md

TwinView (幻屏): C++17 LAN screen-mirroring / wireless second monitor.
Cross-platform (Windows / Linux X11 / macOS). Single package, no monorepo — everything is in `src/` and one `makefile`.

Full user docs: `README.md` (Chinese) + `README_EN.md` (English twin). This file only covers what an agent would otherwise get wrong.

**Rule: keep `README.md` and `README_EN.md` in sync** — any edit to one must be mirrored in the other. Terminology: say 「扩展 / Extend」 for the real extended desktop (virtual-display mode); no "真·扩展屏 / True extend" qualifiers.

## Build

```bash
make                  # all three: bin/sender, bin/receiver, bin/app
make sender           # one binary (also: receiver, app)
make debug            # -g -O0 -DDEBUG — note: runs clean first, full rebuild
make check            # env/source self-check (the only "verification" step)
make install-deps     # apt/dnf/pacman/brew + MSYS2
make OPT_FLAGS="-O3 -march=x86-64 -flto"   # portable build for releases/installer
```

- Default release flags: `-O3 -march=native -flto` — host-specific, do not redistribute. **Release/installer builds must override OPT_FLAGS** with `-march=x86-64` (see above).
- Binaries + runtime DLL closure output to `bin/`; objects go to `build/`.
- `gpu_accelerate.c` is C (gcc); all other sources are C++17 (g++). Keep it valid C.
- Makefile header deps are listed manually (see `Header Change Tracking Maps` block). Adding a new `.h` without adding it there causes stale builds.
- SDL flags are added only to `app`/`receiver` objects (`OBJ_APP OBJ_RECEIVER`). Never add them to `sender` — pkg-config sdl2 cflags contain `-Dmain=SDL_main`, which breaks sender's link since it has no SDL2main.
- No CMake, no test suite, no lint/format/typecheck config, no CI workflows. Do not invent test commands.

### Toolchain on this machine

- `make`/`g++` are not in the Windows PATH — run builds via `C:\msys64\usr\bin\bash.exe -lc "export PATH=/mingw64/bin:/usr/bin:$PATH OS=Windows_NT && cd /d/projects/TwinView && make ..."`. The makefile's whole Windows branch is keyed on `$(OS)`; in sandboxed shells where `OS` is unset, links silently lose all system libs (`-lws2_32`, `-lgdi32`, `-lsetupapi`, …) and there is no `.exe` suffix.
- Git is `D:\Program Files\Git\cmd\git.exe` (not in PATH). GitHub 443 direct often times out while curl works; push with `-c http.proxy=http://127.0.0.1:7890` (FlClash).
- Windows PowerShell here is 5.1: **no `&&`** in commands. Avoid `$_` in double-quoted one-liners (bash eats it) — write a `.ps1` file instead.
- Interactive UAC elevation (`Start-Process -Verb RunAs`) is silently blocked in this environment — can't automate elevated tests; ask the user to double-click the exe.

## Installer (Inno Setup)

- `installer/TwinView.iss` packages `bin/` + `scripts/` + `vdd/` drivers (excludes the >100 MB vendor GUI) into `dist/TwinView-Setup-<ver>.exe`.
- GitHub Release assets must include both `TwinView-Setup-<ver>.exe` and a **fixed-name copy `TwinView-Setup.exe`** — `https://github.com/wzy-777/TwinView/releases/latest/download/TwinView-Setup.exe` is the permanent latest-installer link (the README download badges point at it).
- Build: `"D:\Program Files\Inno Setup 6\ISCC.exe" installer\TwinView.iss` (ISCC is at that custom path on this machine, not Program Files).
- Wizard language: this Inno install lacks `ChineseSimplified.isl`, so the installer UI is English; a comment in the .iss explains how to enable Chinese. Bump `AppVersion` in the .iss when releasing (also the version badge in both READMEs and the banners in `sender.cpp` and `receiver.cpp`).
- The post-install "launch" checkbox must keep the `runascurrentuser` flag: `postinstall` entries default to `runasoriginaluser` (unelevated) and `app.exe` is `requireAdministrator`, so without it CreateProcess fails with ERROR_ELEVATION_REQUIRED. `[UninstallRun]` removes the `TwinView` Task Scheduler job registered by app auto-start (task name must match `applyAutoStart` in app.cpp).

## Windows specifics (easy to get wrong)

- `sender` links as a **console** subsystem app (logs + interactive prompts + Ctrl+C). `receiver` and `app` link with `-mwindows` (GUI, stdout discarded).
- SDL2_ttf needs a CJK font at runtime; `app.cpp` walks an ~11-path candidate list (`msyh.ttc`/`simhei.ttf`/NotoSansCJK/wqy/…) and exits with an error if none is found. Caveat: the list also contains non-CJK `DejaVuSans.ttf`, so on a machine with no CJK font the probe can "succeed" via DejaVu and render tofu instead of erroring.
- The runtime DLL closure must sit in `bin/` next to the exes (already there). After adding a library, verify with `ldd bin/<exe>.exe` and copy missing DLLs into `bin/`.
- Extend mode needs the MttVDD indirect-display driver installed once (`vdd/install_vdd.ps1`). The virtual display lifecycle lives in `src/vdd.cpp`/`vdd.h` (`vddStartup`/`vddShutdown`/`vddForceDetach`, CCD API + SetupAPI device enable/disable) and is linked into both `sender` and `app`. **Never break the Ctrl+C graceful path** — a hard exit leaves the virtual display attached to the user's desktop. Covered exit paths: normal end, send failure, Ctrl+C, console-window close (detaches inside the `CTRL_CLOSE_EVENT` handler), `atexit`. The GUI stops the sender by sending Ctrl+C (`GenerateConsoleCtrlEvent`), hard-kills only after an 8 s timeout, and calls `vddForceDetach()` as a safety net after a hard kill.
- A CCD detach alone leaves the monitor visible in Settings > Display — teardown MUST also disable the PnP device (hardware ID `Root\MttVDD`, instance `ROOT\DISPLAY\000x`; match on hardware IDs, never touch other vendors' virtual display adapters). Enabling/disabling requires elevation: `sender` and `app` embed a `requireAdministrator` manifest via `res/admin.rc` + `res/admin.manifest` (windres, see makefile `RES_ADMIN`). `receiver` is not elevated. Disable failure degrades gracefully (warn + CCD-only). A disabled device has no CCD path — `vddStartup` must enable the device **before** the `vddInstalled()` gate.
- `scripts/*.bat` are convenience wrappers that launch `bin/` exes; `启动投屏发送.bat` takes the receiver IP as an argument.

## Architecture (actual wiring)

| File | Role |
|------|------|
| `src/sender.cpp` | capture (GDI/X11/CoreGraphics), handshake, streaming, runtime reconfigure (SETP). |
| `src/vdd.cpp/.h` | Windows virtual display (MttVDD) lifecycle: enable/disable PnP device (SetupAPI) + attach/detach/position via CCD API. Linked into sender + app. |
| `src/receiver.cpp` | SSDP broadcast, SDL2 fullscreen render, hosts the two sidecar services |
| `src/discover.cpp/.h` | SSDP discovery engine (UDP 1900); parses SSDP `NAME:` into `DiscoveredDevice.name` |
| `src/gpu_accelerate.c/.h` | remote CPU offload protocol (RLE compress / color convert) served on 8082 — only used by the RLE codec path |
| `src/ports.cpp/.h` | remote port inspector on 8083 (LIST/GET/KILL) |
| `src/proto.h` | protocol v2 shared by sender/receiver: HsRequestV2/HsResponseV2, CodecId, control packets |
| `src/codec.h/.cpp` | FFmpeg Encoder/Decoder (H.264/H.265/AV1). Linked into sender + receiver only (app is FFmpeg-free) |
| `src/config.h/.cpp` | GUI settings (%LOCALAPPDATA%\\TwinView\\config.ini), key=value (incl. `auto_start` → Task Scheduler `TwinView` job, `auto_receive`) |
| `src/history.h/.cpp` | connection history (history.ini, cap 20, newest first) |
| `src/app.cpp` | SDL2 + SDL2_ttf GUI launcher: left nav (主页/设置) + session flows and connection history inside 主页 (history rows under the 接收/发送 cards; no separate history tab); SSDP search, device list (投屏/扩展), manual IP, settings; spawns sender/receiver as child processes with hidden console + stdout pipe (STAT| status panel + log window) and a **stdin pipe carrying `SETP|` reconfigure lines** |

- `ports.cpp` has a hidden `main()` behind `-DPORTS_MAIN` — can be compiled standalone as a port-inspection CLI. The makefile never sets this flag; don't be confused by two `main` functions in the tree.
- `sender` CLI: `sender [ip] [port] [--mode mirror|extend] [--codec auto|h264|h265|av1|raw|rle] [--hw 0|1] [--bitrate <kbps>] [--quality 0-51] [--fps <n>] [--scale <pct>] [--chroma 420|444]`. `--mode` skips all interactive prompts — the GUI depends on it. Keep it working non-interactively. After streaming starts, a reader thread consumes stdin lines (`SETP|k=v|…` / `PING`) — stdin may be a GUI pipe, so never treat it as an interactive prompt channel once the loop begins.
- File-size orientation: `app.cpp` (~2000 lines) is now the largest source, then `ports.cpp` (~1600) and `sender.cpp` (~1585).
- `receiver`'s 8082/8083 sidecar services are unconditional `for(;;) accept` loops that never check `g_running` — once `g_running` clears (window quit / SIGINT), `main` blocks joining them and never reaches final cleanup (stats flush / shutdown summary skipped; standalone runs linger). The GUI's graceful stop does not apply to it in the default hidden-console launch: `receiver` is a GUI-subsystem exe with no console, so `stopChild`'s `AttachConsole` fails and it is TerminateProcess'd immediately — the 8 s Ctrl+C-then-kill path is for the console-subsystem `sender` (POSIX GUI: SIGINT → 3 s → SIGKILL). Any shutdown refactor must handle these loops.
- The 8083 port inspector has **no authentication** — any LAN peer that can reach the port can LIST/GET/KILL receiver-side processes. Keep it LAN-only.

## Protocol constants (defined in `src/proto.h`)

- Handshake over TCP 8081 (**protocol v2**, all u32 network byte order, magic `0x54564853` 'TVHS'):
  - Request (sender→receiver) `HsRequestV2` **48 bytes**: magic, version=2, width, height, fps, mode, codec, codec_flags, bitrate_kbps, quality, gop_frames, reserved.
  - Response (receiver→sender) `HsResponseV2` **36 bytes**: magic, version, recv_width, recv_height, status, codec, codec_flags, reserved[2].
  - `mode`: 0 = mirror, 1 = extend. `codec`: 0=raw 1=rle 2=h264 3=h265 4=av1. `status`: 0=ok 1=version 2=codec 3=dims 4=decoder 5=legacy.
  - Receiver peeks 4 bytes first: non-TVHS magic ⇒ legacy peer, explicit error (no black screen). Old 16-byte handshake is rejected.
- Stream packets: `[u32 pkt_size][payload]`; control = `u32 0xFFFFFFFF` + `u32 ctrl_type` + body. `CTRL_RESIZE=1` body = HsRequestV2; `CTRL_FORCE_IDR=2` / `CTRL_EOS=3` body empty; `CTRL_RECONFIG=4` body = HsRequestV2 (runtime param change — receiver re-opens the decoder and sets `skip_until_key`, see the shared resize/reconfig handler in receiver.cpp).
- Coded payload = one access unit per packet (annex-B / OBU). Raw/RLE unchanged. 8082 remote RLE is connected **only** while `codec=rle` (connect/disconnect follows runtime codec switches).
- STAT| machine-readable status lines on stdout (parsed by the GUI): `STAT|k=v|k=v|...` with keys t/frames/fps/kbps/codec/hw/mode/src/dst/quality or `event=start|stop|error|applied|pong` + `msg=`. The parser replaces its state per line, but not every line is a full snapshot: only periodic lines and `event=applied` carry the full key set — `start`/`stop`/`error`/`pong` carry subsets (sender `error` = `msg=` only; the receiver emits its own `start`/`stop`/`error`/`resize`/`reconfig` variants). The GUI special-cases event lines; check `parseStatLine` in app.cpp before changing emitter key sets.
- GUI→sender stdin line protocol (runtime reconfigure): `SETP|codec=…|hw=…|bitrate=…|quality=…|fps=…|scale=…|chroma=…` (any subset; keys map to the CLI flags; numeric values must be whole decimal numbers — parse with a strict whole-token check, never `atoi`; all-or-nothing validation, reply `STAT|event=applied|…` or `STAT|event=error|msg=bad_value_*`). `PING` → `STAT|event=pong`.
- Ports: 1900/udp SSDP, 8081/tcp video, 8082/tcp compute offload (`GPU_ACCEL_PORT`), 8083/tcp port inspector (`PORTS_SERVICE_PORT`).
- SSDP discovery: sender probes **per local NIC** over 3 rounds (~100 ms apart), each sending multicast + 255.255.255.255 + a directed per-NIC subnet broadcast (enterprise nets often drop multicast). Replies are keyed on the **datagram source address**, with the LOCATION IP probed only as a fallback (multi-homed receivers may advertise the wrong NIC). Receiver's LOCATION uses `getLocalIPForPeer(requester)` (same-subnet NIC). Receiver advertises `NAME: <hostname>`.
- Windows `SO_RCVTIMEO`/`SO_SNDTIMEO` take DWORD milliseconds, not `struct timeval` (POSIX). Passing a timeval silently gives ~5 ms timeouts.
- Sender and receiver must be the **same binary version** (protocol v2). Mismatched builds fail the handshake with an explicit error — never silently black-screen.

## Repo hygiene (`.gitignore` is aggressive)

Ignored and NOT for commit: `test/` (local diagnostics — not a test suite), `build/`, `bin/`, `dist/`, `.mimocode/`, all `*.txt`, all `*.json`, `vdd/VDD Control.exe`, `vdd/VDD Control.pdb`, `vdd/vdd.zip` (>100 MB vendor binaries; download separately if needed).
Consequence: a `*.txt`/`*.json` file you create will never be committed — use another extension for anything meant to be tracked. `res/` (manifest resources) IS tracked.
