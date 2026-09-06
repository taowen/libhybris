#!/usr/bin/env bash
# Cross-compile this checkout for aarch64 glibc without the parent project.
# Produces install/, runtime/, and a provenance manifest.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PARENT="$(cd "$ROOT/../.." && pwd)"
OUT="$ROOT/tests/baseline/build"
HEADERS=""
CLEAN=0
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"

usage() {
    cat <<'EOF'
Usage: tools/build-aarch64.sh [--headers DIR] [--out DIR] [--clean]

Builds libhybris for aarch64 glibc and stages:
  $OUT/install   installed hybris libraries
  $OUT/runtime   glibc loader and DT_NEEDED runtime .so files
  $OUT/manifest.json  ELF sha256/build-id for every staged binary

--headers defaults to ../../android-headers when this tree sits in ardesk.
The AArch64 toolchain comes from tools/ensure-glibc-builder.sh in the parent
project when present; otherwise set BUILDER_IMAGE to a Debian-based image
that has aarch64-linux-gnu-gcc, autoconf, wayland, vulkan and X11 -dev:arm64.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headers) HEADERS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$HEADERS" ]]; then
    if [[ -f "$PARENT/third_party/android-headers/android-config.h" ]]; then
        HEADERS="$PARENT/third_party/android-headers"
    else
        echo "pass --headers pointing at android-config.h / android-version.h" >&2
        exit 2
    fi
fi
if [[ ! -f "$HEADERS/android-config.h" || ! -f "$HEADERS/android-version.h" ]]; then
    echo "android headers missing android-config.h or android-version.h: $HEADERS" >&2
    exit 2
fi

if [[ -z "${BUILDER_IMAGE:-}" ]]; then
    if [[ -x "$PARENT/tools/ensure-glibc-builder.sh" ]]; then
        BUILDER_IMAGE="$("$PARENT/tools/ensure-glibc-builder.sh")"
    else
        echo "set BUILDER_IMAGE to an aarch64 glibc cross toolchain image" >&2
        exit 2
    fi
fi

mkdir -p "$OUT"
SRC_COPY="$OUT/src"
INSTALL="$OUT/install"
RUNTIME="$OUT/runtime"
STUBS="$OUT/stubs"
PC="$OUT/pc"
LOG="$OUT/hybris-build.log"

if [[ "$CLEAN" = 1 ]]; then
    rm -rf "$SRC_COPY" "$INSTALL" "$RUNTIME" "$STUBS" "$PC" "$LOG"
fi

if command -v rsync >/dev/null; then
    rsync -a --delete --exclude .git --exclude tests/baseline/build "$ROOT/" "$SRC_COPY/"
else
    rm -rf "$SRC_COPY"
    mkdir -p "$SRC_COPY"
    cp -a "$ROOT/." "$SRC_COPY/"
    rm -rf "$SRC_COPY/tests/baseline/build"
fi

# Bind the headers as they are; they are not rewritten.
HEADERS_ABS="$(cd "$HEADERS" && pwd)"
OUT_ABS="$(cd "$OUT" && pwd)"
SRC_ABS="$(cd "$SRC_COPY" && pwd)"

"$CONTAINER_ENGINE" run --rm --network host --userns=keep-id \
    --volume "$SRC_ABS:/src:Z" \
    --volume "$HEADERS_ABS:/headers:Z" \
    --volume "$OUT_ABS:/out:Z" \
    --workdir /src \
    "$BUILDER_IMAGE" bash -eu -c '
