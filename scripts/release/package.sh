#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — build the installer only
# ============================================================================
# The packaging half of release.sh: portable build (optional) → Inno Setup →
# dist/Mivara-Setup-<ver>.exe + fixed-name copy + SHA256SUMS.txt. It does not
# touch git, GitHub, winget or the version files, so it is safe to run while
# preparing a local test install.
#
# Usage:
#   bash scripts/release/package.sh                       # build + package
#   bash scripts/release/package.sh --skip-build          # package bin/ as-is
#   bash scripts/release/package.sh --notes dist/RELEASE_NOTES_v0.11.5.md
#
# Requires Git Bash + MSYS2 + Inno Setup. Close any app started from bin/.
# ============================================================================

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
source "$RELEASE_SCRIPTS_DIR/build.sh"

SETUP=""
HASH=""

package_installer() {
  local ver="$1" notes="${2:-}" skip_build="${3:-0}"
  validate_version "$ver"
  require_repo
  mkdir -p dist

  if [ "$skip_build" = 1 ]; then
    warn "跳过构建（--skip-build），复用 bin/ 里的二进制"
    check_binaries "$ver"
  else
    build_and_check "$ver"
  fi

  step "打包安装包"
  local iscc
  iscc="$(iscc_bin)"
  [ -x "$iscc" ] || die "找不到 Inno Setup 编译器: ${iscc:-<未找到>}（设置 ISCC=/path/ISCC.exe）"

  if [ "$skip_build" = 1 ] && [ -f "dist/Mivara-Setup-$ver.exe" ]; then
    warn "复用已存在的 dist/Mivara-Setup-$ver.exe"
  else
    "$iscc" installer/Mivara.iss >/dev/null || die "ISCC 失败"
  fi

  SETUP="dist/Mivara-Setup-$ver.exe"
  [ -f "$SETUP" ] || die "ISCC 没有产出 $SETUP"
  cp -f "$SETUP" dist/Mivara-Setup.exe
  cmp -s "$SETUP" dist/Mivara-Setup.exe || die "固定名副本与版本名文件不一致"
  ok "dist/Mivara-Setup.exe（固定名副本，更新器下载的就是它）"

  HASH="$(sha256sum "$SETUP" | awk '{print $1}')"
  local hash_up
  hash_up="$(printf '%s' "$HASH" | tr 'a-f' 'A-F')"
  # sha256sum's own "hash *name" format — the form the in-app updater was
  # verified against.
  {
    printf '%s *%s\n' "$HASH" "Mivara-Setup-$ver.exe"
    printf '%s *%s\n' "$HASH" "Mivara-Setup.exe"
  } > dist/SHA256SUMS.txt
  ok "SHA-256 $HASH"

  # Release notes must quote the asset hash (AGENTS.md requirement). The hash
  # is only known once the installer exists, so refresh whatever 64-hex token
  # is already in the notes instead of making the user rebuild to learn it.
  if [ -n "$notes" ] && [ -f "$notes" ]; then
    if grep -Eq '[0-9a-fA-F]{64}' "$notes"; then
      if ! grep -qi "$HASH" "$notes"; then
        sed -i -E "s/[0-9a-fA-F]{64}/$HASH/g" "$notes"
        warn "发行说明里的旧哈希已替换为本次的 $HASH"
      fi
    else
      printf '\n**SHA-256**：`%s`\n' "$HASH" >> "$notes"
      warn "发行说明里原本没有哈希，已在末尾追加一行"
    fi
    grep -qi "$HASH" "$notes" || die "发行说明的哈希回填失败: $notes"
    ok "发行说明已包含本次哈希"
  elif [ -n "$notes" ]; then
    warn "找不到发行说明 $notes（仅打包，不要求它）"
  fi
  return 0
}

main_package() {
  local ver="" notes="" skip_build=0
  while [ $# -gt 0 ]; do
    case "$1" in
      -h|--help)   usage_from_header "$0" 20; exit 0 ;;
      --skip-build) skip_build=1 ;;
      --ver)       shift; ver="${1:-}" ;;
      --notes)     shift; notes="${1:-}" ;;
      --no-notes)  notes="" ;;
      -*)          die "unknown option: $1 (see --help)" ;;
      *)           die "多余参数: $1" ;;
    esac
    shift
  done
  require_repo
  ver="${ver:-$(read_version)}"
  if [ -z "$notes" ]; then
    local def="dist/RELEASE_NOTES_v${ver}.md"
    [ -f "$def" ] && notes="$def"
  fi
  [ -x "$MSYS_BASH" ] || die "找不到 MSYS2 bash: $MSYS_BASH（构建需要它）"
  package_installer "$ver" "$notes" "$skip_build"

  step "打包完成"
  printf '  安装包 : %s\n' "$SETUP"
  printf '  固定名 : dist/Mivara-Setup.exe\n'
  printf '  哈希   : %s\n' "$HASH"
  printf '  校验和 : dist/SHA256SUMS.txt\n'
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main_package "$@"
fi
