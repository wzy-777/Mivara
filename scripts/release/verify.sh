#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — verify a published release
# ============================================================================
# Checks the published assets against the local dist/ artifacts: the
# releases/latest redirect, the asset list, GitHub's server-computed digests
# and (optionally) a real download of the fixed-name installer.
#
# Usage:
#   bash scripts/release/verify.sh 0.11.5
#   bash scripts/release/verify.sh 0.11.5 --verify-download
#   bash scripts/release/verify.sh 0.11.5 --no-verify-download
#
# This is also the recovery path for an aborted release: it never builds,
# commits or pushes, so it can be rerun after a network failure.
#
# When dist/SHA256SUMS-linux.txt lists the version being verified, the four
# Linux assets and their server digests are checked as well.
# ============================================================================

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

# ---------------------------------------------------------------------------
# Published-release verification
# ---------------------------------------------------------------------------
# GitHub's release API reports a server-computed `digest` for each asset. That
# checks the published bytes without downloading them, which matters when the
# release CDN is unreachable from this machine (TLS reset / proxy required) even
# though github.com and the API work. The small SHA256SUMS.txt is still fetched
# as a CDN sanity check; the 70 MB installer is re-downloaded only when the
# digests are missing or --verify-download was given.
ASSETS_TSV=""
asset_field() {
  printf '%s\n' "$ASSETS_TSV" \
    | awk -F'\t' -v n="$1" -v f="$2" '$1 == n { print $f; exit }'
}

