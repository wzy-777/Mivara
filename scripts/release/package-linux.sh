#!/usr/bin/env bash
# ============================================================================
# Mivara (幻屏) — build the Linux packages
# ============================================================================
# Linux-only half of the release pipeline. Rebuilds bin/{app,sender,receiver}
# with the portable flags and produces:
#
#   dist/Mivara-linux-x86_64-<ver>.tar.gz   portable bundle (bundled runtime)
#   dist/Mivara-linux-x86_64.tar.gz         fixed-name copy of the same file
#   dist/mivara_<ver>_amd64.deb             Debian/Ubuntu package
#   dist/Mivara-linux-amd64.deb             fixed-name copy of the same file
#   dist/SHA256SUMS-linux.txt               sha256sum lines for all four
#
# The tar.gz is self-contained apart from glibc/X11/desktop libraries: the
# FFmpeg/SDL/codec closure is copied into lib/ and the binaries get an
# $ORIGIN RPATH. The .deb deliberately does NOT bundle those libraries — it
# depends on the distribution packages instead, so use it on the distro whose
# FFmpeg ABI it was built against (CI builds on Ubuntu 22.04).
#
# Usage:
#   bash scripts/release/package-linux.sh              # build + package
#   bash scripts/release/package-linux.sh --skip-build # package bin/ as-is
#   bash scripts/release/package-linux.sh --ver 0.13.0
#   bash scripts/release/package-linux.sh --no-deb     # portable tar only
#
# Requirements (Debian/Ubuntu host): g++ make pkg-config libx11-dev
# libsdl2-dev libsdl2-ttf-dev libavcodec-dev libavutil-dev libswscale-dev
# patchelf dpkg-dev. Run `make install-deps` first for the compiler libs.
# ============================================================================

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

LINUX_ARCH="${LINUX_ARCH:-x86_64}"
DEB_ARCH="${DEB_ARCH:-}"
DIST="${DIST:-$ROOT/dist}"
WORK="$DIST/linux"
BUNDLE_DIR=""
TAR_NAME=""
TAR_FIXED=""
DEB_NAME=""
DEB_FIXED=""
SUMS_NAME="SHA256SUMS-linux.txt"

require_linux_host() {
  [ "$(uname -s 2>/dev/null)" = Linux ] \
    || die "Linux 打包只能在 Linux 主机上运行（Windows 请用 CI：.github/workflows/release-linux.yml）"
  command -v make >/dev/null 2>&1 || die "找不到 make"
  command -v g++  >/dev/null 2>&1 || die "找不到 g++"
  command -v patchelf >/dev/null 2>&1 \
    || die "找不到 patchelf（sudo apt-get install patchelf）"
  command -v readelf >/dev/null 2>&1 || die "找不到 readelf（binutils）"
  [ -n "$DEB_ARCH" ] || case "$LINUX_ARCH" in
    x86_64)  DEB_ARCH=amd64 ;;
    aarch64) DEB_ARCH=arm64 ;;
    *) die "未知架构 $LINUX_ARCH（请设置 LINUX_ARCH/DEB_ARCH）" ;;
  esac
}

set_asset_names() {
  local ver="$1"
  BUNDLE_DIR="Mivara-${ver}-linux-${LINUX_ARCH}"
  TAR_NAME="Mivara-linux-${LINUX_ARCH}-${ver}.tar.gz"
  TAR_FIXED="Mivara-linux-${LINUX_ARCH}.tar.gz"
  DEB_NAME="mivara_${ver}_${DEB_ARCH}.deb"
  DEB_FIXED="Mivara-linux-${DEB_ARCH}.deb"
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
build_linux_portable() {
  local flags="$PORTABLE_FLAGS" jobs
  # PORTABLE_FLAGS carries -march=x86-64, which only makes sense on x86_64.
  case "$LINUX_ARCH" in
    x86_64) ;;
    *) flags="-O3 -flto" ;;
  esac
  step "构建 Linux 版本（portable flags）"
  echo "    make clean && make -j$(nproc 2>/dev/null || echo 1) OPT_FLAGS=\"$flags\" && make check"
  jobs="$(nproc 2>/dev/null || echo 1)"
  ( cd "$ROOT" && make clean && make -j"$jobs" OPT_FLAGS="$flags" && make check ) \
    || die "Linux 构建失败"
  ok "bin/app · bin/sender · bin/receiver 已重建（portable flags）"
}

