#!/usr/bin/env bash
# Optional standard Vulkan capture/replay tools; no automatic baseline download.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/tests/baseline/build/gfxreconstruct}"
repository=https://github.com/taowen/gfxreconstruct.git
revision=1f918617ec0d34c0ee7a23a9b7199bfd5e343283
engine="${CONTAINER_ENGINE:-podman}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
exec 9>"$out/.build.lock"
flock -n 9 || { echo "capture build already active: $out" >&2; exit 2; }
src="$out/source"
if [[ ! -d "$src/.git" ]]; then
    git init -q "$src"
    git -C "$src" remote add origin "$repository"
fi
if [[ -n "$(git -C "$src" status --porcelain)" ]]; then
    echo "capture source has local changes; commit them in the fork before updating the pinned revision: $src" >&2
    exit 2
fi
if ! git -C "$src" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$src" fetch --depth=1 "$repository" "$revision"
fi
git -C "$src" checkout --detach "$revision"
git -C "$src" submodule update --init --depth=1 \
    external/Vulkan-Headers external/SPIRV-Headers external/SPIRV-Reflect
python3 - "$src" "$out" "$root" <<'INPUTS'
import json
from pathlib import Path
import sys
source, out, root = map(Path, sys.argv[1:])
sys.path.insert(0, str(root / 'tools'))
from build_inputs import tree_identity
(out / 'source-inputs.json').write_text(json.dumps(tree_identity(source), indent=2) + '\n')
INPUTS
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
aarch64-linux-gnu-g++ --version > /work/compiler.txt
'
git -C "$src" submodule status > "$out/submodules.txt"
"$engine" image inspect --format '{{.Id}}' "$image" > "$out/builder-image.txt"
printf '%s\n' "$revision" > "$out/source-revision.txt"
printf '%s\n' "$repository" > "$out/source-repository.txt"
python3 - "$out" "$root" <<'PY'
import hashlib
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
install = root / 'install'
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
manifest = {name: (root / name).read_text() for name in
            ('source-repository.txt', 'source-revision.txt', 'submodules.txt', 'builder-image.txt')}
repository = pathlib.Path(sys.argv[2])
manifest['compiler'] = (root / 'compiler.txt').read_text()
manifest['source_tree_sha256'] = json.loads((root / 'source-inputs.json').read_text())['sha256']
manifest['source_inputs_sha256'] = digest(root / 'source-inputs.json')
manifest['input_fingerprint_script_sha256'] = digest(repository / 'tools/build_inputs.py')
manifest['build_script_sha256'] = digest(repository / 'tools/build-capture-tools.sh')
manifest['container_recipe_sha256'] = digest(repository / 'tools/container/Containerfile.capture')
manifest['cmake_cache_sha256'] = digest(root / 'build/CMakeCache.txt')
manifest['builder_packages_sha256'] = digest(root / 'builder-packages.txt')
manifest['files'] = {str(p.relative_to(install)): digest(p)
                     for p in install.rglob('*') if p.is_file()}
(install / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
echo "$out/install"
