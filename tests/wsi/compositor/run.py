#!/usr/bin/env python3
"""Run WSI clients in a fresh dedicated compositor process."""
import argparse
import hashlib
import fcntl
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--build', type=Path)
p.add_argument('--trace', action='store_true')
p.add_argument('--repeat', type=int, default=1, help='sequential clients sharing one compositor process (1–100)')
p.add_argument('--icd-hal')
p.add_argument('--vulkan-loader', type=Path)
p.add_argument('--icd-mali-loader-quirk', action='store_true')
p.add_argument('--swapchain-review', action='store_true', help='Exercise swapchain timeout, retirement, allocator and multi-present boundaries')
a = p.parse_args()
if a.swapchain_review and not a.icd_hal: p.error('--swapchain-review requires --icd-hal')
if (a.icd_hal is None) != (a.vulkan_loader is None):
    p.error('--icd-hal and --vulkan-loader must be supplied together')
if a.icd_mali_loader_quirk and not a.icd_hal:
    p.error('--icd-mali-loader-quirk requires --icd-hal')
if not 1 <= a.repeat <= 100: p.error('--repeat must be between 1 and 100')
root = Path(__file__).resolve().parents[1]
package = 'io.taowen.hybriswsitest'
adb = [os.environ.get('ADB', 'adb'), '-s', a.serial]
def shell(*command, **kwargs):
    return subprocess.run(adb + ['shell', *command], timeout=10, **kwargs)
out = root / 'build/isolated' / (time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
out.mkdir(parents=True)
# Coordinate this wrapper's own runs on the same device. Never stop Ardesk.
lock = (out.parent / (a.serial.replace(':', '_').replace('/', '_') + '.lock')).open('w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
record = {'serial': a.serial, 'package': package, 'status': 'starting',
          'requested_runs': a.repeat, 'runs': [],
          'wrapper_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
try:
    before = shell('pidof', package, capture_output=True, text=True)
    record['previous_pid'] = before.stdout.strip()
    shell('am', 'force-stop', package, check=True, capture_output=True)
    shell('run-as', package, 'rm', '-f', 'files/runtime/wayland-0', 'files/runtime/wayland-0.lock', check=True)
    shell('am', 'start', '-n', package + '/.CompositorActivity', check=True)
    deadline = time.monotonic() + 20
    while True:
        ready = shell('run-as', package, 'test', '-S', 'files/runtime/wayland-0', capture_output=True)
        current = shell('pidof', package, capture_output=True, text=True)
        values = current.stdout.split()
        if ready.returncode == 0 and len(values) == 1 and values[0].isdigit(): break
        if time.monotonic() >= deadline: raise RuntimeError('test compositor socket did not become ready')
        time.sleep(0.2)
    record['pid'] = current.stdout.strip()
    def identity():
        current = shell('pidof', package, capture_output=True, text=True, check=True)
        if current.stdout.split() != [record['pid']]:
            raise RuntimeError('compositor PID changed during repeated clients')
        stat = shell('run-as', package, 'cat', '/proc/' + record['pid'] + '/stat',
                     capture_output=True, text=True, check=True).stdout
        # Field 22 after the parenthesized comm (which can contain spaces).
        return {'pid': record['pid'], 'starttime': stat.rsplit(')', 1)[1].split()[19]}

    def fd_snapshot(name):
        value = shell('run-as', package, 'ls', '-l', '/proc/' + record['pid'] + '/fd',
                      capture_output=True, text=True, check=True)
        (out / name).write_text(value.stdout)
        return {'path': name, 'count': len(re.findall(r' \d+ -> ', value.stdout))}

    record['identity'] = identity()
    record['fd_before'] = fd_snapshot('compositor-fd-before.txt')
    record['runner_exit'] = 0
    for index in range(a.repeat):
        if identity() != record['identity']:
            raise RuntimeError('compositor process identity changed')
        directory = out / ('probe' if a.repeat == 1 else 'probe-' + str(index + 1))
        command = [sys.executable, str(root / 'run.py'), '--serial', a.serial,
                   '--package', package, '--out', str(directory)]
        if a.build: command += ['--build', str(a.build)]
        if a.trace: command += ['--trace']
        if a.icd_hal:
            command += ['--icd-hal', a.icd_hal, '--vulkan-loader', str(a.vulkan_loader)]
        if a.icd_mali_loader_quirk: command += ['--icd-mali-loader-quirk']
        if a.swapchain_review: command += ['--swapchain-review']
        code = subprocess.run(command).returncode
        entry = {'iteration': index + 1, 'runner_exit': code}
        record['runs'].append(entry)
        results = list(directory.glob('*/result.json'))
        if len(results) != 1:
            raise RuntimeError('runner did not produce exactly one result')
        entry['result_path'] = str(results[0].relative_to(out))
        entry['result'] = json.loads(results[0].read_text())
        clients = re.findall(r'^WSI_CLIENT pid=(\d+)$',
                             (results[0].parent / 'probe.log').read_text(), re.MULTILINE)
        if len(clients) != 1: raise RuntimeError('missing unique client PID evidence; rebuild WSI probe')
        entry['client_pid'] = int(clients[0])
        entry['fd_after'] = fd_snapshot('compositor-fd-after-' + str(index + 1) + '.txt')
        entry['identity_after'] = identity()
        if entry['identity_after'] != record['identity']:
            raise RuntimeError('compositor restarted during probe')
        if code or entry['result']['status'] != 'PASS':
            record['runner_exit'] = 1
            break
    record['status'] = 'PASS' if len(record['runs']) == a.repeat and not record['runner_exit'] else 'FAIL'
except BaseException as error:
    record['status'] = 'ERROR'
    record['error'] = str(error)
    raise

finally:
    shell('am', 'force-stop', package, check=True, capture_output=True)
    remaining = shell('pidof', package, capture_output=True, text=True)
    record['remaining_pid'] = remaining.stdout.strip()
    (out / 'isolation.json').write_text(json.dumps(record, indent=2))
    print(out)
if record['remaining_pid']: raise SystemExit('test process remained after stop')
raise SystemExit(record['runner_exit'])
