#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — submit / update the winget-pkgs pull request
# ============================================================================
#   ./winget/submit.sh 0.10.0             # create or update the PR
#   ./winget/submit.sh 0.10.0 --dry-run   # print the plan, write nothing
#
# Called automatically by release.sh once the GitHub Release is live
# (disable with `release.sh ... --no-winget-pr`).
#
# Rules encoded here (see microsoft/winget-pkgs doc/Submission.md):
#   * One PR per package. While a PR is open the fork's PR branch is updated
#     in place; no second PR is opened for the same package.
#   * A new-package PR carries exactly ONE version: older unpublished version
#     dirs are removed from the PR branch. Versions already published upstream
#     are never touched.
#   * The repository is huge (~1M files), so nothing is cloned: blobs / trees /
#     commits are written through the Git Data API against the fork.
#   * The fork's PR branch is what gets updated because that is the only way to
#     move an existing PR; new PRs get a dedicated branch (`mivara-<version>`).
#
# Requirements: gh (logged in), git, bash, curl not needed.
# ============================================================================

set -euo pipefail

WINGET_REPO="${WINGET_REPO:-microsoft/winget-pkgs}"
PUBLISHER="${WINGET_PUBLISHER:-wzy-777}"
PACKAGE="${WINGET_PACKAGE:-Mivara}"
PACKAGE_ID="${PUBLISHER}.${PACKAGE}"
RELEASE_REPO="${MIVARA_REPO:-wzy-777/Mivara}"
LETTER="$(printf '%s' "$PUBLISHER" | cut -c1 | tr 'A-Z' 'a-z')"
PKG_PATH="manifests/${LETTER}/${PUBLISHER}/${PACKAGE}"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m  %s\n' "$*"; }
warn() { printf '    \033[33m!\033[0m   %s\n' "$*" >&2; }
die()  { printf '\n\033[31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
VER=""
DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)  usage ;;
    --dry-run)  DRY=1 ;;
    -*)         die "unknown option: $1 (see --help)" ;;
    *)          [ -z "$VER" ] || die "多余参数: $1"; VER="$1" ;;
  esac
  shift
done
[ -n "$VER" ] || die "用法: ./winget/submit.sh <version> [--dry-run]"
printf '%s' "$VER" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
  || die "版本号必须是 X.Y.Z 形式: $VER"

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

