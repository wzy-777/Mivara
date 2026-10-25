# ============================================================================
# Mivara PROJECT MAKEFILE  –  Screen Extender / Screen Share Application
# ============================================================================
#
# Platforms  : Linux · macOS · Windows (MinGW / MSYS2)
# C++ std    : C++17
#
# Layout     : sources in src/, objects in build/, binaries + runtime
#              DLLs in bin/.  Binaries are linked per-target:
#                sender   – console, no SDL (capture + stream only)
#                receiver – SDL2 (video window)
#                app      – SDL2 + SDL2_ttf (Chinese GUI launcher)
#
# Key targets
#   make                – build all components (default)
#   make app            – build launcher only
#   make sender         – build sender only
#   make receiver       – build receiver only
#   make debug          – build with debug symbols
#   make clean          – remove build artefacts
#   make check          – verify build environment + sources
#   make install-deps   – install system dependencies (Linux / macOS / MSYS2)
#   make run            – run launcher
#   make run-sender     – run sender directly
#   make run-receiver   – run receiver directly
#   make help           – print this summary
#
# ============================================================================

# --- RUNTIME OPTIMIZATIONS ---
# -O3: Maximum compiler optimization loops for performance critical math/streaming.
# -march=native: Utilizes native instruction sets (AVX, SSE) on the host computer.
# -flto: Link-Time Optimization. Allows global inline optimizations across modules.
OPT_FLAGS := -O3 -march=native -flto

CXX      = g++
CC       = gcc
CXXFLAGS = -std=c++17 -Wall -Wextra -Wno-unused-parameter $(OPT_FLAGS)
CFLAGS   = $(OPT_FLAGS) -Wall -Wextra
SRCDIR   = src
BUILDDIR = build
BINDIR   = bin

# ============================================================================
# PLATFORM DETECTION
# ============================================================================

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# --------------------------------------------------------------------------
# Linux
# --------------------------------------------------------------------------
ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    EXE      :=

    # SDL2 – prefer pkg-config, then sdl2-config, then plain -l flags.
    ifneq ($(shell pkg-config --exists sdl2 SDL2_ttf 2>/dev/null && echo 1),)
        SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf)
        SDL_LIBS   := $(shell pkg-config --libs   sdl2 SDL2_ttf)
    else ifneq ($(shell pkg-config --exists sdl2 2>/dev/null && echo 1),)
        SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
        SDL_LIBS   := $(shell pkg-config --libs sdl2) -lSDL2_ttf
    else ifneq ($(shell which sdl2-config 2>/dev/null),)
        SDL_CFLAGS := $(shell sdl2-config --cflags)
        SDL_LIBS   := $(shell sdl2-config --libs) -lSDL2_ttf
    else
        SDL_CFLAGS := -I/usr/include/SDL2 -D_REENTRANT
        SDL_LIBS   := -lSDL2 -lSDL2_ttf
    endif

    CXXFLAGS += -D_REENTRANT
    NET_LIBS := -lpthread
    SENDER_LIBS   := $(NET_LIBS) $(OPT_FLAGS) -lX11
    RECEIVER_LIBS := $(SDL_LIBS) $(NET_LIBS) $(OPT_FLAGS)
    APP_LIBS      := $(SDL_LIBS) $(NET_LIBS) $(OPT_FLAGS)
    RES_ADMIN     :=
endif

# --------------------------------------------------------------------------
# macOS
# --------------------------------------------------------------------------
ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    EXE      :=

    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)

    SDL_CFLAGS := -I$(BREW_PREFIX)/include/SDL2
    SDL_LIBS   := -L$(BREW_PREFIX)/lib -lSDL2 -lSDL2_ttf

    NET_LIBS := -lpthread
    SENDER_LIBS   := $(NET_LIBS) $(OPT_FLAGS) -framework CoreGraphics -framework CoreFoundation
    RECEIVER_LIBS := $(SDL_LIBS) $(NET_LIBS) $(OPT_FLAGS)
    APP_LIBS      := $(SDL_LIBS) $(NET_LIBS) $(OPT_FLAGS)
    RES_ADMIN     :=
endif

