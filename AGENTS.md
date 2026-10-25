# AGENTS.md

Mivara (幻屏): C++17 LAN screen-mirroring / wireless second monitor.
Cross-platform (Windows / Linux X11 / macOS). Single package, no monorepo — everything is in `src/` and one `makefile`.

User docs: `README.md` / `README_EN.md` are the short landing pages; long-form documentation lives in `docs/zh/` and `docs/en/` (index: `docs/README.md`, screenshots in `docs/assets/`). This root file holds high-frequency agent rules; detailed maintainer guidance is linked below.

**Rule: keep `README.md` and `README_EN.md` in sync** — any edit to one must be mirrored in the other. Every `docs/zh/<page>.md` has a `docs/en/<page>.md` twin with the same heading order, tables and code blocks; edit both. The README badge anchors (`#环境要求` / `#requirements`) and the SmartScreen anchors depend on those exact headings. Terminology: say 「扩展 / Extend」 for the real extended desktop (virtual-display mode); no "真·扩展屏 / True extend" qualifiers.

## Build

```bash
make                  # all three: bin/sender, bin/receiver, bin/app
make sender           # one binary (also: receiver, app)
make debug            # -g -O0 -DDEBUG — note: runs clean first, full rebuild
make run              # run the GUI from bin/
make run-sender       # run bin/sender directly
make run-receiver     # run bin/receiver directly
make check            # env/source self-check (the only "verification" step)
make install-deps     # apt/dnf/pacman/brew + MSYS2
# On Windows, make also runs copy-dlls, which copies the mingw64 runtime DLL closure into bin/.
make OPT_FLAGS="-O3 -march=x86-64 -flto"   # portable build for releases/installer
```

- Default release flags: `-O3 -march=native -flto` — host-specific, do not redistribute. **Release/installer builds must override OPT_FLAGS** with `-march=x86-64` (see above).
- Binaries + runtime DLL closure output to `bin/`; objects go to `build/`.
- `gpu_accelerate.c` is C (gcc); all other sources are C++17 (g++). Keep it valid C.
- Makefile header deps are listed manually (see `Header Change Tracking Maps` block). Adding a new `.h` without adding it there causes stale builds. `make check` keeps a second hand-written source list (the `check:` target) — add new or renamed sources there too, or it reports MISSING.
- SDL flags are added only to the `app`/`receiver` objects and to `theme.o` (`OBJ_APP OBJ_RECEIVER OBJ_THEME` — theme.h pulls in `<SDL2/SDL.h>`, and theme.o is linked into `app` only). Never add them to `sender` — pkg-config sdl2 cflags contain `-Dmain=SDL_main`, which breaks sender's link since it has no SDL2main.
- No CMake, no test suite, no lint/format/typecheck config. The only CI is `.github/workflows/release-installer.yml` (portable installer build); do not invent test commands.

### Toolchain

- `make`/`g++` are not in the Windows PATH — run builds through MSYS2: `C:/msys64/usr/bin/bash.exe -lc "export PATH=/mingw64/bin:/usr/bin:$PATH OS=Windows_NT && cd /d/<repo> && make ..."`. The makefile's whole Windows branch is keyed on `$(OS)`; in sandboxed shells where `OS` is unset, links silently lose all system libs (`-lws2_32`, `-lgdi32`, `-lsetupapi`, …) and there is no `.exe` suffix.
- Git and `gh` are expected on PATH (or set `GH=`). If direct GitHub access is blocked, set `PROXY=<http proxy>`; `release.sh` only falls back to it after a direct attempt fails.
- Machine-specific toolchain notes (exact install paths, local shell quirks) live in `.claude/local-notes.md`, which is gitignored; read it if present. Keep this file portable.

## Task-specific maintainer guides

Read every guide relevant to the area being changed. These are authoritative project rules, not optional background. Use ordinary links only: do not eagerly import these files into `AGENTS.md` or `CLAUDE.md`.

| Change area | Required guide |
|-------------|----------------|
| Installer, packaging, releases, winget or signing | [Installer and release](docs/agents/installer.md) |
| Windows manifests, DPI, runtime DLLs, VDD or stitch input | [Windows specifics](docs/agents/windows.md) |
| Source wiring, CLI behavior, services, GUI or updater | [Architecture](docs/agents/architecture.md) |
| Handshakes, packets, STAT/stdin channels, ports or discovery | [Protocol](docs/agents/protocol.md) |

Keep each guide synchronized with the code it describes. Add detailed rules to the appropriate guide; reserve this root file for cross-cutting, high-frequency constraints. Keep every agent-instruction file below Claude Code's 40,000-character limit, with headroom for future edits.

## Repo hygiene (`.gitignore` is aggressive)

Ignored and NOT for commit: `test/` (local diagnostics — not a test suite), `build/`, `bin/`, `dist/`, `.mimocode/`, all `*.txt`, all `*.json`, `vdd/VDD Control.exe`, `vdd/VDD Control.pdb`, `vdd/vdd.zip` (>100 MB vendor binaries; download separately if needed).
Consequence: a `*.txt`/`*.json` file you create will never be committed — use another extension for anything meant to be tracked. `res/` (manifest resources) IS tracked.
