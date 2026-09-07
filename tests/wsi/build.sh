#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT:-$ROOT/tests/wsi/build}"
mkdir -p "$OUT/src"
cp "$ROOT/tests/wsi/probe_wayland.c" "$ROOT/tests/wsi/probe_icd_surface.c" "$ROOT/tests/wsi/surface_lifecycle.c" "$ROOT/tests/wsi/surface_lifecycle.h" "$OUT/src/"
engine="${CONTAINER_ENGINE:-podman}"
image="${BUILDER_IMAGE:-$("$ROOT/tools/ensure-builder.sh")}"
image_id="$("$engine" image inspect --format '{{.Id}}' "$image")"
"$engine" run --rm --userns=keep-id --volume "$(cd "$OUT" && pwd):/out:Z" --workdir /out/src "$image_id" bash -eu -c '
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
cp /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml .
wayland-scanner client-header xdg-shell.xml xdg-shell-client-protocol.h
wayland-scanner private-code xdg-shell.xml xdg-shell-protocol.c
aarch64-linux-gnu-gcc --version > /out/compiler.txt
pkg-config --modversion wayland-client vulkan wayland-protocols > /out/dependency-versions.txt
aarch64-linux-gnu-gcc -O2 -g -Wall -Wextra probe_wayland.c surface_lifecycle.c xdg-shell-protocol.c -lwayland-client -ldl -pthread -o /out/probe-wayland
aarch64-linux-gnu-gcc -O2 -g -Wall -Wextra probe_icd_surface.c surface_lifecycle.c xdg-shell-protocol.c -lwayland-client -ldl -pthread -o /out/probe-icd-surface
'
python3 - "$ROOT" "$OUT" "$image_id" <<'PY'
import json, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1] + '/tools')
from manifest import sha256_file, build_id
from build_inputs import tree_identity
out = Path(sys.argv[2])
(out / 'probe-manifest.json').write_text(json.dumps({'builder_id': sys.argv[3], 'source': tree_identity(out / 'src'),
    'compiler': (out / 'compiler.txt').read_text(), 'dependency_versions': (out / 'dependency-versions.txt').read_text(),
    'build_script_sha256': sha256_file(Path(sys.argv[1]) / 'tests/wsi/build.sh'),
    'binary_sha256': sha256_file(out / 'probe-wayland'), 'binary_build_id': build_id(out / 'probe-wayland'),
    'icd_surface_sha256': sha256_file(out / 'probe-icd-surface'),
    'icd_surface_build_id': build_id(out / 'probe-icd-surface')}, indent=2))
print(out)
PY