# --------------------------------------------------------------------------
# Windows (MinGW / MSYS2)
# --------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    EXE      := .exe

    SDL2_PREFIX ?= C:/msys64/mingw64

    # Prefer MSYS2 pkg-config: it supplies SDL2main and the correct library order.
    ifneq ($(shell pkg-config --exists sdl2 SDL2_ttf 2>/dev/null && echo 1),)
        SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf)
        SDL_LIBS   := $(shell pkg-config --libs sdl2 SDL2_ttf)
    else
        SDL_CFLAGS := -I$(SDL2_PREFIX)/include/SDL2
        SDL_LIBS   := -L$(SDL2_PREFIX)/lib -lmingw32 -mwindows -lSDL2main -lSDL2 -lSDL2_ttf
    endif

    CXXFLAGS += -DWIN32_LEAN_AND_MEAN
    CFLAGS   += -DWIN32_LEAN_AND_MEAN

    NET_LIBS := -lws2_32 -liphlpapi -lpthread
    # sender: console subsystem (logs + Ctrl+C graceful stop), no SDL
    # -lsetupapi: enable/disable the Root\MttVDD device (vdd.cpp)
    # -ladvapi32: registry override lookup for vdd_settings.xml (vdd.cpp)
    # -luser32: stitch KVM (hooks, Raw Input, SendInput, MessageBox) in kvm.cpp
    SENDER_LIBS   := $(NET_LIBS) $(OPT_FLAGS) -lgdi32 -lsetupapi -ladvapi32 -luser32
    RECEIVER_LIBS := $(SDL_LIBS) $(NET_LIBS) $(OPT_FLAGS) -luser32
    # app: -lwinhttp (update check/download), -lbcrypt (SHA-256 via CNG),
    #      -ladvapi32 (SetNamedSecurityInfo + SDDL on the update directory),
    #      -lshell32 + -luser32 (tray icon, balloons, exit confirmation)
    APP_LIBS      := $(SDL_LIBS) $(NET_LIBS) $(OPT_FLAGS) -lsetupapi \
                     -lwinhttp -lbcrypt -ladvapi32 -lshell32 -luser32
    # requireAdministrator manifest resource for sender + app
    RES_ADMIN := $(BUILDDIR)/admin_rc.o
endif

# ============================================================================
# FFMPEG (libavcodec / libavutil / libswscale) – codec.cpp, sender, receiver
# ============================================================================

ifneq ($(shell pkg-config --exists libavcodec libavutil libswscale 2>/dev/null && echo 1),)
    FFMPEG_CFLAGS := $(shell pkg-config --cflags libavcodec libavutil libswscale)
    FFMPEG_LIBS   := $(shell pkg-config --libs   libavcodec libavutil libswscale)
else
    FFMPEG_CFLAGS :=
    FFMPEG_LIBS   := -lavcodec -lswscale -lavutil
endif

# sender + receiver link the codec stack (app stays FFmpeg-free)
SENDER_LIBS   += $(FFMPEG_LIBS)
RECEIVER_LIBS += $(FFMPEG_LIBS)

# Windows: copy the mingw64 runtime DLL closure into bin/ (wired into 'all')
ifeq ($(OS),Windows_NT)
    DLL_TARGET := copy-dlls
else
    DLL_TARGET :=
endif

# ============================================================================
# BINARY NAMES & OBJECTS
# ============================================================================

SENDER_BIN   := $(BINDIR)/sender$(EXE)
RECEIVER_BIN := $(BINDIR)/receiver$(EXE)
APP_BIN      := $(BINDIR)/app$(EXE)

OBJ_DISCOVER := $(BUILDDIR)/discover.o
OBJ_GPU      := $(BUILDDIR)/gpu_accelerate.o
OBJ_PORTS    := $(BUILDDIR)/ports.o
OBJ_VDD      := $(BUILDDIR)/vdd.o
OBJ_CODEC    := $(BUILDDIR)/codec.o
OBJ_CONFIG   := $(BUILDDIR)/config.o
OBJ_HISTORY  := $(BUILDDIR)/history.o
OBJ_APP      := $(BUILDDIR)/app.o
OBJ_SENDER   := $(BUILDDIR)/sender.o
OBJ_RECEIVER := $(BUILDDIR)/receiver.o
OBJ_UPDATE   := $(BUILDDIR)/update.o
OBJ_THEME    := $(BUILDDIR)/theme.o
OBJ_KVM      := $(BUILDDIR)/kvm.o
OBJ_AUTH     := $(BUILDDIR)/auth.o
OBJ_TRAY     := $(BUILDDIR)/tray.o

