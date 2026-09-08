#!/usr/bin/env bash
# Build VVL 1.4.362 with matching dependencies and the grouped-decoration iterator fix.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/tests/baseline/build/validation-build}"
engine="${CONTAINER_ENGINE:-podman}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
exec 9>"$out/.build.lock"
flock -n 9 || { echo "validation build already active: $out" >&2; exit 2; }
mkdir -p "$out/sources" "$out/src"
: > "$out/source-revisions.txt"
while read -r name repository revision; do
    source_dir="$out/sources/$name"
    if [[ ! -d "$source_dir/.git" ]]; then git init -q "$source_dir"; fi
    if ! git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$source_dir" fetch --depth=1 "$repository" "$revision"
    fi
    if [[ -n "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]]; then
        echo "validation dependency has local changes: $source_dir" >&2; exit 2
    fi
    git -C "$source_dir" checkout --detach "$revision"
    rm -rf "$out/src/$name"
    mkdir -p "$out/src/$name"
    git -C "$source_dir" archive "$revision" | tar -x -C "$out/src/$name"
    printf '%s %s %s\n' "$name" "$repository" "$revision" >> "$out/source-revisions.txt"
done <<'SOURCES'
Vulkan-ValidationLayers https://github.com/KhronosGroup/Vulkan-ValidationLayers.git 538f91f14cd39274263eb15e6b4228f355370353
Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git ee2ec5fd83dafce291024683b50dc89219333076
Vulkan-Utility-Libraries https://github.com/KhronosGroup/Vulkan-Utility-Libraries.git 2176ec8c5f5d2272161277ab96fe5b8f7633113e
SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers.git 496543121ce6419f23d6fa5d7194ba66c36212d2
SPIRV-Tools https://github.com/taowen/SPIRV-Tools.git adc7d8b01ae855292822192ae870ab1df19e40a3
SOURCES
python3 - "$out" "$root" <<'INPUTS'
import json
from pathlib import Path
import sys
out, root = map(Path, sys.argv[1:])
sys.path.insert(0, str(root / 'tools'))
from build_inputs import tree_identity
inputs = {p.name: tree_identity(p) for p in sorted((out / 'src').iterdir())}
(out / 'source-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
INPUTS
base="$("$root/tools/ensure-builder.sh")"
recipe="$root/tools/container/Containerfile.capture"
key="$(printf '%s\n%s\n' "$base" "$(sha256sum "$recipe")" | sha256sum | cut -d' ' -f1)"
image="localhost/libhybris-capture:$key"
if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    "$engine" build --network=host --build-arg BASE_IMAGE="$base" --file "$recipe" --tag "$image" "$root/tools/container"
fi
"$engine" run --rm --userns=keep-id --user "$(id -u):$(id -g)" --volume "$out:/work:Z" "$image" bash -eu -c '
rm -rf /work/deps
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
common=(-G Ninja -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_PREFIX_PATH=/work/deps -DCMAKE_INSTALL_LIBDIR=lib)
build_dependency() {
    local name="$1"; shift
    cmake -S "/work/src/$name" -B "/work/build/$name" "${common[@]}" -DCMAKE_INSTALL_PREFIX=/work/deps "$@"
    cmake --build "/work/build/$name" --parallel 4
    cmake --install "/work/build/$name"
}
build_dependency Vulkan-Headers
build_dependency SPIRV-Headers
build_dependency Vulkan-Utility-Libraries -DBUILD_TESTS=OFF
build_dependency SPIRV-Tools -DSPIRV-Headers_SOURCE_DIR=/work/src/SPIRV-Headers \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF
cmake -S /work/src/Vulkan-ValidationLayers -B /work/build/Vulkan-ValidationLayers "${common[@]}" \
    -DCMAKE_INSTALL_PREFIX=/work/install -DBUILD_TESTS=OFF -DBUILD_WERROR=OFF -DUSE_CUSTOM_HASH_MAP=ON -DUPDATE_DEPS=OFF
cmake --build /work/build/Vulkan-ValidationLayers --parallel 4
rm -rf /work/install
cmake --install /work/build/Vulkan-ValidationLayers
dpkg-query -W > /work/builder-packages.txt
aarch64-linux-gnu-g++ --version > /work/compiler.txt
'
"$engine" image inspect --format '{{.Id}}' "$image" > "$out/builder-image.txt"
python3 - "$out" "$root" <<'PY'
import hashlib
import json
import pathlib
import sys
out, root = map(pathlib.Path, sys.argv[1:])
sys.path.insert(0, str(root / 'tools'))
from manifest import build_id
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
install = out / 'install'
manifest = {name: (out / name).read_text() for name in ('source-revisions.txt', 'builder-image.txt', 'compiler.txt')}
manifest['source_inputs_sha256'] = sha(out / 'source-inputs.json')
manifest['source_tree_sha256'] = {name: item['sha256'] for name, item in json.loads((out / 'source-inputs.json').read_text()).items()}
manifest['input_fingerprint_script_sha256'] = sha(root / 'tools/build_inputs.py')
manifest['container_recipe_sha256'] = sha(root / 'tools/container/Containerfile.capture')
manifest['build_script_sha256'] = sha(root / 'tools/build-validation-layer.sh')
manifest['builder_packages_sha256'] = sha(out / 'builder-packages.txt')
manifest['cmake_cache_sha256'] = {str(p.relative_to(out)): sha(p) for p in (out / 'build').glob('*/CMakeCache.txt')}
manifest['files'] = {str(p.relative_to(install)): {'sha256': sha(p), 'build_id': build_id(p) if p.suffix == '.so' else None}
                     for p in install.rglob('*') if p.is_file()}
(install / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
printf '%s\n' "$out/install"
