#!/usr/bin/env python3
"""Run the same headless probe against Android and libhybris, without an APK."""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import uuid
from capture import stage_tools, run_capture
from shader_evidence import stage_shader_reference
from instance_evidence import instance_evidence
from device_evidence import device_evidence

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from manifest import sha256_file, unexpected_platforms, verify_manifest  # noqa: E402

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--case', action='append', dest='selected_cases', help='Run only BACKEND-MODE (repeatable); omitted runs all cases')
p.add_argument('--hybris-lib', type=Path, help='Installed directory containing libEGL.so.1 and libhybris/')
p.add_argument('--runtime', type=Path, help='Directory containing glibc loader and runtime dependencies')
p.add_argument('--manifest', type=Path, help='Provenance JSON from tools/manifest.py')
p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent / 'build/results')
p.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent / 'build/bundle')
p.add_argument('--bc-textures', choices=('missing', 'force'), help='Enable experimental BC1-7 image fallback for ICD cases')
p.add_argument('--point-size-compat', action='store_true', help='Remove constant-one PointSize outputs only in eligible non-point pipelines')
p.add_argument('--unused-builtins', action='store_true', help='Remove provably unaccessed output clip/cull declarations in ICD shaders')
p.add_argument('--scaled-format-trace', action='store_true', help='Audit raw/effective scaled format decisions; requires scaled compatibility')
p.add_argument('--packed-vertex-compat', choices=('missing', 'force'), help='Enable experimental packed SNORM vertex swizzle for ICD cases')
p.add_argument('--scaled-vertex-compat', choices=('missing', 'force'), help='Enable experimental scaled vertex fallback for ICD cases')
p.add_argument('--icd-hal', help='Run additional standard-loader cases with this Android Vulkan HAL path')
p.add_argument('--icd-mali-loader-quirk', action='store_true', help='Opt in to the build-id-scoped Mali MMUD loader-check workaround for ICD cases')
p.add_argument('--vulkan-loader', type=Path, help='glibc AArch64 standard libvulkan.so.1 for --icd-hal')
p.add_argument('--validation-build-manifest', type=Path, help='Build provenance from tools/build-validation-layer.sh')
p.add_argument('--validation-manifest', type=Path, help='Original layer JSON matching --validation-layer')
p.add_argument('--validation-layer', type=Path, help='glibc AArch64 libVkLayer_khronos_validation.so; requires --icd-hal')
p.add_argument('--capture-tools', type=Path, help='GFXReconstruct install from tools/build-capture-tools.sh; requires --icd-hal')
a = p.parse_args()
if a.point_size_compat and not a.icd_hal:
    p.error('--point-size-compat requires --icd-hal')
if a.unused_builtins and not a.icd_hal:
    p.error('--unused-builtins requires --icd-hal')
if a.bc_textures and not a.icd_hal:
    p.error('--bc-textures requires --icd-hal')
if a.scaled_format_trace and not a.scaled_vertex_compat:
    p.error('--scaled-format-trace requires --scaled-vertex-compat')
if a.scaled_vertex_compat and not a.icd_hal:
    p.error('--scaled-vertex-compat requires --icd-hal')
if a.packed_vertex_compat and not a.icd_hal:
    p.error('--packed-vertex-compat requires --icd-hal')
if a.icd_mali_loader_quirk and not a.icd_hal:
    p.error('--icd-mali-loader-quirk requires --icd-hal')
if bool(a.validation_layer) != bool(a.validation_manifest):
    p.error('--validation-layer and --validation-manifest must be supplied together')
if a.validation_build_manifest and not a.validation_layer:
    p.error('--validation-build-manifest requires --validation-layer')
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

for binary in ['probe-bionic', 'probe-glibc', 'probe-glibc-linked', 'libtls-fixture.so', 'libtls-native-fixture.so']:
    if not (a.bundle / binary).is_file():
        raise SystemExit(f'missing {a.bundle / binary}; run tests/baseline/build.sh')

