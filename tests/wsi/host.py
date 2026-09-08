"""Client transport and observation of an already running compositor APK."""
import fcntl
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import shutil
import subprocess
import tarfile
import time
from manifest import sha256_file, verify_manifest

PACKAGE = 'io.taowen.hybriswsitest'

class Host:
    def __init__(self, serial, out, package=PACKAGE):
        self.package = package
        self.adb = [os.environ.get('ADB', 'adb'), '-s', serial]
        self.out = out
        self.files = '/data/user/0/' + self.package + '/files'
        self.record = {'serial': serial, 'package': package, 'runs': [],
                       'host_sha256': sha256_file(Path(__file__)), 'screen_settle_seconds': .6}
        locks = Path(__file__).resolve().parent / 'build/locks'
        locks.mkdir(parents=True, exist_ok=True)
        self.lock = (locks / (serial.replace(':', '_').replace('/', '_') + '.lock')).open('w')
        fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def shell(self, command, **kwargs):
        kwargs.setdefault('timeout', 10)
        return subprocess.run(self.adb + ['shell', command], **kwargs)

    def app(self, command, **kwargs):
        return self.shell('run-as ' + self.package + ' sh -c ' + shlex.quote(command), **kwargs)

    def prop(self, name):
        return self.shell('getprop ' + shlex.quote(name), capture_output=True, text=True, check=True).stdout.strip()

    def identity(self):
        values = self.shell('pidof ' + self.package, capture_output=True, text=True).stdout.split()
        if len(values) != 1 or not values[0].isdigit(): return None
        stat = self.app('cat /proc/' + values[0] + '/stat', capture_output=True, text=True)
        return {'pid': values[0], 'starttime': stat.stdout.rsplit(')', 1)[1].split()[19]} if stat.returncode == 0 else None

    def attach(self):
        # The compositor owns its sockets and server processes. Attach only;
        # never install, restart, unlink sockets, or stop the external service.
        deadline = time.monotonic() + 20
        previous = None; since = time.monotonic()
        while True:
            ready = self.app('test -S files/runtime/wayland-0', capture_output=True).returncode == 0
            current = self.identity() if ready else None
            if current and current == previous and time.monotonic() - since >= .3: break
            if current != previous: previous = current; since = time.monotonic()
            if time.monotonic() >= deadline: raise RuntimeError('installed compositor is not running with a stable Wayland socket; start it through its owning APK')
            time.sleep(.1)
        self.record['identity'] = current
        apk = self.shell('pm path ' + self.package, capture_output=True, text=True, check=True).stdout.strip().removeprefix('package:')
        self.record['apk_sha256'] = self.shell('sha256sum ' + shlex.quote(apk), capture_output=True, text=True, check=True).stdout.split()[0]
        self.record['fd_before'] = self.fd_snapshot('compositor-fd-before.txt')

    def check_identity(self):
        current = self.identity()
        if current != self.record['identity']: raise RuntimeError('compositor PID/starttime changed during probe')
        return current

    def fd_snapshot(self, name):
        pid = self.record['identity']['pid']
        value = self.app('ls -l /proc/' + pid + '/fd', capture_output=True, text=True, check=True)
        (self.out / name).write_text(value.stdout)
        return {'path': name, 'count': len(re.findall(r' \d+ -> ', value.stdout))}

    def close(self):
        errors = []
        try:
            if 'identity' in self.record: self.check_identity()
        except (OSError, RuntimeError, subprocess.SubprocessError) as error: errors.append(str(error))
        self.record['service_lifecycle'] = 'external; left running'
        self.record['cleanup_errors'] = errors
        if errors: self.record['status'] = 'FAIL'
        (self.out / 'isolation.json').write_text(json.dumps(self.record, indent=2) + '\n')
        self.lock.close()

    def upload(self, stage, remote):
        archive = stage.parent / 'stage.tar'
        try:
            with tarfile.open(archive, 'w') as bundle: bundle.add(stage, arcname='.')
            self.app('mkdir -p ' + shlex.quote(remote), check=True)
            with archive.open('rb') as data:
                self.app('cd ' + shlex.quote(remote) + ' && tar xf -', stdin=data, check=True, timeout=30)
        finally:
            archive.unlink(missing_ok=True)

    def stop_process(self, remote, pid_file='runner.pid'):
        value = self.app('cat ' + shlex.quote(remote + '/' + pid_file), capture_output=True, text=True)
        if value.returncode or not value.stdout.strip().isdigit(): return
        pid = value.stdout.strip()
        cwd = self.app('readlink /proc/' + pid + '/cwd', capture_output=True, text=True)
        if cwd.returncode == 0 and cwd.stdout.strip() == remote:
            self.app('kill -TERM ' + pid, capture_output=True)

    def execute(self, command, remote, out, timeout, diagnostics=None):
        launch = 'cd ' + shlex.quote(remote) + ' && echo $$ > runner.pid && exec env ' + command
        process = subprocess.Popen(self.adb + ['shell', 'run-as ' + self.package + ' sh -c ' + shlex.quote(launch)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        selector = selectors.DefaultSelector(); selector.register(process.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + timeout; last_output = time.monotonic(); pending = b''; screens = set()
        try:
            with (out / 'probe.log').open('wb') as log:
                while selector.get_map():
                    if time.monotonic() > deadline: raise subprocess.TimeoutExpired(command, timeout)
                    if diagnostics and time.monotonic() - last_output > 10:
                        diagnostics.snapshot(remote, 'no client output for 10 seconds')
                    for key, _ in selector.select(.1):
                        data = os.read(key.fileobj.fileno(), 65536)
                        if not data: selector.unregister(key.fileobj); continue
                        last_output = time.monotonic(); log.write(data); log.flush(); pending += data
                        while b'\n' in pending:
                            line, pending = pending.split(b'\n', 1); print(line.decode(errors='replace'), flush=True)
                            match = re.search(rb'^(?:WSI_FRAME|X11_RESIZE_FRAME) epoch=(\d+) frame=(0|7)\b', line)
                            simple = re.search(rb'^X11_FRAME frame=(0|7)\b', line)
                            frame = match.groups() if match else (b'0', simple[1]) if simple else None
                            if frame and frame not in screens:
                                screens.add(frame)
                                # Submission/release is not physical display completion. Both
                                # clients keep these two frames still for two seconds.
                                time.sleep(.6)
                                path = out / ('screen-' + frame[0].decode() + '-' + frame[1].decode() + '.png')
                                with path.open('wb') as picture:
                                    subprocess.run(self.adb + ['exec-out', 'screencap', '-p'], stdout=picture, check=True, timeout=10)
            return process.wait(timeout=5)
        except (OSError, subprocess.SubprocessError):
            if diagnostics: diagnostics.snapshot(remote, "runner failed before stopping owned client")
            raise
        finally:
            selector.close()
            if process.poll() is None:
                self.stop_process(remote)
                try: process.communicate(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.communicate()


def stage_runtime(build, stage):
    provenance = json.loads((build / 'manifest.json').read_text())
    verify_manifest(provenance, build / 'install/usr/lib/hybris', build / 'runtime')
    stage.mkdir(parents=True)
    shutil.copytree(build / 'install/usr/lib/hybris', stage / 'hybris', symlinks=True)
    shutil.copytree(build / 'runtime', stage / 'glibc', symlinks=True)
    verify_manifest(provenance, stage / 'hybris', stage / 'glibc')
    return provenance
