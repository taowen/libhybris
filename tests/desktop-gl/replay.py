"""Replay the fixed GL fixture and compare its retained readback images."""
import json
import re
import shlex
import subprocess
from capture_files import preserve_binaries
from replay_readbacks import readback_request, compare_readbacks


def replay_capture(shell, remote, out, library_path, env, memory, sha):
    evidence = out / 'capture'
    request, readbacks = readback_request(out, memory)
    encoded = json.dumps(request, indent=2) + '\n'
    (evidence / 'replay-request.json').write_text(encoded)
    shell('cat > ' + shlex.quote(remote + '/replay-request.json'), input=encoded,
          text=True, check=True, timeout=10)
    shell('mkdir ' + shlex.quote(remote + '/replay'), check=True, timeout=10)
    # Replay Vulkan calls with the captured ICD settings and without a capture layer.
    tool_env = {k: v for k, v in env.items() if k.startswith('HYBRIS_') or k == 'VK_DRIVER_FILES'}
    command = ('cd ' + shlex.quote(remote) + ' && echo $$ > replay-tool.pid && exec env ' +
               shlex.join(k + '=' + v for k, v in tool_env.items()) +
               ' ./runtime/ld-linux-aarch64.so.1 --library-path ' + shlex.quote(library_path) +
               ' ./capture-tools/gfxrecon-replay --swapchain offscreen -m ' + shlex.quote(memory) +
               ' --dump-resources replay-request.json --dump-resources-dir replay desktop.gfxr')
    record = {'command': command, 'memory_translation': memory, 'exit_code': None,
              'scope': 'fixed probe replay readbacks; original rendering status remains separate'}
    try:
        result = shell('sh -c ' + shlex.quote(command), capture_output=True, timeout=60)
        record['exit_code'] = result.returncode
        log = result.stdout + result.stderr
        (evidence / 'replay.log').write_bytes(log)
        record['files'] = preserve_binaries(shell, remote, evidence, sha, 'replay', 'replay-files.json')
        if result.returncode:
            raise ValueError('GFXReconstruct replay returned an error')
        record['diagnostics'] = [line for line in log.decode(errors='replace').splitlines()
                                 if re.search(r'\[gfxrecon\]\s+(WARNING|ERROR|FATAL)|GROUP_ERROR_FATAL|VK_ERROR_DEVICE_LOST', line)]
        comparisons = compare_readbacks(out, readbacks, sha)
        (evidence / 'replay-readbacks.json').write_text(json.dumps(comparisons, indent=2) + '\n')
        record['readbacks'] = len(comparisons)
        record['matched_images'] = sum(r['comparison'] == 'MATCH' for r in comparisons)
        record['mismatched_images'] = sum(r['comparison'] == 'MISMATCH' for r in comparisons)
        record['uncompared_images'] = sum(r['comparison'] == 'NOT_SAVED_BY_PROBE' for r in comparisons)
        if any(r['comparison'] == 'MISMATCH' for r in comparisons):
            raise ValueError('replayed image differs from the captured GL readback')
        if record['diagnostics']:
            raise ValueError('replay reported a tool warning/error or GPU fault')
        record['status'] = 'PASS'
    except subprocess.TimeoutExpired as error:
        record.update(status='TIMEOUT', timed_out=True)
        (evidence / 'replay.log').write_bytes((error.stdout or b'') + (error.stderr or b''))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        record.update(status='FAIL', error=str(error))
    finally:
        (evidence / 'replay-result.json').write_text(json.dumps(record, indent=2) + '\n')
    return record