verify_release() {
  : "${VER:?}" "${TAG:?}" "${HASH:?}"
  step "线上校验"

  LATEST="$(curl_net -sI -o /dev/null -w '%{redirect_url}' "$REPO_URL/releases/latest" 2>/dev/null || true)"
  case "$LATEST" in
    */tag/"$TAG") ok "releases/latest → $TAG" ;;
    *)
      API_LATEST="$(gh_read api "repos/${REPO_SLUG}/releases/latest" --jq .tag_name 2>/dev/null || true)"
      if [ "$API_LATEST" = "$TAG" ]; then
        warn "本机无法读取 releases/latest 的重定向（${LATEST:-无响应}），但 GitHub API 显示 latest=$TAG"
      else
        die "releases/latest 没有指向 $TAG（重定向: ${LATEST:-<无>}，API: ${API_LATEST:-<无>}）—— 更新器会认为没有新版本"
      fi
      ;;
  esac

  ASSETS_TSV="$(gh_read api "repos/${REPO_SLUG}/releases/tags/${TAG}" \
                  --jq '.assets[] | [.name, .size, (.digest // "")] | @tsv')" \
    || die "无法读取 Release $TAG 的资产列表（API 连续超时）"
  ASSETS="$(printf '%s\n' "$ASSETS_TSV" | awk -F'\t' 'NF { printf "%s ", $1 }')"
  for a in "Mivara-Setup-$VER.exe" "Mivara-Setup.exe" "SHA256SUMS.txt"; do
    [ -n "$(asset_field "$a" 1)" ] || die "Release 缺少资产: $a（现有: $ASSETS）"
  done

  # Linux assets are optional for older releases and --no-linux, but when the
  # local SHA256SUMS-linux.txt lists THIS version they are part of this
  # release and must be checked like the Windows ones. (dist/ can still hold
  # the previous release's checksum file, so never trust its mere presence.)
  LINUX_ARCH="${LINUX_ARCH:-x86_64}"
  DEB_ARCH="${DEB_ARCH:-amd64}"
  LINUX_NAMES=()
  LINUX_SUMS_DIGEST=""
  LINUX_SUMS_CURRENT=0
  if [ -f dist/SHA256SUMS-linux.txt ]; then
    if grep -q "  Mivara-linux-${LINUX_ARCH}-${VER}.tar.gz$" dist/SHA256SUMS-linux.txt; then
      LINUX_SUMS_CURRENT=1
    else
      warn "dist/SHA256SUMS-linux.txt 不是 $VER 的产物，跳过 Linux 校验"
    fi
  fi
  if [ "$LINUX_SUMS_CURRENT" = 1 ]; then
    # Default IFS splits the sha256sum line into hash + name (IFS= would keep
    # the whole line in $lh and leave $ln empty).
    while read -r lh ln; do
      [ -n "${ln:-}" ] || continue
      case "$lh" in
        *[!0-9a-fA-F]*) die "dist/SHA256SUMS-linux.txt 行格式异常: $lh $ln" ;;
      esac
      LINUX_NAMES+=("$ln")
    done < dist/SHA256SUMS-linux.txt
    [ "${#LINUX_NAMES[@]}" -gt 0 ] || die "dist/SHA256SUMS-linux.txt 为空"
    [ -n "$(asset_field SHA256SUMS-linux.txt 1)" ] \
      || die "Release 缺少 Linux 资产: SHA256SUMS-linux.txt（现有: $ASSETS）"
    for a in "${LINUX_NAMES[@]}"; do
      [ -n "$(asset_field "$a" 1)" ] || die "Release 缺少 Linux 资产: $a（现有: $ASSETS）"
    done
    ok "Windows + Linux 资产齐全: $ASSETS"
  else
    ok "三个 Windows 资产齐全: $ASSETS"
  fi

  SUMS_LOCAL_HASH="$(sha256sum dist/SHA256SUMS.txt | awk '{print $1}')"
  SUMS_DIGEST="$(asset_field SHA256SUMS.txt 3)"
  DIGEST_OK=1
  for a in "Mivara-Setup-$VER.exe" "Mivara-Setup.exe"; do
    d="$(asset_field "$a" 3)"
    if [ -n "$d" ]; then
      [ "$d" = "sha256:$HASH" ] \
        || die "$a 的服务端摘要不符（$d ≠ sha256:$HASH）"
    else
      DIGEST_OK=0
    fi
  done
  if [ -n "$SUMS_DIGEST" ]; then
    [ "$SUMS_DIGEST" = "sha256:$SUMS_LOCAL_HASH" ] \
      || die "SHA256SUMS.txt 的服务端摘要不符（$SUMS_DIGEST ≠ sha256:$SUMS_LOCAL_HASH）"
  else
    DIGEST_OK=0
  fi

  if [ "${#LINUX_NAMES[@]}" -gt 0 ]; then
    for a in "${LINUX_NAMES[@]}"; do
      case "$a" in SHA256SUMS-linux.txt) continue ;; esac
      want="$(awk -v n="$a" '$2 == n { print $1; exit }' dist/SHA256SUMS-linux.txt)"
      [ -n "$want" ] || die "SHA256SUMS-linux.txt 缺少 $a 的哈希"
      d="$(asset_field "$a" 3)"
      if [ -n "$d" ]; then
        [ "$d" = "sha256:$want" ] \
          || die "$a 的服务端摘要不符（$d ≠ sha256:$want）"
      else
        DIGEST_OK=0
      fi
    done
    LINUX_SUMS_DIGEST="$(asset_field SHA256SUMS-linux.txt 3)"
    if [ -n "$LINUX_SUMS_DIGEST" ]; then
      LINUX_SUMS_HASH="$(sha256sum dist/SHA256SUMS-linux.txt | awk '{print $1}')"
      [ "$LINUX_SUMS_DIGEST" = "sha256:$LINUX_SUMS_HASH" ] \
        || die "SHA256SUMS-linux.txt 的服务端摘要不符（$LINUX_SUMS_DIGEST ≠ sha256:$LINUX_SUMS_HASH）"
    else
      DIGEST_OK=0
    fi
  fi

  if [ "$DIGEST_OK" = 1 ]; then
    if [ "${#LINUX_NAMES[@]}" -gt 0 ]; then
      ok "GitHub 服务端摘要与本地构建一致（Windows + Linux 资产）"
    else
      ok "GitHub 服务端摘要与本地构建一致（Windows 资产）"
    fi
  else
    warn "GitHub 未返回完整的资产摘要，线上字节只能靠下载复验"
  fi

  case "$VERIFY_DOWNLOAD" in
    none)
      if [ "$DIGEST_OK" = 1 ]; then
        warn "已跳过全部下载复验（--no-verify-download）；服务端摘要已校验"
        return 0
      fi
      die "--no-verify-download 且 GitHub 未返回资产摘要：无法验证线上字节"
      ;;
    full) FULL_DL=1 ;;
    *)    if [ "$DIGEST_OK" = 1 ]; then FULL_DL=0; else FULL_DL=1; fi ;;
  esac

  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT

  # The small checksum file is cheap and proves the release CDN serves this
  # release; do it even when the digests already matched.
  if curl_net -fsSL -o "$TMP/SHA256SUMS.txt" \
       "$REPO_URL/releases/download/$TAG/SHA256SUMS.txt"; then
    diff -q "$TMP/SHA256SUMS.txt" dist/SHA256SUMS.txt >/dev/null \
      || die "已发布的 SHA256SUMS.txt 与本地不一致"
    ok "已发布的 SHA256SUMS.txt 与本地一致"
  elif [ -n "$SUMS_DIGEST" ]; then
    warn "本机无法直接下载 SHA256SUMS.txt；服务端摘要已校验，下载复验跳过"
  else
    die "下载已发布的 SHA256SUMS.txt 失败，且服务端没有摘要可校验"
  fi

  if [ "${#LINUX_NAMES[@]}" -gt 0 ]; then
    if curl_net -fsSL -o "$TMP/SHA256SUMS-linux.txt" \
         "$REPO_URL/releases/download/$TAG/SHA256SUMS-linux.txt"; then
      diff -q "$TMP/SHA256SUMS-linux.txt" dist/SHA256SUMS-linux.txt >/dev/null \
        || die "已发布的 SHA256SUMS-linux.txt 与本地不一致"
      ok "已发布的 SHA256SUMS-linux.txt 与本地一致"
    elif [ -n "$LINUX_SUMS_DIGEST" ]; then
      warn "本机无法直接下载 SHA256SUMS-linux.txt；服务端摘要已校验，下载复验跳过"
    else
      die "下载已发布的 SHA256SUMS-linux.txt 失败，且服务端没有摘要可校验"
    fi
  fi

  if [ "$FULL_DL" = 0 ]; then
    ok "服务端摘要已覆盖资产字节，跳过下载（--verify-download 可强制复验）"
    return 0
  fi

  if curl_net -fsSL -o "$TMP/Mivara-Setup.exe" \
       "$REPO_URL/releases/latest/download/Mivara-Setup.exe"; then
    DL_HASH="$(sha256sum "$TMP/Mivara-Setup.exe" | awk '{print $1}')"
    [ "$DL_HASH" = "$HASH" ] \
      || die "下载回来的安装包哈希不符（线上 $DL_HASH ≠ 本地 $HASH）"
    ok "固定名直链下载的安装包哈希与本地一致 —— 应用内更新会校验通过"
  elif [ -n "$(asset_field Mivara-Setup.exe 3)" ]; then
    warn "本机无法直接下载固定名安装包；服务端摘要已校验，下载复验跳过"
  else
    die "从固定名永久直链下载失败，且服务端没有摘要可校验"
  fi

  # Linux assets are small compared with the Windows installer; --verify-download
  # re-fetches every one of them.
  local n want got
  for n in "${LINUX_NAMES[@]}"; do
    case "$n" in SHA256SUMS-linux.txt) continue ;; esac
    want="$(awk -v x="$n" '$2 == x { print $1; exit }' dist/SHA256SUMS-linux.txt)"
    if curl_net -fsSL -o "$TMP/$n" "$REPO_URL/releases/download/$TAG/$n"; then
      got="$(sha256sum "$TMP/$n" | awk '{print $1}')"
      [ "$got" = "$want" ] \
        || die "下载回来的 $n 哈希不符（线上 $got ≠ 本地 $want）"
      ok "$n 下载复验通过"
    elif [ -n "$(asset_field "$n" 3)" ]; then
      warn "本机无法直接下载 $n；服务端摘要已校验，下载复验跳过"
    else
      die "下载 $n 失败，且服务端没有摘要可校验"
    fi
  done
}

