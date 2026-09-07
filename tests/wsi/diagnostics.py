"""Bounded compositor log capture and read-only process snapshots for a WSI run."""
from collections import deque
import atexit
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import threading


class Diagnostics:
    def __init__(self, adb, package, directory, app):
        self.adb, self.package, self.directory, self.app = adb, package, Path(directory), app
        self.records, self.logs = [], []
        self.snapshot_taken = False
        self.finished = False
        atexit.register(self.finish)
        self.pid = None
        try:
            result = subprocess.run(adb + ['shell', 'pidof', package], capture_output=True, timeout=5)
            values = result.stdout.decode().split()
            if result.returncode or len(values) != 1 or not values[0].isdigit():
                raise ValueError('cannot identify exactly one compositor process')
            self.pid = int(values[0])
            # Start at the current end of logcat without clearing any device log.
            proc = subprocess.Popen(adb + ['logcat', '--pid=' + str(self.pid), '-T', '1', '-v', 'threadtime'],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            ring = deque(maxlen=128)  # Last 128 * 4096 bytes, never an unbounded log.
            counts = {'bytes_seen': 0}
            def drain():
                while True:
                    chunk = proc.stdout.read1(4096)
                    if not chunk:
                        break
                    counts['bytes_seen'] += len(chunk)
                    ring.append(chunk)
            thread = threading.Thread(target=drain, daemon=True)
            thread.start()
            self.logs.append((proc, thread, ring, counts))
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            self.records.append({'operation': 'start compositor log', 'error': str(error)})

    def snapshot(self, remote, reason):
        if self.snapshot_taken:
            return
        self.snapshot_taken = True
        # PID/cwd checks bind the client snapshot to this run, including stalls
        # before Vulkan initialization. No signal or debugger attachment needed.
        script = 'p=$(cat ' + shlex.quote(remote + '/runner.pid') + '); '
        script += 'case "$p" in ""|*[!0-9]*) exit 2;; esac; '
        script += '[ "$(readlink /proc/$p/cwd)" = ' + shlex.quote(remote) + ' ] || exit 3; '
        script += 'cat /proc/$p/stat /proc/$p/status; head -c 131072 /proc/$p/maps; ls -l /proc/$p/fd; '
        script += 'for t in /proc/$p/task/*; do echo "$t"; cat "$t/comm" "$t/wchan"; echo; done'
        self._save('client-snapshot.txt', script)
        if self.pid:
            base = '/proc/' + str(self.pid)
            self._save('compositor-snapshot.txt', 'cat ' + base + '/stat ' + base + '/status; '
                       'head -c 131072 ' + base + '/maps; ls -l ' + base + '/fd')
        self.records.append({'operation': 'snapshot', 'reason': reason})
        self.screen('diagnostic-screen.png')

    def _save(self, name, script):
        try:
            # Apply the byte limit on the device as well as the host timeout.
            result = self.app('set -o pipefail; { ' + script + '; } 2>&1 | head -c 262144',
                              capture_output=True, timeout=5)
            (self.directory / name).write_bytes(result.stdout)
            self.records.append({'operation': name, 'exit_code': result.returncode,
                                 'bytes': len(result.stdout), 'limit': 262144,
                                 'limit_reached': len(result.stdout) == 262144})
        except (OSError, subprocess.SubprocessError) as error:
            self.records.append({'operation': name, 'error': str(error)})

    def screen(self, name):
        try:
            with (self.directory / name).open('wb') as output:
                result = subprocess.run(self.adb + ['exec-out', 'screencap', '-p'],
                                        stdout=output, stderr=subprocess.PIPE, timeout=5)
            self.records.append({'operation': name, 'exit_code': result.returncode})
        except (OSError, subprocess.SubprocessError) as error:
            self.records.append({'operation': name, 'error': str(error)})

    def finish(self):
        if self.finished:
            return
        self.finished = True
        for proc, thread, ring, counts in self.logs:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            thread.join(timeout=3)
            data = b''.join(ring)
            (self.directory / 'compositor.log').write_bytes(data)
            self.records.append({'operation': 'compositor log', 'pid': self.pid,
                                 'bytes_seen': counts['bytes_seen'], 'bytes_saved': len(data),
                                 'truncated': counts['bytes_seen'] > len(data), 'limit': 524288,
                                 'reader_finished': not thread.is_alive(),
                                 'initial_tail_lines_requested': 1})
        self.records.append({'collector_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()})
        (self.directory / 'diagnostics.json').write_text(json.dumps(self.records, indent=2))
