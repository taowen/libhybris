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
a = p.parse_args()
if not 5 <= a.timeout <= 300: p.error('timeout must be between 5 and 300 seconds')
if not re.fullmatch(r'[A-Za-z0-9_.]+', a.package): p.error('invalid package')
adb = [os.environ.get('ADB', 'adb'), '-s', a.serial]
def shell(command, **kwargs): return subprocess.run(adb + ['shell', command], **kwargs)
def app(command, **kwargs): return shell('run-as ' + shlex.quote(a.package) + ' sh -c ' + shlex.quote(command), **kwargs)
def prop(name): return shell('getprop ' + shlex.quote(name), check=True, capture_output=True, text=True).stdout.strip()
provenance = json.loads((a.build / 'manifest.json').read_text())
probe_provenance = json.loads((a.probe / 'probe-manifest.json').read_text())
if sha256_file(a.probe / 'probe-wayland') != probe_provenance['binary_sha256']:
    raise SystemExit('probe hash mismatch; rebuild it')
verify_manifest(provenance, a.build / 'install/usr/lib/hybris', a.build / 'runtime')
run_id = time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8]
out = a.out / run_id
stage = out / 'stage'
stage.mkdir(parents=True)
shutil.copytree(a.build / 'install/usr/lib/hybris', stage / 'hybris', symlinks=True)
shutil.copytree(a.build / 'runtime', stage / 'glibc', symlinks=True)
shutil.copy2(a.probe / 'probe-wayland', stage / 'probe-wayland')
verify_manifest(provenance, stage / 'hybris', stage / 'glibc')
files = '/data/user/0/' + a.package + '/files'
remote = files + '/hybris-wsi-' + run_id
sdk = prop('ro.build.version.sdk')
env = {'HYBRIS_LINKER_DIR': remote + '/hybris/libhybris/linker',
       'HYBRIS_EGLPLATFORM_DIR': remote + '/hybris/libhybris',
       'HYBRIS_VULKANPLATFORM_DIR': remote + '/hybris/libhybris',
       'HYBRIS_EGLPLATFORM': 'wayland', 'HYBRIS_VULKANPLATFORM': 'wayland',
       'HYBRIS_ANDROID_SDK_VERSION': sdk, 'XDG_RUNTIME_DIR': files + '/runtime',
       'WAYLAND_DISPLAY': a.wayland}
if a.trace: env.update(HYBRIS_TRACE='1', HYBRIS_LOGGING_LEVEL='warn')
command = ' '.join(k + '=' + shlex.quote(v) for k, v in env.items())
command += ' ./glibc/ld-linux-aarch64.so.1 --library-path ./hybris:./glibc ./probe-wayland'
metadata = {'run_id': run_id, 'serial': a.serial, 'package': a.package,
            'fingerprint': prop('ro.build.fingerprint'), 'sdk': sdk,
            'command': command, 'remote': remote, 'host_timeout_seconds': a.timeout, 'runner_sha256': sha256_file(Path(__file__)), 'hybris': provenance, 'probe': probe_provenance}
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
except (OSError, subprocess.CalledProcessError) as error:
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
if code == 0:
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
(out / 'result.json').write_text(json.dumps({'status': status, 'exit_code': code}, indent=2))
print(out)
print(status, code)
raise SystemExit(0 if code in (0, 3) else 1)