# ============================================================================
# PHONY TARGET DECLARATIONS
# ============================================================================

.PHONY: all clean debug sender receiver app run run-sender run-receiver check install-deps help copy-dlls

# Default Target
all: $(SENDER_BIN) $(RECEIVER_BIN) $(APP_BIN) $(DLL_TARGET)
	@echo "========================================="
	@echo "Build complete!  → $(BINDIR)/"
	@echo "  sender$(EXE)   receiver$(EXE)   app$(EXE)"
	@echo "========================================="

# Directory Generation Rule (Safe for concurrent parallel -j compilation)
$(BUILDDIR) $(BINDIR):
	mkdir -p $@

# ============================================================================
# COMPILATION RULES (Using High-Performance Pattern Matching)
# ============================================================================

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# requireAdministrator manifest resource (Windows only; RES_ADMIN empty elsewhere)
ifneq ($(RES_ADMIN),)
$(RES_ADMIN): res/admin.rc res/admin.manifest | $(BUILDDIR)
	windres res/admin.rc -O coff -o $@
endif

# SDL headers/flags only for the two SDL binaries – sender must NOT get
# -Dmain=SDL_main (pkg-config sdl2 cflags) since it links without SDL2main.
# theme.o carries SDL_Color, so it needs the SDL headers too (it has no main,
# so the -Dmain=SDL_main that comes with the cflags is harmless there).
$(OBJ_APP) $(OBJ_RECEIVER) $(OBJ_THEME): CXXFLAGS += $(SDL_CFLAGS)

# codec.o needs FFmpeg headers
$(OBJ_CODEC): CXXFLAGS += $(FFMPEG_CFLAGS)

# Header Change Tracking Maps
$(OBJ_DISCOVER): $(SRCDIR)/discover.h
$(OBJ_PORTS): $(SRCDIR)/ports.h
$(OBJ_GPU): $(SRCDIR)/gpu_accelerate.h
$(OBJ_VDD): $(SRCDIR)/vdd.h
$(OBJ_CODEC): $(SRCDIR)/codec.h $(SRCDIR)/proto.h
$(OBJ_CONFIG): $(SRCDIR)/config.h
$(OBJ_HISTORY): $(SRCDIR)/history.h
$(OBJ_UPDATE): $(SRCDIR)/update.h $(SRCDIR)/version.h
$(OBJ_THEME): $(SRCDIR)/theme.h
$(OBJ_KVM): $(SRCDIR)/kvm.h $(SRCDIR)/proto.h $(SRCDIR)/auth.h
$(OBJ_AUTH): $(SRCDIR)/auth.h
$(OBJ_TRAY): $(SRCDIR)/tray.h
$(OBJ_SENDER): $(SRCDIR)/discover.h $(SRCDIR)/gpu_accelerate.h $(SRCDIR)/kvm.h $(SRCDIR)/ports.h $(SRCDIR)/vdd.h $(SRCDIR)/codec.h $(SRCDIR)/proto.h $(SRCDIR)/auth.h $(SRCDIR)/version.h
$(OBJ_RECEIVER): $(SRCDIR)/discover.h $(SRCDIR)/gpu_accelerate.h $(SRCDIR)/kvm.h $(SRCDIR)/ports.h $(SRCDIR)/codec.h $(SRCDIR)/proto.h $(SRCDIR)/auth.h $(SRCDIR)/config.h $(SRCDIR)/version.h
$(OBJ_APP): $(SRCDIR)/discover.h $(SRCDIR)/vdd.h $(SRCDIR)/kvm.h $(SRCDIR)/config.h $(SRCDIR)/history.h $(SRCDIR)/update.h $(SRCDIR)/version.h $(SRCDIR)/theme.h $(SRCDIR)/auth.h $(SRCDIR)/tray.h

# ============================================================================
# TARGET LINKING
# ============================================================================

$(SENDER_BIN): $(OBJ_SENDER) $(OBJ_DISCOVER) $(OBJ_GPU) $(OBJ_PORTS) $(OBJ_VDD) $(OBJ_CODEC) $(OBJ_KVM) $(OBJ_AUTH) $(RES_ADMIN) | $(BINDIR)
	$(CXX) $^ -o $@ $(SENDER_LIBS)
	@echo "Built $(SENDER_BIN)"

