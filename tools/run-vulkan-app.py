#!/usr/bin/env python3
"""Observe a device application through the shared Vulkan backend and diagnostics."""
import argparse
import json
from pathlib import Path
import re
import shlex
import shutil
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/wsi'))
from host import Host, PACKAGE
from diagnostics import Diagnostics
from manifest import sha256_file
from vulkan_backend import stage_backend, verify_backend_maps
from vulkan_layers import add_layer, stage_validation
from capture_stage import stage_tools
from capture import preserve_capture


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--package', default=PACKAGE)
    p.add_argument('--backend', choices=('hybris', 'turnip'), required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--mesa-build', type=Path)
    p.add_argument('--icd-hal')
    p.add_argument('--vulkan-loader', type=Path)
    p.add_argument('--icd-mali-loader-quirk', action='store_true')
    p.add_argument('--probe', type=Path, required=True, help='verified Wayland probe build, used only to query ICD version')
    p.add_argument('--program', required=True, help='absolute device path to a glibc ELF executable')
    p.add_argument('--library-path', required=True, help='absolute device library directories, separated by colons')
    p.add_argument('--env', action='append', default=[], help='application environment NAME=VALUE (repeatable)')
    p.add_argument('--preload', help='absolute device path to the application runtime interposer')
    p.add_argument('--runtime-dir')
    p.add_argument('--wayland-display', default='wayland-0')
    p.add_argument('--duration', type=float, default=30)
    p.add_argument('--capture-tools', type=Path)
    p.add_argument('--validation-layer', type=Path)
    p.add_argument('--validation-manifest', type=Path)
    p.add_argument('--capture-position', choices=('before', 'after'), default='after', help='API capture position relative to compatibility transformations')
    p.add_argument('--out', type=Path, default=ROOT / 'tests/wsi/build/apps')
    p.add_argument('arguments', nargs=argparse.REMAINDER)
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+', a.package): p.error('invalid package')
    if not a.program.startswith('/') or any(not v.startswith('/') for v in a.library_path.split(':')):
        p.error('program and library directories must be absolute device paths')
    if a.preload and not a.preload.startswith('/'): p.error('--preload must be an absolute device path')
    if not 5 <= a.duration <= 300: p.error('duration must be 5–300 seconds')
    if bool(a.validation_layer) != bool(a.validation_manifest): p.error('supply both validation layer and manifest')
    if a.validation_layer and a.capture_tools: p.error('validation and capture require separate runs with the pinned tools')
    if a.backend == 'hybris' and (not a.icd_hal or not a.vulkan_loader): p.error('hybris requires --icd-hal and --vulkan-loader')
    if a.backend == 'turnip' and (not a.mesa_build or a.icd_hal or a.vulkan_loader or a.icd_mali_loader_quirk):
        p.error('Turnip requires --mesa-build and does not use HAL options')
    extra = {}
    for value in a.env:
        name, sep, content = value.partition('=')
        if not sep or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name): p.error('invalid --env')
        if name.startswith(('VK_', 'GFXRECON_')) or name in ('LD_PRELOAD', 'LD_LIBRARY_PATH'):
            p.error('loader and capture settings are owned by the runner')
        extra[name] = content
    a.runtime_dir = a.runtime_dir or '/data/user/0/' + a.package + '/files/runtime'
    out = a.out / (time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
    out.mkdir(parents=True)
    host = Host(a.serial, out, a.package, a.runtime_dir, a.wayland_display)
    remote = host.files + '/vulkan-app-' + out.name
    diagnostics = None
    metadata = {'status': 'FAIL', 'application_acceptance': 'unverified', 'program': a.program,
                'remote': remote, 'duration_seconds': a.duration, 'runner_sha256': sha256_file(Path(__file__))}
    metadata['helper_sha256'] = {name: sha256_file(ROOT / name) for name in (
        'tools/vulkan_backend.py', 'tools/vulkan_layers.py', 'tests/wsi/host.py',
        'tests/wsi/diagnostics.py', 'tests/wsi/capture.py')}
    try:
        host.attach()
        stage = out / 'stage'
        backend = stage_backend(a, stage, remote)
        metadata['backend'] = backend
        env = dict(extra, **backend['env'])
        env.update(XDG_RUNTIME_DIR=a.runtime_dir, WAYLAND_DISPLAY=a.wayland_display,
                   HYBRIS_ANDROID_SDK_VERSION=host.prop('ro.build.version.sdk'))
        backend_libraries = [remote + '/' + part.removeprefix('./')
                             for part in backend['library_path'].split(':')]
        # Keep the selected loader and its matching glibc together. Application
        # GL/EGL libraries must precede the staged Android ABI frontends.
        libraries = ':'.join([path for path in backend_libraries if not path.endswith('/hybris')] +
                             a.library_path.split(':') +
                             [path for path in backend_libraries if path.endswith('/hybris')])
        if a.validation_layer:
            metadata.update(stage_validation(a.validation_layer, a.validation_manifest, stage, remote, env))
            settings = stage / 'validation-settings'
            settings.mkdir()
            (settings / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
            env['VK_LAYER_SETTINGS_PATH'] = remote + '/validation-settings'
            metadata['synchronization_validation_requested'] = True
        if a.capture_tools:
            stage_tools(a.capture_tools, stage, metadata, sha256_file)
            add_layer(env, 'VK_LAYER_LUNARG_gfxreconstruct', remote + '/capture-tools', before=a.capture_position == 'before')
            env.update(GFXRECON_CAPTURE_FILE=remote + '/application.gfxr', GFXRECON_CAPTURE_FILE_TIMESTAMP='false')
            libraries += ':' + remote + '/capture-tools:' + remote + '/capture-tools/runtime'
            metadata['capture_position'] = a.capture_position
        provenance = json.loads((a.probe / 'probe-manifest.json').read_text())
        if sha256_file(a.probe / 'probe-wayland') != provenance['binary_sha256']: raise ValueError('version probe hash mismatch')
        shutil.copy2(a.probe / 'probe-wayland', stage / 'probe-wayland')
        metadata['version_probe'] = provenance
        host.upload(stage, remote)
        prefix = shlex.join([k + '=' + v for k, v in env.items()]) + ' ./glibc/ld-linux-aarch64.so.1 --library-path ' + shlex.quote(libraries)
        query = host.app('cd ' + shlex.quote(remote) + ' && env ' + prefix + ' ./probe-wayland --icd-version', capture_output=True, text=True, timeout=30)
        (out / 'icd-version.log').write_text(query.stdout + query.stderr)
        versions = re.findall(r'^WSI_ICD_VERSION (\d+\.\d+\.\d+)$', query.stdout, re.M)
        if query.returncode or len(versions) != 1: raise ValueError('ICD version query failed')
        driver = json.dumps({'file_format_version': '1.0.0', 'ICD': {'library_path': remote + '/' + backend['driver'], 'api_version': versions[0]}})
        (stage / 'driver.json').write_text(driver)
        host.app('cat > ' + shlex.quote(remote + '/driver.json'), input=driver, text=True, check=True)
        metadata['program_sha256'] = host.app('sha256sum ' + shlex.quote(a.program), capture_output=True, text=True, check=True).stdout.split()[0]
        if a.preload:
            metadata['preload'] = {'path': a.preload, 'sha256': host.app('sha256sum ' + shlex.quote(a.preload), capture_output=True, text=True, check=True).stdout.split()[0]}
            prefix = 'LD_PRELOAD=' + shlex.quote(a.preload) + ' ' + prefix
        arguments = a.arguments[1:] if a.arguments[:1] == ['--'] else a.arguments
        command = prefix + ' ' + shlex.join([a.program] + arguments)
        metadata['command'] = command
        diagnostics = Diagnostics(host.adb, a.package, out, host.app)
        metadata['exit_code'] = host.execute(command, remote, out, a.duration, diagnostics, observe=True)
        maps_path = out / 'client-maps.txt'
        if not maps_path.exists(): raise ValueError('application exited before live mappings were collected')
        mapping = next(item for item in diagnostics.records if item['operation'] == 'client-maps.txt')
        if mapping.get('exit_code') != 0 or mapping.get('limit_reached'): raise ValueError('live mappings are incomplete')
        verify_backend_maps(backend, maps_path.read_text())
        host.hash_mappings(maps_path.read_text(), out)
        if a.validation_layer:
            if '/layers/libVkLayer_khronos_validation.so' not in maps_path.read_text(): raise ValueError('validation layer missing from live mappings')
            log = (out / 'probe.log').read_text(errors='replace')
            errors = re.findall(r'Validation Error|VUID-[A-Za-z0-9_-]+|SYNC-HAZARD-[A-Z_-]+', log)
            metadata['validation_error_markers'] = len(errors)
            if errors: raise ValueError('validation errors observed; see probe.log')
        if a.capture_tools and '/capture-tools/libVkLayer_gfxreconstruct.so' not in maps_path.read_text():
            raise ValueError('capture layer missing from live mappings')
        if metadata['exit_code'] not in (None, 0): raise ValueError('application exited unsuccessfully')
        if host.record.get('gpu_fault_count'): raise ValueError('GPU fault observed')
        metadata['status'] = 'OBSERVED'
    except (Exception, KeyboardInterrupt) as error:
        metadata['error'] = str(error)
    finally:
        if a.capture_tools and diagnostics:
            try:
                metadata['capture_sha256'] = preserve_capture(host.app, remote, out, name='application.gfxr')
            except Exception as error:
                metadata['status'] = 'FAIL'
                metadata['capture_error'] = str(error)
        if diagnostics: diagnostics.finish()
        host.record['status'] = metadata['status']
        host.close()
        if host.record['status'] == 'FAIL': metadata['status'] = 'FAIL'
        (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(str(out), metadata['status'])
    return 0 if metadata['status'] == 'OBSERVED' else 1


if __name__ == '__main__':
    sys.exit(main())