set -euo pipefail
HOST_TRIPLE=aarch64-linux-gnu
CC_BIN=${HOST_TRIPLE}-gcc
CXX_BIN=${HOST_TRIPLE}-g++
BUILD_DIR=/src/hybris
OUT_DIR=/out/install
PC_DIR=/out/pc
STUB_DIR=/out/stubs
RUNTIME_DIR=/out/runtime
BUILD_LOG=/out/hybris-build.log
mkdir -p "$OUT_DIR" "$PC_DIR" "$STUB_DIR" "$RUNTIME_DIR"
: >"$BUILD_LOG"
run_logged() {
    if "$@" >>"$BUILD_LOG" 2>&1; then
        tail -n 3 "$BUILD_LOG"
    else
        echo "ERROR: command failed: $*" >&2
        tail -n 80 "$BUILD_LOG" >&2
        exit 1
    fi
}
gen_stub() {
    local soname="$1"
    if [[ ! -f "$STUB_DIR/$soname" ]]; then
        "$CC_BIN" -shared -nostdlib -Wl,-soname,"$soname" \
            -x c /dev/null -o "$STUB_DIR/$soname"
        ln -sf "$soname" "$STUB_DIR/${soname%.so.*}.so"
    fi
}
gen_stub libwayland-client.so.0
gen_stub libwayland-server.so.0
gen_stub libwayland-egl.so.1
gen_stub libvulkan.so.1
gen_stub libX11.so.6
gen_stub libxcb.so.1
gen_stub libX11-xcb.so.1
HOST_WAYLAND_INCLUDE="$(pkg-config --variable=includedir wayland-client 2>/dev/null || echo /usr/include)"
HOST_WAYLAND_PROTOCOLS_DATADIR="$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null || echo /usr/share/wayland-protocols)"
HOST_WAYLAND_SCANNER="$(command -v wayland-scanner)"
HOST_VULKAN_INCLUDE="$(pkg-config --variable=includedir vulkan 2>/dev/null || echo /usr/include)"
HOST_X11_INCLUDE="$(pkg-config --variable=includedir x11 2>/dev/null || echo /usr/include)"
HOST_XCB_INCLUDE="$(pkg-config --variable=includedir xcb 2>/dev/null || echo /usr/include)"
write_pc() {
    local name="$1" cflags="$2" libs="$3"
    cat >"$PC_DIR/$name.pc" <<EOF
Name: $name
Description: target-side $name (synthesised for aarch64 cross-build)
Version: 1.22.0
Cflags: $cflags
Libs: -Wl,--no-as-needed $libs
EOF
}
find_guest_libdir() {
    local name="$1"
    local dir
    for dir in /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu \
               /usr/aarch64-linux-gnu/lib; do
        if [[ -e "$dir/lib${name}.so" || -e "$dir/lib${name}.so.0" ||
              -e "$dir/lib${name}.so.1" || -e "$dir/lib${name}.so.6" ]]; then
            printf "%s" "$dir"
            return 0
        fi
    done
    return 1
}
link_libs() {
    local name="$1" dir
    if dir="$(find_guest_libdir "$name")"; then
        printf "%s" "-L$dir -l${name}"
    else
        echo "ERROR: no aarch64 lib${name} in the builder image" >&2
        exit 1
    fi
}
write_pc wayland-client "-I$HOST_WAYLAND_INCLUDE" "$(link_libs wayland-client)"
write_pc wayland-server "-I$HOST_WAYLAND_INCLUDE" "$(link_libs wayland-server)"
write_pc wayland-egl "-I$HOST_WAYLAND_INCLUDE" "$(link_libs wayland-egl)"
write_pc vulkan "-I$HOST_VULKAN_INCLUDE" "$(link_libs vulkan)"
write_pc x11 "-I$HOST_X11_INCLUDE" "$(link_libs X11)"
write_pc xcb "-I$HOST_XCB_INCLUDE" "$(link_libs xcb)"
write_pc x11-xcb "-I$HOST_X11_INCLUDE" "$(link_libs X11-xcb)"
cat >"$PC_DIR/wayland-scanner.pc" <<EOF
wayland_scanner=$HOST_WAYLAND_SCANNER
Name: wayland-scanner
Description: host wayland-scanner tool
Version: 1.22.0
EOF
cat >"$PC_DIR/wayland-protocols.pc" <<EOF
pkgdatadir=$HOST_WAYLAND_PROTOCOLS_DATADIR
Name: wayland-protocols
Description: host wayland-protocols data
Version: 1.32
EOF
export CC="$CC_BIN" CXX="$CXX_BIN"
export AR="${HOST_TRIPLE}-ar" STRIP="${HOST_TRIPLE}-strip"
export RANLIB="${HOST_TRIPLE}-ranlib" LD="${HOST_TRIPLE}-ld"
export NM="${HOST_TRIPLE}-nm" OBJDUMP="${HOST_TRIPLE}-objdump"
export PKG_CONFIG_PATH="$PC_DIR"
export PKG_CONFIG_LIBDIR="$PC_DIR"
export CPPFLAGS="-idirafter $HOST_WAYLAND_INCLUDE -idirafter /usr/include"
if [[ ! -x "$BUILD_DIR/configure" ]]; then
    echo "==> autogen.sh"
    run_logged env -C "$BUILD_DIR" NOCONFIGURE=1 ./autogen.sh
