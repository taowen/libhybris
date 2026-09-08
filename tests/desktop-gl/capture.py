"""Preserve a desktop GL run's Vulkan capture, including failed rendering runs."""
import json
import shlex
import subprocess
from vertex_capture import vertex_draws
from indirect_uploads import upload_evidence
from capture_files import preserve_binaries, check_references


def collect_capture(shell, remote, out, library_path, sha):
    evidence = out / 'capture'
    evidence.mkdir()
    source = shell('cat ' + shlex.quote(remote + '/desktop.gfxr'), capture_output=True, timeout=30)
    if source.returncode or not source.stdout:
        raise ValueError('Vulkan capture is missing or empty')
    binary = evidence / 'desktop.gfxr'
    binary.write_bytes(source.stdout)
    device_hash = shell('sha256sum ' + shlex.quote(remote + '/desktop.gfxr'),
                        capture_output=True, text=True, check=True, timeout=10).stdout.split()[0]
    if sha(binary) != device_hash:
        raise ValueError('saved capture differs from device file')
    command = ('cd ' + shlex.quote(remote) + ' && echo $$ > capture-tool.pid && exec ./runtime/ld-linux-aarch64.so.1 --library-path ' +
               shlex.quote(library_path) + ' ./capture-tools/gfxrecon-convert --include-binaries --format jsonl '
               '--output calls.jsonl desktop.gfxr')
    entry = {'command': command, 'exit_code': None}
    try:
        result = shell('sh -c ' + shlex.quote(command), capture_output=True, timeout=60)
        entry['exit_code'] = result.returncode
        (evidence / 'convert.log').write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise ValueError('GFXReconstruct conversion failed')
    except subprocess.TimeoutExpired as error:
        entry['timed_out'] = True
        (evidence / 'convert.log').write_bytes((error.stdout or b'') + (error.stderr or b''))
        raise
    finally:
        (evidence / 'commands.json').write_text(json.dumps(entry, indent=2) + '\n')
    converted = shell('cat ' + shlex.quote(remote + '/calls.jsonl'), capture_output=True, check=True, timeout=30)
    calls = evidence / 'calls.jsonl'
    calls.write_bytes(converted.stdout)
    binaries = preserve_binaries(shell, remote, evidence, sha)
    binaries['references'] = check_references(calls, evidence)
    vertices = vertex_draws(calls)
    uploads = upload_evidence(calls, evidence)
    (evidence / 'indirect-uploads.json').write_text(json.dumps(uploads, indent=2) + '\n')
    (evidence / 'vertex-draws.json').write_text(json.dumps(vertices, indent=2) + '\n')
    count = vertices['function_calls']
    maps = (out / 'maps.txt').read_text()
    if 'libVkLayer_gfxreconstruct.so' not in maps:
        raise ValueError('capture layer mapping was not observed')
    return {'status': 'PASS', 'capture_sha256': device_hash, 'calls_sha256': sha(calls),
            'indirect_uploads_found': sum(u['status'] == 'FOUND' for u in uploads),
            'indirect_uploads_unavailable': sum(u['status'] != 'FOUND' for u in uploads),
            'binaries': binaries, 'function_calls': count, 'recorded_draws': vertices['recorded_draws'],
            'submitted_draws': vertices['submitted_draws'],
            'vertex_analysis_status': 'PARTIAL' if vertices['unexpanded_commands'] or any(
                not d['vertex_state_complete'] for d in vertices['draws']) else 'PASS',
            'replay': 'not performed'}