check_linux_binaries() {
  local ver="${1:-$(read_version)}" b
  for b in app sender receiver; do
    [ -x "bin/$b" ] || die "构建产物缺失或不可执行: bin/$b（先运行 make）"
    grep -aq "$ver" "bin/$b" || die "bin/$b 里找不到版本号 $ver —— 构建可能是旧的"
  done
  ok "bin/app · bin/sender · bin/receiver 均为 $ver"
}

# ---------------------------------------------------------------------------
# Portable bundle
# ---------------------------------------------------------------------------
# Runtime libraries that must come from the host: glibc, the X11/Wayland/GL
# stack, audio services and the desktop/font stack. Everything else (FFmpeg,
# codec backends, SDL2, SDL2_ttf, libstdc++, ...) is bundled so the tar works
# on distros with a different FFmpeg ABI.
is_system_lib() {
  case "$1" in
    # glibc + dynamic loader
    ld-linux*|libc.so.*|libm.so.*|libmvec.so.*|libpthread.so.*|libdl.so.*|\
    librt.so.*|libresolv.so.*|libutil.so.*|libnsl.so.*|libcrypt.so.*|libanl.so.*)
      return 0 ;;
    # X11 / Wayland / OpenGL / DRM (must match the running desktop)
    libX11*|libXext*|libXau*|libXdmcp*|libxcb*|libGL*|libEGL*|libOpenGL*|\
    libGLX*|libGLdispatch*|libdrm*|libgbm*|libwayland*|libxkbcommon*|\
    libXcursor*|libXi*|libXinerama*|libXrandr*|libXScrnSaver*|libXss*|\
    libXxf86vm*|libXfixes*|libXrender*|libSM*|libICE*|libXtst*|libXpresent*|\
    libXv*|libdecor*)
      return 0 ;;
    # system services, compression, i18n and the font/graphics stack
    libdbus-1*|libsystemd*|libudev*|libselinux*|libapparmor*|libcap*|\
    libgcrypt*|libgpg-error*|liblz4*|libzstd*|liblzma*|libbz2*|libz.so.*|\
    libexpat*|libffi*|libpcre*|libpcre2*|libglib-2.0*|libgobject-2.0*|\
    libgio-2.0*|libgmodule-2.0*|libgthread-2.0*|libmount*|libblkid*|\
    libbsd*|libmd*|libicu*|libxml2*|libcairo*|libpango*|libpangocairo*|\
    libpangoft2*|libpixman*|libfontconfig*|libfreetype*|libharfbuzz*|\
    libgraphite2*|libfribidi*|libthai*|libdatrie*|libpng*|libjpeg*|\
    libtiff*|liblcms2*|libbrotli*|librsvg*|libgdk_pixbuf*|libgdk-3*|libgtk-3*)
      return 0 ;;
    # audio
    libasound*|libpulse*|libpulsecommon*|libpipewire*|libjack*|libsndfile*|\
    libFLAC*|libmpg123*|libsamplerate*|libasyncns*)
      return 0 ;;
    # hardware accel (drivers are host-specific)
    libva.so*|libva-drm*|libva-x11*|libvdpau*|libvpl*|libOpenCL*)
      return 0 ;;
  esac
  return 1
}

lib_soname() {
  local lib="$1" soname
  soname="$(readelf -d "$lib" 2>/dev/null \
    | awk '/\(SONAME\)/ { gsub(/[\[\]]/, "", $NF); print $NF; exit }')"
  printf '%s' "${soname:-$(basename "$lib")}"
}

