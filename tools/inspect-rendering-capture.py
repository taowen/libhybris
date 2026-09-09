#!/usr/bin/env python3
"""Inspect dynamic-rendering submission sequences in GFXReconstruct JSONL."""
import argparse
import collections
import copy
import hashlib
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def registry_commands(path):
    commands = {}
    aliases = {}
    for command in ET.parse(path).findall('./commands/command'):
        if command.get('alias'):
            aliases[command.get('name')] = command.get('alias')
        else:
            name = command.findtext('proto/name')
            commands[name] = set(command.get('tasks', '').split(',')) - {''}
    for alias, original in aliases.items():
        seen = {alias}
        while original in aliases and original not in seen:
            seen.add(original)
            original = aliases[original]
        commands[alias] = commands.get(original, set())
    return commands


def rendering_key(info):
    result = copy.deepcopy(info)
    # The specification permits these three bits to differ on resume.
    result['flags'] = number(result['flags']) & ~7
    return result


def inspect_batch(submit_index, batch_index, recordings, commands, views, images):
    result = {'queue_submit_index': submit_index, 'batch': batch_index,
              'recordings': [], 'renderings': [], 'findings': [], 'uncovered': []}
    active = None
    suspended = None
    intervening = []
    pipeline = None
    descriptor_binds = {}
    vertex_binds = {}
    index_bind = None
    push_constants = []

    def finding(kind, index, **detail):
        result['findings'].append({'kind': kind, 'index': index, **detail})

    for handle, recording in recordings:
        if recording is None or recording['end'] is None:
            result['uncovered'].append({'command_buffer': handle,
                                        'reason': 'complete recording is unavailable'})
            # Unknown commands could have resumed or suspended rendering.
            active = suspended = None
            intervening = []
            continue
        result['recordings'].append({'command_buffer': handle,
                                     'begin_index': recording['begin'],
                                     'end_index': recording['end']})
        # Binding state does not carry between primary command buffers.
        pipeline, descriptor_binds, vertex_binds, index_bind, push_constants = None, {}, {}, None, []
        for index, name, args in recording['commands']:
            if name in ('vkCmdBeginRendering', 'vkCmdBeginRenderingKHR'):
                info = args['pRenderingInfo']
                flags = number(info['flags'])
                scope = {'begin_index': index, 'command_buffer': handle,
                         'flags': flags, 'rendering_info': info, 'attachments': [], 'draws': []}
                for attachment in (info.get('pColorAttachments') or []) + [
                        info.get('pDepthAttachment'), info.get('pStencilAttachment')]:
                    if not attachment or not attachment.get('imageView'):
                        continue
                    view_handle = attachment['imageView']
                    view = views.get(view_handle)
                    image_handle = view['info']['image'] if view else None
                    scope['attachments'].append({'view': view_handle, 'image': image_handle,
                                                  'view_creation': view,
                                                  'image_creation': images.get(image_handle)})
                result['renderings'].append(scope)
                if active:
                    finding('begin_while_rendering', index, previous_begin=active['begin_index'])
                if flags & 4:
                    if suspended is None:
                        finding('resume_without_suspend_in_known_commands', index)
                    else:
                        if intervening:
                            finding('commands_between_suspend_and_resume', index,
                                    suspended_begin=suspended['begin_index'],
                                    suspended_end=suspended['end_index'], commands=intervening)
                        if rendering_key(suspended['rendering_info']) != rendering_key(info):
                            finding('resume_parameters_differ', index,
                                    suspended_begin=suspended['begin_index'])
                elif suspended is not None:
                    finding('rendering_before_suspended_scope_resumes', index,
                            suspended_begin=suspended['begin_index'], commands=intervening)
                active, suspended, intervening = scope, None, []
            elif name in ('vkCmdEndRendering', 'vkCmdEndRenderingKHR'):
                if active is None:
                    finding('end_without_begin_in_known_commands', index)
                else:
                    active['end_index'] = index
                    suspended = active if active['flags'] & 2 else None
                active = None
            else:
                tasks = commands.get(name)
                if tasks is None:
                    result['uncovered'].append({'index': index, 'command': name,
                                                'reason': 'command absent from supplied registry'})
                elif suspended and tasks & {'action', 'synchronization'}:
                    intervening.append({'index': index, 'command': name, 'tasks': sorted(tasks)})
                if name == 'vkCmdExecuteCommands':
                    result['uncovered'].append({'index': index, 'command': name,
                                                'reason': 'secondary command buffers are not expanded'})
                if name.startswith('vkCmdBind') and name not in (
                        'vkCmdBindPipeline', 'vkCmdBindDescriptorSets',
                        'vkCmdBindVertexBuffers', 'vkCmdBindIndexBuffer'):
                    result['uncovered'].append({'index': index, 'command': name,
                                                'reason': 'binding command is not reconstructed'})
                    pipeline, descriptor_binds, vertex_binds, index_bind = None, {}, {}, None
                if name == 'vkCmdBindPipeline' and args['pipelineBindPoint'] == 'VK_PIPELINE_BIND_POINT_GRAPHICS':
                    pipeline = {'index': index, 'pipeline': args['pipeline']}
                elif name == 'vkCmdBindDescriptorSets' and args['pipelineBindPoint'] == 'VK_PIPELINE_BIND_POINT_GRAPHICS':
                    for offset, descriptor in enumerate(args['pDescriptorSets'] or []):
                        descriptor_binds[args['firstSet'] + offset] = {
                            'index': index, 'set': descriptor, 'layout': args['layout'],
                            'dynamic_offsets': args['pDynamicOffsets']}
                elif name == 'vkCmdBindVertexBuffers':
                    for offset, buffer in enumerate(args['pBuffers'] or []):
                        vertex_binds[args['firstBinding'] + offset] = {
                            'index': index, 'buffer': buffer, 'offset': args['pOffsets'][offset]}
                elif name == 'vkCmdBindIndexBuffer':
                    index_bind = {'index': index, **args}
                elif name == 'vkCmdPushConstants':
                    push_constants.append({'index': index, 'layout': args['layout'],
                                           'offset': args['offset'], 'size': args['size'],
                                           'stage_flags': args['stageFlags']})
                if active and name.startswith('vkCmdDraw'):
                    active['draws'].append({'index': index, 'command': name, 'args': args,
                                           'pipeline_bind': copy.deepcopy(pipeline),
                                           'descriptor_binds': copy.deepcopy(descriptor_binds),
                                           'vertex_binds': copy.deepcopy(vertex_binds),
                                           'index_bind': copy.deepcopy(index_bind),
                                           'push_constant_writes': list(push_constants)})
        if active:
            finding('rendering_not_ended_in_command_buffer', recording['end'],
                    begin_index=active['begin_index'])
            active = None
    if suspended:
        finding('suspended_scope_not_resumed_in_known_batch', submit_index,
                suspended_begin=suspended['begin_index'], commands=intervening)
    return result


