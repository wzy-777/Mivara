#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — one-shot release
# ============================================================================
#   build (portable) → Windows installer → commit → push → tag → Linux CI →
#   GitHub Release → winget PR → verify
#
# The individual stages live in scripts/release/ and can be run on their own:
#   bump.sh <ver>      version.h + README badges + winget/<ver>/
#   build.sh           portable build + binary version check
#   package.sh         Inno Setup installer + SHA256SUMS.txt (no git/GitHub)
#   linux.sh <ver>     Linux tar.gz + .deb via GitHub Actions, upload them
#   winget.sh <ver>    finalize winget/<ver>/ after packaging
#   verify.sh <ver>    re-check a published release (no build/commit/push)
# This file composes them and adds preflight, commit/push/tag/release.
#
# Usage:
#   ./release.sh 0.9.3              # bump + full release
#   ./release.sh                    # release the version already in src/version.h
#   ./release.sh 0.9.3 --dry-run    # everything except commit/push/tag/release
#   ./release.sh 0.9.3 --yes        # no confirmation prompt (CI-like use)
#   ./release.sh 0.9.3 --skip-build # reuse dist/ artifacts from a previous run
#   ./release.sh 0.9.3 --no-linux   # Windows-only release (no Linux packages)
#   ./release.sh 0.9.3 --no-winget  # do not create/update winget/<ver>/
#   ./release.sh 0.9.3 --no-winget-pr  # keep winget/<ver>/ but do not submit the PR
#   ./release.sh 0.9.3 --verify-only   # re-check an already published release
#   ./release.sh 0.9.3 --verify-download  # force re-downloading the published assets
#   ./release.sh 0.9.3 --no-verify-download  # digests only, no CDN download at all
#
# Installer-only (no git, no GitHub): bash scripts/release/package.sh
#
# Requirements: run from Git Bash (git + gh must work from it). The build itself
# is delegated to MSYS2 bash because /mingw64 is not mounted in Git Bash. `*.sh`
# is pinned to LF in .gitattributes: a CRLF shebang breaks bash.
# ============================================================================

set -euo pipefail

RELEASE_ROOT="$(cd "$(dirname "$0")" && pwd)"
ROOT="$RELEASE_ROOT"
source "$RELEASE_ROOT/scripts/release/lib.sh"
source "$RELEASE_SCRIPTS_DIR/bump.sh"
source "$RELEASE_SCRIPTS_DIR/build.sh"
source "$RELEASE_SCRIPTS_DIR/package.sh"
source "$RELEASE_SCRIPTS_DIR/linux.sh"
source "$RELEASE_SCRIPTS_DIR/winget.sh"
source "$RELEASE_SCRIPTS_DIR/verify.sh"

usage() {
  usage_from_header "$0" 36
  exit 0
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
VER=""
DRY_RUN=0
YES=0
SKIP_BUILD=0
DO_LINUX=1
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
    --no-linux)       DO_LINUX=0 ;;
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

require_repo
MSYS_BASH="${MSYS_BASH:-C:/msys64/usr/bin/bash.exe}"

# ---------------------------------------------------------------------------
# 1. Resolve version
# ---------------------------------------------------------------------------
step "解析版本号"

CUR_VER="$(read_version)"
[ -z "$VER" ] && VER="$CUR_VER"
validate_version "$VER"

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
if [ "$VERIFY_ONLY" = 1 ]; then
  case "$VERIFY_DOWNLOAD" in
    full) exec bash "$RELEASE_SCRIPTS_DIR/verify.sh" "$VER" --verify-download ;;
    none) exec bash "$RELEASE_SCRIPTS_DIR/verify.sh" "$VER" --no-verify-download ;;
    *)    exec bash "$RELEASE_SCRIPTS_DIR/verify.sh" "$VER" ;;
  esac
fi

# ---------------------------------------------------------------------------
# 2. Preflight
# ---------------------------------------------------------------------------
step "预检"

[ -x "$MSYS_BASH" ] || die "找不到 MSYS2 bash: $MSYS_BASH（构建需要它）"
[ -n "$(detect_gh)" ] || die "找不到 gh CLI（设置 GH=/path/to/gh.exe）"

# Linux packages are built by GitHub Actions on the pushed tag. The workflow
# and its packaging script must be part of the release commit, or the tag
# cannot build them.
if [ "$DO_LINUX" = 1 ]; then
  [ -f ".github/workflows/release-linux.yml" ] \
    || die "缺少 .github/workflows/release-linux.yml（Linux 打包 CI）"
  [ -f "scripts/release/package-linux.sh" ] \
    || die "缺少 scripts/release/package-linux.sh"
  ok "Linux 打包已启用（CI · ubuntu-22.04）"
else
  warn "已禁用 Linux 打包（--no-linux）"
fi

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

init_gh_auth
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
PRE_VER="$CUR_VER"
HAD_WINGET_DIR=0
[ -d "winget/$VER" ] && HAD_WINGET_DIR=1
bump_version "$VER" "$DO_WINGET"

# ---------------------------------------------------------------------------
# 4. Build + package
# ---------------------------------------------------------------------------
build_and_check "$VER"
package_installer "$VER" "$NOTES" 1

