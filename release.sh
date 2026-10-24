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
#
# What it automates (AGENTS.md "Installer" checklist):
#   * portable build  : make clean + OPT_FLAGS="-O3 -march=x86-64 -flto"
#   * installer       : ISCC → dist/Mivara-Setup-<ver>.exe + fixed-name copy
#   * SHA256SUMS.txt  : the in-app updater is fail-closed without it
#   * version bump    : src/version.h + both README badges + winget/<ver>/
#   * publish         : commit → push → lightweight tag → GitHub Release (3 assets)
#   * winget PR       : winget/submit.sh creates/updates the microsoft/winget-pkgs
#                       PR in place (one PR per package — see that script)
#   * verification    : releases/latest redirect, asset list, and a re-download of
#                       the published installer hashed against the local one
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
usage() { sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
VER=""
DRY_RUN=0
YES=0
SKIP_BUILD=0
DO_WINGET=1
DO_WINGET_PR=1
VERIFY_DOWNLOAD=1
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
    --no-verify-download) VERIFY_DOWNLOAD=0 ;;
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
  for p in "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe"; do
    if [ -x "$p" ]; then printf '%s' "$p"; return; fi
  done
  printf ''
}
ISCC_BIN="$(detect_iscc)"

detect_gh() {
  if [ -n "${GH:-}" ] && [ -x "${GH}" ]; then printf '%s' "$GH"; return; fi
  if command -v gh >/dev/null 2>&1; then command -v gh; return; fi
  for p in "/c/Program Files/GitHub CLI/gh.exe" "/c/Program Files (x86)/GitHub CLI/gh.exe"; do
    if [ -x "$p" ]; then printf '%s' "$p"; return; fi
  done
  printf ''
}
GH_BIN="$(detect_gh)"

gh() { "$GH_BIN" "$@"; }

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
  if curl "$@"; then return 0; fi
  [ -n "$PROXY" ] || return 1
  warn "curl 直连失败，改用代理 $PROXY 重试"
  curl -x "$PROXY" "$@"
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

REMOTE_TAG="$(git ls-remote --tags "$REMOTE" "refs/tags/$TAG" 2>/dev/null || true)"
[ -z "$REMOTE_TAG" ] || die "远端已存在 tag $TAG —— 这个版本发过了"
ok "远端无 tag $TAG"

gh auth status >/dev/null 2>&1 || die "gh 未登录（gh auth login）"
gh api "repos/${REPO_SLUG}/releases/tags/${TAG}" >/dev/null 2>&1 \
  && die "GitHub 上已存在 release $TAG"
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
ok "bin/app.exe · sender.exe · receiver.exe 均为 $VER"

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
    warn "winget PR 未提交成功——发布本身已完成。可稍后重跑：./winget/submit.sh $VER"
  fi
elif [ "$DO_WINGET" = 1 ]; then
  warn "跳过 winget PR（--no-winget-pr）；可稍后运行 ./winget/submit.sh $VER"
fi

# ---------------------------------------------------------------------------
# 11. Live verification (against the published bytes, not the local files)
# ---------------------------------------------------------------------------
step "线上校验"

LATEST="$(curl_net -sI -o /dev/null -w '%{redirect_url}' "$REPO_URL/releases/latest" || true)"
case "$LATEST" in
  */tag/"$TAG") ok "releases/latest → $TAG" ;;
  *) die "releases/latest 没有指向 $TAG（实际: ${LATEST:-<无>}）—— 更新器会认为没有新版本" ;;
esac

ASSETS="$(gh api "repos/${REPO_SLUG}/releases/tags/${TAG}" --jq '.assets[].name' | sort | tr '\n' ' ')"
for a in "Mivara-Setup-$VER.exe" "Mivara-Setup.exe" "SHA256SUMS.txt"; do
  case " $ASSETS " in
    *" $a "*) ;;
    *) die "Release 缺少资产: $a（现有: $ASSETS）" ;;
  esac
done
ok "三个资产齐全: $ASSETS"

if [ "$VERIFY_DOWNLOAD" = 1 ]; then
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT

  curl_net -fsSL -o "$TMP/SHA256SUMS.txt" "$REPO_URL/releases/download/$TAG/SHA256SUMS.txt" \
    || die "下载已发布的 SHA256SUMS.txt 失败"
  diff -q "$TMP/SHA256SUMS.txt" dist/SHA256SUMS.txt >/dev/null \
    || die "已发布的 SHA256SUMS.txt 与本地不一致"
  ok "已发布的 SHA256SUMS.txt 与本地一致"

  curl_net -fsSL -o "$TMP/Mivara-Setup.exe" "$REPO_URL/releases/latest/download/Mivara-Setup.exe" \
    || die "从固定名永久直链下载失败"
  DL_HASH="$(sha256sum "$TMP/Mivara-Setup.exe" | awk '{print $1}')"
  [ "$DL_HASH" = "$HASH" ] \
    || die "下载回来的安装包哈希不符（线上 $DL_HASH ≠ 本地 $HASH）"
  ok "固定名直链下载的安装包哈希与本地一致 —— 应用内更新会校验通过"
fi

step "完成"
printf '  Release : %s/releases/tag/%s\n' "$REPO_URL" "$TAG"
printf '  SHA-256 : %s\n' "$HASH"
if [ "$DO_WINGET" = 1 ] && [ "$DO_WINGET_PR" = 1 ]; then
  printf '\n  winget  : PR 已提交/更新，等待自动校验与社区版主合并\n'
else
  printf '\n  仍需人工完成：winget 清单提交（./winget/submit.sh %s）\n' "$VER"
fi
