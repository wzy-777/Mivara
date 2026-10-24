# Building & Packaging

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/building.md)

This page covers building Mivara from source on Linux, Windows and macOS, what each build target does, and how to package the Windows installer. Installing the virtual display driver required by extend mode is covered in [Virtual Display Driver](virtual-display.md).

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
- Extend mode: virtual display driver (see [Virtual Display Driver](virtual-display.md))
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

## Build Targets

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

## Windows Console Notes

- `sender` is linked as a console-subsystem binary — it shows logs and exits gracefully on Ctrl+C (including detaching the virtual display).
- `receiver` / `app` are linked with `-mwindows` (GUI subsystem), so stdout is discarded; to see receiver logs, relink receiver manually with `-mconsole`.

## Packaging the Installer

Build the installer with Inno Setup:

```powershell
& ISCC.exe installer\Mivara.iss
# If ISCC is not on PATH, use its full path or add it to PATH first
# Output: dist\Mivara-Setup-<version>.exe
```

A release must also carry the fixed-name copy `dist/Mivara-Setup.exe` (the in-app "Download update" and the README download badge both use that fixed URL) **and** `SHA256SUMS.txt` (the checksum anchor the in-app install verifies; without it only the browser download is offered). CI produces all three; for a local build copy it by hand: `copy dist\Mivara-Setup-<version>.exe dist\Mivara-Setup.exe`.

The installer bundles `bin/` (exes + DLLs), `scripts/`, the `vdd/` virtual display driver (x86/AMD64 and ARM64; the oversized vendor GUI is excluded), and the documentation (`README.md`, `README_EN.md`, `LICENSE`, `CODE_SIGNING_POLICY.md`, `SECURITY.md`, `THIRD-PARTY-NOTICES.md`, `LICENSES/GPL-3.0-or-later`, `vdd/LICENSE`, `docs/`) so the relative links inside the installed READMEs resolve offline; it also creates Start-menu/desktop shortcuts. The "Launch Mivara" checkbox on the finish page starts the elevated GUI directly (the installer launches it with its elevated token — no second UAC prompt).
