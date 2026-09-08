#!/usr/bin/env python3
"""Run client probes against an installed, already running compositor APK."""
import argparse
import json
import re
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from host import Host, PACKAGE


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--package', default=PACKAGE, help='installed debuggable compositor package; must already be running')
    p.add_argument('--runtime-dir', help='device-side XDG_RUNTIME_DIR; defaults to the package files/runtime directory')
    p.add_argument('--wayland-display', dest='wayland', default='wayland-0', help='existing Wayland socket name or absolute device path')
    p.add_argument('--display', default=':1', help='existing local X display supplied by the compositor')
    p.add_argument('--xauthority', help='device-side Xauthority path accessible to the compositor UID')
    p.add_argument('--platform', choices=('wayland', 'xcb', 'xlib'), default='wayland')
    p.add_argument('--case', choices=('present', 'swapchain-review', 'control', 'resize', 'missing-protocol', 'acquire-timeout'), default='present')
    p.add_argument('--repeat', type=int, default=1, help='1–100 sequential clients sharing one compositor process')
    p.add_argument('--backend', choices=('hybris', 'turnip'), default='hybris')
    p.add_argument('--mesa-build', type=Path, default=ROOT / 'tests/desktop-gl/build', help='verified product Mesa runtime from desktop-gl build.sh')
    p.add_argument('--build', type=Path, default=ROOT / 'tests/baseline/build')
    p.add_argument('--probe', type=Path, help='override the selected platform probe build directory')
    p.add_argument('--out', type=Path, default=ROOT / 'tests/wsi/build/results')
    p.add_argument('--timeout', type=float, help='host watchdog seconds (5–300)')
    p.add_argument('--trace', action='store_true')
    p.add_argument('--icd-hal', help='Android Vulkan HAL; required for Vulkan window cases (frontend windows are retired)')
    p.add_argument('--vulkan-loader', type=Path)
    p.add_argument('--icd-mali-loader-quirk', action='store_true')
    p.add_argument('--validation-layer', type=Path)
    p.add_argument('--validation-manifest', type=Path)
    p.add_argument('--capture-tools', type=Path)
    a = p.parse_args()
    wayland = a.platform == 'wayland'
    if a.case not in (('present', 'swapchain-review') if wayland else ('present', 'control', 'resize', 'missing-protocol', 'acquire-timeout')):
        p.error('case is not supported by this platform')
    if not 1 <= a.repeat <= 100: p.error('--repeat must be between 1 and 100')
    if a.backend == 'hybris':
        if (wayland or a.case != 'control') and not a.icd_hal:
            p.error('supply --icd-hal and --vulkan-loader for the hybris backend')
        if (a.icd_hal is None) != (a.vulkan_loader is None): p.error('supply both --icd-hal and --vulkan-loader')
    elif a.icd_hal or a.vulkan_loader or a.icd_mali_loader_quirk:
        p.error('Turnip uses the loader/ICD from --mesa-build and has no vendor HAL quirk')
    if a.backend == 'turnip' and a.case == 'swapchain-review':
        p.error('swapchain-review exercises hybris-specific allocation hooks')
    if (a.validation_layer is None) != (a.validation_manifest is None): p.error('supply both validation layer and manifest')
    if a.case == 'control' and (a.validation_layer or a.capture_tools):
        p.error('control has no Vulkan workload to validate or capture')
    if a.capture_tools and (not wayland or a.validation_layer or a.case == 'swapchain-review'):
        p.error('capture requires a separate Wayland present run: pinned capture tooling cannot combine validation or allocator-failure review')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+', a.package): p.error('invalid Android package name')
    if not re.fullmatch(r':\d+(?:\.0)?', a.display): p.error('--display must select a local X display, for example :1')
    if a.trace and not wayland: p.error('--trace is the Wayland native-window trace; X11 protocol trace is always collected')
    if a.probe is None: a.probe = ROOT / ('tests/wsi/build' if wayland else 'tests/x11/build')
    if a.timeout is None: a.timeout = (180 if a.validation_layer or a.capture_tools else 65) if wayland else 35
    if not 5 <= a.timeout <= 300: p.error('--timeout must be between 5 and 300 seconds')
    a.api = a.platform
    if a.runtime_dir is None: a.runtime_dir = '/data/user/0/' + a.package + '/files/runtime'
    if not a.runtime_dir.startswith('/'): p.error('--runtime-dir must be an absolute device path')
    if not a.wayland or (not a.wayland.startswith('/') and '/' in a.wayland): p.error('--wayland-display must be a socket name or absolute device path')
    a.swapchain_review = a.case == 'swapchain-review'
    out = a.out / (time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
    out.mkdir(parents=True)
    host = Host(a.serial, out, a.package, a.runtime_dir, a.wayland if wayland else None)
    host.record.update(backend=a.backend, platform=a.platform, case=a.case, requested_runs=a.repeat, status='FAIL')
    started = time.monotonic()
    try:
        host.attach()
        if wayland:
            from wayland import run
        else:
            from x11 import run
        for index in range(a.repeat):
            host.check_identity()
            directory = out / ('probe' if a.repeat == 1 else 'probe-' + str(index + 1))
            code = run(a, host, directory)
            result = json.loads((directory / 'result.json').read_text())
            clients = re.findall(r'^(?:WSI|X11)_CLIENT pid=(\d+)$', (directory / 'probe.log').read_text(), re.M)
            if len(clients) != 1: raise RuntimeError('missing unique client PID evidence')
            host.record['runs'].append({'iteration': index + 1, 'runner_exit': code,
                'result_path': str((directory / 'result.json').relative_to(out)), 'status': result['status'], 'client_pid': int(clients[0]),
                'fd_after': host.fd_snapshot('compositor-fd-after-' + str(index + 1) + '.txt'),
                'identity_after': host.check_identity()})
            if code or result['status'] != 'PASS':
                host.record['status'] = result['status']; break
        if len(host.record['runs']) == a.repeat and all(r['status'] == 'PASS' for r in host.record['runs']):
            host.record['status'] = 'PASS'
    except (Exception, KeyboardInterrupt, SystemExit) as error:
        host.record['error'] = str(error)
    finally:
        host.record['elapsed_seconds'] = round(time.monotonic() - started, 3)
        host.close()
        (out / 'result.json').write_text(json.dumps(host.record, indent=2) + '\n')
        print(out); print(host.record['status'])
    return 0 if host.record['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
