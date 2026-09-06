#!/usr/bin/env python3
"""Replay one saved baseline case under LLDB and retain first-stop evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shlex
import shutil
import subprocess
import time
import uuid


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--result', required=True, type=Path,
                   help='Saved run directory containing device.json and stage/')
    p.add_argument('--case', required=True)
    p.add_argument('--lldb-server', required=True, type=Path,
                   help='Android AArch64 lldb-server from the NDK')
    p.add_argument('--lldb', default='lldb')
    p.add_argument('--out', type=Path)
    p.add_argument('--stop-command', action='append', default=[], help='Extra LLDB command after collection')
    p.add_argument('--timeout', type=int, default=90)
    a = p.parse_args()
    metadata = json.loads((a.result / 'device.json').read_text())
    entry = metadata['commands'][a.case]
    fingerprint = subprocess.run(['adb', '-s', a.serial, 'shell', 'getprop ro.build.fingerprint'],
                                 check=True, capture_output=True, text=True, timeout=10).stdout.strip()
    if fingerprint != metadata['ro.build.fingerprint']:
        p.error('device fingerprint differs from the saved run')
    out = (a.out or a.result / ('debug-' + a.case + '-' + uuid.uuid4().hex[:8])).resolve()
    out.mkdir(parents=True, exist_ok=False)
    remote = '/data/local/tmp/hybris-debug-' + uuid.uuid4().hex[:12]
    adb = ['adb', '-s', a.serial]

    def run(args, **kw):
        return subprocess.run(args, check=True, timeout=30, **kw)

    def shell(command, **kw):
        return run(adb + ['shell', command], **kw)

    stage = out / 'stage'
    shutil.copytree(a.result / 'stage', stage, symlinks=False)
    for path in stage.rglob('*.json'):
        contents = path.read_text()
        if entry['directory'] in contents:
            path.write_text(contents.replace(entry['directory'], remote))
    shutil.copy2(a.lldb_server, stage / 'lldb-server')
    hashes = {str(f.relative_to(stage)): hashlib.sha256(f.read_bytes()).hexdigest()
              for f in stage.rglob('*') if f.is_file()}
    (out / 'staged-sha256.json').write_text(json.dumps(hashes, indent=2))
    for name in ('device.json', 'manifest.json', 'probe-manifest.json'):
        source = a.result / name
        if source.exists():
            shutil.copy2(source, out / ('source-' + name))
    command = entry['command'].replace(entry['directory'], remote)
    words = shlex.split(command)
    executable = next(i for i, word in enumerate(words) if '=' not in word)
    # Preserve run.py's shell expansion of $PWD in environment assignments.
    prefix = command[:command.index(words[executable])]
    arguments = shlex.join(words[executable:])
    port = random.randrange(20000, 50000)
    launch = ('cd ' + shlex.quote(remote) + ' && echo $$ > server.pid && exec env '
              + prefix + './lldb-server gdbserver 127.0.0.1:' + str(port) + ' -- ' + arguments)
    server = None
    forward = None
    try:
        shell('mkdir ' + shlex.quote(remote))
        run(adb + ['push', str(stage) + '/.', remote + '/'])
        shell('chmod 755 ' + shlex.quote(remote + '/lldb-server'))
        with (out / 'server.log').open('wb') as log:
            server = subprocess.Popen(adb + ['shell', launch], stdout=log, stderr=subprocess.STDOUT)
            forward = run(adb + ['forward', 'tcp:0', 'tcp:' + str(port)],
                          capture_output=True, text=True).stdout.strip()
            time.sleep(0.5)
            if server.poll() is not None:
                raise RuntimeError('lldb-server exited; see server.log')
            job = dict(adb=adb, out=str(out), remote=remote, port=forward,
                       command=command, case=a.case, stop_commands=a.stop_command, source_result=str(a.result.resolve()))
            job['lldb_version'] = run([a.lldb, '--version'], capture_output=True, text=True).stdout.strip()
            job['tool_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in (Path(__file__), Path(__file__).with_name('debug_collect.py'))}
            (out / 'job.json').write_text(json.dumps(job, indent=2))
            helper = Path(__file__).with_name('debug_collect.py').resolve()
            expression = ('script import runpy; _ = runpy.run_path(' + repr(str(helper))
                          + ', init_globals={"job_path": ' + repr(str(out / 'job.json')) + '})')
            pid_command = ('script _ = open(' + repr(str(out / 'pid'))
                           + ', "w").write(str(lldb.debugger.GetSelectedTarget().GetProcess().GetProcessID()))')
            with (out / 'lldb.log').open('wb') as client_log:
                completed = subprocess.run([a.lldb, '-b', '-o', 'target create --arch aarch64 ' + shlex.quote(str(stage / 'glibc/ld-linux-aarch64.so.1')),
                                            '-o', 'gdb-remote ' + forward, '-o', pid_command,
                                            '-o', 'process handle SIGALRM --stop false --notify false --pass false',
                                            '-o', 'continue', '-o', expression, '-k', expression], stdout=client_log,
                                           stderr=subprocess.STDOUT, timeout=a.timeout)
            if completed.returncode or not (out / 'stop.json').exists():
                raise RuntimeError('LLDB collection failed; see lldb.log')
    finally:
        pid_file = out / 'pid'
        if pid_file.exists() and pid_file.read_text().isdigit():
            pid = pid_file.read_text()
            kill_probe = ('case $(readlink /proc/' + pid + '/exe) in '
                          + shlex.quote(remote) + '/*) kill -9 ' + pid + ';; esac')
            subprocess.run(adb + ['shell', kill_probe], timeout=10, capture_output=True)
        cleanup = ('p=$(cat ' + shlex.quote(remote + '/server.pid') + ' 2>/dev/null); '
                   'if [ -n "$p" ] && [ "$(readlink /proc/$p/exe)" = '
                   + shlex.quote(remote + '/lldb-server') + ' ]; then kill "$p"; fi')
        subprocess.run(adb + ['shell', cleanup], timeout=10, capture_output=True)
        if server and server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        if forward:
            subprocess.run(adb + ['forward', '--remove', 'tcp:' + forward], timeout=10)
        subprocess.run(adb + ['shell', 'rm -rf ' + shlex.quote(remote)], timeout=15)
    print(out)


if __name__ == '__main__':
    main()
