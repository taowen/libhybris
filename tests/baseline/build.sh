#!/usr/bin/env bash
# Build the headless probes. Library + runtime come from tools/build-aarch64.sh.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
OUT="${OUT:-$PWD/build}"
BUNDLE="$OUT/bundle"
SOURCES=(probe_point_size.c probe_bc_images.c probe_bc.c compat/bc_decode.c probe_native_buffer.c probe_scaled.c probe_memory_ranges.c probe_timeline.c probe_timeline_queues.c render_path.c probe_render_owners.c probe.c probe_stdio.c probe_common.c probe_egl.c probe_egl_lifecycle.c probe_vulkan.c probe_dispatch.c
         probe_icd_version.c probe_tls_mrs.c probe_groups.c probe_lifecycle.c probe_vulkan_init.c probe_alloc.c probe_lock_init.c probe_cond_init.c probe_cond_clock.c probe_shared_unavailable.c probe_tls_bounds.c probe_tls_destructor.c probe_caps.c probe_caps2.c probe_properties2.c probe_widget.c probe_validation.c)
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
BUNDLE="$(cd "$BUNDLE" && pwd)"
PROBE_SRC="$BUNDLE/src"
rm -rf "$PROBE_SRC"
mkdir -p "$PROBE_SRC/shaders" "$PROBE_SRC/compat/shaders" "$PROBE_SRC/reference"
cp -r reference/bc7 reference/bc6h "$PROBE_SRC/reference/"
cp "$ROOT/hybris/vulkan/compat/"bc_decode.{c,h} "$PROBE_SRC/compat/"
cp "$ROOT/hybris/vulkan/compat/shaders/"bc7.glsl "$ROOT/hybris/vulkan/compat/shaders/"bc6h.glsl "$ROOT/hybris/vulkan/compat/shaders/"LICENSE.bcdec "$ROOT/hybris/vulkan/compat/shaders/"bc_decode.{comp,inc} "$ROOT/hybris/vulkan/compat/shaders/generate-bc.py" "$PROBE_SRC/compat/shaders/"
cp bc_half_verify.h bc6h_fixture.inc bc7_fixture.inc widget_pipeline.h bc_image_verify.h bc_fixture.h native_buffer_render.h native_buffer_fixture.h scaled_divisor.h scaled_fixture.h allocation_fixture.h render_path.h tls_fixture.cpp sync_fixture.h stdio_fixture.h widget_fixture.h "$PROBE_SRC/"
for source in "${SOURCES[@]}"; do
    [[ "$source" == compat/* ]] || cp "$source" "$PROBE_SRC/"
done
cp probe.h dispatch_commands.inc capability_fields.inc feature_compare.inc property_compare.inc "$PROBE_SRC/"
cp shaders/point-size.vert shaders/point-size.frag shaders/point-size.inc shaders/generate-point-size.py shaders/scaled.builtins.inc shaders/generate-unused-builtins.py shaders/widget-multi.vert shaders/widget-multi.frag shaders/widget-multi.vert.inc shaders/widget-multi.frag.inc shaders/generate-widget-multi.py shaders/bc-sample.comp shaders/bc-sample.inc shaders/generate-bc-sample.py shaders/scaled.divisor.vert shaders/scaled.divisor.inc shaders/group_decorations.py shaders/scaled.group.inc shaders/scaled.group-multi.inc shaders/scaled.group-spec.inc shaders/scaled.spec-direct.inc shaders/scaled.spec.vert shaders/scaled.spec.inc shaders/scaled.aggregate.vert shaders/scaled.matrix.inc shaders/scaled.array.inc shaders/scaled.nested.inc shaders/scaled.matarray.inc shaders/scaled.literal.inc shaders/generate-scaled.py shaders/scaled.multi.inc shaders/scaled.vert shaders/scaled.frag shaders/scaled.vert.inc shaders/scaled.frag.inc shaders/widget.vert shaders/widget.frag shaders/widget.vert.inc shaders/widget.frag.inc shaders/widget-large.vert.inc shaders/widget-large.frag.inc "$PROBE_SRC/shaders/"


# Use the pinned repository toolchain unless the caller explicitly overrides it.
if [[ -z "${GLIBC_CC:-}" ]]; then
    if [[ -z "${BUILDER_IMAGE:-}" ]]; then
        BUILDER_IMAGE="$("$ROOT/tools/ensure-builder.sh")"
    fi
    engine="${CONTAINER_ENGINE:-podman}"
    BUILDER_ID="$("$engine" image inspect --format '{{.Id}}' "$BUILDER_IMAGE")"
    HOST_HYBRIS="$(cd "$HYBRIS_LIB" && pwd)"
    compile_glibc() {
        local cflags="$1" libs="$2" dest="$3"
        local engine="${CONTAINER_ENGINE:-podman}"
        "$engine" run --rm --network host --userns=keep-id \
            --volume "$(cd "$PROBE_SRC" && pwd):/src:Z" \
            --volume "$(cd "$BUNDLE" && pwd):/out:Z" \
            --volume "$HOST_HYBRIS:/hybris:Z" \
            --workdir /src "$BUILDER_ID" \
            aarch64-linux-gnu-gcc -O2 -Wall -Wextra -pthread $cflags \
            "${SOURCES[@]}" -ldl -lpthread ${libs//$HYBRIS_LIB//hybris} -o "/out/$(basename "$dest")"
    }
else
    compile_glibc() {
        local cflags="$1" libs="$2" dest="$3"
        (cd "$PROBE_SRC" && "$GLIBC_CC" -O2 -Wall -Wextra -pthread $cflags "${SOURCES[@]}" -ldl -lpthread $libs -o "$dest")
    }
fi

compile_glibc "" "" "$BUNDLE/probe-glibc"
compile_glibc "-DHYBRIS_PROBE_LINKED" \
    "-L$HYBRIS_LIB -Wl,-rpath-link,$HYBRIS_LIB -lvulkan" \
    "$BUNDLE/probe-glibc-linked"
(cd "$PROBE_SRC" && "$BIONIC_CC" -O2 -Wall -Wextra -pthread "${SOURCES[@]}" -ldl -o "$BUNDLE/probe-bionic")
(cd "$PROBE_SRC" && "$BIONIC_CC" -x c++ -std=c++11 -fPIC -shared -fno-exceptions -fno-rtti \
    tls_fixture.cpp -o "$BUNDLE/libtls-fixture.so")
(cd "$PROBE_SRC" && "$BIONIC_CC" -x c++ -std=c++11 -fPIC -shared -fno-exceptions -fno-rtti \
    -fno-emulated-tls tls_fixture.cpp -o "$BUNDLE/libtls-native-fixture.so")
python3 - "$BUNDLE" "$BIONIC_CC" "${GLIBC_CC:-}" "${BUILDER_ID:-}" "$ROOT/tools" <<'PYTHON'
import json
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, sys.argv[5])
from build_inputs import tree_identity
from manifest import sha256_file, build_id

bundle = Path(sys.argv[1])
payload = {
    'source': tree_identity(bundle / 'src'),
    'bionic_compiler': subprocess.check_output([sys.argv[2], '--version'], text=True),
    'glibc_builder_image_id': sys.argv[4] or None,
    'glibc_compiler_override': (
        subprocess.check_output([sys.argv[3], '--version'], text=True) if sys.argv[3] else None),
    'build_script_sha256': sha256_file(Path(sys.argv[5]).parent / 'tests/baseline/build.sh'),
    'binaries': [
        {'name': name, 'sha256': sha256_file(bundle / name), 'build_id': build_id(bundle / name)}
        for name in ('probe-glibc', 'probe-glibc-linked', 'probe-bionic', 'libtls-fixture.so', 'libtls-native-fixture.so')],
}
(bundle / 'probe-manifest.json').write_text(json.dumps(payload, indent=2) + '\n')
PYTHON
echo "$BUNDLE"
