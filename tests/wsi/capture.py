"""Window capture/replay against the existing Wayland probe's copy-to-buffer path."""
import hashlib
import json
import re
import shlex
import shutil
from pathlib import Path

SAVED = ((0, 0), (0, 7), (1, 0), (1, 7), (2, 0), (2, 7))
SIZES = ((320, 240), (448, 288), (256, 192))


def stage_tools(install, stage, metadata, sha256):
    provenance = json.loads((install / 'manifest.json').read_text())
    for name, expected in provenance['files'].items():
        if sha256(install / name) != expected:
            raise ValueError('capture tool manifest mismatch: ' + name)
    metadata['capture_build'] = provenance
    dest = stage / 'capture-tools'
    dest.mkdir()
    for name in ('gfxrecon-replay', 'gfxrecon-convert'):
        shutil.copy2(install / 'bin' / name, dest / name)
    layers = list(install.glob('lib*/**/libVkLayer_gfxreconstruct.so'))
    if len(layers) != 1:
        raise ValueError('expected one installed GFXReconstruct layer')
    shutil.copy2(layers[0], dest / layers[0].name)
    shutil.copytree(install / 'runtime', dest / 'runtime')
    original = install / 'share/vulkan/explicit_layer.d/VkLayer_gfxreconstruct.json'
    manifest = json.loads(original.read_text())
    if manifest['layer']['name'] != 'VK_LAYER_LUNARG_gfxreconstruct':
        raise ValueError('unexpected capture layer name')
    manifest['layer']['library_path'] = './libVkLayer_gfxreconstruct.so'
    (dest / 'capture.json').write_text(json.dumps(manifest))
    metadata['capture_tools'] = {
        str(path.relative_to(dest)): sha256(path)
        for path in dest.rglob('*') if path.is_file()}


def _bgra_to_rgba(data):
    pixels = bytearray(data)
    for i in range(0, len(pixels), 4):
        pixels[i], pixels[i + 2] = pixels[i + 2], pixels[i]
    return bytes(pixels)


def _json_documents(raw):
    text = raw.decode() if isinstance(raw, (bytes, bytearray)) else raw
    decoder = json.JSONDecoder()
    idx = 0
    docs = []
    while idx < len(text):
        while idx < len(text) and text[idx].isspace():
            idx += 1
        if idx >= len(text):
            break
        obj, end = decoder.raw_decode(text, idx)
        docs.append(obj)
        idx = end
    if not docs:
        raise ValueError('empty replay resource report')
    return docs


def _transfer_commands(docs, expected_indexes):
    # GFXReconstruct 1.0.5 writes one dump-resources object per transfer. Each
    # transferCommands array pads earlier slots with null; flattening those
    # arrays is not the dumped copy list.
    by_index = {}
    for doc in docs:
        items = doc if isinstance(doc, list) else [doc]
        for item in items:
            if not isinstance(item, dict):
                continue
            for cmd in item.get('transferCommands') or []:
                if not cmd:
                    continue
                index = cmd.get('cmdIndex')
                if index is None:
                    raise ValueError('replay dump is missing cmdIndex')
                by_index[index] = cmd
    missing = [index for index in expected_indexes if index not in by_index]
    if missing:
        raise ValueError('replay dump missing copies %s' % missing)
    extra = sorted(set(by_index) - set(expected_indexes))
    if extra:
        raise ValueError('replay dump has unexpected copies %s' % extra)
    return [by_index[index] for index in expected_indexes]


