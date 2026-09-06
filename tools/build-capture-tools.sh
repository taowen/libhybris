#!/usr/bin/env bash
# Optional standard Vulkan capture/replay tools; no automatic baseline download.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/tests/baseline/build/gfxreconstruct}"
revision=c2ff0eecc7a7f43aa236a5c98097a685b928b782
engine="${CONTAINER_ENGINE:-podman}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
src="$out/source"
if [[ ! -d "$src/.git" ]]; then
    git init -q "$src"
    git -C "$src" remote add origin https://github.com/LunarG/gfxreconstruct.git
fi
if ! git -C "$src" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$src" fetch --depth=1 origin "$revision"
fi
if [[ -n "$(git -C "$src" status --porcelain --untracked-files=no)" ]]; then
    echo "capture source has local changes: $src" >&2
    exit 2
fi
git -C "$src" checkout --detach "$revision"
git -C "$src" submodule update --init --depth=1 \
    external/Vulkan-Headers external/SPIRV-Headers external/SPIRV-Reflect
base="$("$root/tools/ensure-builder.sh")"
recipe="$root/tools/container/Containerfile.capture"
key="$(printf '%s\n%s\n' "$base" "$(sha256sum "$recipe")" | sha256sum | cut -d' ' -f1)"
image="localhost/libhybris-capture:$key"
if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    "$engine" build --network=host --build-arg BASE_IMAGE="$base" \
        --file "$recipe" --tag "$image" "$root/tools/container"
fi
"$engine" run --rm --userns=keep-id --user "$(id -u):$(id -g)" --volume "$out:/work:Z" "$image" \
    bash -eu -c '
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
cmake -S /work/source -B /work/build -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_WERROR=OFF \
    -DGFXRECON_ENABLE_OPENXR=OFF -DGFXRECON_TOCPP_SUPPORT=OFF \
    -DRUN_TESTS=OFF -DGFXRECON_INCLUDE_TEST_APPS=OFF \
    -DLZ4_LIBRARY=/usr/lib/aarch64-linux-gnu/liblz4.so \
    -DZSTD_LIBRARY=/usr/lib/aarch64-linux-gnu/libzstd.so \
    -DZLIB_LIBRARY=/usr/lib/aarch64-linux-gnu/libz.so \
    -DCMAKE_INSTALL_PREFIX=/work/install
cmake --build /work/build --parallel 4
rm -rf /work/install
cmake --install /work/build
mkdir -p /work/install/runtime
for name in libz.so.1 liblz4.so.1 libzstd.so.1 libxxhash.so.0 libxcb-keysyms.so.1 \
            libXrandr.so.2 libXrender.so.1 libXext.so.6; do
    cp -L /usr/lib/aarch64-linux-gnu/$name /work/install/runtime/
done
dpkg-query -W > /work/builder-packages.txt
'
git -C "$src" submodule status > "$out/submodules.txt"
"$engine" image inspect --format '{{.Id}}' "$image" > "$out/builder-image.txt"
printf '%s\n' "$revision" > "$out/source-revision.txt"
python3 - "$out" <<'PY'
import hashlib
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
install = root / 'install'
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
manifest = {name: (root / name).read_text() for name in
            ('source-revision.txt', 'submodules.txt', 'builder-image.txt')}
manifest['builder_packages_sha256'] = digest(root / 'builder-packages.txt')
manifest['files'] = {str(p.relative_to(install)): digest(p)
                     for p in install.rglob('*') if p.is_file()}
(install / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
echo "$out/install"
