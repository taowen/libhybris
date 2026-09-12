#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
arlinux="$(cd "$root/../.." && pwd)"
"$arlinux/tools/build/mesa.sh"
mesa_commit="$(cat "$arlinux/build/mesa-upstream/mesa-commit.txt")"
image="$("$arlinux/tools/ensure-glibc-builder.sh")"
image_id="$(podman image inspect --format '{{.Id}}' "$image")"
out="$root/tests/desktop-gl/build"
mkdir -p "$out"
podman run --rm --userns=keep-id -v "$arlinux:/work:z" --workdir /work "$image_id" bash -eu -c '
build=/work/third_party/libhybris/tests/desktop-gl/build
rm -rf "$build/runtime"
python3 /work/third_party/libhybris/tests/desktop-gl/stage.py
aarch64-linux-gnu-gcc -O2 -Wall -Wextra /work/third_party/libhybris/tests/desktop-gl/probe.c /work/third_party/libhybris/tests/desktop-gl/packed_draw.c /work/third_party/libhybris/tests/desktop-gl/glx_context.c /work/third_party/libhybris/tests/desktop-gl/vertex_prepass.c /work/third_party/libhybris/tests/desktop-gl/procedural_draw.c /work/third_party/libhybris/tests/desktop-gl/attribute_draw.c /work/third_party/libhybris/tests/desktop-gl/multidraw_draw.c /work/third_party/libhybris/tests/desktop-gl/indexed_draw.c /work/third_party/libhybris/tests/desktop-gl/resource_draw.c -lX11 -ldl -o "$build/probe"
aarch64-linux-gnu-gcc --version > "$build/compiler.txt"
'
python3 - "$out" "$image_id" "$mesa_commit" "$arlinux/build/mesa-upstream" <<'PY'
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1])
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
(out/'manifest.json').write_text(json.dumps({'builder_id':sys.argv[2],'mesa_commit':sys.argv[3],
 'product_mesa':{p.name:p.read_text() for p in Path(sys.argv[4]).glob('mesa-*.txt')},
 'compiler':(out/'compiler.txt').read_text(),'probe_sha256':sha(out/'probe'),
 'runtime':{str(p.relative_to(out/'runtime')):sha(p) for p in (out/'runtime').rglob('*') if p.is_file()},
 'sources':{p.name:sha(p) for p in out.parent.iterdir() if p.is_file()}},indent=2))
PY
