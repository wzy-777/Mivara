#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — shared release tooling
# ============================================================================
# Sourced by release.sh (repo root) and by the standalone scripts in this
# directory (build.sh / package.sh / bump.sh / winget.sh / verify.sh).
#
# Keep this file free of side effects that need the network or GitHub: it may
# be sourced by a local packaging run that has no gh login and no network.
# Network-dependent setup lives in init_gh_auth() and is called explicitly.
# ============================================================================

set -euo pipefail

RELEASE_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ROOT:-$(cd "$RELEASE_SCRIPTS_DIR/../.." && pwd)}"

# ---------------------------------------------------------------------------
# Configuration (override via environment)
# ---------------------------------------------------------------------------
REPO_SLUG="${REPO_SLUG:-wzy-777/Mivara}"
REPO_URL="https://github.com/${REPO_SLUG}"
REMOTE="${REMOTE:-origin}"
BRANCH="${BRANCH:-main}"
PROXY="${PROXY:-}"   # optional; only tried after a direct attempt fails
PORTABLE_FLAGS="${PORTABLE_FLAGS:--O3 -march=x86-64 -flto}" # repository MUST be archived with this
MSYS_BASH="${MSYS_BASH:-C:/msys64/usr/bin/bash.exe}"

# ---------------------------------------------------------------------------
# Terminal helpers
# ---------------------------------------------------------------------------
step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m  %s\n' "$*"; }
warn() { printf '    \033[33m!\033[0m   %s\n' "$*" >&2; }
die()  { printf '\n\033[31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# GNU timeout is present in Git Bash and Linux, but not in stock macOS.
timeout_cmd() {
  local d="$1"; shift
  if command -v timeout >/dev/null 2>&1; then timeout "$d" "$@"; else "$@"; fi
}

# Print the leading comment block of a script as help text. Stops at the first
# non-comment line, so a header can grow without updating a line range.
usage_from_header() {
  awk 'NR == 1 { next }
       /^#/ { sub(/^# ?/, ""); print; next }
       { exit }' "${1:-$0}"
}

require_repo() {
  cd "$ROOT"
  [ -f makefile ] && [ -d src ] || die "看起来不是 Mivara 仓库根目录: $ROOT"
}

# ---------------------------------------------------------------------------
# Version helpers (single source: src/version.h)
# ---------------------------------------------------------------------------
read_version() {
  local v
  v="$(sed -n 's/^#define[[:space:]]\+MIVARA_VERSION[[:space:]]\+"\([^"]*\)".*/\1/p' "$ROOT/src/version.h")"
  [ -n "$v" ] || die "src/version.h 里找不到 MIVARA_VERSION"
  printf '%s' "$v"
}

validate_version() {
  case "$1" in
    ''|*[!0-9.]*) die "版本号必须是 X.Y.Z 形式（不带 v）: $1" ;;
  esac
  printf '%s' "$1" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || die "版本号必须是 X.Y.Z 形式（不带 v）: $1"
}

# ---------------------------------------------------------------------------
# Tool detection (ISCC / gh)
# ---------------------------------------------------------------------------
ISCC_BIN="${ISCC_BIN:-}"

detect_iscc() {
  if [ -n "$ISCC_BIN" ]; then printf '%s' "$ISCC_BIN"; return; fi
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

iscc_bin() {
  ISCC_BIN="$(detect_iscc)"
  printf '%s' "$ISCC_BIN"
}

GH_BIN="${GH_BIN:-}"

detect_gh() {
  if [ -n "$GH_BIN" ] && [ -x "$GH_BIN" ]; then printf '%s' "$GH_BIN"; return; fi
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

gh_bin() {
  GH_BIN="$(detect_gh)"
  [ -n "$GH_BIN" ] || die "找不到 gh CLI（设置 GH=/path/to/gh.exe）"
  printf '%s' "$GH_BIN"
}

gh() { "$(gh_bin)" "$@"; }

# Read-only GitHub calls retry with backoff. This network intermittently drops
# api.github.com's TLS handshake; a single timeout must not look like a missing
# release or an invalid token.
gh_read() {
  local n=0 delay=2
  until timeout_cmd 60 "$(gh_bin)" "$@"; do
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
# never overridden. Network + gh required — call this explicitly, never at
# source time (a local packaging run must not touch the network).
init_gh_auth() {
  [ -n "$(gh_bin)" ] || die "找不到 gh CLI（设置 GH=/path/to/gh.exe）"
  [ -n "$GH_BIN" ] || return 0
  if [ -z "${GH_TOKEN:-}" ] && [ -z "${GITHUB_TOKEN:-}" ]; then
    if ! timeout_cmd 30 "$GH_BIN" auth status >/dev/null 2>&1; then
      # GCM can return an expired stored OAuth token; a git network call gives
      # it the 401 it needs to refresh, and the next fill returns the new value.
      timeout_cmd 60 git ls-remote --heads "$REMOTE" >/dev/null 2>&1 || true
      local gcm_token=""
      gcm_token="$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill 2>/dev/null \
                     | sed -n 's/^password=//p' || true)"
      if [ -n "$gcm_token" ]; then
        export GH_TOKEN="$gcm_token"
        # curl reaches api.github.com more reliably than gh here; a failed check
        # is not proof the token is bad (it is usually a TLS timeout), so keep it
        # and let the retrying callers report a real auth error.
        if curl -fsS -m 15 -H "Authorization: Bearer $gcm_token" \
             -H "User-Agent: Mivara-release" https://api.github.com/user >/dev/null 2>&1; then
          ok "使用 Git Credential Manager 中的 GitHub 凭据"
        else
          warn "Git Credential Manager 凭据暂时无法验证（网络超时）；仍将尝试使用"
        fi
      fi
    fi
  fi
}

# ---------------------------------------------------------------------------
# Network helpers
# ---------------------------------------------------------------------------
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
