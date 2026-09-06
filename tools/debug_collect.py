"""Executed inside LLDB by debug-baseline.py; not a standalone command."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import lldb

job = json.loads(Path(job_path).read_text())
out = Path(job['out'])
adb = job['adb']
debugger = lldb.debugger
target = debugger.GetSelectedTarget()
process = target.GetProcess()
if not process.IsValid():
    raise RuntimeError('remote connection failed')
(out / 'pid').write_text(str(process.GetProcessID()))
try:
    status = dict(pid=process.GetProcessID(), state=process.GetState(),
                  exit_status=process.GetExitStatus(), threads=[])
    for thread in process if process.GetState() == lldb.eStateStopped else []:
        frames = []
        for frame in thread:
            frames.append(dict(pc=hex(frame.GetPC()), sp=hex(frame.GetSP()),
                               function=frame.GetFunctionName()))
        status['threads'].append(dict(tid=thread.GetThreadID(),
                                      reason=thread.GetStopDescription(1024), frames=frames))
    if process.GetState() == lldb.eStateStopped:
        maps = subprocess.run(adb + ['shell', 'cat /proc/' + str(process.GetProcessID()) + '/maps'],
                              check=True, capture_output=True, timeout=10).stdout.decode()
        (out / 'maps.txt').write_text(maps)
        mappings = []
        for line in maps.splitlines():
            fields = line.split(None, 5)
            if len(fields) != 6 or not fields[5].startswith('/'):
                continue
            start, end = (int(v, 16) for v in fields[0].split('-'))
            mappings.append(dict(start=start, end=end, offset=int(fields[2], 16), path=fields[5]))
        for thread in status['threads']:
            for frame in thread['frames']:
                pc = int(frame['pc'], 16)
                match = next((m for m in mappings if m['start'] <= pc < m['end']), None)
                if match:
                    frame['module'] = match['path']
                    frame['file_offset'] = hex(pc - match['start'] + match['offset'])
        debugger.HandleCommand('thread backtrace all')
        debugger.HandleCommand('register read --all')
        debugger.HandleCommand('disassemble --start-address `$pc-32` --count 32')
        debugger.HandleCommand('memory read --format x --size 8 --count 64 $sp')
        debugger.HandleCommand('image list -o -f')
        files = {}
        for path in sorted({m['path'] for m in mappings}):
            if path.startswith(job['remote'] + '/'):
                local = out / 'stage' / path[len(job['remote']) + 1:]
            elif path.endswith('.so') or '.so.' in path:
                local = out / 'sysroot' / path.lstrip('/')
                local.parent.mkdir(parents=True, exist_ok=True)
                pulled = subprocess.run(adb + ['pull', path, str(local)],
                                        capture_output=True, timeout=30)
                if pulled.returncode:
                    files[path] = {'error': pulled.stderr.decode(errors='replace')}
                    continue
            else:
                continue
            if local.is_file():
                item = dict(local=str(local.relative_to(out)),
                            sha256=hashlib.sha256(local.read_bytes()).hexdigest())
                info = subprocess.run(['readelf', '-n', str(local)], capture_output=True, text=True)
                item['build_ids'] = [line.strip() for line in info.stdout.splitlines()
                                     if 'Build ID:' in line]
                files[path] = item
        (out / 'mapped-files.json').write_text(json.dumps(files, indent=2))
        for path, item in files.items():
            if 'local' not in item:
                continue
            spec = lldb.SBModuleSpec()
            spec.SetFileSpec(lldb.SBFileSpec(str(out / item['local'])))
            module = lldb.SBModule(spec)
            if module.IsValid():
                target.AddModule(module)
            if module.IsValid():
                # ELF virtual addresses need not equal file offsets.
                elf = (out / item['local']).read_bytes()
                phoff = struct.unpack_from('<Q', elf, 32)[0]
                entsize, count = struct.unpack_from('<HH', elf, 54)
                zero_load = next((struct.unpack_from('<IIQQQQQQ', elf, phoff + i * entsize)
                                  for i in range(count)
                                  if struct.unpack_from('<I', elf, phoff + i * entsize)[0] == 1
                                  and struct.unpack_from('<Q', elf, phoff + i * entsize + 8)[0] == 0), None)
                initial = next((m for m in mappings if m['path'] == path and m['offset'] == 0), None)
                if not zero_load or not initial:
                    item['load_error'] = 'no offset-zero PT_LOAD mapping'
                    continue
                bias = initial['start'] - zero_load[3]
                loaded = target.SetModuleLoadAddress(module, bias)
                item['load_error'] = None if loaded.Success() else str(loaded)
            else:
                item['load_error'] = 'invalid module'
        (out / 'mapped-files.json').write_text(json.dumps(files, indent=2))
        debugger.HandleCommand('image lookup --address $pc')
        debugger.HandleCommand('thread backtrace all')
        for command in job.get('stop_commands', []):
            debugger.HandleCommand(command)
        # Save bounded memory around pointer-valued registers for offline inspection.
        memory = {}
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        for name in ['x' + str(i) for i in range(31)] + ['sp']:
            address = frame.FindRegister(name).GetValueAsUnsigned()
            error = lldb.SBError()
            data = process.ReadMemory(address, 512 if name != 'sp' else 16384, error)
            if data:
                filename = 'memory-' + name + '.bin'
                (out / filename).write_bytes(data)
                memory[name] = dict(address=hex(address), file=filename, size=len(data))
        (out / 'memory.json').write_text(json.dumps(memory, indent=2))
        status['symbolized_threads'] = []
        for thread in process:
            frames = [str(frame) for frame in thread]
            frame = thread.GetFrameAtIndex(0)
            registers = {name: frame.FindRegister(name).GetValue()
                         for name in ['x' + str(i) for i in range(29)] + ['fp', 'lr', 'sp', 'pc', 'cpsr']}
            status['symbolized_threads'].append(dict(tid=thread.GetThreadID(), frames=frames, registers=registers))
    (out / 'stop.json').write_text(json.dumps(status, indent=2))
finally:
    if process.IsValid() and process.GetState() != lldb.eStateExited:
        process.Kill()
