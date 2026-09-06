#!/usr/bin/env bash
# Build the headless probes. Library + runtime come from tools/build-aarch64.sh.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
OUT="${OUT:-$PWD/build}"
BUNDLE="$OUT/bundle"
HYBRIS_LIB="${HYBRIS_LIB:-$OUT/install/usr/lib/hybris}"
RUNTIME="${RUNTIME:-$OUT/runtime}"
if [[ -z "${BIONIC_CC:-}" ]]; then
    : "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME or BIONIC_CC}"
    BIONIC_CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang"
fi

if [[ ! -f "$HYBRIS_LIB/libvulkan.so.1" ]]; then
    echo "missing $HYBRIS_LIB/libvulkan.so.1; run tools/build-aarch64.sh first" >&2
    exit 2
fi
if [[ ! -x "$BIONIC_CC" ]]; then
    echo "set BIONIC_CC to an Android NDK aarch64 clang wrapper" >&2
    exit 2
fi

mkdir -p "$BUNDLE"

if [[ -z "${GLIBC_CC:-}" ]]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null; then
        GLIBC_CC=aarch64-linux-gnu-gcc
    fi
fi
if [[ -z "${GLIBC_CC:-}" ]]; then
    if [[ -z "${BUILDER_IMAGE:-}" ]]; then
        PARENT="$(cd "$ROOT/../.." && pwd)"
        if [[ -x "$PARENT/tools/ensure-glibc-builder.sh" ]]; then
            BUILDER_IMAGE="$("$PARENT/tools/ensure-glibc-builder.sh")"
        else
            echo "set GLIBC_CC or BUILDER_IMAGE for the glibc probes" >&2
            exit 2
        fi
    fi
    HOST_HYBRIS="$(cd "$HYBRIS_LIB" && pwd)"
    compile_glibc() {
        local cflags="$1" libs="$2" dest="$3"
        local engine="${CONTAINER_ENGINE:-podman}"
        "$engine" run --rm --network host --userns=keep-id \
            --volume "$PWD:/src:Z" \
            --volume "$(cd "$BUNDLE" && pwd):/out:Z" \
            --volume "$HOST_HYBRIS:/hybris:Z" \
            --workdir /src "$BUILDER_IMAGE" \
            aarch64-linux-gnu-gcc -O2 -Wall -Wextra $cflags \
            probe.c -ldl ${libs//$HYBRIS_LIB//hybris} -o "/out/$(basename "$dest")"
    }
else
    compile_glibc() {
        local cflags="$1" libs="$2" dest="$3"
        "$GLIBC_CC" -O2 -Wall -Wextra $cflags probe.c -ldl $libs -o "$dest"
    }
fi

compile_glibc "" "" "$BUNDLE/probe-glibc"
compile_glibc "-DHYBRIS_PROBE_LINKED" \
    "-L$HYBRIS_LIB -Wl,-rpath-link,$HYBRIS_LIB -lvulkan" \
    "$BUNDLE/probe-glibc-linked"
"$BIONIC_CC" -O2 -Wall -Wextra probe.c -ldl -o "$BUNDLE/probe-bionic"
echo "$BUNDLE"
