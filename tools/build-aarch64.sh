#!/usr/bin/env bash
# Cross-compile this checkout for aarch64 glibc without the parent project.
# Produces install/, runtime/, and a provenance manifest.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/tests/baseline/build"
HEADERS=""
PROTOCOLS="${ARDESK_WSI_PROTOCOL_DIR:-$ROOT/../../protocols}"
CLEAN=0
DEBUG_BUILD=0
INCREMENTAL=0
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"

usage() {
    cat <<'EOF'
Usage: tools/build-aarch64.sh [--headers DIR] [--out DIR] [--clean] [--debug] [--incremental]

Builds libhybris for aarch64 glibc and stages:
  $OUT/install   installed hybris libraries
  $OUT/runtime   glibc loader and DT_NEEDED runtime .so files
  $OUT/manifest.json  ELF sha256/build-id for every staged binary

Without --headers, fetch the pinned Android headers using this repository's
tools/fetch-android-headers.sh. tools/ensure-builder.sh builds the pinned
Debian cross-toolchain recipe. BUILDER_IMAGE may explicitly override it.
ARDESK_WSI_PROTOCOL_DIR selects the shared Ardesk protocol package (defaults
to ../../protocols when checked out inside Ardesk). It is snapshotted and
included in cache validation and build provenance.
Build snapshots, header/compiler identities and ELF hashes are recorded.
--debug enables existing libhybris logging/trace macros (runtime opt-in).
--incremental reuses a completed compiler cache for C/C++/assembly edits.
Headers, build rules, file additions/deletions and toolchain/config changes
force a clean rebuild. The deployable install/runtime are always staged fresh.
--clean overrides --incremental. Default builds remain clean.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headers) HEADERS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        --debug) DEBUG_BUILD=1; shift ;;
        --incremental) INCREMENTAL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! -f "$PROTOCOLS/ardesk-wsi-protocols.pc" ]]; then
    echo "Set ARDESK_WSI_PROTOCOL_DIR to the Ardesk protocols package directory." >&2
    exit 2
fi

mkdir -p "$OUT"
exec 9>"$OUT/.build.lock"
if ! flock -n 9; then
    echo "another build owns $OUT; use a separate --out or wait for it" >&2
    exit 2
fi
BUILD_STARTED="$(python3 -c 'import time; print(time.monotonic())')"

if [[ -z "$HEADERS" ]]; then
    HEADERS="$(HYBRIS_DEPS_DIR="${HYBRIS_DEPS_DIR:-$OUT/deps}" "$ROOT/tools/fetch-android-headers.sh")"
fi
if [[ ! -f "$HEADERS/android-config.h" || ! -f "$HEADERS/android-version.h" ]]; then
    echo "android headers missing android-config.h or android-version.h: $HEADERS" >&2
    exit 2
fi

if [[ -z "${BUILDER_IMAGE:-}" ]]; then
    BUILDER_IMAGE="$("$ROOT/tools/ensure-builder.sh")"
fi

mkdir -p "$OUT"
SRC_COPY="$OUT/src"
HEADERS_COPY="$OUT/headers"
INSTALL="$OUT/install"
RUNTIME="$OUT/runtime"

BUILDER_ID="$("$CONTAINER_ENGINE" image inspect --format '{{.Id}}' "$BUILDER_IMAGE")"
CACHE_OPTIONS=()
if [[ "$INCREMENTAL" = 1 && "$CLEAN" != 1 ]]; then CACHE_OPTIONS+=(--incremental); fi
if [[ "$DEBUG_BUILD" = 1 ]]; then CACHE_OPTIONS+=(--debug); fi
python3 "$ROOT/tools/prepare-build.py" prepare --root "$ROOT" --out "$OUT" \
    --headers "$HEADERS" --protocols "$PROTOCOLS" --builder "$BUILDER_ID" --started "$BUILD_STARTED" "${CACHE_OPTIONS[@]}"
CACHE_MODE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$OUT/build-report.json")"

HEADERS_ABS="$(cd "$HEADERS_COPY" && pwd)"
OUT_ABS="$(cd "$OUT" && pwd)"
SRC_ABS="$(cd "$SRC_COPY" && pwd)"

