#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — one-shot release
# ============================================================================
#   build (portable) → artifacts → commit → push → tag → GitHub Release →
#   winget PR → verify
#
# Usage:
#   ./release.sh 0.9.3              # bump + full release
#   ./release.sh                    # release the version already in src/version.h
#   ./release.sh 0.9.3 --dry-run    # everything except commit/push/tag/release
#   ./release.sh 0.9.3 --yes        # no confirmation prompt (CI-like use)
#   ./release.sh 0.9.3 --skip-build # reuse dist/ artifacts from a previous run
#   ./release.sh 0.9.3 --no-winget  # do not create/update winget/<ver>/
#   ./release.sh 0.9.3 --no-winget-pr  # keep winget/<ver>/ but do not submit the PR
#   ./release.sh 0.9.3 --verify-only   # re-check an already published release
#   ./release.sh 0.9.3 --verify-download  # force re-downloading the published assets
#   ./release.sh 0.9.3 --no-verify-download  # digests only, no CDN download at all
#
# What it automates (AGENTS.md "Installer" checklist):
#   * portable build  : make clean + OPT_FLAGS="-O3 -march=x86-64 -flto"
#   * installer       : ISCC → dist/Mivara-Setup-<ver>.exe + fixed-name copy
#   * SHA256SUMS.txt  : the in-app updater is fail-closed without it
#   * version bump    : src/version.h + both README badges + winget/<ver>/
#   * publish         : commit → push → lightweight tag → GitHub Release (3 assets)
#   * winget PR       : winget/submit.sh creates/updates the microsoft/winget-pkgs
#                       PR in place (one PR per package — see that script)
#   * verification    : releases/latest redirect, asset list and server-side asset
#                       digests (no 70 MB re-download by default — GitHub computes
#                       the digest from the uploaded bytes; --verify-download opts
#                       back into the end-to-end download)
#
# Requirements: run from Git Bash (git + gh must work from it). The build itself
# is delegated to MSYS2 bash because /mingw64 is not mounted in Git Bash.
# ============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration (override via environment)
# ---------------------------------------------------------------------------
REPO_SLUG="${REPO_SLUG:-wzy-777/Mivara}"
REPO_URL="https://github.com/${REPO_SLUG}"
REMOTE="${REMOTE:-origin}"
BRANCH="${BRANCH:-main}"
PROXY="${PROXY:-}"   # optional; only tried after a direct attempt fails
PORTABLE_FLAGS="-O3 -march=x86-64 -flto" # repository MUST be archived with this

# ---------------------------------------------------------------------------
# Terminal helpers
# ---------------------------------------------------------------------------
step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m  %s\n' "$*"; }
warn() { printf '    \033[33m!\033[0m   %s\n' "$*" >&2; }
die()  { printf '\n\033[31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
usage() { sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }
# GNU timeout is present in Git Bash and Linux, but not in stock macOS.
timeout_cmd() {
  local d="$1"; shift
  if command -v timeout >/dev/null 2>&1; then timeout "$d" "$@"; else "$@"; fi
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
VER=""
DRY_RUN=0
YES=0
SKIP_BUILD=0
DO_WINGET=1
DO_WINGET_PR=1
VERIFY_DOWNLOAD=auto   # auto | full | none
VERIFY_ONLY=0
NOTES=""
TITLE=""

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)        usage ;;
    --dry-run)        DRY_RUN=1 ;;
    --yes|-y)         YES=1 ;;
    --skip-build)     SKIP_BUILD=1 ;;
    --no-winget)      DO_WINGET=0 ;;
    --no-winget-pr)   DO_WINGET_PR=0 ;;
    --verify-only)    VERIFY_ONLY=1 ;;
    --verify-download) VERIFY_DOWNLOAD=full ;;
    --no-verify-download) VERIFY_DOWNLOAD=none ;;
    --notes)          shift; NOTES="${1:-}" ;;
    --title)          shift; TITLE="${1:-}" ;;
    --proxy)          shift; PROXY="${1:-}" ;;
    -*)               die "unknown option: $1 (see --help)" ;;
    *)                [ -z "$VER" ] || die "多余参数: $1"; VER="$1" ;;
  esac
  shift