def inspect(path, registry):
    commands = registry_commands(registry)
    recordings, pools, images, views = {}, {}, {}, {}
    result = {'input': str(path), 'input_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
              'registry_sha256': hashlib.sha256(registry.read_bytes()).hexdigest(),
              'submissions': [], 'uncovered': [],
              'scope': 'dynamic-rendering command order and binding call references; not Vulkan validation or a pixel verdict'}
    previous_index = -1
    for line_number, line in enumerate(path.open(), 1):
        if not line.strip():
            continue
        call = json.loads(line)
        if 'index' not in call:
            continue
        index = call['index']
        if index <= previous_index:
            raise ValueError(f'line {line_number}: call indices must increase')
        previous_index = index
        function = call.get('function', {})
        name, args = function.get('name'), function.get('args', {})
        successful = function.get('return', 'VK_SUCCESS') in ('VK_SUCCESS', 0)
        if name == 'vkCreateImage' and successful:
            images[args['pImage']] = {'index': index, 'info': args['pCreateInfo']}
        elif name == 'vkCreateImageView' and successful:
            views[args['pView']] = {'index': index, 'info': args['pCreateInfo']}
        elif name == 'vkDestroyImage':
            images.pop(args['image'], None)
        elif name == 'vkDestroyImageView':
            views.pop(args['imageView'], None)
        elif name == 'vkAllocateCommandBuffers' and successful:
            for handle in args['pCommandBuffers']:
                pools[handle] = args['pAllocateInfo']['commandPool']
        elif name in ('vkResetCommandPool', 'vkDestroyCommandPool') and successful:
            for handle in list(pools):
                if pools[handle] == args['commandPool']:
                    recordings.pop(handle, None)
                    if name == 'vkDestroyCommandPool':
                        pools.pop(handle)
        elif name == 'vkFreeCommandBuffers':
            for handle in args['pCommandBuffers']:
                recordings.pop(handle, None)
                pools.pop(handle, None)
        elif name == 'vkResetCommandBuffer' and successful:
            recordings.pop(args['commandBuffer'], None)
        elif name == 'vkBeginCommandBuffer' and successful:
            recordings[args['commandBuffer']] = {'begin': index, 'end': None, 'commands': []}
        elif name == 'vkEndCommandBuffer' and successful:
            if args['commandBuffer'] in recordings:
                recordings[args['commandBuffer']]['end'] = index
        elif name and name.startswith('vkCmd'):
            recording = recordings.get(args.get('commandBuffer'))
            if recording is not None:
                recording['commands'].append((index, name, args))
            else:
                result['uncovered'].append({'index': index, 'command': name,
                                            'reason': 'recording start is unavailable'})
        elif name in ('vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR'):
            for batch_index, batch in enumerate(args.get('pSubmits') or []):
                handles = (batch.get('pCommandBuffers') or []) if name == 'vkQueueSubmit' else [
                    item['commandBuffer'] for item in batch.get('pCommandBufferInfos') or []]
                report = inspect_batch(index, batch_index, [(h, recordings.get(h)) for h in handles],
                                       commands, views, images)
                report['queue'] = args['queue']
                report['submit_result'] = function.get('return')
                result['submissions'].append(report)
    if not result['submissions']:
        raise ValueError('capture contains no submission batch to inspect')
    for batch in result['submissions']:
        batch['complete_primary_recordings'] = not batch['uncovered']
        # Missing/secondary recordings may contain the matching begin/end.
        # Preserve observations without presenting them as conclusive errors.
        for item in batch['findings']:
            item['evidence'] = ('complete_primary_recordings' if batch['complete_primary_recordings']
                                else 'partial_command_stream')
    counts = collections.Counter(f['kind'] for batch in result['submissions'] for f in batch['findings'])
    result['finding_counts'] = dict(counts)
    result['uncovered_count'] = len(result['uncovered']) + sum(
        len(batch['uncovered']) for batch in result['submissions'])
    result['status'] = 'FINDINGS' if counts else 'NO_FINDINGS_WITHIN_SCOPE'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='gfxrecon-convert --format jsonl output')
    parser.add_argument('--registry', required=True, type=Path, help='pinned Vulkan-Headers registry/vk.xml')
    parser.add_argument('--output', type=Path, help='write JSON here; default is stdout')
    args = parser.parse_args()
    try:
        report = inspect(args.capture, args.registry)
        data = json.dumps(report, indent=2) + '\n'
        if args.output:
            args.output.write_text(data)
            print(json.dumps({key: report[key] for key in ('status', 'finding_counts', 'uncovered_count')}))
        else:
            print(data, end='')
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        print(f'capture inspection failed: {error}', file=sys.stderr)
        return 2
    return 1 if report['finding_counts'] else 0


if __name__ == '__main__':
    sys.exit(main())
