#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — Linux packages for a release
# ============================================================================
# Builds the Linux packages in GitHub Actions on the tag that is being
# released, downloads the artifact into dist/, verifies it, appends the
# Linux hashes to the release notes and uploads the assets to the GitHub
# Release. Sourced by release.sh; also runnable on its own to repair a
# release whose Linux build failed.
#
# Usage:
#   bash scripts/release/linux.sh 0.13.0              # build, upload, edit notes
#   bash scripts/release/linux.sh 0.13.0 --no-upload   # build + fetch only
#   bash scripts/release/linux.sh 0.13.0 --reuse       # reuse dist/ artifacts
#   bash scripts/release/linux.sh 0.13.0 --notes dist/RELEASE_NOTES_v0.13.0.md
#
# The tag v<ver> must already be pushed. The workflow file must exist on that
# tag (it is part of the release commit). Requires gh with the `workflow`
# scope; the GCM token used by release.sh already has it.
# ============================================================================

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

DIST="${DIST:-$ROOT/dist}"
LINUX_ARCH="${LINUX_ARCH:-x86_64}"
DEB_ARCH="${DEB_ARCH:-amd64}"
LINUX_WORKFLOW="release-linux.yml"
LINUX_ARTIFACT="linux-packages"
LINUX_READY=0

linux_asset_names() {
  local ver="$1"
  printf '%s\n' \
    "Mivara-linux-${LINUX_ARCH}-${ver}.tar.gz" \
    "Mivara-linux-${LINUX_ARCH}.tar.gz" \
    "mivara_${ver}_${DEB_ARCH}.deb" \
    "Mivara-linux-${DEB_ARCH}.deb" \
    "SHA256SUMS-linux.txt"
}

linux_assets_present() {
  local ver="$1" n
  while IFS= read -r n; do
    [ -f "$DIST/$n" ] || return 1
  done < <(linux_asset_names "$ver")
  return 0
}

linux_verify_local() {
  local ver="$1"
  linux_assets_present "$ver" || die "dist/ 缺少 Linux 产物（需要 4 个包 + SHA256SUMS-linux.txt）"
  grep -q "  Mivara-linux-${LINUX_ARCH}-${ver}.tar.gz$" "$DIST/SHA256SUMS-linux.txt" \
    || die "dist/SHA256SUMS-linux.txt 不是 $ver 的产物"
  ( cd "$DIST" && sha256sum -c SHA256SUMS-linux.txt >/dev/null ) \
    || die "dist/SHA256SUMS-linux.txt 校验失败"
  cmp -s "$DIST/Mivara-linux-${LINUX_ARCH}-${ver}.tar.gz" \
         "$DIST/Mivara-linux-${LINUX_ARCH}.tar.gz" \
    || die "固定名 tar 与版本名文件不一致"
  cmp -s "$DIST/mivara_${ver}_${DEB_ARCH}.deb" \
         "$DIST/Mivara-linux-${DEB_ARCH}.deb" \
    || die "固定名 deb 与版本名文件不一致"
  ok "Linux 产物本地校验通过（$ver, ${LINUX_ARCH}/${DEB_ARCH}）"
}

linux_run_ids() {
  local sha="$1"
  gh_read run list --workflow="$LINUX_WORKFLOW" --event=workflow_dispatch --limit=50 \
    --json databaseId,headSha \
    --jq ".[] | select(.headSha == \"$sha\") | .databaseId"
}

linux_run_url() {
  printf '%s/actions/runs/%s' "$REPO_URL" "$1"
}

