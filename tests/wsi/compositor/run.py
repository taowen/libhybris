#!/usr/bin/env python3
"""Run one probe in a fresh process of the dedicated compositor test package."""
import argparse
import hashlib
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--build', type=Path)
p.add_argument('--trace', action='store_true')
a = p.parse_args()
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
    command = [sys.executable, str(root / 'run.py'), '--serial', a.serial,
               '--package', package, '--out', str(out / 'probe')]
    if a.build: command += ['--build', str(a.build)]
    if a.trace: command += ['--trace']
    record['runner_exit'] = subprocess.run(command).returncode
    record['status'] = 'complete'
finally:
    shell('am', 'force-stop', package, check=True, capture_output=True)
    remaining = shell('pidof', package, capture_output=True, text=True)
    record['remaining_pid'] = remaining.stdout.strip()
    (out / 'isolation.json').write_text(json.dumps(record, indent=2))
    print(out)
if record['remaining_pid']: raise SystemExit('test process remained after stop')
raise SystemExit(record['runner_exit'])
