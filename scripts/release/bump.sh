#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — version bump
# ============================================================================
# Writes a new version into the single source (src/version.h), the README
# badges and the winget manifest directory. Idempotent: rerunning with the
# version already in the tree only verifies the files.
#
# Usage:
#   bash scripts/release/bump.sh 0.11.5
#   bash scripts/release/bump.sh 0.11.5 --no-winget   # no winget/<ver>/ clone
# ============================================================================

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

newest_winget_version() {
  ls -1 "$ROOT/winget" 2>/dev/null \
    | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1 || true
}

bump_version() {
  local ver="$1" do_winget="${2:-1}"
  validate_version "$ver"
  require_repo

  local cur
  cur="$(read_version)"
  step "版本落盘 ($cur → $ver)"

  if [ "$ver" != "$cur" ]; then
    sed -i "s/^#define MIVARA_VERSION \".*\"/#define MIVARA_VERSION \"$ver\"/" src/version.h
    grep -q "#define MIVARA_VERSION \"$ver\"" src/version.h || die "改写 src/version.h 失败"
    ok "src/version.h → $ver"

    local f
    for f in README.md README_EN.md; do
      [ -f "$f" ] || die "缺少 $f"
      sed -i "s/version-[0-9][0-9.]*-blue/version-$ver-blue/" "$f"
      grep -q "version-$ver-blue" "$f" || die "$f 的版本徽章没更新成功"
    done
    ok "README.md / README_EN.md 徽章 → $ver"
  else
    grep -q "version-$ver-blue" README.md    || die "README.md 徽章不是 $ver"
    grep -q "version-$ver-blue" README_EN.md || die "README_EN.md 徽章不是 $ver"
    ok "README 徽章已是 $ver"
  fi

  if [ "$do_winget" = 1 ]; then
    if [ ! -d "winget/$ver" ]; then
      local prev
      prev="$(newest_winget_version)"
      [ -n "$prev" ] || die "winget/ 下没有可参照的旧版本目录"
      cp -r "winget/$prev" "winget/$ver"
      # a plain version substitution rewrites PackageVersion, InstallerUrl,
      # ReleaseNotesUrl and DisplayName in one go; ReleaseDate + InstallerSha256
      # are fixed after the build (scripts/release/winget.sh).
      sed -i "s/${prev}/${ver}/g" "winget/$ver"/*.yaml
      ok "winget/$ver/ 由 $prev 生成"
    else
      ok "winget/$ver/ 已存在"
    fi
    local f
    for f in wzy-777.Mivara.yaml wzy-777.Mivara.installer.yaml wzy-777.Mivara.locale.zh-CN.yaml; do
      [ -f "winget/$ver/$f" ] || die "清单缺失: winget/$ver/$f"
      grep -q "PackageVersion: $ver" "winget/$ver/$f" || die "winget/$ver/$f 的 PackageVersion 不是 $ver"
    done
    ok "三份清单包内版本一致"
  else
    warn "跳过 winget 清单（--no-winget）"
  fi
}

main_bump() {
  local ver="" do_winget=1
  while [ $# -gt 0 ]; do
    case "$1" in
      -h|--help)   usage_from_header "$0" 16; exit 0 ;;
      --no-winget) do_winget=0 ;;
      -*)          die "unknown option: $1 (see --help)" ;;
      *)           [ -z "$ver" ] || die "多余参数: $1"; ver="$1" ;;
    esac
    shift
  done
  [ -n "$ver" ] || die "缺少版本号（例如 0.11.5）"
  bump_version "$ver" "$do_winget"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main_bump "$@"
fi