SOURCE_COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
SOURCE_DIRTY=()
if [[ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null || true)" ]]; then
    SOURCE_DIRTY=(--source-dirty)
fi
python3 "$ROOT/tools/build_inputs.py" --source "$OUT/inputs" --headers "$HEADERS_COPY" \
    --recipe "$ROOT/tools/container/Containerfile.aarch64" --image-id "$BUILDER_ID" \
    --build-script "$ROOT/tools/build-aarch64.sh" \
    --out "$OUT/build-inputs.json"

"$CONTAINER_ENGINE" run --rm --network host --userns=keep-id \
    --volume "$SRC_ABS:/src:Z" \
    --volume "$HEADERS_ABS:/headers:ro,Z" \
    --volume "$OUT_ABS:/out:Z" \
    --env HYBRIS_STANDALONE_DEBUG="$DEBUG_BUILD" \
    --env HYBRIS_STANDALONE_CACHE="$CACHE_MODE" \
    --workdir /src \
    "$BUILDER_ID" bash -eu -c '
set -euo pipefail
HOST_TRIPLE=aarch64-linux-gnu
CC_BIN=${HOST_TRIPLE}-gcc
CXX_BIN=${HOST_TRIPLE}-g++
BUILD_DIR=/src/hybris
OUT_DIR=/out/install
RUNTIME_DIR=/out/runtime
BUILD_LOG=/out/hybris-build.log
mkdir -p "$OUT_DIR" "$RUNTIME_DIR"
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
# Query target packages, not fabricated .pc versions or host libraries.
export PKG_CONFIG_PATH=/src/protocols
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
HOST_WAYLAND_SCANNER="$(command -v wayland-scanner)"
pkg-config --modversion wayland-client wayland-server wayland-egl vulkan x11 xcb x11-xcb > /out/target-package-versions.txt
export CC="$CC_BIN" CXX="$CXX_BIN"
export AR="${HOST_TRIPLE}-ar" STRIP="${HOST_TRIPLE}-strip"
export RANLIB="${HOST_TRIPLE}-ranlib" LD="${HOST_TRIPLE}-ld"
export NM="${HOST_TRIPLE}-nm" OBJDUMP="${HOST_TRIPLE}-objdump"
export CPPFLAGS="-idirafter /usr/include"
# Platform wrappers intentionally depend on libraries resolved later.
export LDFLAGS="-Wl,--no-as-needed"
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
    --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/system_ext/lib64
)
if [[ "$HYBRIS_STANDALONE_DEBUG" = 1 ]]; then CONFIGURE_ARGS+=(--enable-debug --enable-trace); fi
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
if [[ "$HYBRIS_STANDALONE_CACHE" != incremental ]]; then
    for relink in platforms/common egl/platforms/common; do
        if [[ -f "$BUILD_DIR/$relink/Makefile" ]]; then
            run_logged make -C "$BUILD_DIR/$relink" clean
        fi
    done
fi
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
    libhybris-vulkan-icd.so.0 \
    libVkLayer_hybris_compat.so \
    libhybris/eglplatform_null.so \
    libhybris/linker/q.so
do
    [[ -f "$LIB_DIR/$lib" ]] || MISSING="$MISSING $lib"
done
if [[ -n "$MISSING" ]]; then
    echo "ERROR: missing built libraries:$MISSING" >&2
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

# Include every installed platform plugin, not only libraries used headlessly.
# Android libraries loaded through the separate linker are recorded at runtime.
python3 /src/tools/stage-runtime.py --hybris "$LIB_DIR" --runtime "$RUNTIME_DIR" \
    --search /usr/aarch64-linux-gnu/lib --search /lib/aarch64-linux-gnu \
    --search /usr/lib/aarch64-linux-gnu \
    --require ld-linux-aarch64.so.1 --require libpthread.so.0 \
    --require libdl.so.2 --require librt.so.1
dpkg-query -W > /out/builder-packages.txt
"$CC_BIN" --version > /out/compiler.txt
"$CC_BIN" -dumpmachine >> /out/compiler.txt
"$CC_BIN" -dumpversion >> /out/compiler.txt
echo "$LIB_DIR"
'

INSTALL_LIB="$INSTALL/usr/lib/hybris"
if [[ ! -f "$INSTALL_LIB/libhybris-common.so.1.0.0" ]]; then
    echo "build did not produce $INSTALL_LIB" >&2
    exit 1
fi

COMPILER="$(tr '\n' ' ' < "$OUT/compiler.txt" 2>/dev/null || echo unknown)"
CONFIGURE_ARGS="$(tr '\n' ' ' < "$OUT/configure-args.txt" | sed "s|--with-android-headers=/headers|--with-android-headers=$HEADERS_ABS|")"

python3 "$ROOT/tools/manifest.py" \
    --hybris-lib "$INSTALL_LIB" \
    --runtime "$RUNTIME" \
    --out "$OUT/manifest.json" \
    --source-commit "$SOURCE_COMMIT" \
    --build-mode "$CACHE_MODE" \
    --inputs "$OUT/build-inputs.json" \
    --packages "$OUT/builder-packages.txt" \
    "${SOURCE_DIRTY[@]}" \
    --headers "$HEADERS_ABS" \
    --compiler "$COMPILER" \
    --configure-args "$CONFIGURE_ARGS"

python3 "$ROOT/tools/prepare-build.py" finish --out "$OUT"
echo "$INSTALL_LIB"
echo "$RUNTIME"
echo "$OUT/manifest.json"