fi
CONFIGURE_ARGS=(
    --host="$HOST_TRIPLE"
    --prefix=/usr/lib/hybris
    --libdir=/usr/lib/hybris
    --with-android-headers=/headers
    --enable-arch=arm64
    --enable-wayland
    --enable-x11
    --disable-wayland_serverside_buffers
    --enable-adreno-quirks
    --enable-mali-quirks
    --enable-property-cache
    --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64
)
printf "%s\n" "${CONFIGURE_ARGS[@]}" > /out/configure-args.txt
FINGERPRINT="standalone args=${CONFIGURE_ARGS[*]}"
STAMP="$BUILD_DIR/.standalone-configure-stamp"
NEED_CONFIGURE=0
if [[ ! -f "$BUILD_DIR/Makefile" ]]; then
    NEED_CONFIGURE=1
elif [[ "$(cat "$STAMP" 2>/dev/null || true)" != "$FINGERPRINT" ]]; then
    echo "==> stale configure; distclean"
    ( cd "$BUILD_DIR" && make distclean ) >/dev/null 2>&1 || true
    NEED_CONFIGURE=1
fi
if [[ "$NEED_CONFIGURE" = 1 ]]; then
    echo "==> configure"
    rm -f "$STAMP"
    run_logged env -C "$BUILD_DIR" ./configure "${CONFIGURE_ARGS[@]}"
    printf "%s\n" "$FINGERPRINT" >"$STAMP"
fi
DIRS="include properties libsync platforms hardware ui gralloc egl glesv1 glesv2 hwc2 vulkan utils"
JOBS="$(nproc)"
echo "==> make + install"
for relink in platforms/common egl/platforms/common egl/platforms/x11 \
              egl/platforms/wayland vulkan/platforms; do
    if [[ -f "$BUILD_DIR/$relink/Makefile" ]]; then
        run_logged make -C "$BUILD_DIR/$relink" clean
    fi
done
run_logged make -C "$BUILD_DIR/common" -j"$JOBS" SUBDIRS=. libhybris-common.la
run_logged make -C "$BUILD_DIR/common" install-libLTLIBRARIES DESTDIR="$OUT_DIR"
run_logged make -C "$BUILD_DIR/common/q" -j"$JOBS"
run_logged make -C "$BUILD_DIR/common/q" install DESTDIR="$OUT_DIR"
for dir in $DIRS; do
    if [[ -f "$BUILD_DIR/$dir/Makefile" ]]; then
        echo "    -- $dir"
        run_logged make -C "$BUILD_DIR/$dir" -j"$JOBS"
        run_logged make -C "$BUILD_DIR/$dir" install DESTDIR="$OUT_DIR"
    fi
