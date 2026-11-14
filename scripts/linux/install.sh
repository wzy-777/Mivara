#!/bin/sh
# ============================================================================
# Mivara (幻屏) — install the portable Linux bundle
# ============================================================================
# Installs the binaries, bundled runtime libraries, desktop entry and icon
# from this directory. The bundle is self-contained apart from glibc, X11 and
# the usual desktop libraries, so no distro packages are required.
#
# Usage:
#   ./install.sh                     # per-user install into ~/.local
#   sudo ./install.sh                # system-wide install into /usr/local
#   ./install.sh --prefix /opt/mivara
#   ./install.sh --uninstall         # remove the same prefix
#   sudo ./install.sh --prefix /usr --uninstall
#
# Environment:
#   PREFIX   install prefix (default: /usr/local as root, ~/.local otherwise)
#   DESTDIR  staging root for package builders
#
# After installing, launch "Mivara" from the application menu or run
# `mivara` from a terminal.
# ============================================================================

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX="${PREFIX:-}"
DESTDIR="${DESTDIR:-}"
ACTION=install

usage() {
  sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
}

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)  usage ;;
    --prefix)   shift; [ $# -gt 0 ] || { echo "missing --prefix value" >&2; exit 2; }; PREFIX="$1" ;;
    --uninstall) ACTION=uninstall ;;
    -*)         echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    *)          echo "unexpected argument: $1" >&2; exit 2 ;;
  esac
  shift
done

if [ -z "$PREFIX" ]; then
  if [ "$(id -u)" -eq 0 ]; then PREFIX=/usr/local; else PREFIX="$HOME/.local"; fi
fi
# Strip a trailing slash so the generated paths stay stable.
PREFIX=${PREFIX%/}
[ -n "$PREFIX" ] || PREFIX=/

APPDIR="$PREFIX/lib/mivara"
BINDIR="$PREFIX/bin"
APPDESKTOP="$PREFIX/share/applications"
ICONDIR="$PREFIX/share/icons/hicolor/256x256/apps"
DOCDIR="$PREFIX/share/doc/mivara"

D_APPDIR="$DESTDIR$APPDIR"
D_BINDIR="$DESTDIR$BINDIR"
D_APPDESKTOP="$DESTDIR$APPDESKTOP"
D_ICONDIR="$DESTDIR$ICONDIR"
D_DOCDIR="$DESTDIR$DOCDIR"

quote_desktop() {
  # Desktop Entry Exec values use double quotes; only backslash and the quote
  # itself need escaping inside them.
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

uninstall() {
  rm -f "$D_BINDIR/mivara" "$D_BINDIR/mivara-sender" "$D_BINDIR/mivara-receiver"
  rm -f "$D_APPDESKTOP/mivara.desktop"
  rm -f "$D_ICONDIR/mivara.png"
  rm -rf "$D_APPDIR" "$D_DOCDIR"
  if command -v update-desktop-database >/dev/null 2>&1 && [ -z "$DESTDIR" ]; then
    update-desktop-database "$APPDESKTOP" >/dev/null 2>&1 || true
  fi
  echo "Mivara removed from $PREFIX"
}

install_files() {
  [ -x "$HERE/bin/app" ] || { echo "error: $HERE/bin/app is missing (extract the whole archive)" >&2; exit 1; }
  [ -x "$HERE/bin/sender" ] || { echo "error: $HERE/bin/sender is missing" >&2; exit 1; }
  [ -x "$HERE/bin/receiver" ] || { echo "error: $HERE/bin/receiver is missing" >&2; exit 1; }

  install -d "$D_APPDIR/bin" "$D_APPDIR/lib" "$D_BINDIR" "$D_APPDESKTOP" \
             "$D_ICONDIR" "$D_DOCDIR"
  cp -R "$HERE/bin/." "$D_APPDIR/bin/"
  if [ -d "$HERE/lib" ]; then
    cp -R "$HERE/lib/." "$D_APPDIR/lib/"
  fi

  # Keep the license/third-party texts with the installed binaries.
  cp -R "$HERE/README.md" "$HERE/README_EN.md" "$HERE/LICENSE" \
        "$HERE/SECURITY.md" "$HERE/THIRD-PARTY-NOTICES.md" "$D_DOCDIR/"
  cp -R "$HERE/LICENSES" "$HERE/docs" "$D_DOCDIR/"

  # The GUI resolves sender/receiver next to /proc/self/exe, so the symlink
  # must point at the real binary inside lib/mivara, never be copied.
  ln -sfn "../lib/mivara/bin/app" "$D_BINDIR/mivara"
  ln -sfn "../lib/mivara/bin/sender" "$D_BINDIR/mivara-sender"
  ln -sfn "../lib/mivara/bin/receiver" "$D_BINDIR/mivara-receiver"

  exec_path="$APPDIR/bin/app"
  exec_quoted=$(quote_desktop "$exec_path")
  sed "s|^Exec=.*|Exec=\"$exec_quoted\"|" "$HERE/mivara.desktop" > "$D_APPDESKTOP/mivara.desktop"
  chmod 644 "$D_APPDESKTOP/mivara.desktop"
  cp "$HERE/mivara.png" "$D_ICONDIR/mivara.png"
  chmod 644 "$D_ICONDIR/mivara.png"

  if command -v update-desktop-database >/dev/null 2>&1 && [ -z "$DESTDIR" ]; then
    update-desktop-database "$APPDESKTOP" >/dev/null 2>&1 || true
  fi
  if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -z "$DESTDIR" ]; then
    gtk-update-icon-cache -q -t -f "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
  fi

  echo "Mivara installed to $APPDIR"
  echo "  launcher : mivara"
  echo "  desktop  : $APPDESKTOP/mivara.desktop"
  echo "  docs     : $DOCDIR"

  if command -v fc-list >/dev/null 2>&1; then
    if ! fc-list :lang=zh 2>/dev/null | grep -q .; then
      echo "warning: no Simplified Chinese font found; install fonts-noto-cjk or fonts-wqy-microhei" >&2
    fi
  fi

  case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) echo "note: $BINDIR is not on PATH yet; use the application menu or add it to PATH" ;;
  esac
}

case "$ACTION" in
  install)   install_files ;;
  uninstall) uninstall ;;
esac
