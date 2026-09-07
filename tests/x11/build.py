#!/usr/bin/env python3
"""Build the independent XCB probe, Android supervisor and TAWC-DRI Xwayland.

The supplied NDK prefix is a dependency input, not an install destination.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--xwayland-source', type=Path, required=True)
p.add_argument('--ndk-prefix', type=Path, required=True)
p.add_argument('--ndk', type=Path, default=Path.home() / 'Android/Sdk/ndk/29.0.14206865')
a = p.parse_args()
out = ROOT / 'tests/x11/build'; out.mkdir(exist_ok=True)
source = out / 'xwayland-source'; build = out / 'xwayland-build'
patch = ROOT / 'tests/x11/patches/tawc-dri.patch'
def run(*args, **kwargs): return subprocess.run([str(x) for x in args], check=True, **kwargs)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
revision = subprocess.check_output(['git', '-C', str(a.xwayland_source), 'rev-parse', 'HEAD'], text=True).strip()
inputs = {'xwayland_commit': revision, 'patch_sha256': sha(patch),
          'ndk_prefix': str(a.ndk_prefix.resolve()),
          'cross_files': {name: sha(a.ndk_prefix / name) for name in ('android-cross.ini', 'native.ini')},
          'server_build_mode': 'release/tawc-0.3/no-glamor/no-glx/no-drm/no-dri3/no-mitshm',
          'dependency_files': {str(f.relative_to(a.ndk_prefix)): sha(f)
              for directory in ('include', 'lib', 'share/pkgconfig')
              for f in (a.ndk_prefix / directory).rglob('*') if f.is_file()}}
identity = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
stamp = out / 'server-inputs.json'
if not stamp.is_file() or json.loads(stamp.read_text()).get('identity') != identity:
    if source.exists(): shutil.rmtree(source)
    if build.exists(): shutil.rmtree(build)
    source.mkdir()
    with subprocess.Popen(['git', '-C', str(a.xwayland_source), 'archive', revision], stdout=subprocess.PIPE) as archive:
        run('tar', 'xf', '-', '-C', source, stdin=archive.stdout)
        archive.stdout.close()
        if archive.wait(): raise RuntimeError('source archive failed')
    run('git', 'init', '-q', source)
    run('git', '-C', source, 'apply', patch)
    run('meson', 'setup', build, source, '--cross-file', a.ndk_prefix / 'android-cross.ini',
        '--native-file', a.ndk_prefix / 'native.ini', '--buildtype=release', '-Dtawc=true',
        *('-D' + key + '=false' for key in ('glamor', 'glx', 'xwayland_ei', 'libdecor', 'docs', 'devel-docs',
          'docs-pdf', 'secure-rpc', 'xdmcp', 'xdm-auth-1', 'libunwind', 'xselinux', 'dri3', 'drm', 'xvfb',
          'systemd_notify', 'mitshm', 'ipv6', 'input_thread', 'xf86bigfont')),
        '-Dsha1=libsha1', '-Dxkb_dir=/data/user/0/io.taowen.hybriswsitest/files/xkb')
    stamp.write_text(json.dumps({'identity': identity, **inputs}, indent=2))
run('meson', 'compile', '-C', build, '-j', '4')
cc = a.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang'
run(cc, '-O2', '-g', '-Wall', '-Wextra', ROOT / 'tests/x11/session.c', '-o', out / 'x11-session')
# Build only the glibc client in the established pinned cross-builder.
engine = os.environ.get('CONTAINER_ENGINE', 'podman')
image = subprocess.check_output([str(ROOT / 'tools/ensure-builder.sh')], text=True).strip()
image_id = subprocess.check_output([engine, 'image', 'inspect', '--format', '{{.Id}}', image], text=True).strip()
client = out / 'client-source'; client.mkdir(exist_ok=True)
for name in ('probe_xcb.c', 'render.c', 'resize.c', 'render.h'): shutil.copy2(ROOT / 'tests/x11' / name, client / name)
run(engine, 'run', '--rm', '--userns=keep-id', '--volume', str(out) + ':/out:Z', '--workdir', '/out/client-source',
    image_id, 'bash', '-eu', '-c',
    'aarch64-linux-gnu-gcc -O2 -g -Wall -Wextra probe_xcb.c render.c resize.c -lxcb -lX11-xcb -lX11 -ldl -o /out/probe-xcb')
server = out / 'server'
if server.exists(): shutil.rmtree(server)
server.mkdir(); shutil.copy2(build / 'hw/xwayland/Xwayland', server / 'Xwayland')
# Bionic system libraries must stay device supplied.
external = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libandroid.so', 'libz.so'}
pending = [server / 'Xwayland']; seen = set()
while pending:
    binary = pending.pop()
    for name in subprocess.check_output(['patchelf', '--print-needed', str(binary)], text=True).splitlines():
        if name in external or name in seen: continue
        dependency = a.ndk_prefix / 'lib' / name
        if not dependency.is_file(): raise RuntimeError('missing bionic dependency: ' + name)
        shutil.copy2(dependency, server / name); seen.add(name); pending.append(server / name)
manifest = {'server_inputs': inputs, 'builder': image_id, 'ndk_compiler': str(cc),
            'ndk_compiler_sha256': sha(cc), 'sources': {f.name: sha(f) for f in (ROOT / 'tests/x11').iterdir() if f.is_file()},
            'files': {str(f.relative_to(out)): sha(f) for f in [out / 'probe-xcb', out / 'x11-session', *server.iterdir()]}}
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(out)