probe_manifest = a.bundle / 'probe-manifest.json'
probe_provenance = None
if probe_manifest.is_file():
    probe_provenance = json.loads(probe_manifest.read_text())
    expected = {entry['name']: entry['sha256'] for entry in probe_provenance['binaries']}
    actual = {name: sha256_file(a.bundle / name)
              for name in ('probe-bionic', 'probe-glibc', 'probe-glibc-linked', 'libtls-fixture.so', 'libtls-native-fixture.so')}
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
metadata['instance_evidence_sha256'] = sha256_file(here / 'instance_evidence.py')
metadata['device_evidence_sha256'] = sha256_file(here / 'device_evidence.py')
metadata['packed_evidence_sha256'] = sha256_file(here / 'packed_evidence.py')
metadata['commands'] = {}
metadata['driver_observations'] = {}
metadata['icd_mali_loader_quirk_requested'] = a.icd_mali_loader_quirk
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
for binary in ['probe-bionic', 'probe-glibc', 'probe-glibc-linked', 'libtls-fixture.so', 'libtls-native-fixture.so']:
    shutil.copy2(a.bundle / binary, stage / binary)
    if probe_provenance and sha256_file(stage / binary) != expected[binary]:
        raise SystemExit('probe changed while staging: ' + binary)


cases = [
    ('hybris', 'tls-mrs', 'probe-glibc'),
    ('native', 'groups', 'probe-bionic'),
    ('native', 'groups-dlsym', 'probe-bionic'),
    ('hybris', 'groups', 'probe-glibc'),
    ('hybris', 'groups-dlsym', 'probe-glibc'),
    ('native', 'rwlock-monotonic', 'probe-bionic'),
    ('hybris', 'rwlock-monotonic', 'probe-glibc'),
    ('native', 'mutex-monotonic', 'probe-bionic'),
    ('hybris', 'mutex-monotonic', 'probe-glibc'),
    ('native', 'ubo-template', 'probe-bionic'),
    ('hybris', 'ubo-template', 'probe-glibc'),
    ('native', 'ubo-staged', 'probe-bionic'),
    ('hybris', 'ubo-staged', 'probe-glibc'),
    ('native', 'ubo-large', 'probe-bionic'),
    ('hybris', 'ubo-large', 'probe-glibc'),
    ('native', 'ubo-multi', 'probe-bionic'),
    ('hybris', 'ubo-multi', 'probe-glibc'),
    ('native', 'ubo-dynamic', 'probe-bionic'),
    ('hybris', 'ubo-dynamic', 'probe-glibc'),
    ('native', 'stdio', 'probe-bionic'),
    ('hybris', 'stdio', 'probe-glibc'),
    ('native', 'vk-alloc', 'probe-bionic'),
    ('hybris', 'vk-alloc', 'probe-glibc'),
    ('native', 'rwlock-kind', 'probe-bionic'),
    ('hybris', 'rwlock-kind', 'probe-glibc'),
    ('native', 'sync-destroy', 'probe-bionic'),
    ('hybris', 'sync-destroy', 'probe-glibc'),
    ('native', 'vk', 'probe-bionic'),
    ('native', 'vk-dlsym', 'probe-bionic'),
    ('native', 'vk-gdpa', 'probe-bionic'),
    ('native', 'vk-core11', 'probe-bionic'),
    ('native', 'vk-khr11', 'probe-bionic'),
    ('native', '2', 'probe-bionic'),
    ('native', 'egl-life', 'probe-bionic'),
    ('native', '3', 'probe-bionic'),
    ('native', '0', 'probe-bionic'),
    ('native', 'dispatch', 'probe-bionic'),
    ('native', 'life', 'probe-bionic'),
    ('native', 'vk-init', 'probe-bionic'),
    ('native', 'unload', 'probe-bionic'),
    ('native', 'tls', 'probe-bionic'),
    ('native', 'caps', 'probe-bionic'),
    ('native', 'caps2', 'probe-bionic'),
    ('native', 'native-buffer', 'probe-bionic'),
    ('native', 'ubo', 'probe-bionic'),
    ('hybris', 'vk', 'probe-glibc'),
    ('hybris', 'vk-dlsym', 'probe-glibc'),
    ('hybris', 'vk-gdpa', 'probe-glibc'),
    ('hybris', 'vk-core11', 'probe-glibc'),
    ('hybris', 'vk-khr11', 'probe-glibc'),
    ('hybris', '2', 'probe-glibc'),
    ('hybris', 'egl-life', 'probe-glibc'),
    ('hybris', '3', 'probe-glibc'),
    ('hybris', '0', 'probe-glibc'),
    ('hybris', 'dispatch', 'probe-glibc'),
    ('hybris', 'life', 'probe-glibc'),
    ('hybris', 'vk-init', 'probe-glibc'),
    ('hybris', 'mutex-init', 'probe-glibc'),
    ('hybris', 'rwlock-init', 'probe-glibc'),
    ('hybris', 'cond-init', 'probe-glibc'),
    ('hybris', 'cond-clock', 'probe-glibc'),
    ('hybris', 'shared-unavailable', 'probe-glibc'),
    ('hybris', 'unload', 'probe-glibc'),
    ('hybris', 'init', 'probe-glibc'),
    ('hybris', 'tls', 'probe-glibc'),
    ('hybris', 'tls-bounds', 'probe-glibc'),
    ('hybris', 'tls-dtor', 'probe-glibc'),
    ('hybris', 'caps', 'probe-glibc'),
    ('hybris', 'wsi-disabled', 'probe-glibc'),
    ('hybris', 'caps2', 'probe-glibc'),
    ('hybris', 'native-buffer', 'probe-glibc'),
    ('hybris', 'ubo', 'probe-glibc'),
    ('hybris-linked', 'dispatch', 'probe-glibc-linked'),
    ('hybris-linked', 'vk', 'probe-glibc-linked'),
]

