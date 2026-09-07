#!/usr/bin/env python3
"""Run one isolated X11 developer case; never starts or stops Ardesk."""
import argparse
import fcntl
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
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tests/wsi')]
from manifest import sha256_file, verify_manifest
from screen_evidence import verify_epoch
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--validation-layer', type=Path)
p.add_argument('--validation-manifest', type=Path)
p.add_argument('--api', choices=('xcb', 'xlib'), default='xcb')
p.add_argument('--case', choices=('control', 'present', 'missing-protocol', 'acquire-timeout'), default='present')
p.add_argument('--build', type=Path, default=ROOT / 'tests/baseline/build')
p.add_argument('--probe', type=Path, default=ROOT / 'tests/x11/build')
p.add_argument('--server-binary', type=Path, help='Explicit alternate Xwayland, e.g. an unextended server for missing-protocol')
p.add_argument('--icd-hal')
p.add_argument('--vulkan-loader', type=Path)
p.add_argument('--icd-mali-loader-quirk', action='store_true')
a = p.parse_args()
started = time.monotonic()
if (a.validation_layer is None) != (a.validation_manifest is None): p.error('supply both validation layer and manifest')
if a.case != 'control' and (not a.icd_hal or not a.vulkan_loader): p.error('present requires --icd-hal and --vulkan-loader')
package = 'io.taowen.hybriswsitest'
adb = [os.environ.get('ADB', 'adb'), '-s', a.serial]
def shell(command, **kw): return subprocess.run(adb + ['shell', command], **kw)
def app(command, **kw): return shell('run-as ' + package + ' sh -c ' + shlex.quote(command), **kw)
def prop(name): return shell('getprop ' + name, capture_output=True, text=True, check=True, timeout=10).stdout.strip()
out = a.probe / 'results' / (time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
out.mkdir(parents=True)
lockdir = ROOT / 'tests/wsi/build/isolated'; lockdir.mkdir(exist_ok=True)
lock = (lockdir / (a.serial.replace(':', '_').replace('/', '_') + '.lock')).open('w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
probe = json.loads((a.probe / 'manifest.json').read_text())
for name, digest in probe['files'].items():
    if sha256_file(a.probe / name) != digest: raise SystemExit('probe/server hash mismatch: ' + name)
hybris = json.loads((a.build / 'manifest.json').read_text())
verify_manifest(hybris, a.build / 'install/usr/lib/hybris', a.build / 'runtime')
stage = out / 'stage'; stage.mkdir()
shutil.copytree(a.build / 'install/usr/lib/hybris', stage / 'hybris', symlinks=True)
shutil.copytree(a.build / 'runtime', stage / 'glibc', symlinks=True)
shutil.copytree(a.probe / 'server', stage / 'x11')
if a.server_binary: shutil.copy2(a.server_binary, stage / 'x11/Xwayland')
for name in ('probe-xcb', 'x11-session'): shutil.copy2(a.probe / name, stage / name)
files = '/data/user/0/' + package + '/files'
remote = files + '/hybris-x11-' + out.name
libraries = './standard:./hybris:./glibc'
env = {'XDG_RUNTIME_DIR': files + '/runtime', 'WAYLAND_DISPLAY': 'wayland-0',
       'XKB_CONFIG_ROOT': files + '/xkb', 'LD_LIBRARY_PATH': remote + '/x11',
       'HYBRIS_X11_TRACE': '1', 'HYBRIS_ANDROID_SDK_VERSION': prop('ro.build.version.sdk'),
       'HYBRIS_LINKER_DIR': remote + '/hybris/libhybris/linker',
       'HYBRIS_EGLPLATFORM_DIR': remote + '/hybris/libhybris',
       'HYBRIS_VULKANPLATFORM_DIR': remote + '/hybris/libhybris'}
if a.validation_layer:
    (stage / 'layers').mkdir()
    shutil.copy2(a.validation_layer, stage / 'layers/libVkLayer_khronos_validation.so')
    layer = json.loads(a.validation_manifest.read_text())
    if layer['layer']['name'] != 'VK_LAYER_KHRONOS_validation': raise ValueError('unexpected layer')
    layer['layer']['library_path'] = './libVkLayer_khronos_validation.so'
    (stage / 'layers/validation.json').write_text(json.dumps(layer))
    shutil.copy2(a.validation_manifest, out / 'validation-original.json')
    env['VK_LAYER_PATH'] = remote + '/layers'; env['HYBRIS_X11_VALIDATION'] = '1'
if a.api == 'xlib': env['HYBRIS_X11_XLIB'] = '1'
if a.icd_hal: env['HYBRIS_VULKAN_HAL'] = a.icd_hal
if a.icd_mali_loader_quirk: env['HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK'] = '1'
if a.vulkan_loader:
    (stage / 'standard').mkdir(); shutil.copy2(a.vulkan_loader, stage / 'standard/libvulkan.so.1')
    env['VK_DRIVER_FILES'] = remote + '/driver.json'
prefix = ' '.join(k + '=' + shlex.quote(v) for k, v in env.items())
client = './glibc/ld-linux-aarch64.so.1 --library-path ' + libraries + ' ./probe-xcb '
command = prefix + ' ./x11-session ' + client + a.case
record = {'case': a.case, 'api': a.api, 'serial': a.serial, 'fingerprint': prop('ro.build.fingerprint'),
          'package': package, 'command': command, 'remote': remote, 'probe': probe, 'hybris': hybris,
          'server_sha256': sha256_file(stage / 'x11/Xwayland'), 'runner_sha256': sha256_file(Path(__file__)), 'checker_sha256': sha256_file(ROOT / 'tests/wsi/screen_evidence.py')}
if a.validation_layer: record['validation_layer_sha256'] = sha256_file(stage / 'layers/libVkLayer_khronos_validation.so')
code = 2; process = None

def stop_owned():
    value = app('cat ' + shlex.quote(remote + '/session.pid'), capture_output=True, text=True, timeout=10)
    if not value.stdout.strip().isdigit(): return
    pid = value.stdout.strip()
    cwd = app('readlink /proc/' + pid + '/cwd', capture_output=True, text=True, timeout=10)
    if cwd.returncode == 0 and cwd.stdout.strip() == remote:
        app('kill -TERM ' + pid, capture_output=True, timeout=10)

try:
    # This package is a disposable display host with no desktop startup scripts.
    shell('am force-stop ' + package, check=True, capture_output=True, timeout=10)
    app('rm -f files/runtime/wayland-0 files/runtime/wayland-0.lock', check=True, timeout=10)
    shell('am start -n ' + package + '/.CompositorActivity', check=True, capture_output=True, timeout=10)
    deadline = time.monotonic() + 15
    stable = None; since = time.monotonic()
    while True:
        ready = app('test -S files/runtime/wayland-0', capture_output=True, timeout=10).returncode == 0
        values = shell('pidof ' + package, capture_output=True, text=True, timeout=10).stdout.split()
        if ready and len(values) == 1:
            stat = app('cat /proc/' + values[0] + '/stat', capture_output=True, text=True, timeout=10)
            identity = (values[0], stat.stdout.rsplit(')', 1)[-1].split()[19]) if stat.returncode == 0 else None
            if identity and identity == stable and time.monotonic() - since >= .3: break
            if identity != stable: stable = identity; since = time.monotonic()
        else: stable = None; since = time.monotonic()
        if time.monotonic() > deadline: raise RuntimeError('display host did not reach one stable process')
        time.sleep(.1)
    record['compositor_pid'], record['compositor_starttime'] = stable
    apk = shell('pm path ' + package, capture_output=True, text=True, check=True, timeout=10).stdout.strip().removeprefix('package:')
    record['apk_sha256'] = shell('sha256sum ' + shlex.quote(apk), capture_output=True, text=True, check=True, timeout=10).stdout.split()[0]
    app('mkdir -p ' + shlex.quote(remote), check=True, timeout=10)
    archive = out / 'stage.tar'
    with tarfile.open(archive, 'w') as bundle: bundle.add(stage, arcname='.')
    with archive.open('rb') as data: app('cd ' + shlex.quote(remote) + ' && tar xf -', stdin=data, check=True, timeout=30)
    archive.unlink()
    if a.case != 'control':
        version = app('cd ' + shlex.quote(remote) + ' && ' + prefix + ' ' + client + 'version',
                      capture_output=True, text=True, check=True, timeout=20)
        (out / 'version.log').write_text(version.stdout + version.stderr)
        versions = re.findall(r'^X11_ICD_VERSION (\d+\.\d+\.\d+)$', version.stdout, re.M)
        if len(versions) != 1: raise ValueError('missing ICD version')
        driver = json.dumps({'file_format_version': '1.0.0', 'ICD': {'library_path': remote + '/hybris/libhybris-vulkan-icd.so.0', 'api_version': versions[0]}})
        (stage / 'driver.json').write_text(driver)
        app('cat > ' + shlex.quote(remote + '/driver.json'), input=driver, text=True, check=True, timeout=10)
        record['standard_loader_sha256'] = sha256_file(stage / 'standard/libvulkan.so.1')
    launch = 'cd ' + shlex.quote(remote) + ' && echo $$ > session.pid && exec env ' + command
    record['launch'] = launch
    process = subprocess.Popen(adb + ['shell', 'run-as ' + package + ' sh -c ' + shlex.quote(launch)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector(); selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 35; pending = b''
    with (out / 'probe.log').open('wb') as log:
        while selector.get_map():
            if time.monotonic() > deadline: raise subprocess.TimeoutExpired(launch, 35)
            for key, _ in selector.select(.1):
                data = os.read(key.fileobj.fileno(), 65536)
                if not data: selector.unregister(key.fileobj); continue
                log.write(data); log.flush(); pending += data
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1); print(line.decode(errors='replace'), flush=True)
                    frame = re.search(rb'^X11_FRAME frame=(0|7) ', line)
                    if frame:
                        # Allow the separate Xwayland/Wayland commit to reach the display.
                        time.sleep(.1)
                        with (out / ('screen-0-' + frame[1].decode() + '.png')).open('wb') as picture:
                            subprocess.run(adb + ['exec-out', 'screencap', '-p'], stdout=picture, check=True, timeout=10)
    code = process.wait(timeout=5)
except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
    record['error'] = str(error); code = 124 if isinstance(error, subprocess.TimeoutExpired) else 2
finally:
    cleanup_errors = []
    def cleanup(action):
        global code
        try: return action()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            cleanup_errors.append(str(error)); code = 2
    if process and process.poll() is None:
        cleanup(stop_owned)
        try: process.communicate(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.communicate()
    for name in ('xwayland.log', 'image-0-0.rgba', 'image-0-7.rgba', 'maps.txt'):
        copied = cleanup(lambda: app('cat ' + shlex.quote(remote + '/' + name), capture_output=True, timeout=10))
        if copied is not None and not copied.returncode: (out / name).write_bytes(copied.stdout)
    if (out / 'maps.txt').is_file():
        paths = set()
        for line in (out / 'maps.txt').read_text().splitlines():
            fields = line.split(maxsplit=5)
            if len(fields) == 6 and fields[5].startswith(('/vendor/', '/system/', '/system_ext/', '/apex/')) and '.so' in fields[5]: paths.add(fields[5])
        if paths:
            hashes = cleanup(lambda: app('sha256sum ' + shlex.join(sorted(paths)), capture_output=True, text=True, check=True, timeout=10))
            if hashes is not None: (out / 'android-library-hashes.txt').write_text(hashes.stdout)
    cleanup(lambda: app('rm -rf ' + shlex.quote(remote), check=True, timeout=10))
    current = cleanup(lambda: shell('pidof ' + package, capture_output=True, text=True, timeout=10))
    record['compositor_after'] = current.stdout.strip() if current is not None else None
    if record['compositor_after'] != record.get('compositor_pid'):
        record['isolation_error'] = 'compositor PID changed'; code = 2
    elif record.get('compositor_pid'):
        stat = cleanup(lambda: app('cat /proc/' + record['compositor_pid'] + '/stat', capture_output=True, text=True, check=True, timeout=10))
        if stat is None or stat.stdout.rsplit(')', 1)[-1].split()[19] != record['compositor_starttime']:
            record['isolation_error'] = 'compositor starttime changed'; code = 2
    cleanup(lambda: shell('am force-stop ' + package, capture_output=True, check=True, timeout=10))
    remaining = cleanup(lambda: shell('pidof ' + package, capture_output=True, text=True, timeout=10))
    record['remaining_compositor'] = remaining.stdout.strip() if remaining is not None else None
    if record['remaining_compositor']: code = 2
    record['cleanup_errors'] = cleanup_errors
if code == 0 and a.case == 'present':
    try:
        if a.validation_layer:
            log = (out / 'probe.log').read_text()
            if re.findall(r'^X11_VALIDATION errors=(\d+)$', log, re.M) != ['0'] or re.search(r'^VALIDATION ', log, re.M):
                raise ValueError('validation failed')
            record['validation_layer_sha256'] = sha256_file(stage / 'layers/libVkLayer_khronos_validation.so')
        record['screen'] = verify_epoch(out, 0, (320, 240))
        log = (out / 'probe.log').read_text()
        frames = re.findall(r'^X11_FRAME frame=(\d+) image=(\d+) pixels=76800 exact=1$', log, re.M)
        if [int(f[0]) for f in frames] != list(range(8)): raise ValueError('missing exact eight-frame sequence')
        active = {}; serials = set(); presents = releases = 0
        for event, window, serial, buffer in re.findall(r'^X11_WSI event=(present|release) window=(\d+) serial=(\d+) buffer=(0x[0-9a-f]+)$', log, re.M):
            if event == 'present':
                if buffer in active or serial in serials: raise ValueError('buffer reused before release or duplicate serial')
                active[buffer] = serial; serials.add(serial); presents += 1
            else:
                if active.pop(buffer, None) != serial: raise ValueError('release has no matching present')
                releases += 1
        if presents != 8 or releases < 5: raise ValueError('missing TAWC-DRI presentation/release evidence')
        record['protocol'] = {'presents': presents, 'releases': releases, 'reuse_after_release': True}
        maps = (out / 'maps.txt').read_text()
        if a.validation_layer and 'libVkLayer_khronos_validation.so' not in maps: raise ValueError('validation layer mapping missing')
        if '/standard/libvulkan.so.1' not in maps or 'libhybris-vulkan-icd.so.0' not in maps or '/system/lib64/libvulkan.so' in maps:
            raise ValueError('unexpected Vulkan loader mappings')
    except (ValueError, OSError) as error: record['screen_error'] = str(error); code = 2
if code == 0 and a.case == 'missing-protocol':
    log = (out / 'probe.log').read_text()
    if re.findall(r'^X11_REJECT attempt=(\d+) result=-13$', log, re.M) != [str(i) for i in range(8)]:
        record['rejection_error'] = 'missing eight exact rejections'; code = 2
if code == 0 and a.case == 'acquire-timeout':
    if not re.search(r'^X11_ACQUIRE held=3 zero=NOT_READY finite=TIMEOUT elapsed_ns=\d+ index_unchanged=1 fence_unsignaled=1$', (out / 'probe.log').read_text(), re.M):
        record['acquire_error'] = 'missing timeout verdict'; code = 2
record['status'] = 'PASS'  if code == 0 else 'UNSUPPORTED' if code == 3 else 'TIMEOUT' if code in (124, 142) else 'FAIL'
record['exit_code'] = code
record['elapsed_seconds'] = round(time.monotonic() - started, 3)
(out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
print(out); print(record['status'], code)
raise SystemExit(0 if code == 0 else 1)
