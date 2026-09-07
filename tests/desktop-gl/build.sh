#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
ardesk="$(cd "$root/../.." && pwd)"
mesa_commit=1cb7f0a1c9a5438045f89ad4aa83eda8fbafa09e
[[ "$(git -C "$ardesk/third_party/mesa" rev-parse HEAD)" == "$mesa_commit" ]] || { echo 'unexpected Mesa revision' >&2; exit 1; }
[[ -z "$(git -C "$ardesk/third_party/mesa" status --porcelain)" ]] || { echo 'Mesa checkout must be clean' >&2; exit 1; }
image="${BUILDER_IMAGE:-localhost/ardesk-glibc-arm64:20d8189233233158}"
image_id="$(podman image inspect --format '{{.Id}}' "$image")"
out="$root/tests/desktop-gl/build"
mkdir -p "$out"
podman run --rm --userns=keep-id -v "$ardesk:/work:z" --workdir /work "$image_id" bash -eu -c '
build=/work/third_party/libhybris/tests/desktop-gl/build
if [ ! -f "$build/mesa/build.ninja" ]; then
meson setup "$build/mesa" /work/third_party/mesa --cross-file /work/tools/build/mesa-aarch64.ini \
 --prefix=/usr --libdir=lib --buildtype=release -Dauto_features=disabled \
 -Dgallium-drivers=zink -Dvulkan-drivers= -Dplatforms= -Degl-native-platform=surfaceless \
 -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dglx=disabled -Dgbm=disabled \
 -Dllvm=disabled -Dzstd=disabled -Dshader-cache=false -Dxmlconfig=disabled -Dexpat=disabled \
 -Dzlib=enabled -Dbuild-tests=false -Dtools= -Dvideo-codecs=
fi
ninja -C "$build/mesa" -j12
rm -rf "$build/install" "$build/runtime"
DESTDIR="$build/install" ninja -C "$build/mesa" install
python3 /work/third_party/libhybris/tests/desktop-gl/stage.py
aarch64-linux-gnu-gcc -O2 -Wall -Wextra /work/third_party/libhybris/tests/desktop-gl/probe.c -ldl -o "$build/probe"
aarch64-linux-gnu-gcc --version > "$build/compiler.txt"
'
python3 - "$out" "$image_id" "$mesa_commit" <<'PY'
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1])
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
(out/'manifest.json').write_text(json.dumps({'builder_id':sys.argv[2],'mesa_commit':sys.argv[3],
 'compiler':(out/'compiler.txt').read_text(),'probe_sha256':sha(out/'probe'),
 'runtime':{p.name:sha(p) for p in (out/'runtime').iterdir()},
 'sources':{p.name:sha(p) for p in out.parent.iterdir() if p.is_file()}},indent=2))
PY