linux_dispatch_and_fetch() {
  local ver="$1" tag="$2" sha before id run_id="" i status conclusion deadline attempt
  require_repo
  [ -n "$(gh_bin)" ] || die "找不到 gh CLI（设置 GH=/path/to/gh.exe）"

  sha="$(git rev-parse "$tag^{commit}")" \
    || die "本地找不到 tag $tag（先 git fetch --tags）"

  before="$(linux_run_ids "$sha" 2>/dev/null | tr '\n' ' ' || true)"
  step "触发 Linux 打包（GitHub Actions · $tag）"
  # The tag was pushed seconds ago; GitHub may not have indexed the ref yet.
  for attempt in 1 2 3 4 5; do
    if gh_net workflow run "$LINUX_WORKFLOW" --ref "$tag" -f version="$ver"; then
      break
    fi
    [ "$attempt" = 5 ] && die "gh workflow run 失败。
  gh 需要 workflow scope（gh auth refresh -s workflow），且 $LINUX_WORKFLOW
  必须存在于 tag $tag 上。"
    warn "GitHub 尚未找到 tag $tag，5 秒后重试（$attempt/5）…"
    sleep 5
  done

  for i in $(seq 1 24); do
    for id in $(linux_run_ids "$sha"); do
      case " $before " in
        *" $id "*) ;;
        *) run_id="$id"; break 2 ;;
      esac
    done
    sleep 5
  done
  [ -n "$run_id" ] || die "2 分钟内没有找到本次 workflow_dispatch 的 run。
  检查：gh run list --workflow=$LINUX_WORKFLOW"
  ok "workflow run $run_id 已启动"

  # The workflow itself times out at 30 minutes; wait a little longer so its
  # failure is reported instead of a client-side timeout race.
  deadline=$(( $(date +%s) + 2100 ))
  while :; do
    read -r status conclusion < <(
      gh_read run view "$run_id" --json status,conclusion \
        --jq '[.status, (.conclusion // "")] | @tsv' 2>/dev/null \
        || printf 'unknown\t\n'
    )
    [ "$status" = completed ] && break
    [ "$(date +%s)" -ge "$deadline" ] \
      && die "Linux 构建超过 35 分钟仍未完成：$(linux_run_url "$run_id")"
    printf '    ... %s\n' "${status:-unknown}"
    sleep 10
  done
  if [ "$conclusion" != success ]; then
    gh_read run view "$run_id" --log-failed 2>/dev/null | tail -40 >&2 || true
    die "Linux 构建失败（$conclusion）：$(linux_run_url "$run_id")"
  fi
  ok "Linux 构建完成"

  step "下载 Linux 产物"
  rm -rf "$DIST/linux-ci"
  # The artifact is ~33 MB; gh_read's 60 s API timeout is far too short.
  # Retry once through the optional proxy if the direct download fails.
  if ! timeout_cmd 600 "$(gh_bin)" run download "$run_id" \
         --name "$LINUX_ARTIFACT" -D "$DIST/linux-ci"; then
    if [ -n "$PROXY" ]; then
      warn "直连下载失败，改用代理 $PROXY 重试"
      HTTPS_PROXY="$PROXY" HTTP_PROXY="$PROXY" \
        timeout_cmd 600 "$(gh_bin)" run download "$run_id" \
          --name "$LINUX_ARTIFACT" -D "$DIST/linux-ci" \
        || die "下载 CI 产物失败：$(linux_run_url "$run_id")"
    else
      die "下载 CI 产物失败：$(linux_run_url "$run_id")"
    fi
  fi
  local n src
  while IFS= read -r n; do
    # upload-artifact normally flattens the common dist/ prefix, but keep a
    # find fallback so a future path change does not break the release.
    src="$DIST/linux-ci"
    if [ ! -f "$src/$n" ] && [ -f "$src/dist/$n" ]; then src="$src/dist"; fi
    if [ ! -f "$src/$n" ]; then
      src="$(find "$DIST/linux-ci" -type f -name "$n" -print -quit 2>/dev/null || true)"
      [ -n "$src" ] && src="$(dirname "$src")"
    fi
    [ -n "$src" ] && [ -f "$src/$n" ] || die "CI 产物缺少 $n"
    cp -f "$src/$n" "$DIST/$n"
  done < <(linux_asset_names "$ver")
  ok "产物已复制到 dist/"
  linux_verify_local "$ver"
}

linux_stage() {
  local ver="$1" tag="$2" reuse="${3:-0}"
  if [ "$reuse" = 1 ] && linux_assets_present "$ver"; then
    warn "复用 dist/ 中已有的 Linux 产物（--skip-build）"
    linux_verify_local "$ver"
  else
    linux_dispatch_and_fetch "$ver" "$tag"
  fi
  LINUX_READY=1
}

linux_strip_notes() {
  local notes="$1" tmp
  [ -f "$notes" ] || return 0
  grep -q '^<!-- mivara-linux-assets -->$' "$notes" || return 0
  tmp="$(mktemp)"
  awk '
    /^<!-- mivara-linux-assets -->$/ { skip = 1; next }
    /^<!-- \/mivara-linux-assets -->$/ { skip = 0; next }
    !skip { print }
  ' "$notes" > "$tmp"
  mv -f "$tmp" "$notes"
}