VER_DIR="winget/$VER"
files=("$VER_DIR"/*.yaml)
[ -f "${files[0]}" ] || die "缺少清单目录: $VER_DIR（先运行 release.sh 的 winget 步骤）"
[ "${#files[@]}" -eq 3 ] || die "$VER_DIR 应有 3 份清单，实际 ${#files[@]} 份"

HASH="$(sed -n 's/^  InstallerSha256: *//p' "$VER_DIR/$PACKAGE_ID.installer.yaml" | head -1)"
[ -n "$HASH" ] || die "$VER_DIR 里找不到 InstallerSha256"

# ---------------------------------------------------------------------------
# gh resolution (same lookup order as release.sh)
# ---------------------------------------------------------------------------
detect_gh() {
  if [ -n "${GH:-}" ] && [ -x "${GH}" ]; then printf '%s' "$GH"; return; fi
  if command -v gh >/dev/null 2>&1; then command -v gh; return; fi
  for p in "/c/Program Files/GitHub CLI/gh.exe" "/c/Program Files/GitHub CLI/gh" \
           "/c/Program Files (x86)/GitHub CLI/gh.exe" \
           "/usr/local/bin/gh" "/opt/homebrew/bin/gh"; do
    if [ -x "$p" ]; then printf '%s' "$p"; return; fi
  done
}
GH_BIN="$(detect_gh)"
[ -n "$GH_BIN" ] || die "找不到 gh CLI（设置 GH=/path/to/gh）"
gh() { "$GH_BIN" "$@"; }

# Read-only calls retry: this network drops connections regularly.
read_gh() {
  local n=0
  until gh "$@"; do
    n=$((n + 1))
    [ "$n" -ge 3 ] && return 1
    sleep 2
  done
}
read_api() { read_gh api "$@"; }

step "预检"
gh auth status >/dev/null 2>&1 || die "gh 未登录（gh auth login）"
GH_USER="$(read_api user --jq .login)" || die "无法获取 gh 账号"
ok "gh 账号 $GH_USER"

if command -v winget >/dev/null 2>&1; then
  if winget validate --manifest "$VER_DIR" >/dev/null 2>&1; then
    ok "winget validate 通过"
  else
    die "winget validate 失败：$VER_DIR"
  fi
else
  warn "未安装 winget CLI，跳过本地清单校验"
fi

# ---------------------------------------------------------------------------
# Fork
# ---------------------------------------------------------------------------
FORK="$GH_USER/winget-pkgs"
step "检查 fork"
if read_api "repos/$FORK" --jq .full_name >/dev/null 2>&1; then
  ok "$FORK 已存在"
else
  [ "$DRY" = 1 ] && die "dry-run：fork 不存在，正式运行时会创建"
  warn "fork 不存在，创建 $WINGET_REPO 的 fork（可能要一会儿）"
  gh repo fork "$WINGET_REPO" --clone=false --remote=false >/dev/null 2>&1 \
    || die "创建 fork 失败"
  for _ in $(seq 1 30); do
    sleep 2
    read_api "repos/$FORK" --jq .full_name >/dev/null 2>&1 && break
  done
  read_api "repos/$FORK" --jq .full_name >/dev/null 2>&1 || die "fork 创建后仍不可用"
  ok "fork 已就绪"
fi

# ---------------------------------------------------------------------------
# Published versions + open PR
# ---------------------------------------------------------------------------
step "查询上游与现有 PR"

PUBLISHED=()
if out="$(read_api "repos/$WINGET_REPO/contents/$PKG_PATH" --jq '.[] | select(.type == "dir") | .name' 2>/dev/null)"; then
  while IFS= read -r v; do [ -n "$v" ] && PUBLISHED+=("$v"); done <<< "$out"
fi
if [ "${#PUBLISHED[@]}" -gt 0 ]; then
  ok "上游已发布版本: ${PUBLISHED[*]}"
  for v in "${PUBLISHED[@]}"; do
    if [ "$v" = "$VER" ]; then
      ok "上游已有 $VER，无需提交"
      exit 0
    fi
  done
else
  ok "上游尚未发布该包（本次是新包 PR）"
fi

OPEN_PR=""
HEAD_BRANCH=""
HEAD_SHA=""
PR_OWNER=""
if out="$(read_gh pr list --repo "$WINGET_REPO" --author "$GH_USER" \
            --state open --limit 100 \
            --json number,title,headRefName,headRefOid,headRepositoryOwner \
            --jq '.[] | [.number, .title, .headRefName, .headRefOid, .headRepositoryOwner.login] | @tsv' 2>/dev/null)"; then
  while IFS=$'\t' read -r num title branch oid owner; do
    [ -z "$num" ] && continue
    case "$title" in
      *"$PACKAGE_ID"*)
        OPEN_PR="$num"; HEAD_BRANCH="$branch"; HEAD_SHA="$oid"; PR_OWNER="$owner"
        break
        ;;
    esac
  done <<< "$out"
fi

if [ -n "$OPEN_PR" ]; then
  [ "$PR_OWNER" = "$GH_USER" ] || die "PR #$OPEN_PR 的源仓库是 $PR_OWNER，不是 $GH_USER，无法自动更新"
  ok "发现同包 PR #$OPEN_PR（分支 $HEAD_BRANCH）——将原地更新"
else
  ok "没有同包的 open PR —— 将新建"
fi

# ---------------------------------------------------------------------------
# Branch / base commit
# ---------------------------------------------------------------------------
BRANCH=""
BASE_SHA=""
if [ -n "$OPEN_PR" ]; then
  BRANCH="$HEAD_BRANCH"
  BASE_SHA="$HEAD_SHA"
else
  BRANCH="mivara-$VER"
  BASE_BRANCH="$(read_api "repos/$FORK" --jq .default_branch)"
  BASE_SHA="$(read_api "repos/$FORK/git/ref/heads/$BASE_BRANCH" --jq .object.sha)"
  # A previous failed run may have left the branch behind.
  if left="$(read_api "repos/$FORK/git/ref/heads/$BRANCH" --jq .object.sha 2>/dev/null)"; then
    BASE_SHA="$left"
    warn "分支 $BRANCH 已存在（上次未完成的运行？），在其上继续"
  fi
fi
BASE_TREE="$(read_api "repos/$FORK/git/commits/$BASE_SHA" --jq .tree.sha)"

# ---------------------------------------------------------------------------
# What the PR branch currently carries
# ---------------------------------------------------------------------------
HEAD_DIRS=()
if [ -n "$OPEN_PR" ]; then
  if out="$(read_api "repos/$FORK/contents/$PKG_PATH?ref=$BRANCH" --jq '.[] | select(.type == "dir") | .name' 2>/dev/null)"; then
    while IFS= read -r v; do [ -n "$v" ] && HEAD_DIRS+=("$v"); done <<< "$out"
  fi