$(RECEIVER_BIN): $(OBJ_RECEIVER) $(OBJ_DISCOVER) $(OBJ_GPU) $(OBJ_PORTS) $(OBJ_CODEC) $(OBJ_KVM) $(OBJ_AUTH) $(OBJ_CONFIG) | $(BINDIR)
	$(CXX) $^ -o $@ $(RECEIVER_LIBS)
	@echo "Built $(RECEIVER_BIN)"

$(APP_BIN): $(OBJ_APP) $(OBJ_DISCOVER) $(OBJ_VDD) $(OBJ_CONFIG) $(OBJ_HISTORY) $(OBJ_UPDATE) $(OBJ_THEME) $(OBJ_AUTH) $(OBJ_TRAY) $(RES_ADMIN) | $(BINDIR)
	$(CXX) $^ -o $@ $(APP_LIBS)
	@echo "Built $(APP_BIN)"

# Windows: copy the mingw64 runtime DLL closure (incl. FFmpeg) into bin/
# Resolve the closure with objdump, not ldd: MSYS2's ldd debug-loads each target,
# and CreateProcess on the requireAdministrator sender/app fails with
# ERROR_ELEVATION_REQUIRED ("Permission denied"), so ldd silently reported
# nothing for exactly the two binaries that need SDL2_ttf / the FFmpeg DLLs.
# objdump reads the import tables recursively without executing anything.
copy-dlls: $(SENDER_BIN) $(RECEIVER_BIN) $(APP_BIN)
	@echo "Copying runtime DLLs into $(BINDIR)/ ..."
	@queue="$(SENDER_BIN) $(RECEIVER_BIN) $(APP_BIN)"; seen=""; \
	while [ -n "$$queue" ]; do \
	    set -- $$queue; f=$$1; shift; queue="$$*"; \
	    for d in $$(objdump -p "$$f" 2>/dev/null | awk '/DLL Name/{print $$3}'); do \
	        case " $$seen " in *" $$d "*) continue;; esac; \
	        seen="$$seen $$d"; \
	        if [ -f "/mingw64/bin/$$d" ]; then \
	            cp -f "/mingw64/bin/$$d" "$(BINDIR)/" 2>/dev/null || true; \
	        fi; \
	        if [ -f "$(BINDIR)/$$d" ]; then queue="$$queue $(BINDIR)/$$d"; fi; \
	    done; \
	done
	@echo "DLL closure updated"

# Aliases
sender:   $(SENDER_BIN)
receiver: $(RECEIVER_BIN)
app:      $(APP_BIN)

# ============================================================================
# DEBUG TARGET (Disables heavy optimization flags, injects debug items)
# ============================================================================

debug: OPT_FLAGS := -g -O0 -DDEBUG
debug: clean all
	@echo "Debug build complete"

# ============================================================================
# CLEANING
# ============================================================================

clean:
	rm -rf $(BUILDDIR)
	rm -f $(SENDER_BIN) $(RECEIVER_BIN) $(APP_BIN)
	@echo "Cleaned"

# ============================================================================
# RUN HOOKS
# ============================================================================

run: $(APP_BIN)
	./$(APP_BIN)

run-sender: $(SENDER_BIN)
	./$(SENDER_BIN)

run-receiver: $(RECEIVER_BIN)
	./$(RECEIVER_BIN)

# ============================================================================
# DIAGNOSTICS & SYSTEM ENVIRONMENT TARGETS
# ============================================================================

