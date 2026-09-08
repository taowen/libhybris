"""Locate CPU-upload bytes copied into indirect buffers; do not simulate GPU writes."""
import hashlib
import json
import struct


def upload_evidence(path, directory):
    bindings, command_buffers, uploads, fills, records = {}, {}, [], {}, []
    for line in path.open():
        call = json.loads(line)
        meta = call.get('meta', {})
        if meta.get('name') == 'FillMemoryCommand':
            uploads.append((call['index'], meta['args']))
        function = call.get('function', {})
        name, args = function.get('name', ''), function.get('args', {})
        index = call.get('index')
        if name == 'vkBindBufferMemory' and function.get('return') == 'VK_SUCCESS':
            bindings[args['buffer']] = (args['memory'], args['memoryOffset'])
        elif name in ('vkBindBufferMemory2', 'vkBindBufferMemory2KHR') and function.get('return') == 'VK_SUCCESS':
            for binding in args['pBindInfos']:
                bindings[binding['buffer']] = (binding['memory'], binding['memoryOffset'])
        elif name == 'vkBeginCommandBuffer' and function.get('return') == 'VK_SUCCESS':
            command_buffers[args['commandBuffer']] = []
        elif name.startswith('vkCmd'):
            command_buffers[args['commandBuffer']].append(call)
        elif name in ('vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR') and function.get('return') == 'VK_SUCCESS':
            prefix = []
            for submit in args.get('pSubmits') or []:
                handles = submit.get('pCommandBuffers') or [b['commandBuffer'] for b in submit.get('pCommandBufferInfos') or []]
                for handle in handles:
                    for command in command_buffers.get(handle, []):
                        f, command_index = command['function'], command['index']
                        a, n = f['args'], f['name']
                        if n == 'vkCmdFillBuffer':
                            fills[(args['queue'], a['dstBuffer'])] = (command_index, index, a)
                        if n.startswith(('vkCmdDrawIndirectCount', 'vkCmdDrawIndexedIndirectCount')):
                            record = {'draw_index': command_index, 'submit_index': index,
                                      'scope': 'CPU upload and recorded copy provenance; GPU argument contents are not observed'}
                            try:
                                record.update(decode_upload(a, n, prefix, bindings, uploads, directory))
                                fill = fills.get((args['queue'], a['countBuffer']))
                                if fill:
                                    fill_index, submit_index, fill_args = fill
                                    if fill_args['dstOffset'] <= a['countBufferOffset'] and fill_args['dstOffset'] + fill_args['size'] >= a['countBufferOffset'] + 4:
                                        record['last_submitted_count_fill'] = {
                                            'call_index': fill_index, 'submit_index': submit_index, 'value': fill_args['data'],
                                            'note': 'initialization only; subsequent shader or aliased writes are not excluded'}
                                record['status'] = 'FOUND'
                            except ValueError as error:
                                record.update(status='UNAVAILABLE', reason=str(error))
                            records.append(record)
                        prefix.append(command)
    return records


def decode_upload(draw, name, prefix, bindings, uploads, directory):
    indexed = name.startswith('vkCmdDrawIndexed')
    layout = '<IIIiI' if indexed else '<IIII'
    width = struct.calcsize(layout)
    count, stride = draw['maxDrawCount'], draw['stride']
    if not count:
        raise ValueError('zero maximum draw count has no argument fetch')
    extent = (count - 1) * stride + width
    copies = []
    for command in prefix:
        f = command['function']
        if f['name'] != 'vkCmdCopyBuffer':
            continue
        a = f['args']
        if a['dstBuffer'] != draw['buffer']:
            continue
        for region in a['pRegions']:
            if region['dstOffset'] < draw['offset'] + extent and draw['offset'] < region['dstOffset'] + region['size']:
                copies.append((command['index'], a, region))
    if not copies:
        raise ValueError('no preceding buffer copy in this submit covers the indirect arguments')
    copy_index, copy, region = copies[-1]
    if region['dstOffset'] > draw['offset'] or region['dstOffset'] + region['size'] < draw['offset'] + extent:
        raise ValueError('last overlapping copy does not cover the complete argument range')
    if copy['srcBuffer'] not in bindings:
        raise ValueError('source buffer memory binding was not recorded')
    memory, memory_offset = bindings[copy['srcBuffer']]
    offset = memory_offset + region['srcOffset'] + draw['offset'] - region['dstOffset']
    matches = [(i, a) for i, a in uploads if a['memory_id'] == memory and a['offset'] < offset + extent and offset < a['offset'] + a['size']]
    if not matches:
        raise ValueError('source memory has no captured CPU upload')
    upload_index, upload = matches[-1]
    if upload['offset'] > offset or upload['offset'] + upload['size'] < offset + extent:
        raise ValueError('last overlapping upload does not cover the complete argument range')
    data = (directory / upload['data']).read_bytes()
    if len(data) != upload['size']:
        raise ValueError('captured memory binary size differs from its metadata')
    start = offset - upload['offset']
    payload = data[start:start + extent]
    keys = ('indexCount', 'instanceCount', 'firstIndex', 'vertexOffset', 'firstInstance') if indexed else (
        'vertexCount', 'instanceCount', 'firstVertex', 'firstInstance')
    commands = [dict(zip(keys, struct.unpack_from(layout, payload, i * stride))) for i in range(count)]
    return {'max_draw_count': count, 'stride': stride, 'candidate_commands': commands,
            'copy_index': copy_index, 'source_buffer': copy['srcBuffer'],
            'source_memory': memory, 'source_memory_offset': offset,
            'upload_index': upload_index, 'upload_file': upload['data'],
            'argument_range_sha256': hashlib.sha256(payload).hexdigest()}