bundle_runtime_libs() {
  local stage="$1"
  local queue="bin/app bin/sender bin/receiver" f lib base seen=""

  mkdir -p "$stage/lib"
  step "收集运行时库（FFmpeg / SDL / 编解码后端）"
  while [ -n "$queue" ]; do
    set -- $queue
    f="$1"
    shift
    queue="$*"
    while IFS= read -r lib; do
      [ -n "$lib" ] || continue
      base="$(lib_soname "$lib")"
      case " $seen " in *" $base "*) continue ;; esac
      seen="$seen $base"
      is_system_lib "$base" && continue
      cp -Lf "$lib" "$stage/lib/$base"
      queue="$queue $stage/lib/$base"
    done < <(
      LD_LIBRARY_PATH="$stage/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        ldd "$f" 2>/dev/null | awk '/=> \// { print $3 }'
    )
  done
  [ -n "$(ls -A "$stage/lib" 2>/dev/null)" ] \
    || die "没有收集到任何运行时库（ldd 失败？）"
  ok "已打包 $(ls -1 "$stage/lib" | wc -l) 个运行时库"
}

patch_rpath() {
  local stage="$1" f
  local bins=("$stage/bin/app" "$stage/bin/sender" "$stage/bin/receiver")
  for f in "${bins[@]}"; do
    patchelf --set-rpath '$ORIGIN/../lib' "$f" \
      || die "patchelf 失败: $f"
  done
  # DT_RUNPATH is not inherited, so every bundled library resolves its own
  # direct dependencies from its own directory. This is what makes transitive
  # lookups such as libavcodec -> libx264 find the bundled copy.
  for f in "$stage"/lib/*; do
    patchelf --set-rpath '$ORIGIN' "$f" || die "patchelf 失败: $f"
  done
}

stage_portable() {
  local stage="$1"
  rm -rf "$stage"
  mkdir -p "$stage/bin"
  step "组装便携包"
  cp -f bin/app bin/sender bin/receiver "$stage/bin/"
  cp -f README.md README_EN.md LICENSE SECURITY.md THIRD-PARTY-NOTICES.md "$stage/"
  cp -R LICENSES docs "$stage/"
  cp -f res/mivara-256.png "$stage/mivara.png"
  cp -f res/mivara.desktop "$stage/mivara.desktop"
  cp -f scripts/linux/install.sh "$stage/install.sh"
  chmod 755 "$stage/bin/"* "$stage/install.sh"
  ln -sfn bin/app "$stage/mivara"

  bundle_runtime_libs "$stage"
  patch_rpath "$stage"

  # Fail closed if any bundled dependency cannot be resolved from the bundle
  # or the host.
  local b out
  for b in app sender receiver; do
    out="$(ldd "$stage/bin/$b" 2>&1)" || die "ldd 失败: $b"
    printf '%s\n' "$out" | grep -q 'not found' \
      && die "便携包缺少依赖: $(printf '%s\n' "$out" | grep 'not found' | head -3 | tr '\n' ' ')"
  done
  ok "便携包依赖自检通过"
}

package_tar() {
  local tar_opts=()
  if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    tar_opts=(--sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner)
  fi
  step "生成 tar.gz"
  tar "${tar_opts[@]}" -cf - -C "$WORK" "$BUNDLE_DIR" | gzip -9n > "$DIST/$TAR_NAME" \
    || die "tar 失败"
  cp -f "$DIST/$TAR_NAME" "$DIST/$TAR_FIXED"
  cmp -s "$DIST/$TAR_NAME" "$DIST/$TAR_FIXED" || die "固定名 tar 副本不一致"
  ok "$TAR_NAME ($(du -h "$DIST/$TAR_NAME" | cut -f1))"
}

# ---------------------------------------------------------------------------
# Debian package
# ---------------------------------------------------------------------------
deb_depends() {
  local root="$1" ver="$2"
  local dir="$WORK/dpkg-work" out date
  local bins=("$root/usr/lib/mivara/app" "$root/usr/lib/mivara/sender" \
              "$root/usr/lib/mivara/receiver")

  # Preferred path: dpkg-shlibdeps knows the SONAME -> package mapping and the
  # minimum symbol versions, and unlike a recursive ldd it reports the direct
  # dependencies only (libavcodec pulls its own codec backends).
  rm -rf "$dir"
  mkdir -p "$dir/debian"
  cat > "$dir/debian/control" <<'EOF'
Source: mivara
Section: net
Priority: optional
Maintainer: Mivara <wzy-777@users.noreply.github.com>
Standards-Version: 4.6.0

Package: mivara
Architecture: any
Depends: ${shlibs:Depends}
Description: Mivara Linux package
 Mivara local package staging data.
EOF
  date="$(date -u -d "@${SOURCE_DATE_EPOCH:-$(date +%s)}" \
          '+%a, %d %b %Y %H:%M:%S +0000' 2>/dev/null \
          || date -u '+%a, %d %b %Y %H:%M:%S +0000')"
  cat > "$dir/debian/changelog" <<EOF
mivara ($ver) unstable; urgency=medium

  * Release $ver.

 -- Mivara <wzy-777@users.noreply.github.com>  $date
EOF
  if out="$( cd "$dir" && dpkg-shlibdeps -O "${bins[@]}" 2>/dev/null )" \
     && [ -n "$out" ]; then
    out="${out#shlibs:Depends=}"
    if [ -n "$out" ]; then printf '%s' "$out"; return 0; fi
  fi
  warn "dpkg-shlibdeps 失败，回退到 dpkg -S 推导依赖（会包含传递依赖）"

  # Fallback: map every recursively resolved library back to its package.
  # Modern Debian/Ubuntu merge /lib into /usr/lib (and many sonames are
  # symlinks), but dpkg -S matches neither the merged name nor the symlink;
  # resolve to the real file first or every dependency is silently skipped.
  local lib pkg pkgver deps="" seen="" pkglist=""
  while IFS= read -r lib; do
    [ -n "$lib" ] || continue
    lib="$(readlink -f "$lib" 2>/dev/null || printf '%s' "$lib")"
    pkg="$(dpkg -S "$lib" 2>/dev/null \
      | awk -F: '/^[A-Za-z0-9][A-Za-z0-9+.-]*:/ { print $1; exit }')"
    if [ -z "$pkg" ]; then
      die "无法把 $lib 映射到 Debian 包；deb 的 Depends 会不完整。"
    fi
    case " $seen " in *" $pkg "*) continue ;; esac
    seen="$seen $pkg"
    pkglist="$pkglist $pkg"
  done < <(ldd "${bins[@]}" 2>/dev/null | awk '/=> \// { print $3 }' | sort -u)

  for pkg in $(printf '%s\n' $pkglist | sort -u); do
    pkgver="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
    if [ -n "$pkgver" ]; then
      deps="${deps}${deps:+, }$pkg (>= $pkgver)"
    else
      deps="${deps}${deps:+, }$pkg"
    fi
  done
  [ -n "$deps" ] || die "无法计算 deb 依赖"
  printf '%s' "$deps"
}

write_deb_copyright() {
  local dst="$1/usr/share/doc/mivara/copyright"
  mkdir -p "$(dirname "$dst")"
  cat > "$dst" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Mivara
Source: https://github.com/wzy-777/Mivara

Files: *
Copyright: 2026 wzy-777
License: MIT
 The source tree is licensed under the MIT License; the full text is
 installed as /usr/share/doc/mivara/LICENSE.

Files: usr/lib/mivara/app usr/lib/mivara/sender usr/lib/mivara/receiver
Copyright: 2026 wzy-777
License: GPL-3.0-or-later
 These binaries link against GPL-3.0-or-later FFmpeg libraries and are
 distributed under the same terms. The full text is installed as
 /usr/share/doc/mivara/LICENSES/GPL-3.0-or-later and is also available at
 /usr/share/common-licenses/GPL-3.
EOF
  chmod 644 "$dst"
}

stage_deb() {
  local root="$1" ver="$2" deps size
  rm -rf "$root"
  step "组装 Debian 包"
  install -Dm755 bin/app      "$root/usr/lib/mivara/app"
  install -Dm755 bin/sender   "$root/usr/lib/mivara/sender"
  install -Dm755 bin/receiver "$root/usr/lib/mivara/receiver"

  mkdir -p "$root/usr/bin"
  ln -sfn ../lib/mivara/app      "$root/usr/bin/mivara"
  ln -sfn ../lib/mivara/sender   "$root/usr/bin/mivara-sender"
  ln -sfn ../lib/mivara/receiver "$root/usr/bin/mivara-receiver"

  install -Dm644 res/mivara.desktop \
    "$root/usr/share/applications/mivara.desktop"
  install -Dm644 res/mivara-256.png \
    "$root/usr/share/icons/hicolor/256x256/apps/mivara.png"

  local doc="$root/usr/share/doc/mivara"
  install -d "$doc"
  install -m644 README.md README_EN.md LICENSE SECURITY.md \
    THIRD-PARTY-NOTICES.md "$doc/"
  cp -R LICENSES docs "$doc/"
  write_deb_copyright "$root"

  deps="$(deb_depends "$root" "$ver")"
  size="$(du -sk --apparent-size "$root" | cut -f1)"

  mkdir -p "$root/DEBIAN"
  cat > "$root/DEBIAN/control" <<EOF
Package: mivara
Version: $ver
Architecture: $DEB_ARCH
Maintainer: Mivara <wzy-777@users.noreply.github.com>
Installed-Size: $size
Depends: $deps
Recommends: fonts-noto-cjk | fonts-wqy-microhei | fonts-wqy-zenhei
Section: net
Priority: optional
Homepage: https://github.com/wzy-777/Mivara
Description: LAN screen mirroring and wireless second display
 Mivara mirrors a computer's screen over the local network and can use
 another computer as a second display. It discovers peers with SSDP,
 streams H.264/H.265/AV1 video and offers a two-way file browser.
 .
 This package contains the GUI launcher (mivara), the sender and the
 receiver. Extend mode requires Windows on the sender side; Linux supports
 mirroring, receiving and file transfer.
EOF
  chmod 644 "$root/DEBIAN/control"
  if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    find "$root" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
  fi
  ok "Depends: $deps"
}

package_deb() {
  step "生成 .deb"
  if dpkg-deb --help 2>&1 | grep -q -- '--root-owner-group'; then
    dpkg-deb --build --root-owner-group "$WORK/deb-root" "$DIST/$DEB_NAME" >/dev/null \
      || die "dpkg-deb 失败"
  else
    dpkg-deb --build "$WORK/deb-root" "$DIST/$DEB_NAME" >/dev/null \
      || die "dpkg-deb 失败"
  fi
  cp -f "$DIST/$DEB_NAME" "$DIST/$DEB_FIXED"
  cmp -s "$DIST/$DEB_NAME" "$DIST/$DEB_FIXED" || die "固定名 deb 副本不一致"
  ok "$DEB_NAME ($(du -h "$DIST/$DEB_NAME" | cut -f1))"
}

# ---------------------------------------------------------------------------
# Checksums + verification
# ---------------------------------------------------------------------------
write_linux_sums() {
  ( cd "$DIST" && sha256sum "$TAR_NAME" "$TAR_FIXED" "$DEB_NAME" "$DEB_FIXED" \
      > "$SUMS_NAME" ) || die "生成 $SUMS_NAME 失败"
}

verify_linux_packages() {
  local ver="$1" b out
  step "校验 Linux 产物"
  ( cd "$DIST" && sha256sum -c "$SUMS_NAME" >/dev/null ) \
    || die "SHA256SUMS-linux.txt 校验失败"

  # Redirect the listings instead of piping into grep -q: with pipefail a
  # grep that exits on the first match can SIGPIPE the producer and make the
  # whole pipeline look like a failure.
  tar -tzf "$DIST/$TAR_NAME" > "$WORK/tar-list.txt" || die "tar 列表读取失败"
  grep -qx "$BUNDLE_DIR/bin/app" "$WORK/tar-list.txt" \
    || die "tar 包缺少 bin/app"
  grep -qx "$BUNDLE_DIR/install.sh" "$WORK/tar-list.txt" \
    || die "tar 包缺少 install.sh"
  grep -q "^$BUNDLE_DIR/lib/libav" "$WORK/tar-list.txt" \
    || die "tar 包没有打包 FFmpeg 运行时库（lib/ 为空？）"

  if [ -f "$DIST/$DEB_NAME" ]; then
    dpkg-deb --info "$DIST/$DEB_NAME" > "$WORK/deb-info.txt" \
      || die "deb 信息读取失败"
    grep -q "Version: $ver" "$WORK/deb-info.txt" \
      || die "deb 的 Version 不是 $ver"
    dpkg-deb -c "$DIST/$DEB_NAME" > "$WORK/deb-list.txt" \
      || die "deb 文件列表读取失败"
    grep -q './usr/lib/mivara/app' "$WORK/deb-list.txt" \
      || die "deb 缺少 /usr/lib/mivara/app"
    grep -q './usr/share/applications/mivara.desktop' "$WORK/deb-list.txt" \
      || die "deb 缺少桌面入口"
  fi

  for b in app sender receiver; do
    grep -aq "$ver" "$WORK/$BUNDLE_DIR/bin/$b" \
      || die "打包后的 bin/$b 缺少版本号 $ver"
    out="$(ldd "$WORK/$BUNDLE_DIR/bin/$b" 2>&1)" || die "ldd 失败: $b"
    printf '%s\n' "$out" | grep -q 'not found' \
      && die "打包后的 bin/$b 仍缺少依赖"
  done
  ok "tar.gz / deb / SHA256SUMS-linux.txt 校验通过"
}

package_linux() {
  local ver="$1" no_deb="${2:-0}"
  set_asset_names "$ver"
  mkdir -p "$DIST"
  rm -rf "$WORK"
  mkdir -p "$WORK"
  # Never let a previous run's package survive into this one's checksums.
  rm -f "$DIST/$TAR_NAME" "$DIST/$TAR_FIXED" \
        "$DIST/$DEB_NAME" "$DIST/$DEB_FIXED" "$DIST/$SUMS_NAME"

  stage_portable "$WORK/$BUNDLE_DIR"
  package_tar

  if [ "$no_deb" = 1 ]; then
    warn "跳过 .deb（--no-deb）"
    # Keep the checksum file limited to the files that actually exist.
    ( cd "$DIST" && sha256sum "$TAR_NAME" "$TAR_FIXED" > "$SUMS_NAME" )
  else
    stage_deb "$WORK/deb-root" "$ver"
    package_deb
    write_linux_sums
  fi

  verify_linux_packages "$ver"
}

main_package_linux() {
  local ver="" skip_build=0 no_deb=0
  while [ $# -gt 0 ]; do
    case "$1" in
      -h|--help)   usage_from_header "$0" 36; exit 0 ;;
      --skip-build) skip_build=1 ;;
      --no-deb)    no_deb=1 ;;
      --ver)       shift; ver="${1:-}" ;;
      -*)          die "unknown option: $1 (see --help)" ;;
      *)           die "多余参数: $1" ;;
    esac
    shift
  done
  require_repo
  require_linux_host
  if [ "$no_deb" = 0 ]; then
    command -v dpkg-deb >/dev/null 2>&1 \
      || die "找不到 dpkg-deb（sudo apt-get install dpkg-dev）"
    command -v dpkg-shlibdeps >/dev/null 2>&1 \
      || die "找不到 dpkg-shlibdeps（sudo apt-get install dpkg-dev）"
    command -v dpkg >/dev/null 2>&1 \
      || die "找不到 dpkg（sudo apt-get install dpkg-dev）"
  fi
  ver="${ver:-$(read_version)}"
  validate_version "$ver"

  if [ "$skip_build" = 1 ]; then
    warn "跳过构建（--skip-build），复用 bin/ 里的二进制"
    check_linux_binaries "$ver"
  else
    build_linux_portable
    check_linux_binaries "$ver"
  fi

  package_linux "$ver" "$no_deb"

  step "Linux 打包完成"
  printf '  便携包 : dist/%s\n' "$TAR_NAME"
  printf '  固定名 : dist/%s\n' "$TAR_FIXED"
  if [ "$no_deb" = 0 ]; then
    printf '  deb    : dist/%s\n' "$DEB_NAME"
    printf '  固定名 : dist/%s\n' "$DEB_FIXED"
  fi
  printf '  校验和 : dist/%s\n' "$SUMS_NAME"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main_package_linux "$@"
fi
