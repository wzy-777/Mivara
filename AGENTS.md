# AGENTS.md

TwinView (幻屏): C++17 LAN screen-mirroring / wireless second monitor.
Cross-platform (Windows / Linux X11 / macOS). Single package, no monorepo — everything is in `src/` and one `makefile`.

Full user docs: `README.md` (Chinese, thorough). This file only covers what an agent would otherwise get wrong.

## Build

```bash
make                  # all three: bin/sender, bin/receiver, bin/app
make sender           # one binary (also: receiver, app)
make debug            # -g -O0 -DDEBUG — note: runs clean first, full rebuild
make check            # env/source self-check (the only "verification" step)
make install-deps     # apt/dnf/pacman/brew + MSYS2
```

- Release flags: `-O3 -march=native -flto` — binaries are host-specific, do not redistribute builds.
- Binaries + runtime DLL closure output to `bin/`; objects go to `build/`.
- `gpu_accelerate.c` is C (gcc); all other sources are C++17 (g++). Keep it valid C.
- Makefile header deps are listed manually (see `Header Change Tracking` block). Adding a new `.h` without adding it there causes stale builds.
- SDL flags are added only to `app`/`receiver` objects (`OBJ_APP OBJ_RECEIVER`). Never add them to `sender` — pkg-config sdl2 cflags contain `-Dmain=SDL_main`, which breaks sender's link since it has no SDL2main.
- No CMake, no test suite, no lint/format/typecheck config, no CI workflows. Do not invent test commands.

## Installer (Inno Setup)

- `installer/TwinView.iss` packages `bin/` + `scripts/` + `vdd/` drivers (excludes the >100 MB vendor GUI) into `dist/TwinView-Setup-<ver>.exe`.
- Build: `"D:\Program Files\Inno Setup 6\ISCC.exe" installer\TwinView.iss` (ISCC is at that custom path on this machine, not Program Files).
- Wizard language: this Inno install lacks `ChineseSimplified.isl`, so the installer UI is English; a comment in the .iss explains how to enable Chinese. Bump `AppVersion` in the .iss when releasing.

## Windows specifics (easy to get wrong)

- `sender` links as a **console** subsystem app (logs + interactive prompts + Ctrl+C). `receiver` and `app` link with `-mwindows` (GUI, stdout discarded).
- SDL2_ttf needs a CJK font at runtime; `app.cpp` probes `msyh.ttc`/`simhei.ttf`/NotoSansCJK/wqy and exits with an error if none found.
- The runtime DLL closure must sit in `bin/` next to the exes (already there). After adding a library, verify with `ldd bin/<exe>.exe` and copy missing DLLs into `bin/`.
- Extend mode needs the MttVDD indirect-display driver installed once (`vdd/install_vdd.ps1`). `sender` auto-attaches/detaches a virtual display per session via CCD API (`vddStartup`/`vddShutdown` in `sender.cpp`). **Never break the Ctrl+C graceful path** — a hard exit leaves the virtual display attached to the user's desktop. The GUI stops the sender by sending Ctrl+C (`GenerateConsoleCtrlEvent`) and only hard-kills after a 3 s timeout.
- `scripts/*.bat` are convenience wrappers that launch `bin/` exes; `启动投屏发送.bat` takes the receiver IP as an argument.

## Architecture (actual wiring)

| File | Role |
|------|------|
| `src/sender.cpp` | capture (GDI/X11/CoreGraphics), handshake, streaming, virtual-display lifecycle. Biggest file — most work lands here. |
| `src/receiver.cpp` | SSDP broadcast, SDL2 fullscreen render, hosts the two sidecar services |
| `src/discover.cpp/.h` | SSDP discovery engine (UDP 1900) |
| `src/gpu_accelerate.c/.h` | remote CPU offload protocol (RLE compress / color convert) served on 8082 |
| `src/ports.cpp/.h` | remote port inspector on 8083 (LIST/GET/KILL) |
| `src/app.cpp` | SDL2 + SDL2_ttf GUI launcher: 接收/发送 screens, SSDP search, device list (投屏/扩展), manual IP; spawns sender/receiver as child processes |

- `ports.cpp` has a hidden `main()` behind `-DPORTS_MAIN` — can be compiled standalone as a port-inspection CLI. The makefile never sets this flag; don't be confused by two `main` functions in the tree.
- `sender` CLI: `sender [ip] [port] [--mode mirror|extend]`. `--mode` skips all interactive prompts — the GUI depends on it. Keep it working non-interactively.

## Protocol constants (defined near top of the relevant file)

- Handshake over TCP 8081: 16-byte request (w, h, fps, mode) / 12-byte response (w, h, status), network byte order. `mode`: 0 = mirror, 1 = extend (`MODE_MIRROR` / `MODE_TRUE_EXTEND` in `sender.cpp`).
- Ports: 1900/udp SSDP, 8081/tcp video, 8082/tcp compute offload (`GPU_ACCEL_PORT`), 8083/tcp port inspector (`PORTS_SERVICE_PORT`).
- Sender and receiver must be the **same binary version** — handshake/mode mismatches render as a black screen on the receiver. When touching the handshake or mode enum, consider both sides together.

## Repo hygiene (`.gitignore` is aggressive)

Ignored and NOT for commit: `test/` (local diagnostics: `fdump.cpp`, `vdd_ctl.cpp`, captured bitmaps, logs — not a test suite), `build/`, `bin/`, all `*.txt`, all `*.json`, `vdd/VDD Control.exe`, `vdd/vdd.zip` (>100 MB vendor binaries; download separately if needed).
Consequence: a `*.txt`/`*.json` file you create will never be committed — use another extension for anything meant to be tracked.
