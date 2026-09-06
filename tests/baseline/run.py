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
from capture import stage_tools, run_capture

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from manifest import sha256_file, unexpected_platforms, verify_manifest  # noqa: E402

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--hybris-lib', type=Path, help='Installed directory containing libEGL.so.1 and libhybris/')
p.add_argument('--runtime', type=Path, help='Directory containing glibc loader and runtime dependencies')
p.add_argument('--manifest', type=Path, help='Provenance JSON from tools/manifest.py')
p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent / 'build/results')
p.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent / 'build/bundle')
p.add_argument('--icd-hal', help='Run additional standard-loader cases with this Android Vulkan HAL path')
p.add_argument('--vulkan-loader', type=Path, help='glibc AArch64 standard libvulkan.so.1 for --icd-hal')
p.add_argument('--validation-manifest', type=Path, help='Original layer JSON matching --validation-layer')
p.add_argument('--validation-layer', type=Path, help='glibc AArch64 libVkLayer_khronos_validation.so; requires --icd-hal')
p.add_argument('--capture-tools', type=Path, help='GFXReconstruct install from tools/build-capture-tools.sh; requires --icd-hal')
a = p.parse_args()
if bool(a.validation_layer) != bool(a.validation_manifest):
    p.error('--validation-layer and --validation-manifest must be supplied together')
if a.validation_layer and not a.icd_hal:
    p.error('--validation-layer requires --icd-hal')
if a.capture_tools and not a.icd_hal:
    p.error('--capture-tools requires --icd-hal')
if bool(a.icd_hal) != bool(a.vulkan_loader):
    p.error('--icd-hal and --vulkan-loader must be supplied together')
here = Path(__file__).resolve().parent
default_out = Path(__file__).resolve().parent / 'build'
if a.hybris_lib is None:
    a.hybris_lib = default_out / 'install/usr/lib/hybris'
if a.runtime is None:
    a.runtime = default_out / 'runtime'
if a.manifest is None and a.hybris_lib == default_out / 'install/usr/lib/hybris' and a.runtime == default_out / 'runtime':
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

probe_manifest = a.bundle / 'probe-manifest.json'
probe_provenance = None
if probe_manifest.is_file():
    probe_provenance = json.loads(probe_manifest.read_text())
    expected = {entry['name']: entry['sha256'] for entry in probe_provenance['binaries']}
    actual = {name: sha256_file(a.bundle / name)
              for name in ('probe-bionic', 'probe-glibc', 'probe-glibc-linked')}
    if actual != expected:
        raise SystemExit('probe manifest does not match the supplied executables')

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
    if code in {124, 142}:
        return 'TIMEOUT'
    if code < 0 or code == 139 or code == 134:
        return 'CRASH'
    return 'FAIL'


def kill_remote() -> None:
    # The shell records the exact child PID before exec, including constructors.
    # Check its executable path before killing, so a reused PID is not targeted.
    shell("if [ -f " + remote + "/probe.pid ]; then "
          "read pid < " + remote + "/probe.pid; "
          "case $(readlink /proc/$pid/exe) in " + remote +
          "/*) kill -9 \"$pid\" 2>/dev/null ;; esac; fi",
          check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
          timeout=10)


metadata = {name: prop(name) for name in ['ro.product.model', 'ro.build.fingerprint', 'ro.build.version.sdk']}
metadata['run_id'] = run_id
metadata['commands'] = {}
metadata['driver_observations'] = {}
metadata['mapping_note'] = 'Snapshots observe file-backed paths at named phases. Staged hashes describe deployment files; Android file hashes are collected by path after execution, not from mapped pages.'
metadata['hybris_source_commit'] = subprocess.check_output(
    ['git', '-C', str(here), 'rev-parse', 'HEAD'], text=True
).strip()
metadata['hybris_lib'] = str(a.hybris_lib.resolve())
metadata['runtime'] = str(a.runtime.resolve())
metadata['manifest'] = str(a.manifest.resolve()) if a.manifest else None
metadata['probe_manifest'] = str(probe_manifest.resolve()) if probe_provenance else None
if a.manifest:
    provenance = json.loads(a.manifest.read_text())
    verify_manifest(provenance, a.hybris_lib, a.runtime)
    metadata['binary_source_commit'] = provenance.get('source_commit')
    metadata['binary_source_dirty'] = provenance.get('source_dirty')
    metadata['compiler'] = provenance.get('compiler')
    metadata['configure_args'] = provenance.get('configure_args')
    metadata['note'] = 'binary_source_commit comes from the build manifest, not from inspecting this git HEAD.'
else:
    metadata['note'] = 'No build manifest; source HEAD does not identify the loaded binaries.'
(a.out / 'device.json').write_text(json.dumps(metadata, indent=2) + '\n')
if a.manifest:
    shutil.copy2(a.manifest, a.out / 'manifest.json')
if probe_provenance:
    shutil.copy2(probe_manifest, a.out / 'probe-manifest.json')

