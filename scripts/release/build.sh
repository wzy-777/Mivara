#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — portable build
# ============================================================================
# Rebuilds bin/{sender,receiver,app,stitchcap}.exe with the portable flags and
# verifies that each binary really carries the current version.
#
# Usage:
#   bash scripts/release/build.sh            # build + check
#   bash scripts/release/build.sh --no-check # build only
#
# Requirements: Git Bash + MSYS2 (the build itself runs in MSYS2 bash because
# /mingw64 is not mounted in Git Bash). Close any app started from bin/ — a
# locked .exe cannot be replaced.
# ============================================================================

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

build_portable() {
  step "构建（portable flags）"
  echo "    make clean && make OPT_FLAGS=\"$PORTABLE_FLAGS\" && make check"
  "$MSYS_BASH" -lc "export PATH=/mingw64/bin:/usr/bin:\$PATH; export OS=Windows_NT; cd \"$ROOT\"; make clean && make OPT_FLAGS=\"$PORTABLE_FLAGS\" && make check" \
    || die "构建失败"
  ok "四个二进制已重建（portable flags）"
}

check_binaries() {
  local ver="${1:-$(read_version)}"
  local b
  for b in app sender receiver; do
    [ -f "bin/$b.exe" ] || die "构建产物缺失: bin/$b.exe"
    grep -aq "$ver" "bin/$b.exe" || die "bin/$b.exe 里找不到版本号 $ver —— 构建可能是旧的"
  done
  # stitchcap.exe has no version string, but a missing helper breaks file drop
  # and the installer packages bin/* wholesale, so its presence is part of the
  # check.
  [ -f bin/stitchcap.exe ] || die "构建产物缺失: bin/stitchcap.exe（拼接文件投放助手）"
  ok "bin/app.exe · sender.exe · receiver.exe 均为 $ver，stitchcap.exe 已就绪"
}

build_and_check() {
  local ver="${1:-$(read_version)}"
  build_portable
  check_binaries "$ver"
}

main_build() {
  local check=1
  local ver=""
  while [ $# -gt 0 ]; do
    case "$1" in
      -h|--help)  usage_from_header "$0" 20; exit 0 ;;
      --no-check) check=0 ;;
      --ver)      shift; ver="${1:-}" ;;
      -*)         die "unknown option: $1 (see --help)" ;;
      *)          ver="$1" ;;
    esac
    shift
  done
  require_repo
  [ -x "$MSYS_BASH" ] || die "找不到 MSYS2 bash: $MSYS_BASH（构建需要它）"
  ver="${ver:-$(read_version)}"
  validate_version "$ver"
  build_portable
  [ "$check" = 1 ] && check_binaries "$ver"
  return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main_build "$@"
fi