done

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
cd "$(dirname "$0")"
ROOT="$(pwd -P)"
[ -f makefile ] && [ -d src ] || die "看起来不是 Mivara 仓库根目录: $ROOT"

MSYS_BASH="${MSYS_BASH:-C:/msys64/usr/bin/bash.exe}"

detect_iscc() {
  if [ -n "${ISCC:-}" ]; then printf '%s' "$ISCC"; return; fi
  if command -v iscc >/dev/null 2>&1; then command -v iscc; return; fi
  # Inno Setup 6/7, machine-wide (any drive) or per-user. Winget installed it
  # per-user on this machine, and Program Files is not always on C:.
  for base in "/c/Program Files" "/c/Program Files (x86)" \
              "/d/Program Files" "/d/Program Files (x86)" \
              "/e/Program Files" "/e/Program Files (x86)"; do
    for v in 7 6; do
      p="$base/Inno Setup $v/ISCC.exe"
      if [ -x "$p" ]; then printf '%s' "$p"; return; fi
    done
  done
  if [ -n "${LOCALAPPDATA:-}" ]; then
    local appdata
    appdata="$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || printf '%s' "$LOCALAPPDATA")"
    for v in 7 6; do
      p="$appdata/Programs/Inno Setup $v/ISCC.exe"
      if [ -x "$p" ]; then printf '%s' "$p"; return; fi
    done
  fi
  printf ''
}
ISCC_BIN="$(detect_iscc)"

detect_gh() {
  if [ -n "${GH:-}" ] && [ -x "${GH}" ]; then printf '%s' "$GH"; return; fi
  if command -v gh >/dev/null 2>&1; then command -v gh; return; fi
  for p in "/c/Program Files/GitHub CLI/gh.exe" "/c/Program Files (x86)/GitHub CLI/gh.exe" \
           "/usr/bin/gh" "/usr/local/bin/gh" "/opt/homebrew/bin/gh"; do
    if [ -x "$p" ]; then printf '%s' "$p"; return; fi
  done
  # winget installs to a versioned package directory and only adds a shim to
  # the Windows PATH; Git Bash started before the install does not see it.
  if [ -n "${LOCALAPPDATA:-}" ]; then
    local appdata
    appdata="$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || printf '%s' "$LOCALAPPDATA")"
    for p in "$appdata"/Microsoft/WinGet/Links/gh.exe \
             "$appdata"/Microsoft/WinGet/Packages/GitHub.cli_*/bin/gh.exe \
             "$appdata"/Programs/GitHub\ CLI/gh.exe; do
      if [ -x "$p" ]; then printf '%s' "$p"; return; fi
    done
  fi
  printf ''
}
GH_BIN="$(detect_gh)"

gh() { "$GH_BIN" "$@"; }

# Read-only GitHub calls retry with backoff. This network intermittently drops
# api.github.com's TLS handshake; a single timeout must not look like a missing
# release or an invalid token.
gh_read() {
  local n=0 delay=2
  until timeout_cmd 60 "$GH_BIN" "$@"; do
    n=$((n + 1))
    [ "$n" -ge 4 ] && return 1
    sleep "$delay"
    delay=$((delay * 2))
  done
}
gh_auth_ok() { gh_read auth status >/dev/null 2>&1; }