linux_update_notes() {
  local notes="$1" ver="$2" tmp h n
  if [ ! -f "$notes" ]; then
    warn "找不到发行说明 $notes（Linux 哈希不写入发行说明）"
    return 0
  fi
  if ! linux_assets_present "$ver"; then
    warn "dist/ 缺少 Linux 产物，无法写入发行说明"
    return 1
  fi

  tmp="$(mktemp)"
  # Replace only our marked block, so rerunning never stacks sections and
  # never touches the Windows hash elsewhere in the notes.
  awk '
    /^<!-- mivara-linux-assets -->$/ { skip = 1; next }
    /^<!-- \/mivara-linux-assets -->$/ { skip = 0; next }
    !skip { print }
  ' "$notes" > "$tmp"

  {
    printf '\n<!-- mivara-linux-assets -->\n'
    printf '## Linux\n\n'
    printf '| 文件 | SHA-256 |\n'
    printf '|------|---------|\n'
    # Default IFS: a sha256sum line is "hash  name" and must split into two
    # fields. IFS= would put the whole line into $h and leave $n empty.
    while read -r h n; do
      [ -n "${h:-}" ] || continue
      printf '| `%s` | `%s` |\n' "$n" "$h"
    done < "$DIST/SHA256SUMS-linux.txt"
    printf '\n'
    printf 'Ubuntu/Debian 优先安装 `.deb`（`sudo apt install ./mivara_%s_%s.deb`）；其他发行版解压 `.tar.gz` 后运行 `./install.sh`。\n' \
      "$ver" "$DEB_ARCH"
    printf '预编译包需要 glibc 2.35+（构建于 Ubuntu 22.04）；更旧的系统请从源码构建。安装说明见 [构建与打包](%s/blob/main/docs/zh/building.md)。\n' \
      "$REPO_URL"
    printf '<!-- /mivara-linux-assets -->\n'
  } >> "$tmp"

  mv -f "$tmp" "$notes"
  ok "发行说明已写入 Linux 产物哈希"
}

linux_upload() {
  local tag="$1" ver="$2" n
  local args=()
  while IFS= read -r n; do
    args+=("$DIST/$n")
  done < <(linux_asset_names "$ver")
  step "上传 Linux 产物到 Release"
  gh_net release upload "$tag" "${args[@]}" --clobber \
    || die "gh release upload 失败（Release $tag 是否存在？）"
  ok "Linux 产物已上传"
}

main_linux() {
  local ver="" notes="" upload=1 reuse=0
  while [ $# -gt 0 ]; do
    case "$1" in
      -h|--help)   usage_from_header "$0" 20; exit 0 ;;
      --no-upload) upload=0 ;;
      --reuse)     reuse=1 ;;
      --notes)     shift; notes="${1:-}" ;;
      -*)          die "unknown option: $1 (see --help)" ;;
      *)           [ -z "$ver" ] || die "多余参数: $1"; ver="$1" ;;
    esac
    shift
  done
  require_repo
  ver="${ver:-$(read_version)}"
  validate_version "$ver"
  TAG="v$ver"
  [ -n "$notes" ] || notes="dist/RELEASE_NOTES_v${ver}.md"

  init_gh_auth
  gh_auth_ok || die "gh 未登录或 API 不可达（gh auth login，或设置 GH_TOKEN）"
  if [ "$upload" = 1 ]; then
    gh_read api "repos/${REPO_SLUG}/releases/tags/${TAG}" >/dev/null 2>&1 \
      || die "GitHub 上不存在 Release $TAG（先发布 Windows 版本，或使用 --no-upload）"
  fi

  linux_stage "$ver" "$TAG" "$reuse"
  linux_update_notes "$notes" "$ver"
  if [ "$upload" = 1 ]; then
    linux_upload "$TAG" "$ver"
    if [ -f "$notes" ]; then
      gh_net release edit "$TAG" --notes-file "$notes" >/dev/null \
        || warn "Release 说明更新失败，可手动粘贴 $notes"
    fi
    ok "Linux 版本已补齐到 $TAG"
  else
    warn "已跳过上传（--no-upload）；产物在 dist/"
  fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main_linux "$@"
fi