def verify_window_capture(app, remote, tool_prefix, out, log_text, timeout=120):
    evidence = out / 'capture'
    evidence.mkdir()
    copies_expected = 24
    formats = [int(value) for value in re.findall(r'^WSI format=(\d+) ', log_text, re.M)]
    if len(formats) != 3:
        raise ValueError('expected three swapchain format records, got %u' % len(formats))
    convert = (tool_prefix + './capture-tools/gfxrecon-convert --include-binaries --format jsonl '
               '--output calls.jsonl window.gfxr')
    converted = app('cd ' + shlex.quote(remote) + ' && ' + convert,
                    capture_output=True, timeout=timeout)
    (evidence / 'convert.log').write_bytes((converted.stdout or b'') + (converted.stderr or b''))
    if converted.returncode:
        raise ValueError('gfxrecon-convert failed')
    jsonl = app('cat ' + shlex.quote(remote + '/calls.jsonl'), capture_output=True, timeout=timeout)
    if jsonl.returncode:
        raise ValueError('failed to read capture jsonl')
    (evidence / 'calls.jsonl').write_bytes(jsonl.stdout)
    calls = [json.loads(line) for line in jsonl.stdout.decode().splitlines() if line.strip()]
    begins, copies, submits, presents = [], [], [], []
    for call in calls:
        name = call.get('function', {}).get('name')
        if name == 'vkBeginCommandBuffer': begins.append(call['index'])
        if name == 'vkCmdCopyImageToBuffer': copies.append(call['index'])
        if name == 'vkQueueSubmit': submits.append(call['index'])
        if name == 'vkQueuePresentKHR': presents.append(call['index'])
    if len(copies) < copies_expected or len(begins) < copies_expected or len(submits) < copies_expected:
        raise ValueError('capture is missing the 24-frame copy/submit sequence')
    if len(presents) < copies_expected:
        raise ValueError('capture is missing presented frames')
    window_copies = copies[-copies_expected:]
    window_begins = begins[-copies_expected:]
    window_submits = submits[-copies_expected:]
    request = {
        'BeginCommandBuffer': window_begins,
        'Transfer': [[index] for index in window_copies],
        'QueueSubmit': window_submits,
        'DumpResourcesOptions': {'DumpRawImages': True}}
    (evidence / 'dump.json').write_text(json.dumps(request, indent=2) + '\n')
    app('cat > ' + shlex.quote(remote + '/dump.json'),
        input=(evidence / 'dump.json').read_text(), text=True, check=True, timeout=30)
    replay = (tool_prefix + './capture-tools/gfxrecon-replay --swapchain virtual '
              '--dump-resources dump.json --dump-resources-dir replay window.gfxr')
    replayed = app('cd ' + shlex.quote(remote) + ' && mkdir -p replay && ' + replay,
                   capture_output=True, timeout=timeout)
    (evidence / 'replay.log').write_bytes((replayed.stdout or b'') + (replayed.stderr or b''))
    if replayed.returncode:
        raise ValueError('gfxrecon-replay dump failed')
    reports = app('sh -c ' + shlex.quote('cat ' + remote + '/replay/*_dr.json'),
                  capture_output=True, timeout=timeout)
    if reports.returncode or not reports.stdout:
        raise ValueError('missing replay resource report')
    (evidence / 'replay-report.json').write_bytes(reports.stdout)
    transfers = _transfer_commands(_json_documents(reports.stdout), window_copies)
    bgra = 44  # VK_FORMAT_B8G8R8A8_UNORM
    compared = []
    for saved, (epoch, frame) in enumerate(SAVED):
        copy_index = epoch * 8 + frame
        transfer = transfers[copy_index]
        if transfer.get('cmdType') != 'vkCmdCopyImageToBuffer':
            raise ValueError('replay dump is not a copy-to-buffer')
        regions = transfer['parameters']['regions']
        width, height = SIZES[epoch]
        expected_bytes = width * height * 4
        if len(regions) != 1 or regions[0]['size'] != expected_bytes:
            raise ValueError('unexpected copy region for epoch %u frame %u' % (epoch, frame))
        dump_path = regions[0]['file']
        if not dump_path.startswith('/'):
            dump_path = remote + '/' + dump_path
        dumped = app('cat ' + shlex.quote(dump_path),
                     capture_output=True, timeout=timeout)
        if dumped.returncode:
            raise ValueError('failed to read replay dump ' + regions[0]['file'])
        raw = dumped.stdout
        if formats[epoch] == bgra:
            raw = _bgra_to_rgba(raw)
        recorded = (out / ('image-%u-%u.rgba' % (epoch, frame))).read_bytes()
        if raw != recorded:
            raise ValueError('replay copy does not match live readback for epoch %u frame %u' % (epoch, frame))
        compared.append({'epoch': epoch, 'frame': frame, 'copy_index': window_copies[copy_index],
                         'bytes': expected_bytes, 'sha256': hashlib.sha256(recorded).hexdigest(),
                         'replay_file': regions[0]['file']})
    gfxr = app('sha256sum ' + shlex.quote(remote + '/window.gfxr'), capture_output=True, text=True, timeout=30)
    result = {'status': 'PASS', 'copies': copies_expected, 'presents': len(presents),
              'formats': formats, 'images': compared,
              'capture_sha256': gfxr.stdout.split()[0] if gfxr.returncode == 0 else None,
              'comparison': 'live image-*-*.rgba == replayed vkCmdCopyImageToBuffer',
              'scope': 'three-size window copies; virtual swapchain dump, not a second present'}
    (evidence / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    return result