# If gh has no stored login, reuse the token Git Credential Manager already
# holds for github.com. This is what release runs have used manually; doing it
# here means `./release.sh` works in a fresh shell without exporting GH_TOKEN.
# Only consult GCM when gh itself is not authenticated, so a normal gh login is
# never overridden.
if [ -n "$GH_BIN" ] && [ -z "${GH_TOKEN:-}" ] && [ -z "${GITHUB_TOKEN:-}" ]; then
  if ! timeout_cmd 30 "$GH_BIN" auth status >/dev/null 2>&1; then
    # GCM can return an expired stored OAuth token; a git network call gives it
    # the 401 it needs to refresh, and the next fill returns the new value.
    timeout_cmd 60 git ls-remote --heads "$REMOTE" >/dev/null 2>&1 || true
    GCM_TOKEN="$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill 2>/dev/null \
                   | sed -n 's/^password=//p' || true)"
    if [ -n "$GCM_TOKEN" ]; then
      export GH_TOKEN="$GCM_TOKEN"
      # curl reaches api.github.com more reliably than gh here; a failed check
      # is not proof the token is bad (it is usually a TLS timeout), so keep it
      # and let the retrying callers report a real auth error.
      if curl -fsS -m 15 -H "Authorization: Bearer $GCM_TOKEN" \
           -H "User-Agent: Mivara-release" https://api.github.com/user >/dev/null 2>&1; then
        ok "使用 Git Credential Manager 中的 GitHub 凭据"
      else
        warn "Git Credential Manager 凭据暂时无法验证（网络超时）；仍将尝试使用"
      fi
    fi
  fi
fi

git_net() {
  if git "$@"; then return 0; fi
  [ -n "$PROXY" ] || return 1
  warn "直接连接失败，改用代理 $PROXY 重试"
  git -c "http.proxy=$PROXY" "$@"
}

gh_net() {
  if gh "$@"; then return 0; fi
  [ -n "$PROXY" ] || return 1
  warn "直接连接失败，改用代理 $PROXY 重试"
  HTTPS_PROXY="$PROXY" HTTP_PROXY="$PROXY" gh "$@"
}

# Optional proxy fallback: only used when the direct attempt fails.
# A verification step must not report a false failure after a good release.
curl_net() {
  local n=0 errf
  # Never let a dead CDN hang the release: give up connecting quickly, and
  # abort a stalled body after 60 s below 4 KB/s. Slow-but-alive transfers
  # still finish. curl's per-attempt stderr is held back and only printed if
  # every attempt fails, so a successful retry does not spray errors.
  local opts=(--connect-timeout 10 --speed-limit 4096 --speed-time 60)
  errf="$(mktemp)"
  if curl "${opts[@]}" "$@" 2>"$errf"; then rm -f "$errf"; return 0; fi
  if [ -n "$PROXY" ]; then
    warn "curl 直连失败，改用代理 $PROXY 重试"
    if curl -x "$PROXY" "${opts[@]}" "$@" 2>"$errf"; then rm -f "$errf"; return 0; fi
  fi
  # Direct connections to the release CDN are reset intermittently; retry a
  # couple of times before treating the download as unavailable.
  while [ "$n" -lt 2 ]; do
    n=$((n + 1))
    sleep 2
    if curl "${opts[@]}" "$@" 2>"$errf"; then rm -f "$errf"; return 0; fi
  done
  cat "$errf" >&2
  rm -f "$errf"
  return 1
}

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
  ok "三个资产齐全: $ASSETS"

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
  if [ "$DIGEST_OK" = 1 ]; then
    ok "GitHub 服务端摘要与本地构建一致（两个安装包 + SHA256SUMS.txt）"
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

  if [ "$FULL_DL" = 0 ]; then
    ok "服务端摘要已覆盖安装包字节，跳过 70 MB 下载（--verify-download 可强制复验）"
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
}

# ---------------------------------------------------------------------------
# 1. Resolve version
# ---------------------------------------------------------------------------
step "解析版本号"

