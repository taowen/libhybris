"""Shader/module/layout evidence for the fixed captured widget draw."""
import hashlib
import json
import re
import shutil
import struct
import subprocess
from descriptor_evidence import check_layout


def stage_shader_reference(bundle, out, metadata):
    manifest = json.loads((bundle / 'probe-manifest.json').read_text())
    files = {item['path']: item['sha256'] for item in manifest['source']['files']}
    dest = out / 'shader-reference'
    dest.mkdir()
    for stem, stage in ((stem, stage) for stem in ('widget', 'widget-multi') for stage in ('vert', 'frag')):
        for suffix in (stage, stage + '.inc'):
            relative = 'shaders/' + stem + '.' + suffix
            source = bundle / 'src' / relative
            if hashlib.sha256(source.read_bytes()).hexdigest() != files[relative]:
                raise ValueError('shader source snapshot differs from probe manifest: ' + relative)
            shutil.copy2(source, dest / source.name)
        words = [int(word, 16) for word in re.findall(
            r'0x([0-9a-fA-F]{8})', (dest / (stem + '.' + stage + '.inc')).read_text())]
        (dest / (stem + '.' + stage + '.spv')).write_bytes(struct.pack('<' + 'I' * len(words), *words))
    metadata['shader_evidence_tools'] = {}
    for tool in ('spirv-val', 'spirv-dis'):
        path = shutil.which(tool)
        if not path:
            raise ValueError('capture shader evidence requires host ' + tool)
        metadata['shader_evidence_tools'][tool] = {
            'path': path, 'version': subprocess.check_output([path, '--version'], text=True, timeout=10)}


def check_pipeline(calls, local, reference, bound_pipeline, bound_layout, descriptor_type, multi=False):
    def one(name):
        matches = [call for call in calls if call.get('function', {}).get('name') == name]
        if len(matches) != 1:
            raise ValueError('shader fixture requires exactly one ' + name)
        return matches[0]

    creation = one('vkCreateGraphicsPipelines')
    args = creation['function']['args']
    if (creation['function']['return'] != 'VK_SUCCESS' or args['createInfoCount'] != 1
            or args['pPipelines'] != [bound_pipeline] or len(args['pCreateInfos']) != 1):
        raise ValueError('bound pipeline does not match successful creation')
    bound = one('vkCmdBindPipeline')
    if creation['index'] >= bound['index'] or bound['function']['args']['pipelineBindPoint'] != 'VK_PIPELINE_BIND_POINT_GRAPHICS':
        raise ValueError('pipeline was not created before graphics binding')
    pipeline = args['pCreateInfos'][0]
    layout = one('vkCreatePipelineLayout')['function']['args']
    if pipeline['layout'] != bound_layout or layout['pPipelineLayout'] != bound_layout:
        raise ValueError('pipeline/bound descriptor layout mismatch')
    layout_info = layout['pCreateInfo']
    descriptor_layout = check_layout(calls, bound_layout, descriptor_type, multi)
    render_pass = one('vkCmdBeginRenderPass')['function']['args']['pRenderPassBegin']['renderPass']
    if pipeline['renderPass'] != render_pass or pipeline['subpass'] != 0:
        raise ValueError('pipeline render pass does not match the draw')
    stages = pipeline['pStages']
    expected_stages = {'VK_SHADER_STAGE_VERTEX_BIT': 'vert', 'VK_SHADER_STAGE_FRAGMENT_BIT': 'frag'}
    if pipeline['stageCount'] != 2 or len(stages) != 2 or {s['stage'] for s in stages} != set(expected_stages):
        raise ValueError('unexpected widget shader stages')
    modules = []
    for stage in stages:
        matches = [call for call in calls if call.get('function', {}).get('name') == 'vkCreateShaderModule'
                   and call['function']['args']['pShaderModule'] == stage['module']]
        if len(matches) != 1 or matches[0]['index'] >= creation['index']:
            raise ValueError('pipeline shader module creation is missing or ambiguous')
        module = matches[0]
        info = module['function']['args']['pCreateInfo']
        if (module['function']['return'] != 'VK_SUCCESS' or stage['pName'] != 'main'
                or stage['pSpecializationInfo'] is not None):
            raise ValueError('unexpected shader module/entry point/specialization')
        binary = local / info['pCode']
        if not binary.resolve().is_relative_to(local.resolve()):
            raise ValueError('shader binary path escapes capture output')
        data = binary.read_bytes()
        expected = (reference / (('widget-multi.' if multi else 'widget.') + expected_stages[stage['stage']] + '.spv')).read_bytes()
        if len(data) != info['codeSize'] or data != expected:
            raise ValueError('captured shader differs from the probe build snapshot')
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(binary)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        assembly = subprocess.check_output(['spirv-dis', str(binary)], text=True, timeout=10)
        disassembly = binary.with_suffix(binary.suffix + '.spvasm')
        disassembly.write_text(assembly)
        modules.append({'stage': stage['stage'], 'entry_point': stage['pName'],
                        'module': stage['module'], 'creation_index': module['index'],
                        'code_size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                        'binary': str(binary.relative_to(local)),
                        'disassembly': str(disassembly.relative_to(local)),
                        'interface_decorations': [line.strip() for line in assembly.splitlines()
                                                  if 'OpDecorate ' in line or 'OpMemberDecorate ' in line]})
    result = {'pipeline': bound_pipeline, 'creation_index': creation['index'], 'layout': bound_layout,
              'set_layout': layout_info['pSetLayouts'][0], 'descriptor_layout': descriptor_layout, 'render_pass': render_pass,
              'pipeline_cache': args['pipelineCache'], 'shaders': modules,
              'scope': 'API-input SPIR-V; no driver-transformed shader or internal cache key'}
    (local / 'pipeline.json').write_text(json.dumps(result, indent=2) + '\n')
    return result