done
LIB_DIR="$OUT_DIR/usr/lib/hybris"
MISSING=""
for lib in \
    libhybris-common.so.1.0.0 \
    libEGL.so.1.0.0 \
    libGLESv2.so.2.0.0 \
    libvulkan.so.1.2.183 \
    libhybris/eglplatform_wayland.so \
    libhybris/eglplatform_x11.so \
    libhybris/eglplatform_null.so \
    libhybris/vulkanplatform_null.so \
    libhybris/vulkanplatform_wayland.so \
    libhybris/linker/q.so
do
    [[ -f "$LIB_DIR/$lib" ]] || MISSING="$MISSING $lib"
done
if [[ -n "$MISSING" ]]; then
    echo "ERROR: missing built libraries:$MISSING" >&2
    exit 1
fi
if [[ -e "$LIB_DIR/libhybris/vulkanplatform_x11.so" ]]; then
    echo "ERROR: install contains vulkanplatform_x11.so but source has no such target" >&2
    exit 1
fi
if ! readelf -d "$LIB_DIR/libhybris-platformcommon.so.1.0.0" | grep -q 'libwayland-client.so.0'; then
    echo "ERROR: libhybris-platformcommon.so is missing DT_NEEDED libwayland-client.so.0" >&2
    readelf -d "$LIB_DIR/libhybris-platformcommon.so.1.0.0" >&2
    exit 1
fi
elf_class=$(file "$LIB_DIR/libhybris-common.so.1.0.0")
case "$elf_class" in
    *"aarch64"*) ;;
    *) echo "ERROR: libhybris-common.so is not aarch64: $elf_class" >&2; exit 1 ;;
esac

# Stage glibc runtime files the probes actually load.
copy_runtime() {
    local src="$1" dest="$RUNTIME_DIR/$(basename "$1")"
    if [[ -e "$src" && ! -e "$dest" ]]; then
        cp -L --remove-destination "$src" "$dest"
    fi
}
SYSROOT_LIB=/usr/aarch64-linux-gnu/lib
copy_runtime /lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 || true
copy_runtime "$SYSROOT_LIB/ld-linux-aarch64.so.1" || true
for name in libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
            libstdc++.so.6 libgcc_s.so.1 libwayland-client.so.0 \
            libwayland-server.so.0 libffi.so.8 libX11.so.6 libxcb.so.1 \
            libX11-xcb.so.1 libXau.so.6 libXdmcp.so.6 libbsd.so.0 \
            libmd.so.0; do
    for dir in /usr/aarch64-linux-gnu/lib /lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu; do
        if [[ -e "$dir/$name" ]]; then
            copy_runtime "$dir/$name"
            break
        fi
    done
done
"$CC_BIN" -dumpmachine > /out/compiler.txt
"$CC_BIN" -dumpversion >> /out/compiler.txt
echo "$LIB_DIR"
'

INSTALL_LIB="$INSTALL/usr/lib/hybris"
if [[ ! -f "$INSTALL_LIB/libhybris-common.so.1.0.0" ]]; then
    echo "build did not produce $INSTALL_LIB" >&2
    exit 1
fi

SOURCE_COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
SOURCE_DIRTY=()
if [[ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null || true)" ]]; then
    SOURCE_DIRTY=(--source-dirty)
fi
COMPILER="$(tr '\n' ' ' < "$OUT/compiler.txt" 2>/dev/null || echo unknown)"
CONFIGURE_ARGS="$(tr '\n' ' ' < "$OUT/configure-args.txt" | sed "s|--with-android-headers=/headers|--with-android-headers=$HEADERS_ABS|")"

python3 "$ROOT/tools/manifest.py" \
    --hybris-lib "$INSTALL_LIB" \
    --runtime "$RUNTIME" \
    --out "$OUT/manifest.json" \
    --source-commit "$SOURCE_COMMIT" \
    "${SOURCE_DIRTY[@]}" \
    --headers "$HEADERS_ABS" \
    --compiler "$COMPILER" \
    --configure-args "$CONFIGURE_ARGS"

echo "$INSTALL_LIB"
echo "$RUNTIME"
echo "$OUT/manifest.json"