cases.extend(('hybris', mode, 'probe-glibc') for mode in ('render-owners', 'command-alloc'))

for backend, binary in (('native', 'probe-bionic'), ('hybris', 'probe-glibc')):
    cases.append((backend, 'memory-ranges', binary))
    cases.append((backend, 'vertex-policy', binary))
    cases.append((backend, 'bc-decode', binary))
    cases.append((backend, 'bc-images', binary))
    cases.append((backend, 'point-size', binary))
    cases.append((backend, 'scaled-vertex', binary))
    cases.append((backend, 'scaled-vertex-multi', binary))
    cases.append((backend, 'scaled-vertex-literal', binary))
    cases.extend((backend, 'scaled-vertex-' + shape, binary) for shape in ('packed1', 'packed2', 'packed3', 'packed4', 'builtins', 'matrix', 'array', 'nested', 'matarray', 'spec', 'spec-direct', 'group', 'group-multi', 'group-spec', 'divisor', 'divisor-zero', 'divisor-base', 'divisor-zero-base'))

timeline_queue_cases = ('timeline-queues-core', 'timeline-queues-khr')
timeline_cases = tuple('timeline-' + family + suffix for family in ('core', 'khr')
                       for suffix in ('', '-gdpa', '-elf')) + timeline_queue_cases
for backend, binary in (('native', 'probe-bionic'), ('hybris', 'probe-glibc')):
    cases.extend((backend, mode, binary) for mode in timeline_cases)
cases.extend(('hybris-linked', 'timeline-' + family + '-linked', 'probe-glibc-linked')
             for family in ('core', 'khr'))

render_cases = ('render-core13', 'render-khr13', 'render-core13-elf', 'render-khr13-elf')
for backend, binary in (('native', 'probe-bionic'), ('hybris', 'probe-glibc')):
    cases.extend((backend, mode, binary) for mode in render_cases)
cases.extend(('hybris-linked', mode, 'probe-glibc-linked')
             for mode in ('render-core13-linked', 'render-khr13-linked'))

