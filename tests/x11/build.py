#!/usr/bin/env python3
"""Build only the glibc XCB/Xlib client and its Android watchdog."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ndk', type=Path, default=Path.home() / 'Android/Sdk/ndk/29.0.14206865')
a = p.parse_args()
out = ROOT / 'tests/x11/build'; out.mkdir(exist_ok=True)
def run(*args, **kwargs): return subprocess.run([str(x) for x in args], check=True, **kwargs)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
cc = a.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang'
run(cc, '-O2', '-g', '-Wall', '-Wextra', ROOT / 'tests/x11/session.c', '-o', out / 'x11-session')
# Build only the glibc client in the established pinned cross-builder.
engine = os.environ.get('CONTAINER_ENGINE', 'podman')
image = subprocess.check_output([str(ROOT / 'tools/ensure-builder.sh')], text=True).strip()
image_id = subprocess.check_output([engine, 'image', 'inspect', '--format', '{{.Id}}', image], text=True).strip()
client = out / 'client-source'; client.mkdir(exist_ok=True)
for name in ('probe_xcb.c', 'render.c', 'resize.c', 'render.h'): shutil.copy2(ROOT / 'tests/x11' / name, client / name)
shutil.copy2(ROOT / 'tools/stage-runtime.py', client / 'stage-runtime.py')
shutil.rmtree(out / 'runtime', ignore_errors=True)
run(engine, 'run', '--rm', '--userns=keep-id', '--volume', str(out) + ':/out:Z', '--workdir', '/out/client-source',
    image_id, 'bash', '-eu', '-c',
    'aarch64-linux-gnu-gcc -O2 -g -Wall -Wextra probe_xcb.c render.c resize.c -lxcb -lX11-xcb -lX11 -ldl -o probe-xcb\n'
    'cp probe-xcb /out/probe-xcb\n'
    'python3 stage-runtime.py --hybris /out/client-source --runtime /out/runtime '
    '--search /usr/aarch64-linux-gnu/lib --search /lib/aarch64-linux-gnu --search /usr/lib/aarch64-linux-gnu')
manifest = {'builder': image_id, 'ndk_compiler': str(cc),
            'runtime': {p.name: sha(p) for p in (out / 'runtime').iterdir()},
            'stage_runtime_sha256': sha(client / 'stage-runtime.py'),
            'ndk_compiler_sha256': sha(cc),
            'sources': {name: sha(ROOT / 'tests/x11' / name) for name in
                        ('build.py', 'session.c', 'probe_xcb.c', 'render.c', 'resize.c', 'render.h')},
            'files': {name: sha(out / name) for name in ('probe-xcb', 'x11-session')}}
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(out)