CUR_VER="$(sed -n 's/^#define[[:space:]]\+MIVARA_VERSION[[:space:]]\+"\([^"]*\)".*/\1/p' src/version.h)"
[ -n "$CUR_VER" ] || die "src/version.h 里找不到 MIVARA_VERSION"
[ -z "$VER" ] && VER="$CUR_VER"

echo "$VER" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
  || die "版本号必须是 X.Y.Z 形式（不带 v）: $VER"

TITLE="${TITLE:-Mivara $VER}"
NOTES="${NOTES:-dist/RELEASE_NOTES_v${VER}.md}"
TAG="v${VER}"

if [ "$VER" = "$CUR_VER" ]; then
  ok "src/version.h 已是 $VER"
else
  ok "src/version.h 为 $CUR_VER → 将升级到 $VER"
fi
ok "tag = $TAG   标题 = $TITLE"

# ---------------------------------------------------------------------------
# 1b. Verification-only recovery path
# ---------------------------------------------------------------------------
# A release that is already public must be re-checkable without rebuilding,
# committing or touching tags. This is also the safe way to finish after a
# local CDN/network failure in the normal verification step.
if [ "$VERIFY_ONLY" = 1 ]; then
  step "只校验 $TAG"
  [ -n "$GH_BIN" ] || die "找不到 gh CLI（设置 GH=/path/to/gh.exe）"
  [ -f "dist/Mivara-Setup-$VER.exe" ] \
    || die "--verify-only 需要本地安装包: dist/Mivara-Setup-$VER.exe"
  [ -f dist/SHA256SUMS.txt ] || die "--verify-only 需要 dist/SHA256SUMS.txt"
  HASH="$(sha256sum "dist/Mivara-Setup-$VER.exe" | awk '{print $1}')"
  gh_auth_ok || die "gh 未登录或 API 不可达（gh auth login，或设置 GH_TOKEN）"
  timeout_cmd 60 git ls-remote --tags "$REMOTE" "refs/tags/$TAG" | grep -q . \
    || die "远端没有 tag $TAG —— 这不是一个已发布的版本"
  ok "tag $TAG 已推送"
  verify_release
  step "校验完成（未构建、未提交、未推送）"
  printf '  Release : %s/releases/tag/%s\n' "$REPO_URL" "$TAG"
  printf '  SHA-256 : %s\n' "$HASH"
  exit 0
fi

# ---------------------------------------------------------------------------
# 2. Preflight
# ---------------------------------------------------------------------------
step "预检"

[ -x "$MSYS_BASH" ] || die "找不到 MSYS2 bash: $MSYS_BASH（构建需要它）"
[ -n "$GH_BIN" ]    || die "找不到 gh CLI（设置 GH=/path/to/gh.exe）"

# src/version.h must stay comment-free — ISPP aborts on an inline comment
grep -Eq 'MIVARA_VERSION.*//' src/version.h && die "src/version.h 的 MIVARA_VERSION 行不能有 // 注释（ISCC 会中止）"

CUR_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[ "$CUR_BRANCH" = "$BRANCH" ] || die "当前分支是 $CUR_BRANCH，发布必须在 $BRANCH 上"
ok "分支 $BRANCH"

if [ -n "$(git status --porcelain)" ]; then
  git status --short >&2
  die "工作区不干净。请先提交或清理后再发布（本脚本自己产生的改动除外，但它要求从干净状态开始）"
fi
ok "工作区干净"

# Packaging a new version from old binaries would silently produce an installer
# labelled X.Y.Z that contains the previous build.
if [ "$SKIP_BUILD" = 1 ] && [ "$VER" != "$CUR_VER" ]; then
  die "--skip-build 不能用于版本变更：bin/ 里的二进制还是 $CUR_VER，装出来的包会标错版本"
fi

git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "本地已存在 tag $TAG"
ok "本地无 tag $TAG"

REMOTE_TAG="$(timeout_cmd 60 git ls-remote --tags "$REMOTE" "refs/tags/$TAG" 2>/dev/null || true)"
[ -z "$REMOTE_TAG" ] || die "远端已存在 tag $TAG —— 这个版本发过了"
ok "远端无 tag $TAG"

gh_auth_ok || die "gh 未登录或 API 不可达（gh auth login，或设置 GH_TOKEN）"
if gh_read api "repos/${REPO_SLUG}/releases/tags/${TAG}" >/dev/null 2>&1; then
  die "GitHub 上已存在 release $TAG"
fi
ok "GitHub 上无同名 release"

[ -f "$NOTES" ] || die "缺少发行说明: $NOTES
  请先写好（正文 + SHA-256 校验段），再运行本脚本"
ok "发行说明 $NOTES"

if [ "$DRY_RUN" = 1 ]; then warn "DRY RUN —— 不会 commit / push / tag / release"; fi

# ---------------------------------------------------------------------------
# 3. Version bump (version.h, README badges, winget/<ver>/)
# ---------------------------------------------------------------------------
step "版本落盘"

if [ "$VER" != "$CUR_VER" ]; then
  sed -i "s/^#define MIVARA_VERSION \".*\"/#define MIVARA_VERSION \"$VER\"/" src/version.h
  grep -q "#define MIVARA_VERSION \"$VER\"" src/version.h || die "改写 src/version.h 失败"
  ok "src/version.h → $VER"

  for f in README.md README_EN.md; do
    [ -f "$f" ] || die "缺少 $f"
    sed -i "s/version-[0-9][0-9.]*-blue/version-$VER-blue/" "$f"
    grep -q "version-$VER-blue" "$f" || die "$f 的版本徽章没更新成功"
  done
  ok "README.md / README_EN.md 徽章 → $VER"
else
  grep -q "version-$VER-blue" README.md    || die "README.md 徽章不是 $VER"
  grep -q "version-$VER-blue" README_EN.md || die "README_EN.md 徽章不是 $VER"
  ok "README 徽章已是 $VER"
fi

PREV_VER="$(ls -1 winget 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1 || true)"
WINGET_CREATED=0

if [ "$DO_WINGET" = 1 ]; then
  if [ ! -d "winget/$VER" ]; then
    [ -n "$PREV_VER" ] || die "winget/ 下没有可参照的旧版本目录"
    cp -r "winget/$PREV_VER" "winget/$VER"
    WINGET_CREATED=1
    # a plain version substitution rewrites PackageVersion, InstallerUrl,
    # ReleaseNotesUrl and DisplayName in one go; ReleaseDate + InstallerSha256
    # are fixed after the build below.
    sed -i "s/${PREV_VER}/${VER}/g" "winget/$VER"/*.yaml
    ok "winget/$VER/ 由 $PREV_VER 生成"
  else
    ok "winget/$VER/ 已存在"
  fi
  for f in wzy-777.Mivara.yaml wzy-777.Mivara.installer.yaml wzy-777.Mivara.locale.zh-CN.yaml; do
    [ -f "winget/$VER/$f" ] || die "清单缺失: winget/$VER/$f"
    grep -q "PackageVersion: $VER" "winget/$VER/$f" || die "winget/$VER/$f 的 PackageVersion 不是 $VER"
  done
  ok "三份清单包内版本一致"
else
  warn "跳过 winget 清单（--no-winget）"
fi

# ---------------------------------------------------------------------------
# 4. Build
# ---------------------------------------------------------------------------
step "构建"
if [ "$SKIP_BUILD" = 1 ]; then
  warn "跳过构建（--skip-build），复用 dist/ 里的旧产物"
else
  echo "    make clean && make OPT_FLAGS=\"$PORTABLE_FLAGS\" && make check"
  "$MSYS_BASH" -lc "export PATH=/mingw64/bin:/usr/bin:\$PATH; export OS=Windows_NT; cd \"$ROOT\"; make clean && make OPT_FLAGS=\"$PORTABLE_FLAGS\" && make check" \
    || die "构建失败"
  ok "三个二进制已重建（portable flags）"
fi

for b in app sender receiver; do
  [ -f "bin/$b.exe" ] || die "构建产物缺失: bin/$b.exe"
  grep -aq "$VER" "bin/$b.exe" || die "bin/$b.exe 里找不到版本号 $VER —— 构建可能是旧的"
done
# stitchcap.exe has no version string, but a missing helper breaks file drop and
# the installer packages bin/* wholesale, so its presence is part of the check.
[ -f bin/stitchcap.exe ] || die "构建产物缺失: bin/stitchcap.exe（拼接文件投放助手）"
ok "bin/app.exe · sender.exe · receiver.exe 均为 $VER，stitchcap.exe 已就绪"

# ---------------------------------------------------------------------------
# 5. Installer + checksums
# ---------------------------------------------------------------------------
step "打包安装包"
[ -x "$ISCC_BIN" ] || die "找不到 Inno Setup 编译器: $ISCC_BIN（设置 ISCC=/path/ISCC.exe）"

if [ "$SKIP_BUILD" = 1 ] && [ -f "dist/Mivara-Setup-$VER.exe" ]; then
  warn "复用已存在的 dist/Mivara-Setup-$VER.exe"
else
  "$ISCC_BIN" installer/Mivara.iss >/dev/null || die "ISCC 失败"
fi

SETUP="dist/Mivara-Setup-$VER.exe"
[ -f "$SETUP" ] || die "ISCC 没有产出 $SETUP"
cp -f "$SETUP" dist/Mivara-Setup.exe
cmp -s "$SETUP" dist/Mivara-Setup.exe || die "固定名副本与版本名文件不一致"
ok "dist/Mivara-Setup.exe（固定名副本，更新器下载的就是它）"

HASH="$(sha256sum "$SETUP" | awk '{print $1}')"
HASH_UP="$(printf '%s' "$HASH" | tr 'a-f' 'A-F')"
# sha256sum's own "hash *name" format — the form the in-app updater was verified against
{
  printf '%s *%s\n' "$HASH" "Mivara-Setup-$VER.exe"
  printf '%s *%s\n' "$HASH" "Mivara-Setup.exe"
} > dist/SHA256SUMS.txt
ok "SHA-256 $HASH"

# Release notes must quote the asset hash (AGENTS.md requirement). The hash is
# only known once the installer exists, so refresh whatever 64-hex token is
# already in the notes instead of making the user rebuild to learn the number.
if grep -Eq '[0-9a-fA-F]{64}' "$NOTES"; then
  if ! grep -qi "$HASH" "$NOTES"; then
    sed -i -E "s/[0-9a-fA-F]{64}/$HASH/g" "$NOTES"
    warn "发行说明里的旧哈希已替换为本次的 $HASH"
  fi
else
  printf '\n**SHA-256**：`%s`\n' "$HASH" >> "$NOTES"
  warn "发行说明里原本没有哈希，已在末尾追加一行"
fi
grep -qi "$HASH" "$NOTES" || die "发行说明的哈希回填失败: $NOTES"
ok "发行说明已包含本次哈希"

# ---------------------------------------------------------------------------
# 6. Winget manifest delta
# ---------------------------------------------------------------------------
if [ "$DO_WINGET" = 1 ]; then
  step "winget 清单"
  INST="winget/$VER/wzy-777.Mivara.installer.yaml"
  TODAY="$(date -u +%F)"
  sed -i "s|^ReleaseDate:.*|ReleaseDate: $TODAY|" "$INST"
  sed -i "s|^  InstallerSha256:.*|  InstallerSha256: $HASH_UP|" "$INST"
  grep -q "InstallerSha256: $HASH_UP" "$INST"                     || die "清单 SHA-256 回填失败"
  grep -q "releases/download/v$VER/Mivara-Setup-$VER.exe" "$INST" || die "清单 InstallerUrl 与版本不符"
  grep -q "releases/tag/v$VER" "winget/$VER/wzy-777.Mivara.locale.zh-CN.yaml" || die "清单 ReleaseNotesUrl 与版本不符"
  ok "ReleaseDate=$TODAY  InstallerSha256=$HASH_UP"
  ok "InstallerUrl 使用带版本号的资产名（不要换成固定名）"
fi

# ---------------------------------------------------------------------------
# 7. Show the pending change set
# ---------------------------------------------------------------------------
step "待提交内容"
git status --short
if git diff --quiet && [ -z "$(git status --porcelain)" ]; then
  warn "没有需要提交的改动 —— 将直接给 HEAD 打 tag"
fi

# ---------------------------------------------------------------------------
# 8. Confirm
# ---------------------------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
  step "DRY RUN 结束"
  # The version bump above is real, so downstream checks were real too. Put the
  # tree back the way we found it — a dry run must leave no trace.
  if [ "$VER" != "$CUR_VER" ]; then
    git checkout -- src/version.h README.md README_EN.md
    [ "$WINGET_CREATED" = 1 ] && rm -rf "winget/$VER"
    ok "版本号改动已还原（工作区回到 $CUR_VER）"
  fi
  echo "  构建与校验全部通过。未做任何提交/推送/发布。"
  echo "  复核后去掉 --dry-run 即可正式发布。"
  exit 0
fi

if [ "$YES" != 1 ]; then
  printf '\n即将对 %s 执行：commit → push %s → tag %s → GitHub Release（公开可见）\n' "$REPO_SLUG" "$BRANCH" "$TAG"
  if [ "$DO_WINGET" = 1 ] && [ "$DO_WINGET_PR" = 1 ]; then
    printf '并向 microsoft/winget-pkgs 提交/更新 PR（公开可见）\n'
  fi
  printf '继续? [y/N] '
  read -r ans
  case "$ans" in y|Y|yes|YES) ;; *) die "已取消（没有推送任何东西）" ;; esac
fi

# ---------------------------------------------------------------------------
# 9. Commit + push
# ---------------------------------------------------------------------------
step "提交并推送"
git add src/version.h README.md README_EN.md
[ "$DO_WINGET" = 1 ] && git add "winget/$VER"

if git diff --cached --quiet; then
  warn "无暂存改动，跳过提交"
else
  git commit -m "Release $VER"
  ok "$(git log --oneline -1)"
fi

git_net push "$REMOTE" "$BRANCH" || die "推送分支失败"
ok "已推送 $BRANCH"

# ---------------------------------------------------------------------------
# 10. Tag + release
# ---------------------------------------------------------------------------
step "打 tag 并创建 Release"
git tag "$TAG"                       # lightweight, matching v0.8.0 … v0.9.2
git_net push "$REMOTE" "$TAG" || die "推送 tag 失败"
ok "tag $TAG 已推送"

gh_net release create "$TAG" \
  --title "$TITLE" \
  --notes-file "$NOTES" \
  "$SETUP" dist/Mivara-Setup.exe dist/SHA256SUMS.txt >/dev/null \
  || die "gh release create 失败。
  如果 release 已被部分创建（例如只传上去一个资产），先删掉再重跑：
  gh release delete $TAG --yes --cleanup-tag"
ok "Release 已创建"

# ---------------------------------------------------------------------------
# 10b. winget-pkgs PR (create, or update the open PR in place)
# ---------------------------------------------------------------------------
# Runs after the release so the installer URL winget's validation downloads
# already resolves. A failure here must not fail the release: the artifacts
# are public and correct, and the script is idempotent — rerun it by hand.
if [ "$DO_WINGET" = 1 ] && [ "$DO_WINGET_PR" = 1 ]; then
  step "提交 winget 清单"
  if "$ROOT/winget/submit.sh" "$VER"; then
    ok "winget-pkgs 已提交"
  else
    # GitHub's Git Data API returns transient 429/5xx under load. One delayed
    # retry turns most of those into a fully green release; submit.sh is
    # idempotent, so re-running it is safe.
    warn "首次 winget 提交失败，30 秒后重试一次（GitHub 限流/网络抖动）…"
    sleep 30
    if "$ROOT/winget/submit.sh" "$VER"; then
      ok "winget-pkgs 已提交（重试成功）"
    else
      warn "winget PR 未提交成功——发布本身已完成。可稍后重跑：./winget/submit.sh $VER"
    fi
  fi
elif [ "$DO_WINGET" = 1 ]; then
  warn "跳过 winget PR（--no-winget-pr）；可稍后运行 ./winget/submit.sh $VER"
fi

# ---------------------------------------------------------------------------
# 11. Live verification (against the published bytes, not the local files)
# ---------------------------------------------------------------------------
verify_release

step "完成"
printf '  Release : %s/releases/tag/%s\n' "$REPO_URL" "$TAG"
printf '  SHA-256 : %s\n' "$HASH"
if [ "$DO_WINGET" = 1 ] && [ "$DO_WINGET_PR" = 1 ]; then
  printf '\n  winget  : PR 已提交/更新，等待自动校验与社区版主合并\n'
else
  printf '\n  仍需人工完成：winget 清单提交（./winget/submit.sh %s）\n' "$VER"
fi