# ---------------------------------------------------------------------------
# 5. Winget manifest delta
# ---------------------------------------------------------------------------
if [ "$DO_WINGET" = 1 ]; then
  finalize_winget "$VER" "$HASH"
fi

# ---------------------------------------------------------------------------
# 6. Show the pending change set
# ---------------------------------------------------------------------------
step "待提交内容"
git status --short
if git diff --quiet && [ -z "$(git status --porcelain)" ]; then
  warn "没有需要提交的改动 —— 将直接给 HEAD 打 tag"
fi

# ---------------------------------------------------------------------------
# 7. Dry-run restore
# ---------------------------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
  step "DRY RUN 结束"
  # The version bump above is real, so downstream checks were real too. Put the
  # tree back the way we found it — a dry run must leave no trace.
  if [ "$VER" != "$PRE_VER" ]; then
    git checkout -- src/version.h README.md README_EN.md
    [ "$HAD_WINGET_DIR" = 0 ] && rm -rf "winget/$VER"
    ok "版本号改动已还原（工作区回到 $CUR_VER）"
  fi
  echo "  构建与校验全部通过。未做任何提交/推送/发布。"
  if [ "$DO_LINUX" = 1 ]; then
    echo "  注意：Linux 包由 tag 推送后的 CI 构建，dry-run 无法演练；正式发布时会一并生成。"
  fi
  echo "  复核后去掉 --dry-run 即可正式发布。"
  exit 0
fi

# ---------------------------------------------------------------------------
# 8. Confirm
# ---------------------------------------------------------------------------
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
# 10. Tag + Linux CI + release
# ---------------------------------------------------------------------------
step "打 tag"
git tag "$TAG"                       # lightweight, matching v0.8.0 … v0.9.2
git_net push "$REMOTE" "$TAG" || die "推送 tag 失败"
ok "tag $TAG 已推送"

# Linux packages are built from the pushed tag (so src/version.h is exactly
# the released source) and downloaded into dist/ before the release is
# created. A Linux failure must not strand the Windows installer: create the
# release with the Windows assets, then report the recovery command at the
# end. The stage uses die internally; the subshell keeps that from killing
# this script.
LINUX_FAILED=0
LINUX_ASSETS=()
if [ "$DO_LINUX" = 1 ]; then
  step "构建 Linux 安装包（GitHub Actions · $TAG）"
  if ( linux_stage "$VER" "$TAG" "$SKIP_BUILD" ); then
    if ! linux_update_notes "$NOTES" "$VER"; then
      warn "发行说明的 Linux 哈希写入失败（继续发布）"
    fi
    while IFS= read -r asset; do
      LINUX_ASSETS+=("dist/$asset")
    done < <(linux_asset_names "$VER")
  else
    LINUX_FAILED=1
    warn "Linux 打包失败——仍会发布 Windows 安装包"
    warn "修复后补齐：bash scripts/release/linux.sh $VER"
    if [ -f "$NOTES" ]; then linux_strip_notes "$NOTES"; fi
  fi
else
  # A notes template copied from an older release may still carry the block;
  # never advertise assets this release will not upload.
  if [ -f "$NOTES" ]; then linux_strip_notes "$NOTES"; fi
fi

step "创建 Release"
release_assets=("$SETUP" dist/Mivara-Setup.exe dist/SHA256SUMS.txt)
if [ "${#LINUX_ASSETS[@]}" -gt 0 ]; then
  release_assets+=("${LINUX_ASSETS[@]}")
fi
gh_net release create "$TAG" \
  --title "$TITLE" \
  --notes-file "$NOTES" \
  "${release_assets[@]}" >/dev/null \
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
# A failed Linux stage (or --no-linux) must not make verify_release demand
# Linux assets from a Windows-only release; a partial download or an earlier
# run may have left a checksum file for this version behind. The final error
# below is the actionable one.
if [ "$LINUX_FAILED" = 1 ] || [ "$DO_LINUX" = 0 ]; then
  mv -f dist/SHA256SUMS-linux.txt dist/SHA256SUMS-linux.txt.skipped 2>/dev/null || true
fi
verify_release

if [ "$LINUX_FAILED" = 1 ]; then
  die "Release $TAG 已发布 Windows 资产，但 Linux 安装包缺失。
  恢复（不重建 Windows）：bash scripts/release/linux.sh $VER"
fi

step "完成"
printf '  Release : %s/releases/tag/%s\n' "$REPO_URL" "$TAG"
printf '  SHA-256 : %s\n' "$HASH"
if [ "$DO_LINUX" = 1 ]; then
  printf '  Linux   : tar.gz + .deb 已上传（%s/%s）\n' "$LINUX_ARCH" "$DEB_ARCH"
else
  printf '  Linux   : 已跳过（--no-linux）\n'
fi
if [ "$DO_WINGET" = 1 ] && [ "$DO_WINGET_PR" = 1 ]; then
  printf '\n  winget  : PR 已提交/更新，等待自动校验与社区版主合并\n'
else
  printf '\n  仍需人工完成：winget 清单提交（./winget/submit.sh %s）\n' "$VER"
fi