check:
	@echo "========================================="
	@echo "  BUILD ENVIRONMENT CHECK"
	@echo "========================================="
	@echo "  Platform : $(UNAME_S)"
	@echo "  Compiler : $(CXX)"
	@echo "  CXXFLAGS : $(CXXFLAGS)"
	@echo "========================================="
	@echo "  Source files:"
	@for f in app.cpp sender.cpp receiver.cpp discover.cpp discover.h \
               gpu_accelerate.c gpu_accelerate.h ports.cpp ports.h \
               vdd.cpp vdd.h codec.cpp codec.h proto.h \
               kvm.cpp kvm.h \
               config.cpp config.h history.cpp history.h \
	               update.cpp update.h version.h theme.cpp theme.h \
               auth.cpp auth.h tray.cpp tray.h; do \
        if [ -f $(SRCDIR)/$$f ]; then \
            echo "    OK  $$f"; \
        else \
            echo "    MISSING  $$f"; \
        fi; \
    done
	@for f in res/admin.rc res/admin.manifest; do \
        if [ -f $$f ]; then \
            echo "    OK  $$f"; \
        else \
            echo "    MISSING  $$f"; \
        fi; \
    done
	@echo "========================================="
	@echo "  SDL2 / SDL2_ttf:"
	@if pkg-config --exists sdl2 2>/dev/null; then \
        echo "    OK  SDL2 $$(pkg-config --modversion sdl2)"; \
    else \
        echo "    NOT FOUND via pkg-config (may still link if installed)"; \
    fi
	@if pkg-config --exists SDL2_ttf 2>/dev/null; then \
        echo "    OK  SDL2_ttf $$(pkg-config --modversion SDL2_ttf)"; \
    else \
        echo "    NOT FOUND  SDL2_ttf  <-- required for the GUI (app)"; \
    fi
	@echo "========================================="

install-deps:
	@echo "Installing dependencies..."
	@if command -v apt-get >/dev/null 2>&1; then \
        sudo apt-get update && \
        sudo apt-get install -y g++ make libx11-dev libsdl2-dev libsdl2-ttf-dev \
            libavcodec-dev libavutil-dev libswscale-dev; \
	elif command -v dnf >/dev/null 2>&1; then \
        sudo dnf install -y gcc-c++ make libX11-devel SDL2-devel SDL2_ttf-devel \
            ffmpeg-free-devel || sudo dnf install -y gcc-c++ make libX11-devel \
            SDL2-devel SDL2_ttf-devel ffmpeg-devel; \
	elif command -v yum >/dev/null 2>&1; then \
        sudo yum install -y gcc-c++ make libX11-devel SDL2-devel SDL2_ttf-devel \
            ffmpeg-devel; \
	elif command -v pacman >/dev/null 2>&1; then \
        if [ -n "$$MINGW_PACKAGE_PREFIX" ]; then \
            pacman -S --needed --noconfirm \
                "$${MINGW_PACKAGE_PREFIX}-gcc" make \
                "$${MINGW_PACKAGE_PREFIX}-pkgconf" \
                "$${MINGW_PACKAGE_PREFIX}-SDL2" \
                "$${MINGW_PACKAGE_PREFIX}-SDL2_ttf" \
                "$${MINGW_PACKAGE_PREFIX}-ffmpeg"; \
        else \
            sudo pacman -S --needed --noconfirm gcc make libx11 sdl2 sdl2_ttf ffmpeg; \
        fi; \
	elif command -v brew >/dev/null 2>&1; then \
        brew install sdl2 sdl2_ttf ffmpeg; \
	else \
        echo "Unsupported package manager.  Install manually:"; \
        echo "  g++ / clang++ (C++17)"; \
        echo "  libX11-dev  (Linux screen capture)"; \
        echo "  libSDL2-dev"; \
        echo "  libSDL2-ttf-dev  (GUI launcher)"; \
        echo "  libavcodec-dev libavutil-dev libswscale-dev  (H.264/H.265/AV1)"; \
    fi
	@echo "Done"

help:
	@echo ""
	@echo "  Mivara (幻屏)"
	@echo ""
	@echo "BUILD:"
	@echo "  make              Build all (bin/sender, bin/receiver, bin/app)"
	@echo "  make sender       Build sender only"
	@echo "  make receiver     Build receiver only"
	@echo "  make app          Build GUI launcher only"
	@echo "  make debug        Build with -g -O0 -DDEBUG"
	@echo "  make clean        Remove build output"
	@echo ""
	@echo "RUN:"
	@echo "  make run          Launch the GUI launcher"
	@echo "  make run-sender   Run sender directly"
	@echo "  make run-receiver Run receiver directly"
	@echo ""
	@echo "SETUP:"
	@echo "  make check        Verify environment, sources"
	@echo "  make install-deps Install all system dependencies"
	@echo "  make help         Show this message"
	@echo ""
	@echo "SENDER CLI:"
	@echo "  bin/sender [ip] [port] [--mode mirror|extend|stitch]"
	@echo "    --mode skips all interactive prompts (used by the GUI)"
	@echo "    stitch = software KVM (keyboard+mouse only, no video), Windows only"
