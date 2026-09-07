#!/usr/bin/env python3
"""Run the independent Wayland Vulkan probe in a running debuggable compositor app."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from manifest import sha256_file, verify_manifest
from screen_evidence import verify_screen
from diagnostics import Diagnostics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--timeout', type=float, default=65, help='host watchdog seconds (default 65)')
p.add_argument('--trace', action='store_true', help='record compiled native-window tracepoints without verbose hook logs')
p.add_argument('--package', default='io.taowen.ardesk')
p.add_argument('--wayland', default='wayland-0')
p.add_argument('--build', type=Path, default=ROOT / 'tests/baseline/build')
p.add_argument('--probe', type=Path, default=ROOT / 'tests/wsi/build')
p.add_argument('--out', type=Path, default=ROOT / 'tests/wsi/build/results')
p.add_argument('--icd-hal', help='standard-loader ICD path: Android Vulkan HAL')
p.add_argument('--vulkan-loader', type=Path, help='glibc AArch64 standard libvulkan.so.1 for --icd-hal')
p.add_argument('--icd-mali-loader-quirk', action='store_true', help='Opt in to the build-id-scoped Mali MMUD loader workaround')
a = p.parse_args()
if not 5 <= a.timeout <= 300: p.error('timeout must be between 5 and 300 seconds')
if not re.fullmatch(r'[A-Za-z0-9_.]+', a.package): p.error('invalid package')
if (a.icd_hal is None) != (a.vulkan_loader is None):
    p.error('--icd-hal and --vulkan-loader must be supplied together')
if a.icd_mali_loader_quirk and not a.icd_hal:
    p.error('--icd-mali-loader-quirk requires --icd-hal')
adb = [os.environ.get('ADB', 'adb'), '-s', a.serial]
def shell(command, **kwargs): return subprocess.run(adb + ['shell', command], **kwargs)
def app(command, **kwargs): return shell('run-as ' + shlex.quote(a.package) + ' sh -c ' + shlex.quote(command), **kwargs)
def prop(name): return shell('getprop ' + shlex.quote(name), check=True, capture_output=True, text=True).stdout.strip()
provenance = json.loads((a.build / 'manifest.json').read_text())
probe_provenance = json.loads((a.probe / 'probe-manifest.json').read_text())
probe_name = 'probe-icd-surface' if a.icd_hal else 'probe-wayland'
probe_hash = 'icd_surface_sha256' if a.icd_hal else 'binary_sha256'
if sha256_file(a.probe / probe_name) != probe_provenance[probe_hash]:
    raise SystemExit('probe hash mismatch; rebuild it')
verify_manifest(provenance, a.build / 'install/usr/lib/hybris', a.build / 'runtime')
run_id = time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8]
out = a.out / run_id
stage = out / 'stage'
stage.mkdir(parents=True)
shutil.copytree(a.build / 'install/usr/lib/hybris', stage / 'hybris', symlinks=True)
shutil.copytree(a.build / 'runtime', stage / 'glibc', symlinks=True)
shutil.copy2(a.probe / probe_name, stage / probe_name)
verify_manifest(provenance, stage / 'hybris', stage / 'glibc')
files = '/data/user/0/' + a.package + '/files'
remote = files + '/hybris-wsi-' + run_id
sdk = prop('ro.build.version.sdk')
env = {'HYBRIS_LINKER_DIR': remote + '/hybris/libhybris/linker',
       'HYBRIS_EGLPLATFORM_DIR': remote + '/hybris/libhybris',
       'HYBRIS_VULKANPLATFORM_DIR': remote + '/hybris/libhybris',
       'HYBRIS_ANDROID_SDK_VERSION': sdk, 'XDG_RUNTIME_DIR': files + '/runtime',
       'WAYLAND_DISPLAY': a.wayland}
libraries = './hybris:./glibc'
if a.icd_hal:
    adapter = stage / 'hybris/libhybris-vulkan-icd.so.0'
    if not adapter.is_file():
        raise SystemExit('ICD adapter missing from hybris install')
    (stage / 'standard').mkdir()
    shutil.copy2(a.vulkan_loader, stage / 'standard/libvulkan.so.1')
    env['HYBRIS_VULKAN_HAL'] = a.icd_hal
    if a.icd_mali_loader_quirk:
        env['HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK'] = '1'
    env['VK_DRIVER_FILES'] = remote + '/driver.json'
    libraries = './standard:./hybris:./glibc'
else:
    env['HYBRIS_EGLPLATFORM'] = 'wayland'
    env['HYBRIS_VULKANPLATFORM'] = 'wayland'
if a.trace: env.update(HYBRIS_TRACE='1', HYBRIS_LOGGING_LEVEL='warn')
command = ' '.join(k + '=' + shlex.quote(v) for k, v in env.items())
command += ' ./glibc/ld-linux-aarch64.so.1 --library-path ' + libraries + ' ./' + probe_name
metadata = {'run_id': run_id, 'serial': a.serial, 'package': a.package,
            'fingerprint': prop('ro.build.fingerprint'), 'sdk': sdk,
            'command': command, 'remote': remote, 'host_timeout_seconds': a.timeout, 'runner_sha256': sha256_file(Path(__file__)), 'hybris': provenance, 'probe': probe_provenance,
            'path': 'icd' if a.icd_hal else 'frontend'}
if a.icd_hal:
    metadata['icd_hal'] = a.icd_hal
    metadata['standard_loader_sha256'] = sha256_file(stage / 'standard/libvulkan.so.1')
apk_paths = shell('pm path ' + shlex.quote(a.package), check=True, capture_output=True, text=True).stdout.splitlines()
metadata['package_apks'] = []
for apk in apk_paths:
    if apk.startswith('package:'):
        path = apk[len('package:'):]
        digest = shell('sha256sum ' + shlex.quote(path), check=True, capture_output=True, text=True).stdout.split()[0]
        metadata['package_apks'].append({'path': path, 'sha256': digest})
(out / 'device.json').write_text(json.dumps(metadata, indent=2))
archive = out / 'stage.tar'
with tarfile.open(archive, 'w') as bundle: bundle.add(stage, arcname='.')
process = None
code = 2
diagnostics = Diagnostics(adb, a.package, out, app)
def stop_owned_process():
    value = app('cat ' + shlex.quote(remote + '/runner.pid'), capture_output=True, text=True)
    if value.returncode or not value.stdout.strip().isdigit(): return
    pid = int(value.stdout.strip())
    cwd = app('readlink /proc/' + str(pid) + '/cwd', capture_output=True, text=True)
    if cwd.returncode == 0 and cwd.stdout.strip() == remote:
        app('kill -KILL ' + str(pid), capture_output=True)

try:
    app('mkdir -p ' + shlex.quote(remote), check=True)
    with archive.open('rb') as data:
        app('cd ' + shlex.quote(remote) + ' && tar xf -', stdin=data, check=True)
    archive.unlink()
    if a.icd_hal:
        # Query the staged adapter directly before creating a loader manifest.
        # A fixed 1.3 declaration would misrepresent a HAL reporting 1.1.
        version_command = 'cd ' + shlex.quote(remote) + ' && env ' + command + ' --icd-version'
        version_run = app(version_command, capture_output=True, text=True, timeout=30)
        (out / 'icd-version.log').write_text(version_run.stdout + version_run.stderr)
        versions = re.findall(r'^WSI_ICD_VERSION (\d+\.\d+\.\d+)$', version_run.stdout, re.MULTILINE)
        if version_run.returncode or len(versions) != 1:
            raise RuntimeError('staged ICD version query failed; see icd-version.log')
        driver = json.dumps({'file_format_version': '1.0.0', 'ICD': {
            'library_path': remote + '/hybris/libhybris-vulkan-icd.so.0',
            'api_version': versions[0]}})
        (stage / 'driver.json').write_text(driver)
        app('cat > ' + shlex.quote(remote + '/driver.json'), input=driver, text=True, check=True)
        metadata['icd_api_version'] = versions[0]
        metadata['icd_version_command'] = version_command
        (out / 'device.json').write_text(json.dumps(metadata, indent=2))
    launch = 'cd ' + shlex.quote(remote) + ' && echo $$ > runner.pid && exec env ' + command
    process = subprocess.Popen(adb + ['shell', 'run-as ' + shlex.quote(a.package) + ' sh -c ' + shlex.quote(launch)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + a.timeout
    last_output = time.monotonic()
    pending = b''
    screenshots = set()
    with (out / 'probe.log').open('wb') as log:
        while selector.get_map():
            if time.monotonic() - last_output > 10:
                diagnostics.snapshot(remote, 'no client output for 10 seconds')
            if time.monotonic() > deadline: raise subprocess.TimeoutExpired(command, a.timeout)
            for key, _ in selector.select(0.2):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                last_output = time.monotonic()
                log.write(chunk); log.flush()
                pending += chunk
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    print(line.decode(errors='replace'), flush=True)
                    match = re.search(rb'WSI_FRAME epoch=([0-2]) frame=(0|7)\b', line)
                    if match and match.groups() not in screenshots:
                        screenshots.add(match.groups())
                        name = 'screen-' + match[1].decode() + '-' + match[2].decode() + '.png'
                        with (out / name).open('wb') as picture:
                            subprocess.run(adb + ['exec-out', 'screencap', '-p'], stdout=picture, check=True, timeout=10)
    code = process.wait(timeout=5)
    if code not in (0, 3):
        diagnostics.snapshot(remote, 'client exited unsuccessfully; client may already be gone')
except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
    code = 2
    (out / 'runner-error.txt').write_text(str(error))
    diagnostics.snapshot(remote, 'runner operation failed')
except subprocess.TimeoutExpired:
    code = 124
    diagnostics.snapshot(remote, 'host timeout before terminating owned client')
    # Only the PID written by this run's launcher is targeted.
    stop_owned_process()
    if process:
        try: process.communicate(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.communicate()
finally:
    if process and process.poll() is None:
        stop_owned_process()
        try: process.communicate(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.communicate()
    for name in ['maps-instance.txt', 'maps-surface.txt', 'maps-frame.txt'] + [
            f'image-{epoch}-{frame}.rgba' for epoch in range(3) for frame in (0, 7)]:
        saved = app('cat ' + shlex.quote(remote + '/' + name), capture_output=True)
        if saved.returncode == 0: (out / name).write_bytes(saved.stdout)
    paths = set()
    for mapping in out.glob('maps-*.txt'):
        for line in mapping.read_text().splitlines():
            fields = line.split(maxsplit=5)
            if len(fields) == 6 and fields[5].startswith(('/vendor/', '/system/', '/system_ext/', '/apex/')) and '.so' in fields[5]:
                paths.add(fields[5])
    if paths:
        hashes = app('sha256sum ' + shlex.join(sorted(paths)), capture_output=True, text=True)
        (out / 'android-library-hashes.json').write_text(json.dumps({
            'exit_code': hashes.returncode, 'output': hashes.stdout, 'errors': hashes.stderr}, indent=2))
        if hashes.returncode and code == 0: code = 2
    app('rm -rf ' + shlex.quote(remote), check=True)
    if archive.exists(): archive.unlink()
    diagnostics.finish()
if code == 0 and not a.icd_hal:
    try:
        evidence = verify_screen(out)
    except (ValueError, OSError) as error:
        evidence = {'status': 'FAIL', 'error': str(error)}
        code = 2
    (out / 'screen-evidence.json').write_text(json.dumps(evidence, indent=2))
if code not in (0, 3):
    diagnostics.screen('failure-screen.png')
    (out / 'diagnostics.json').write_text(json.dumps(diagnostics.records, indent=2))
status = 'PASS' if code == 0 else 'UNSUPPORTED' if code == 3 else 'TIMEOUT' if code in (124, 142) else 'CRASH' if code >= 128 else 'FAIL'
(out / 'result.json').write_text(json.dumps({'status': status, 'exit_code': code,
    'scope': 'icd-surface-lifecycle' if a.icd_hal else 'frontend-presentation'}, indent=2))
print(out)
print(status, code)
raise SystemExit(0 if code in (0, 3) else 1)