stage = a.out / 'stage'
if stage.exists():
    shutil.rmtree(stage)
stage.mkdir()
shutil.copytree(a.hybris_lib, stage / 'hybris', symlinks=False)
shutil.copytree(a.runtime, stage / 'glibc', symlinks=False)
for binary in ['probe-bionic', 'probe-glibc', 'probe-glibc-linked']:
    shutil.copy2(a.bundle / binary, stage / binary)
    if probe_provenance and sha256_file(stage / binary) != expected[binary]:
        raise SystemExit('probe changed while staging: ' + binary)


cases = [
    ('native', 'vk', 'probe-bionic'),
    ('native', 'vk-dlsym', 'probe-bionic'),
    ('native', 'vk-gdpa', 'probe-bionic'),
    ('native', 'vk-core11', 'probe-bionic'),
    ('native', 'vk-khr11', 'probe-bionic'),
    ('native', '2', 'probe-bionic'),
    ('native', '3', 'probe-bionic'),
    ('native', '0', 'probe-bionic'),
    ('native', 'dispatch', 'probe-bionic'),
    ('native', 'life', 'probe-bionic'),
    ('native', 'unload', 'probe-bionic'),
    ('native', 'tls', 'probe-bionic'),
    ('native', 'caps', 'probe-bionic'),
    ('native', 'ubo', 'probe-bionic'),
    ('hybris', 'vk', 'probe-glibc'),
    ('hybris', 'vk-dlsym', 'probe-glibc'),
    ('hybris', 'vk-gdpa', 'probe-glibc'),
    ('hybris', 'vk-core11', 'probe-glibc'),
    ('hybris', 'vk-khr11', 'probe-glibc'),
    ('hybris', '2', 'probe-glibc'),
    ('hybris', '3', 'probe-glibc'),
    ('hybris', '0', 'probe-glibc'),
    ('hybris', 'dispatch', 'probe-glibc'),
    ('hybris', 'life', 'probe-glibc'),
    ('hybris', 'unload', 'probe-glibc'),
    ('hybris', 'init', 'probe-glibc'),
    ('hybris', 'tls', 'probe-glibc'),
    ('hybris', 'caps', 'probe-glibc'),
    ('hybris', 'ubo', 'probe-glibc'),
    ('hybris-linked', 'dispatch', 'probe-glibc-linked'),
    ('hybris-linked', 'vk', 'probe-glibc-linked'),
]

if a.icd_hal:
    adapter = stage / 'hybris/libhybris-vulkan-icd.so.0'
    if not adapter.is_file():
        raise SystemExit('ICD adapter missing from hybris install')
    (stage / 'standard').mkdir()
    shutil.copy2(a.vulkan_loader, stage / 'standard/libvulkan.so.1')
    metadata['standard_loader_sha256'] = sha256_file(a.vulkan_loader)
    (stage / 'driver.json').write_text(json.dumps({
        'file_format_version': '1.0.0',
        'ICD': {'library_path': remote + '/hybris/libhybris-vulkan-icd.so.0',
                'api_version': '1.0.0'}}))
    # The manifest starts at 1.0; interface 5 queries the HAL's supported
    # instance version via vkEnumerateInstanceVersion.
    cases += [('icd', mode, 'probe-glibc')
              for mode in ('vk', 'vk-dlsym', 'vk-gdpa', 'vk-core11', 'vk-khr11', 'dispatch', 'life', 'unload', 'tls', 'caps', 'ubo')]
    cases += [('icd-linked', mode, 'probe-glibc-linked') for mode in ('vk', 'dispatch')]
    if a.validation_layer:
        (stage / 'layers').mkdir()
        shutil.copy2(a.validation_layer, stage / 'layers/libVkLayer_khronos_validation.so')
        metadata['validation_layer_sha256'] = sha256_file(a.validation_layer)
        metadata['validation_manifest_sha256'] = sha256_file(a.validation_manifest)
        layer_json = json.loads(a.validation_manifest.read_text())
        if layer_json['layer']['name'] != 'VK_LAYER_KHRONOS_validation':
            raise SystemExit('expected Khronos validation layer manifest')
        layer_json['layer']['library_path'] = './libVkLayer_khronos_validation.so'
        (stage / 'layers/validation.json').write_text(json.dumps(layer_json))
        cases.extend([('icd', mode, 'probe-glibc') for mode in ('validation', 'ubo-validation')])

if a.capture_tools:
    stage_tools(a.capture_tools, stage, metadata, sha256_file)