if a.icd_hal:
    adapter = stage / 'hybris/libhybris-vulkan-icd.so.0'
    if not adapter.is_file():
        raise SystemExit('ICD adapter missing from hybris install')
    (stage / 'standard').mkdir()
    shutil.copy2(a.vulkan_loader, stage / 'standard/libvulkan.so.1')
    metadata['standard_loader_sha256'] = sha256_file(a.vulkan_loader)
    # The direct version probe provisions driver.json before loader cases.
    cases += [('icd', mode, 'probe-glibc')
              for mode in ('version', 'vertex-policy', 'vertex-policy-direct', 'native-buffer', 'bc-decode', 'bc-images', 'bc-images-gdpa', 'bc-images-dlsym', 'memory-ranges', 'groups', 'groups-dlsym', 'vk', 'vk-dlsym', 'vk-gdpa', 'vk-core11', 'vk-khr11', 'dispatch', 'life', 'vk-init', 'vk-alloc', 'icd-alloc-direct', 'unload', 'tls', 'caps', 'caps2', 'ubo', 'ubo-dynamic', 'ubo-multi', 'ubo-large', 'ubo-staged', 'ubo-template')]
    cases += [('icd-linked', mode, 'probe-glibc-linked') for mode in ('vk', 'dispatch', 'point-size-linked')]
    cases.extend(('icd', 'point-size' + route, 'probe-glibc') for route in ('', '-gdpa', '-elf'))
    cases.extend(('icd', mode, 'probe-glibc') for mode in render_cases + timeline_cases + ('scaled-vertex', 'scaled-vertex-gdpa', 'scaled-vertex-elf', 'scaled-vertex-multi', 'scaled-vertex-multi-gdpa', 'scaled-vertex-multi-elf', 'scaled-vertex-literal', 'scaled-vertex-literal-gdpa', 'scaled-vertex-literal-elf'))
    cases.extend(('icd', 'scaled-vertex-' + shape, 'probe-glibc') for shape in ('packed1', 'packed2', 'packed3', 'packed4', 'builtins', 'matrix', 'array', 'nested', 'matarray', 'spec', 'spec-direct', 'group', 'group-multi', 'group-spec', 'divisor', 'divisor-zero', 'divisor-base', 'divisor-zero-base'))
    cases.extend(('icd', 'scaled-vertex-builtins-' + route, 'probe-glibc') for route in ('gdpa', 'elf'))
    cases.append(('icd-linked', 'scaled-vertex-builtins-linked', 'probe-glibc-linked'))
    cases.append(('icd-linked', 'bc-images-linked', 'probe-glibc-linked'))
    cases.append(('icd-linked', 'scaled-vertex-linked', 'probe-glibc-linked'))
    cases.append(('icd-linked', 'scaled-vertex-multi-linked', 'probe-glibc-linked'))
    cases.append(('icd-linked', 'scaled-vertex-literal-linked', 'probe-glibc-linked'))
    cases.extend(('icd-linked', 'timeline-' + family + '-linked', 'probe-glibc-linked')
                 for family in ('core', 'khr'))
    cases.extend(('icd-linked', mode, 'probe-glibc-linked')
                 for mode in ('render-core13-linked', 'render-khr13-linked'))
    if a.validation_layer:
        cases.extend(('icd', 'point-size' + route + '-validation', 'probe-glibc') for route in ('', '-gdpa', '-elf'))
        cases.append(('icd-linked', 'point-size-linked-validation', 'probe-glibc-linked'))
        cases.extend(('icd', 'scaled-vertex-builtins-' + route + '-validation', 'probe-glibc') for route in ('gdpa', 'elf'))
        cases.append(('icd-linked', 'scaled-vertex-builtins-linked-validation', 'probe-glibc-linked'))
        cases.append(('icd-linked', 'bc-images-linked-validation', 'probe-glibc-linked'))
        (stage / 'layers').mkdir()
        shutil.copy2(a.validation_layer, stage / 'layers/libVkLayer_khronos_validation.so')
        metadata['validation_layer_sha256'] = sha256_file(a.validation_layer)
        metadata['validation_manifest_sha256'] = sha256_file(a.validation_manifest)
        if a.validation_build_manifest:
            validation_provenance = json.loads(a.validation_build_manifest.read_text())
            expected_layer = validation_provenance.get('files', {}).get('lib/libVkLayer_khronos_validation.so', {})
            if expected_layer.get('sha256') != metadata['validation_layer_sha256']:
                raise SystemExit('validation build manifest does not identify the selected layer')
            shutil.copy2(a.validation_build_manifest, a.out / 'validation-build-manifest.json')
            metadata['validation_build_manifest_sha256'] = sha256_file(a.validation_build_manifest)
        layer_json = json.loads(a.validation_manifest.read_text())
        if layer_json['layer']['name'] != 'VK_LAYER_KHRONOS_validation':
            raise SystemExit('expected Khronos validation layer manifest')
        layer_json['layer']['library_path'] = './libVkLayer_khronos_validation.so'
        (stage / 'layers/validation.json').write_text(json.dumps(layer_json))
        cases.extend(('icd', 'scaled-vertex-' + shape + '-validation', 'probe-glibc') for shape in ('packed1', 'packed2', 'packed3', 'packed4', 'builtins', 'matrix', 'array', 'nested', 'matarray', 'spec', 'spec-direct', 'group', 'group-multi', 'group-spec', 'divisor', 'divisor-zero', 'divisor-base', 'divisor-zero-base'))
        cases.extend([('icd', mode, 'probe-glibc') for mode in ('scaled-vertex-literal-validation', 'scaled-vertex-literal-gdpa-validation', 'scaled-vertex-multi-validation', 'scaled-vertex-multi-gdpa-validation', 'scaled-vertex-validation', 'scaled-vertex-gdpa-validation', 'memory-ranges-validation', 'bc-decode-validation', 'bc-images-validation', 'bc-images-gdpa-validation', 'bc-images-dlsym-validation', 'timeline-queues-core-validation', 'timeline-queues-khr-validation', 'timeline-core-validation', 'timeline-khr-validation', 'render-core13-validation', 'render-khr13-validation', 'validation', 'ubo-validation', 'ubo-dynamic-validation', 'ubo-multi-validation', 'ubo-large-validation', 'ubo-staged-validation', 'ubo-template-validation')])

