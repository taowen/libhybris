"""Optional GFXReconstruct integration for the fixed headless widget fixture."""
import json
import hashlib
import shlex
import shutil
import subprocess
from draw_evidence import check_draw


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


def run_capture(shell, adb, remote, out, command, metadata, kill_remote):
    """Capture both bindings, replay the copy commands, and compare raw pixels."""
    evidence = out / 'capture'
    evidence.mkdir()
    command = command.replace('--library-path ./standard:./hybris:./glibc',
                              '--library-path ./standard:./hybris:./glibc:./capture-tools:./capture-tools/runtime')

    def run(name, cmd):
        metadata['commands'][name] = {'directory': remote, 'command': cmd}
        try:
            result = shell('cd ' + remote + ' && sh -c ' + shlex.quote(
                'echo $$ > probe.pid; exec env ' + cmd),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=35)
        except subprocess.TimeoutExpired as exc:
            (evidence / (name + '.log')).write_bytes(exc.stdout or b'')
            kill_remote()
            raise
        (evidence / (name + '.log')).write_bytes(result.stdout)
        result.check_returncode()

    def pull(relative, destination):
        subprocess.run(adb + ['pull', remote + '/' + relative, str(destination)],
                       check=True, stdout=subprocess.DEVNULL, timeout=35)

    tool_env = command[:command.index('./glibc/ld-linux-aarch64.so.1')]
    tool_launch = ('./glibc/ld-linux-aarch64.so.1 --library-path '
                   './standard:./hybris:./glibc:./capture-tools:./capture-tools/runtime ')
    comparisons = []
    for binding in ('good', 'bad'):
        folder = 'capture/' + binding
        local = evidence / binding
        local.mkdir()
        shell('mkdir -p ' + remote + '/' + folder + '/reference ' + remote + '/' +
              folder + '/captured ' + remote + '/' + folder + '/replay', check=True, timeout=10)
        probe = command.removesuffix('ubo') + 'ubo-' + binding
        run('reference-' + binding, 'PROBE_WIDGET_DUMP_DIR=$PWD/' + folder + '/reference ' + probe)
        captured = probe.replace('VK_LAYER_PATH=$PWD/layers', 'VK_LAYER_PATH=$PWD/capture-tools')
        run('capture-' + binding, 'PROBE_WIDGET_DUMP_DIR=$PWD/' + folder + '/captured '
            'VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_gfxreconstruct '
            'GFXRECON_CAPTURE_FILE=$PWD/' + folder + '/widget.gfxr '
            'GFXRECON_CAPTURE_FILE_TIMESTAMP=false ' + captured)
        run('convert-' + binding, tool_env + tool_launch + './capture-tools/gfxrecon-convert '
            '--format jsonl --output ' + folder + '/calls.jsonl ' + folder + '/widget.gfxr')
        pull(folder + '/calls.jsonl', local / 'calls.jsonl')
        calls = [json.loads(line) for line in (local / 'calls.jsonl').read_text().splitlines()]
        begins, copies, submits = [], [], []
        draws, passes = [], []
        for call in calls:
            name = call.get('function', {}).get('name')
            if name == 'vkBeginCommandBuffer': begins.append(call['index'])
            if name == 'vkCmdCopyImageToBuffer': copies.append(call['index'])
            if name == 'vkQueueSubmit': submits.append(call['index'])
            if name == 'vkCmdDrawIndexed': draws.append(call['index'])
            if name in ('vkCmdBeginRenderPass', 'vkCmdEndRenderPass'): passes.append(call['index'])
        if not len(begins) == len(copies) == len(submits) == 1:
            raise ValueError('capture does not contain the expected widget submission')
        if not begins[0] < copies[0] < submits[0]:
            raise ValueError('unexpected fixture command ordering')
        request = {'BeginCommandBuffer': begins, 'Transfer': [[copies[0]]], 'QueueSubmit': submits}
        if len(draws) != 1 or len(passes) != 2 or not begins[0] < passes[0] < draws[0] < passes[1] < copies[0]:
            raise ValueError('unexpected widget draw/render-pass structure')
        request.update({'Draw': [draws], 'RenderPass': [[passes]],
                        'DumpResourcesOptions': {'DumpBeforeCommand': True,
                                                 'DumpAllDescriptors': True,
                                                 'DumpRawImages': True,
                                                 'DumpVertexIndexBuffer': True}})
        (local / 'dump.json').write_text(json.dumps(request, indent=2) + '\n')
        subprocess.run(adb + ['push', str(local / 'dump.json'), remote + '/' + folder + '/dump.json'],
                       check=True, stdout=subprocess.DEVNULL, timeout=35)
        run('replay-' + binding, tool_env + tool_launch + './capture-tools/gfxrecon-replay '
            '--dump-resources ' + folder + '/dump.json --dump-resources-dir ' + folder +
            '/replay ' + folder + '/widget.gfxr')
        pull(folder + '/.', local)
        # Each fixture copies exactly one tightly packed RGBA8 16x16 region.
        reports = list((local / 'replay').glob('*_dr.json'))
        if len(reports) != 1:
            raise ValueError('expected one replay resource report')
        transfers = [cmd for item in json.loads(reports[0].read_text())
                     for cmd in item.get('transferCommands', [])]
        if len(transfers) != 1 or transfers[0]['cmdIndex'] != copies[0]:
            raise ValueError('replay dump is not associated with the selected copy')
        transfer = transfers[0]
        if (transfer['beginCommandBufferIndex'] != begins[0] or
                transfer['queueSubmitIndex'] != submits[0] or
                transfer['cmdType'] != 'vkCmdCopyImageToBuffer'):
            raise ValueError('replay command/submit association mismatch')
        regions = transfer['parameters']['regions']
        if len(regions) != 1 or regions[0]['size'] != 1024:
            raise ValueError('unexpected replay copy region')
        dumped = evidence.parent / regions[0]['file']
        if not dumped.resolve().is_relative_to((local / 'replay').resolve()):
            raise ValueError('replay resource path is outside its output directory')
        name = 'widget-' + binding + '.rgba'
        expected = (local / 'reference' / name).read_bytes()
        recorded = (local / 'captured' / name).read_bytes()
        if len(expected) != 1024 or recorded != expected or dumped.read_bytes() != expected:
            raise ValueError('full-image capture/replay mismatch for ' + binding)
        draw_evidence = check_draw(calls, json.loads(reports[0].read_text()), local, evidence,
                                   binding, (begins[0], draws[0], submits[0]), expected)
        comparisons.append({'binding': binding, 'copy_index': copies[0],
                            'draw_evidence': draw_evidence,
                            'submit_index': submits[0], 'replay_file': regions[0]['file'],
                            'rgba_sha256': hashlib.sha256(expected).hexdigest()})
    good, bad = (item['draw_evidence'] for item in comparisons)
    if good['before_sha256'] != bad['before_sha256'] or good['after_sha256'] == bad['after_sha256']:
        raise ValueError('injected binding did not first diverge at the draw attachment')
    (evidence / 'comparison.json').write_text(json.dumps({
        'first_divergent_draw': bad['draw_index'],
        'status': 'PASS', 'images': comparisons, 'bytes_per_image': 1024,
        'comparison': 'reference == captured == replay; exact RGBA8 bytes',
        'scope': 'two fixed headless widget submissions; no WSI/present'}, indent=2) + '\n')
