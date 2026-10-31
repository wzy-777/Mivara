#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — finalize the winget manifest for a built installer
# ============================================================================
# Fills ReleaseDate and InstallerSha256 in winget/<ver>/ and checks that the
# URLs point at the versioned asset. Run after scripts/release/package.sh.
#
# Usage:
#   bash scripts/release/winget.sh 0.11.5            # hash from dist/
#   bash scripts/release/winget.sh 0.11.5 <sha256>
#
# This only edits the manifests; submitting the PR is winget/submit.sh <ver>.
# ============================================================================

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

finalize_winget() {
  local ver="$1" hash="${2:-}"
  validate_version "$ver"
  require_repo

  [ -n "$hash" ] || {
    [ -f "dist/Mivara-Setup-$ver.exe" ] \
      || die "缺少 dist/Mivara-Setup-$ver.exe（先运行 scripts/release/package.sh）"
    hash="$(sha256sum "dist/Mivara-Setup-$ver.exe" | awk '{print $1}')"
  }
  local hash_up
  hash_up="$(printf '%s' "$hash" | tr 'a-f' 'A-F')"

  step "winget 清单"
  local inst="winget/$ver/wzy-777.Mivara.installer.yaml"
  local locale="winget/$ver/wzy-777.Mivara.locale.zh-CN.yaml"
  [ -f "$inst" ] || die "清单缺失: $inst（先运行 scripts/release/bump.sh $ver）"
  [ -f "$locale" ] || die "清单缺失: $locale"

  local today
  today="$(date -u +%F)"
  sed -i "s|^ReleaseDate:.*|ReleaseDate: $today|" "$inst"
  sed -i "s|^  InstallerSha256:.*|  InstallerSha256: $hash_up|" "$inst"
  grep -q "InstallerSha256: $hash_up" "$inst"                     || die "清单 SHA-256 回填失败"
  grep -q "releases/download/v$ver/Mivara-Setup-$ver.exe" "$inst" || die "清单 InstallerUrl 与版本不符"
  grep -q "releases/tag/v$ver" "$locale"                          || die "清单 ReleaseNotesUrl 与版本不符"
  ok "ReleaseDate=$today  InstallerSha256=$hash_up"
  ok "InstallerUrl 使用带版本号的资产名（不要换成固定名）"
}

main_winget() {
  local ver="" hash=""
  while [ $# -gt 0 ]; do
    case "$1" in
      -h|--help) usage_from_header "$0" 16; exit 0 ;;
      -*)        die "unknown option: $1 (see --help)" ;;
      *)         if [ -z "$ver" ]; then ver="$1"; else hash="$1"; fi ;;
    esac
    shift
  done
  [ -n "$ver" ] || die "缺少版本号（例如 0.11.5）"
  finalize_winget "$ver" "$hash"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main_winget "$@"
fi