results = []
observed_paths = set()
try:
    if a.manifest:
        verify_manifest(provenance, stage / 'hybris', stage / 'glibc')
    shell('mkdir -p ' + remote, check=True)
    subprocess.run(adb + ['push', str(stage) + '/.', remote + '/'],
                   check=True, stdout=subprocess.DEVNULL)
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
        if backend in {'icd', 'icd-linked'}:
            command = (
                'HYBRIS_LINKER_DIR=$PWD/hybris/libhybris/linker '
                'HYBRIS_ANDROID_SDK_VERSION=' + shlex.quote(metadata['ro.build.version.sdk']) + ' '
                'HYBRIS_VULKAN_HAL=' + shlex.quote(a.icd_hal) + ' '
                'VK_DRIVER_FILES=$PWD/driver.json VK_LAYER_PATH=$PWD/layers '
                'PROBE_VK=$PWD/standard/libvulkan.so.1 '
                './glibc/ld-linux-aarch64.so.1 --library-path ./standard:./hybris:./glibc ./'
                + binary + ' ')
        name = backend + '-' + mode
        metadata['commands'][name] = {'directory': remote, 'command': command + mode}
        try:
            r = shell(
                'cd ' + remote + ' && sh -c ' + shlex.quote(
                    'echo $$ > probe.pid; exec env ' + command + mode),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=35,
            )
            output, code = r.stdout, r.returncode
        except subprocess.TimeoutExpired as exc:
            output, code = exc.stdout or b'', 124
            kill_remote()
        (a.out / (name + '.log')).write_bytes(output or b'')
        decoded = (output or b'').decode('utf-8', errors='replace')
        registry_entries = []
        for line in decoded.splitlines():
            if line.startswith('REGISTRY_ENTRY '):
                _, command_name, *fields = line.split()
                registry_entries.append(dict(name=command_name, **{
                    key: bool(int(value)) for key, value in (field.split('=') for field in fields)}))
        if registry_entries:
            (a.out / (name + '-registry.json')).write_text(
                json.dumps(registry_entries, indent=2) + '\n')
        metadata['driver_observations'][name] = [
            line for line in decoded.splitlines()
            if line.startswith(('GPU ', 'EGL ', 'GL vendor='))]
        mappings = []
        for line in decoded.splitlines():
            if not line.startswith('MAPPING\t'):
                continue
            _, phase, mapping = line.split('\t', 2)
            parts = mapping.split(None, 5)
            if len(parts) != 6:
                continue
            address, permissions, offset, device, inode, path = parts
            entry = dict(phase=phase, address=address, permissions=permissions,
                         offset=offset, device=device, inode=inode, path=path)
            if path.startswith(remote + '/'):
                relative = path[len(remote) + 1:]
                staged = stage / relative
                if staged.is_file():
                    entry['staged_sha256'] = sha256_file(staged)
            elif path.startswith(('/system/', '/vendor/', '/apex/', '/odm/')):
                observed_paths.add(path)
            mappings.append(entry)
        (a.out / (name + '-mappings.json')).write_text(
            json.dumps(mappings, indent=2) + '\n')
        status = classify(code)
        results.append(dict(case=name, status=status, exit_code=code, binary=binary))
        print(name, status, 'exit=' + str(code), flush=True)
    if a.capture_tools:
        try:
            run_capture(shell, adb, remote, a.out, metadata['commands']['icd-ubo']['command'],
                        metadata, kill_remote)
            code = 0
        except subprocess.TimeoutExpired as exc:
            code = 124
            (a.out / 'capture-error.log').write_text(str(exc) + '\n')
        except subprocess.CalledProcessError as exc:
            code = exc.returncode
            (a.out / 'capture-error.log').write_text(str(exc) + '\n')
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            code = 2
            (a.out / 'capture-error.log').write_text(str(exc) + '\n')
        finally:
            try:
                subprocess.run(adb + ['pull', remote + '/capture/.', str(a.out / 'capture')],
                               check=True, capture_output=True, timeout=35)
            except (OSError, subprocess.SubprocessError) as exc:
                (a.out / 'capture-pull-error.log').write_text(str(exc) + '\n')
        status = classify(code)
        results.append(dict(case='icd-capture-replay', status=status, exit_code=code))
        print('icd-capture-replay', status, 'exit=' + str(code), flush=True)
finally:
    try:
        # Hash Android files named by the snapshots. These are path-content
        # hashes after execution, not a readback of live mapped pages.
        if observed_paths:
            try:
                hashes = shell(
                    'sha256sum ' + ' '.join(shlex.quote(p) for p in sorted(observed_paths)),
                    capture_output=True, text=True, timeout=30)
                (a.out / 'android-mapped-files.sha256').write_text(hashes.stdout)
                (a.out / 'android-mapped-files-errors.log').write_text(hashes.stderr)
            except (OSError, subprocess.SubprocessError) as exc:
                (a.out / 'android-mapped-files-errors.log').write_text(str(exc) + '\n')
    finally:
        try:
            kill_remote()
            shell('rm -rf ' + remote, check=False, timeout=10)
        finally:
            (a.out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
            (a.out / 'device.json').write_text(json.dumps(metadata, indent=2) + '\n')

failed = [r for r in results if r['status'] in {'FAIL', 'TIMEOUT', 'CRASH'}]
raise SystemExit(1 if failed else 0)