if a.capture_tools:
    stage_tools(a.capture_tools, stage, metadata, sha256_file)
    stage_shader_reference(a.bundle, a.out, metadata)

if a.icd_hal and a.selected_cases:
    cases.extend(('icd', mode, 'probe-glibc') for mode in ('vertex-policy-restricted', 'vertex-policy-restricted-direct')
                 if 'icd-' + mode in a.selected_cases)

results = []
capabilities = {}
observed_paths = set()
try:
    if a.manifest:
        verify_manifest(provenance, stage / 'hybris', stage / 'glibc')
    shell('mkdir -p ' + remote, check=True)
    subprocess.run(adb + ['push', str(stage) + '/.', remote + '/'],
                   check=True, stdout=subprocess.DEVNULL)
    if a.selected_cases:
        available = {backend + '-' + mode for backend, mode, _ in cases}
        unknown = set(a.selected_cases) - available
        if unknown:
            raise SystemExit('unknown selected cases: ' + ', '.join(sorted(unknown)))
        cases = [case for case in cases if case[0] + '-' + case[1] in a.selected_cases]
        if a.capture_tools and ('icd', 'ubo', 'probe-glibc') not in cases:
            cases.append(('icd', 'ubo', 'probe-glibc'))
        if any(case[0].startswith('icd') for case in cases) and ('icd', 'version', 'probe-glibc') not in cases:
            cases.insert(0, ('icd', 'version', 'probe-glibc'))
    metadata['selected_cases'] = a.selected_cases
    metadata['scheduled_cases'] = [backend + '-' + mode for backend, mode, _ in cases]
    if a.capture_tools:
        metadata['scheduled_cases'] += ['icd-capture-replay', 'icd-capture-dynamic-replay', 'icd-capture-multi-replay']
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
        if backend in {'icd', 'icd-linked'} and a.icd_mali_loader_quirk:
            command = 'HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK=1 ' + command
        if backend == 'icd' and mode == 'vk-init':
            command = 'HYBRIS_ICD_INSTANCE_TRACE=1 ' + command
        if backend == 'icd' and mode == 'life':
            command = 'HYBRIS_ICD_INSTANCE_TRACE=1 HYBRIS_ICD_DEVICE_TRACE=1 ' + command
        if backend in {'icd', 'icd-linked'} and a.point_size_compat:
            command = 'HYBRIS_VULKAN_COMPAT_POINT_SIZE=1 ' + command
            if mode.startswith('point-size'):
                dump_remote = remote + '/' + backend + '-' + mode + '-shaders'
                shell('mkdir -p ' + shlex.quote(dump_remote), check=True)
                command = 'HYBRIS_VULKAN_SCALED_DUMP_DIR=' + shlex.quote(dump_remote) + ' ' + command
        if backend in {'icd', 'icd-linked'} and a.unused_builtins:
            command = 'HYBRIS_VULKAN_COMPAT_UNUSED_BUILTINS=1 ' + command
        if backend in {'icd', 'icd-linked'} and a.bc_textures:
            command = 'HYBRIS_BC_TEXTURES=' + a.bc_textures + ' ' + command
        if backend in {'icd', 'icd-linked'} and a.packed_vertex_compat:
            command = 'HYBRIS_VULKAN_COMPAT_PACKED_VERTEX=' + ('force' if a.packed_vertex_compat == 'force' else '1') + ' ' + command
            if 'packed' in mode:
                dump_remote = remote + '/' + backend + '-' + mode + '-shaders'
                shell('mkdir -p ' + shlex.quote(dump_remote), check=True)
                command = 'HYBRIS_VULKAN_SCALED_DUMP_DIR=' + shlex.quote(dump_remote) + ' ' + command
        if backend in {'icd', 'icd-linked'} and a.scaled_vertex_compat:
            command = 'HYBRIS_VULKAN_COMPAT_SCALED_VERTEX=' + ('force' if a.scaled_vertex_compat == 'force' else '1') + ' ' + command
            if a.scaled_format_trace:
                command = 'HYBRIS_VULKAN_COMPAT_FORMAT_TRACE=1 ' + command
            if mode.startswith('scaled-vertex'):
                dump_remote = remote + '/' + backend + '-' + mode + '-shaders'
                shell('mkdir -p ' + shlex.quote(dump_remote), check=True)
                command = 'HYBRIS_VULKAN_SCALED_DUMP_DIR=' + shlex.quote(dump_remote) + ' ' + command
        name = backend + '-' + mode
        metadata['commands'][name] = {'directory': remote, 'command': command + mode,
                                      'output': 'stderr merged into stdout on device before adb transport'}
        try:
            r = shell(
                'cd ' + remote + ' && sh -c ' + shlex.quote(
                    'exec 2>&1; echo $$ > probe.pid; exec env ' + command + mode),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=35,
            )
            output, code = r.stdout, r.returncode
        except subprocess.TimeoutExpired as exc:
            output, code = exc.stdout or b'', 124
            kill_remote()
        (a.out / (name + '.log')).write_bytes(output or b'')
        probe_exit_code = code
        shader_evidence_error = None
        decoded = (output or b'').decode('utf-8', errors='replace')
        if backend in {'icd', 'icd-linked'} and mode.startswith('point-size') and a.point_size_compat:
            dump_local = a.out / (name + '-shaders')
            dump_local.mkdir()
            subprocess.run(adb + ['pull', dump_remote + '/.', str(dump_local)], check=True,
                           stdout=subprocess.DEVNULL, timeout=30)
            from point_size_evidence import point_size_evidence
            try:
                evidence = point_size_evidence(dump_local, decoded, a.bundle / 'src/shaders/point-size.inc', a.unused_builtins)
                (a.out / (name + '-shader-evidence.json')).write_text(json.dumps(evidence, indent=2) + '\n')
            except (ValueError, OSError, subprocess.CalledProcessError) as exc:
                shader_evidence_error = str(exc)
                print(name, 'point-size shader evidence failed:', exc)
                code = code or 2
        if backend in {'icd', 'icd-linked'} and 'packed' in mode and a.packed_vertex_compat:
            dump_local = a.out / (name + '-shaders')
            dump_local.mkdir()
            subprocess.run(adb + ['pull', dump_remote + '/.', str(dump_local)], check=True,
                           stdout=subprocess.DEVNULL, timeout=30)
            from packed_evidence import packed_evidence
            try:
                width = int(re.search(r'packed([1-4])', mode).group(1))
                evidence = packed_evidence(dump_local, decoded, a.bundle / 'src/shaders/packed.inc', width)
                (a.out / (name + '-shaders.json')).write_text(json.dumps(evidence, indent=2) + '\n')
            except (ValueError, OSError, subprocess.SubprocessError) as exc:
                shader_evidence_error = str(exc)
                print(name, 'packed shader evidence failed:', exc, flush=True)
                code = code or 2
        if backend in {'icd', 'icd-linked'} and mode.startswith('scaled-vertex') and a.scaled_vertex_compat and 'packed' not in mode:
            dump_local = a.out / (name + '-shaders')
            dump_local.mkdir()
            subprocess.run(adb + ['pull', dump_remote + '/.', str(dump_local)], check=True,
                           stdout=subprocess.DEVNULL, timeout=30)
            from scaled_evidence import scaled_evidence
            try:
                fixture = next((name for name in ('divisor', 'group-multi', 'group-spec', 'group', 'matarray', 'matrix',
                    'nested', 'array', 'spec-direct', 'spec', 'multi', 'builtins', 'literal') if name in mode), 'vert')
                source = a.bundle / ('src/shaders/scaled.' + fixture + '.inc')
                if fixture in {'group-spec', 'matarray', 'matrix', 'nested', 'array', 'spec-direct', 'spec'}:
                    from aggregate_evidence import aggregate_evidence
                    evidence = aggregate_evidence(dump_local, decoded, source)
                else:
                    evidence = scaled_evidence(dump_local, decoded, a.scaled_vertex_compat == 'force', source, unused_builtins=a.unused_builtins)
                if a.scaled_format_trace:
                    from format_evidence import format_evidence
                    native_log = a.out / ('native-' + mode.removesuffix('-validation') + '.log')
                    evidence['formats'] = format_evidence(decoded, native_log.read_text() if native_log.is_file() else None)
                (a.out / (name + '-shaders.json')).write_text(json.dumps(evidence, indent=2) + '\n')
            except (ValueError, OSError, subprocess.SubprocessError) as exc:
                print(name, 'scaled shader evidence failed:', exc, flush=True)
                shader_evidence_error = str(exc)
                if code == 0:
                    code = 2
        if backend == 'icd' and mode == 'version' and code == 0:
            versions = [m.group(1) for line in decoded.splitlines()
                        if (m := re.fullmatch(r'ICD_API_VERSION (\d+\.\d+\.\d+)', line))]
            if len(versions) != 1:
                print('ICD version discovery returned no unique version', flush=True)
                code = 2
            else:
                metadata['icd_api_version'] = versions[0]
                driver_json = stage / 'driver.json'
                driver_json.write_text(json.dumps({
                    'file_format_version': '1.0.0',
                    'ICD': {'library_path': remote + '/hybris/libhybris-vulkan-icd.so.0',
                            'api_version': versions[0]}}))
                subprocess.run(adb + ['push', str(driver_json), remote + '/driver.json'],
                               check=True, stdout=subprocess.DEVNULL, timeout=30)

        if backend == 'icd' and mode == 'vk-init' and code == 0:
            try:
                evidence = instance_evidence(decoded)
                (a.out / (name + '-instances.json')).write_text(json.dumps(evidence, indent=2) + '\n')
            except (ValueError, KeyError) as exc:
                print(name, 'instance evidence failed:', exc, flush=True)
                code = 2
        if backend == 'icd' and mode == 'life' and code == 0:
            try:
                evidence = device_evidence(decoded)
                (a.out / (name + '-devices.json')).write_text(json.dumps(evidence, indent=2) + '\n')
            except (ValueError, KeyError) as exc:
                print(name, 'device evidence failed:', exc, flush=True)
                code = 2
        values = {}
        for line in decoded.splitlines():
            if line.startswith('CAP_VALUE '):
                _, key, value = line.split()
                values[key] = json.loads(value)
        if values:
            if mode in ('caps', 'caps2'):
                capabilities.setdefault(mode, {})[backend] = {'values': values, 'probe_exit_code': code}
            (a.out / (name + '-values.json')).write_text(json.dumps(values, indent=2) + '\n')
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
            if line.startswith(('GPU ', 'EGL ', 'GL vendor=', 'HYBRIS_MALI_MMUD '))]
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
        results.append(dict(case=name, status=status, exit_code=code, probe_exit_code=probe_exit_code,
                            shader_evidence_error=shader_evidence_error, binary=binary))
        print(name, status, 'exit=' + str(code), flush=True)
        if backend == 'icd' and mode == 'version' and code != 0:
            raise SystemExit('ICD version discovery failed; dependent cases were not run')
    if a.capture_tools:
        for dynamic, multi in ((False, False), (True, False), (True, True)):
            folder = 'capture-multi' if multi else 'capture-dynamic' if dynamic else 'capture'
            case_name = 'icd-' + folder + '-replay'
            try:
                run_capture(shell, adb, remote, a.out, metadata['commands']['icd-ubo']['command'],
                            metadata, kill_remote, dynamic=dynamic, multi=multi)
                code = 0
            except subprocess.TimeoutExpired as exc:
                code = 124
                (a.out / (folder + '-error.log')).write_text(str(exc) + '\n')
            except subprocess.CalledProcessError as exc:
                code = exc.returncode
                (a.out / (folder + '-error.log')).write_text(str(exc) + '\n')
            except (OSError, ValueError, subprocess.SubprocessError) as exc:
                code = 2
                (a.out / (folder + '-error.log')).write_text(str(exc) + '\n')
            finally:
                try:
                    subprocess.run(adb + ['pull', remote + '/' + folder + '/.', str(a.out / folder)],
                                   check=True, capture_output=True, timeout=35)
                except (OSError, subprocess.SubprocessError) as exc:
                    (a.out / (folder + '-pull-error.log')).write_text(str(exc) + '\n')
            status = classify(code)
            results.append(dict(case=case_name, status=status, exit_code=code))
            print(case_name, status, 'exit=' + str(code), flush=True)

finally:
    try:
        for mode, observations in capabilities.items():
            if 'native' not in observations:
                continue
            native = observations['native']
            differences = {}
            for backend, observation in observations.items():
                if backend == 'native':
                    continue
                effective = observation['values']
                differences[backend] = {
                    'complete_probes': native['probe_exit_code'] == observation['probe_exit_code'] == 0,
                    'differences': {key: {'native': native['values'].get(key), 'effective': effective.get(key)}
                                    for key in sorted(native['values'].keys() | effective.keys())
                                    if native['values'].get(key) != effective.get(key)}}
            (a.out / ('capability-differences.json' if mode == 'caps' else 'capability2-differences.json')).write_text(json.dumps({
                'scope': ('first enumerated physical device; core features/limits/sparse, device extensions and ten format queries'
                          if mode == 'caps' else 'first physical device; features2 and properties2 core 1.1 chains'),
                'image_query': '2D, optimal, sampled|transfer-dst, flags=0' if mode == 'caps' else None,
                'note': 'Query differences are observations, not proof of rendering semantics or a workaround reason.',
                'comparisons': differences}, indent=2) + '\n')
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