fi

declare -A KEEP=()
for v in "${PUBLISHED[@]}"; do KEEP["$v"]=1; done
KEEP["$VER"]=1

TO_DELETE=()
for v in "${HEAD_DIRS[@]}"; do
  [ -n "${KEEP[$v]:-}" ] || TO_DELETE+=("$v")
done

# Is the target version already byte-identical on the branch?
UP_TO_DATE=0
if [ -n "$OPEN_PR" ]; then
  for v in "${HEAD_DIRS[@]}"; do
    [ "$v" = "$VER" ] || continue
    UP_TO_DATE=1
    declare -A REMOTE_SUM=()
    if out="$(read_api "repos/$FORK/contents/$PKG_PATH/$VER?ref=$BRANCH" --jq '.[] | [.name, .sha] | @tsv' 2>/dev/null)"; then
      while IFS=$'\t' read -r n s; do [ -n "$n" ] && REMOTE_SUM["$n"]="$s"; done <<< "$out"
    fi
    [ "${#REMOTE_SUM[@]}" -eq 3 ] || UP_TO_DATE=0
    for f in "${files[@]}"; do
      b="$(basename "$f")"
      s="$(git hash-object --no-filters "$f")"
      [ "${REMOTE_SUM[$b]:-}" = "$s" ] || UP_TO_DATE=0
    done
    unset REMOTE_SUM
    break
  done
fi

CHANGES=1
if [ "$UP_TO_DATE" = 1 ] && [ "${#TO_DELETE[@]}" -eq 0 ]; then
  CHANGES=0
fi

# ---------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------
step "计划"
[ "${#TO_DELETE[@]}" -gt 0 ] && echo "    删除分支里的未发布版本: ${TO_DELETE[*]}"
if [ "$UP_TO_DATE" = 1 ]; then
  echo "    $VER 清单已在分支上且内容一致"
else
  echo "    写入 $VER 清单（3 个文件）到 $PKG_PATH/$VER/"
fi
if [ -n "$OPEN_PR" ]; then
  echo "    目标: 更新 PR #$OPEN_PR（分支 $BRANCH）"
else
  echo "    目标: 新建 PR（分支 $BRANCH ← ${BASE_SHA:0:7}）"
fi
[ "$CHANGES" = 0 ] && ok "无需提交改动（幂等）"

if [ "$DRY" = 1 ]; then
  step "DRY RUN 结束"
  echo "  未创建任何 blob / commit / PR。"
  exit 0
fi

# ---------------------------------------------------------------------------
# Write: blobs -> tree -> commit -> ref
# ---------------------------------------------------------------------------
NEW_SHA=""
if [ "$CHANGES" = 1 ]; then
  step "提交到 $GH_USER/winget-pkgs"

  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT

  entries=()

  # Deletions: every file in an unpublished version dir on the branch.
  for v in "${TO_DELETE[@]}"; do
    while IFS=$'\t' read -r name _sha; do
      [ -z "$name" ] && continue
      entries+=("{\"path\": \"$PKG_PATH/$v/$name\", \"mode\": \"100644\", \"type\": \"blob\", \"sha\": null}")
    done < <(read_api "repos/$FORK/contents/$PKG_PATH/$v?ref=$BRANCH" \
               --jq '.[] | [.name, .sha] | @tsv')
    echo "    删除 $v/"
  done

  # Additions: one blob per manifest (GitHub dedups identical content).
  for f in "${files[@]}"; do
    b64="$(base64 < "$f" | tr -d '\n')"
    blob="$(gh api -X POST "repos/$FORK/git/blobs" \
              -f content="$b64" -f encoding=base64 --jq .sha)"
    [ -n "$blob" ] || die "创建 blob 失败: $f"
    entries+=("{\"path\": \"$PKG_PATH/$VER/$(basename "$f")\", \"mode\": \"100644\", \"type\": \"blob\", \"sha\": \"$blob\"}")
    echo "    写入 $VER/$(basename "$f")"
  done

  {
    printf '{\n  "base_tree": "%s",\n  "tree": [\n' "$BASE_TREE"
    first=1
    for e in "${entries[@]}"; do
      if [ "$first" = 1 ]; then first=0; else printf ',\n'; fi
      printf '    %s' "$e"
    done
    printf '\n  ]\n}\n'
  } > "$TMP/tree.json"

  TREE_SHA="$(gh api -X POST "repos/$FORK/git/trees" \
                --input "$TMP/tree.json" --jq .sha)"
  [ -n "$TREE_SHA" ] || die "创建 tree 失败"

  if [ "${#PUBLISHED[@]}" -eq 0 ]; then
    COMMIT_MSG="Add $PACKAGE_ID version $VER"
  else
    COMMIT_MSG="Update $PACKAGE_ID to version $VER"
  fi
  {
    printf '{\n  "message": "%s",\n  "tree": "%s",\n  "parents": ["%s"]\n}\n' \
      "$COMMIT_MSG" "$TREE_SHA" "$BASE_SHA"
  } > "$TMP/commit.json"

  NEW_SHA="$(gh api -X POST "repos/$FORK/git/commits" \
               --input "$TMP/commit.json" --jq .sha)"
  [ -n "$NEW_SHA" ] || die "创建 commit 失败"
  ok "commit ${NEW_SHA:0:7} — $COMMIT_MSG"

  if [ -n "$OPEN_PR" ]; then
    gh api -X PATCH "repos/$FORK/git/refs/heads/$BRANCH" \
      -f sha="$NEW_SHA" >/dev/null || die "更新分支 $BRANCH 失败"
    ok "已推送 $BRANCH（PR #$OPEN_PR 会自动带上这个提交）"
  elif read_api "repos/$FORK/git/ref/heads/$BRANCH" --jq .object.sha >/dev/null 2>&1; then
    gh api -X PATCH "repos/$FORK/git/refs/heads/$BRANCH" \
      -f sha="$NEW_SHA" >/dev/null || die "更新分支 $BRANCH 失败"
    ok "已更新分支 $BRANCH"
  else
    gh api -X POST "repos/$FORK/git/refs" \
      -f ref="refs/heads/$BRANCH" -f sha="$NEW_SHA" >/dev/null \
      || die "创建分支 $BRANCH 失败"
    ok "已创建分支 $BRANCH"
  fi
