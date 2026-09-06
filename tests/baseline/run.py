#!/usr/bin/env python3
"""Run the same headless probe against Android and libhybris, without an APK."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from manifest import unexpected_platforms, sha256_file, build_id  # noqa: E402

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--hybris-lib', type=Path, help='Installed directory containing libEGL.so.1 and libhybris/')
p.add_argument('--runtime', type=Path, help='Directory containing glibc loader and runtime dependencies')
p.add_argument('--manifest', type=Path, help='Provenance JSON from tools/manifest.py')
p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent / 'build/results')
p.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent / 'build/bundle')
a = p.parse_args()
here = Path(__file__).resolve().parent
default_out = Path(__file__).resolve().parent / 'build'
if a.hybris_lib is None:
    a.hybris_lib = default_out / 'install/usr/lib/hybris'
if a.runtime is None:
    a.runtime = default_out / 'runtime'
if a.manifest is None:
    candidate = default_out / 'manifest.json'
    a.manifest = candidate if candidate.is_file() else None

if not a.hybris_lib.is_dir():
    raise SystemExit(f'missing --hybris-lib {a.hybris_lib}')
if not a.runtime.is_dir():
    raise SystemExit(f'missing --runtime {a.runtime}')

stale = unexpected_platforms(a.hybris_lib)
if stale:
    raise SystemExit('refusing stale/unknown platform plugins:\n  ' + '\n  '.join(stale))

for binary in ['probe-bionic', 'probe-glibc', 'probe-glibc-linked']:
    if not (a.bundle / binary).is_file():
        raise SystemExit(f'missing {a.bundle / binary}; run tests/baseline/build.sh')

run_id = time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8]
a.out = a.out / run_id
a.out.mkdir(parents=True, exist_ok=True)
adb = [os.environ.get('ADB', 'adb'), '-s', a.serial]
remote = '/data/local/tmp/libhybris-baseline-' + run_id


def shell(command, **kwargs):
    return subprocess.run(adb + ['shell', command], **kwargs)


def prop(name):
    return shell('getprop ' + name, check=True, capture_output=True, text=True).stdout.strip()


def classify(code: int) -> str:
    if code == 0:
        return 'PASS'
    if code == 3:
        return 'UNSUPPORTED'
    if code == 124:
        return 'TIMEOUT'
    if code < 0 or code == 139 or code == 134:
        return 'CRASH'
    return 'FAIL'


def kill_remote(marker: str) -> None:
    # Host timeout is not the same as the device process exiting.
    shell(
        "pids=$(ps -A -o PID,NAME,ARGS 2>/dev/null | grep "
        + shlex.quote(marker)
        + " | grep -v grep | awk '{print $1}'); "
        "if [ -n \"$pids\" ]; then kill -9 $pids 2>/dev/null; fi",
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


metadata = {name: prop(name) for name in ['ro.product.model', 'ro.build.fingerprint', 'ro.build.version.sdk']}
metadata['run_id'] = run_id
metadata['hybris_source_commit'] = subprocess.check_output(
    ['git', '-C', str(here), 'rev-parse', 'HEAD'], text=True
).strip()
metadata['hybris_lib'] = str(a.hybris_lib.resolve())
metadata['runtime'] = str(a.runtime.resolve())
metadata['manifest'] = str(a.manifest.resolve()) if a.manifest else None
if a.manifest:
    provenance = json.loads(a.manifest.read_text())
    metadata['binary_source_commit'] = provenance.get('source_commit')
    metadata['binary_source_dirty'] = provenance.get('source_dirty')
    metadata['compiler'] = provenance.get('compiler')
    metadata['configure_args'] = provenance.get('configure_args')
    metadata['note'] = 'binary_source_commit comes from the build manifest, not from inspecting this git HEAD.'
else:
    metadata['note'] = 'No build manifest; source HEAD does not identify the loaded binaries.'
metadata['loaded'] = {
    'libvulkan': {
        'path': 'libvulkan.so.1.2.183',
        'sha256': sha256_file(a.hybris_lib / 'libvulkan.so.1.2.183'),
        'build_id': build_id(a.hybris_lib / 'libvulkan.so.1.2.183'),
    },
    'libEGL': {
        'path': 'libEGL.so.1.0.0',
        'sha256': sha256_file(a.hybris_lib / 'libEGL.so.1.0.0'),
        'build_id': build_id(a.hybris_lib / 'libEGL.so.1.0.0'),
    },
    'probe-glibc': {
        'sha256': sha256_file(a.bundle / 'probe-glibc'),
        'build_id': build_id(a.bundle / 'probe-glibc'),
    },
}
(a.out / 'device.json').write_text(json.dumps(metadata, indent=2) + '\n')
if a.manifest:
    shutil.copy2(a.manifest, a.out / 'manifest.json')

stage = a.out / 'stage'
if stage.exists():
    shutil.rmtree(stage)
stage.mkdir()
shutil.copytree(a.hybris_lib, stage / 'hybris', symlinks=False)
shutil.copytree(a.runtime, stage / 'glibc', symlinks=False)
for binary in ['probe-bionic', 'probe-glibc', 'probe-glibc-linked']:
    shutil.copy2(a.bundle / binary, stage / binary)

shell('mkdir -p ' + remote, check=True)
subprocess.run(adb + ['push', str(stage) + '/.', remote + '/'], check=True, stdout=subprocess.DEVNULL)

cases = [
    ('native', 'vk', 'probe-bionic'),
    ('native', '2', 'probe-bionic'),
    ('native', '3', 'probe-bionic'),
    ('native', '0', 'probe-bionic'),
    ('native', 'dispatch', 'probe-bionic'),
    ('hybris', 'vk', 'probe-glibc'),
    ('hybris', '2', 'probe-glibc'),
    ('hybris', '3', 'probe-glibc'),
    ('hybris', '0', 'probe-glibc'),
    ('hybris', 'dispatch', 'probe-glibc'),
    ('hybris-linked', 'dispatch', 'probe-glibc-linked'),
    ('hybris-linked', 'vk', 'probe-glibc-linked'),
]

results = []
try:
    for backend, mode, binary in cases:
        if backend == 'native':
            command = (
                'PROBE_VK=libvulkan.so PROBE_EGL=libEGL.so PROBE_GLES=libGLESv2.so '
                './' + binary + ' '
            )
        else:
            command = (
                'HYBRIS_LINKER_DIR=$PWD/hybris/libhybris/linker '
                'HYBRIS_EGLPLATFORM_DIR=$PWD/hybris/libhybris '
                'HYBRIS_VULKANPLATFORM_DIR=$PWD/hybris/libhybris '
                'HYBRIS_EGLPLATFORM=null HYBRIS_VULKANPLATFORM=null '
                'HYBRIS_ANDROID_SDK_VERSION=' + shlex.quote(metadata['ro.build.version.sdk']) + ' '
                './glibc/ld-linux-aarch64.so.1 --library-path ./hybris:./glibc ./' + binary + ' '
            )
        name = backend + '-' + mode
        marker = remote + '/' + binary
        try:
            r = shell(
                'cd ' + remote + ' && ' + command + mode,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=35,
            )
            output, code = r.stdout, r.returncode
        except subprocess.TimeoutExpired as exc:
            output, code = exc.stdout or b'', 124
            kill_remote(marker)
        (a.out / (name + '.log')).write_bytes(output or b'')
        status = classify(code)
        results.append(dict(case=name, status=status, exit_code=code, binary=binary))
        print(name, status, 'exit=' + str(code), flush=True)
finally:
    kill_remote(remote)
    shell('rm -rf ' + remote, check=False)
    (a.out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')

failed = [r for r in results if r['status'] in {'FAIL', 'TIMEOUT', 'CRASH'}]
raise SystemExit(1 if failed else 0)