main_verify() {
  local ver="" want="auto"
  while [ $# -gt 0 ]; do
    case "$1" in
      -h|--help)            usage_from_header "$0" 18; exit 0 ;;
      --verify-download)    want=full ;;
      --no-verify-download) want=none ;;
      -*)                   die "unknown option: $1 (see --help)" ;;
      *)                    [ -z "$ver" ] || die "多余参数: $1"; ver="$1" ;;
    esac
    shift
  done
  require_repo
  ver="${ver:-$(read_version)}"
  validate_version "$ver"
  VER="$ver"
  TAG="v${VER}"
  VERIFY_DOWNLOAD="$want"

  [ -f "dist/Mivara-Setup-$VER.exe" ] \
    || die "需要本地安装包: dist/Mivara-Setup-$VER.exe"
  [ -f dist/SHA256SUMS.txt ] || die "需要 dist/SHA256SUMS.txt"
  HASH="$(sha256sum "dist/Mivara-Setup-$VER.exe" | awk '{print $1}')"

  init_gh_auth
  gh_auth_ok || die "gh 未登录或 API 不可达（gh auth login，或设置 GH_TOKEN）"
  timeout_cmd 60 git ls-remote --tags "$REMOTE" "refs/tags/$TAG" | grep -q . \
    || die "远端没有 tag $TAG —— 这不是一个已发布的版本"
  ok "tag $TAG 已推送"

  verify_release
  step "校验完成（未构建、未提交、未推送）"
  printf '  Release : %s/releases/tag/%s\n' "$REPO_URL" "$TAG"
  printf '  SHA-256 : %s\n' "$HASH"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main_verify "$@"
fi