fi

# ---------------------------------------------------------------------------
# PR title / body / comment
# ---------------------------------------------------------------------------
if [ "${#PUBLISHED[@]}" -eq 0 ]; then
  TITLE="New package: $PACKAGE_ID version $VER"
else
  TITLE="Update: $PACKAGE_ID version $VER"
fi
BODY="Mivara (幻屏) is an MIT-licensed open-source, cross-platform LAN screen-casting / wireless second-monitor tool by wzy-777. The installer is built with MinGW + Inno Setup from the linked repository; it is not code-signed. Silent install (\`/VERYSILENT\`) is supported. Installer SHA-256 matches the GitHub release asset digest."

if [ -z "$OPEN_PR" ]; then
  step "创建 PR"
  PR_URL="$(gh pr create --repo "$WINGET_REPO" \
              --head "$GH_USER:$BRANCH" --base master \
              --title "$TITLE" --body "$BODY")" || die "gh pr create 失败"
  ok "$PR_URL"
else
  step "更新 PR"
  CUR_TITLE="$(read_api "repos/$WINGET_REPO/pulls/$OPEN_PR" --jq .title)"
  if [ "$CUR_TITLE" != "$TITLE" ]; then
    gh pr edit "$OPEN_PR" --repo "$WINGET_REPO" --title "$TITLE" >/dev/null \
      || die "更新 PR 标题失败"
    ok "标题 → $TITLE"
  fi
  if [ "$CHANGES" = 1 ]; then
    if [ "${#PUBLISHED[@]}" -eq 0 ]; then
      COMMENT="Updated to $VER (commit ${NEW_SHA:0:7}). The package has not been published yet, so this pull request now carries only the $VER manifest set instead of the previous unpublished version — a new-package pull request has to contain a single package version. Publisher, installer URL form, ARP entries and silent-install behaviour are unchanged; the installer SHA-256 ($HASH) matches the v$VER GitHub release asset digest."
    else
      COMMENT="Updated to $VER (commit ${NEW_SHA:0:7}). Installer SHA-256 ($HASH) matches the v$VER GitHub release asset digest."
    fi
    gh pr comment "$OPEN_PR" --repo "$WINGET_REPO" --body "$COMMENT" >/dev/null \
      || die "发表更新说明失败"
    ok "已在 PR 里说明本次更新"
  fi
  PR_URL="$(read_api "repos/$WINGET_REPO/pulls/$OPEN_PR" --jq .html_url)"
  ok "$PR_URL"
fi

step "完成"
echo "  清单: $PKG_PATH/$VER/"
echo "  PR  : $PR_URL"
echo "  校验会在推送后自动重跑；最后一步是社区版主人工合并。"
