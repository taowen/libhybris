"""Associate this fixed GL probe's readbacks with captured submissions and files."""
import json
import re


def readback_request(out, memory):
    names = ['image.rgba']
    prefixes = {'VERTEX_PREPASS': 'vertex-prepass', 'ATTRIBUTE_VERTEX': 'attributes',
                'MULTIDRAW_VERTEX': 'multidraw', 'INDEXED_VERTEX': 'indexed',
                'RESOURCE_VERTEX': 'resources', 'PROCEDURAL_VERTEX': 'procedural'}
    for line in (out / 'probe.log').read_text().splitlines():
        if line.startswith('PACKED_DRAW '):
            names.append(None)  # The original probe checks, but does not save, these pixels.
        match = re.match(r'([A-Z_]+) phase=(\d+) (PASS|FAIL) ', line)
        if match and match[1] in prefixes:
            names.append(f'{prefixes[match[1]]}-{match[2]}.rgba')
    buffers, images, readbacks = {}, {}, []
    for line in (out / 'capture/calls.jsonl').open():
        call = json.loads(line)
        function = call.get('function', {})
        name, args = function.get('name'), function.get('args', {})
        if memory == 'rebind' and name in ('vkGetDescriptorEXT', 'vkCmdBindDescriptorBuffersEXT'):
            raise ValueError('pinned tool does not support descriptor-buffer rebind replay; use a separate lazy capture')
        if name == 'vkCreateImage' and function.get('return') == 'VK_SUCCESS':
            images[args['pImage']] = args['pCreateInfo']['format']
        if name == 'vkBeginCommandBuffer' and function.get('return') == 'VK_SUCCESS':
            buffers[args['commandBuffer']] = (call['index'], [])
        if name == 'vkCmdCopyImageToBuffer':
            buffers[args['commandBuffer']][1].append((call['index'], args))
        if name in ('vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR') and function.get('return') == 'VK_SUCCESS':
            for submit in args.get('pSubmits') or []:
                handles = submit.get('pCommandBuffers') or [b['commandBuffer'] for b in submit.get('pCommandBufferInfos') or []]
                for handle in handles:
                    begin, copies = buffers.get(handle, (None, []))
                    for index, copy in copies:
                        if len(copy['pRegions']) != 1:
                            raise ValueError('expected one region per GL readback')
                        region = copy['pRegions'][0]
                        if (region['bufferOffset'] or region['bufferRowLength'] or region['bufferImageHeight'] or
                                region['imageExtent'] != {'width': 16, 'height': 16, 'depth': 1} or
                                region['imageOffset'] != {'x': 0, 'y': 0, 'z': 0} or
                                region['imageSubresource'] != {'aspectMask': '0x00000001', 'mipLevel': 0,
                                                             'baseArrayLayer': 0, 'layerCount': 1}):
                            raise ValueError('readback is outside the fixed tightly packed 16x16 color fixture')
                        readbacks.append({'begin': begin, 'copy': index, 'submit': call['index'],
                                          'image': copy['srcImage'], 'buffer': copy['dstBuffer'],
                                          'format': images.get(copy['srcImage'])})
    if len(names) != len(readbacks):
        raise ValueError('captured readback count differs from the GL fixture log')
    for record, name in zip(readbacks, names):
        record['reference'] = name
        if name and (not (out / name).is_file() or (out / name).stat().st_size != 1024):
            raise ValueError('missing or incomplete GL reference image: ' + name)
    request = {'BeginCommandBuffer': [r['begin'] for r in readbacks],
               'Transfer': [[r['copy']] for r in readbacks],
               'QueueSubmit': [r['submit'] for r in readbacks],
               'DumpResourcesOptions': {'DumpRawImages': True}}
    return request, readbacks


def compare_readbacks(out, readbacks, sha):
    evidence = out / 'capture'
    reports = list((evidence / 'replay').glob('*_dr.json'))
    if len(reports) != 1:
        raise ValueError('expected one replay resource report')
    rows = {}
    for entry in json.loads(reports[0].read_text()):
        for row in entry.get('transferCommands') or []:
            if row is None:  # The pinned JSON writer emits cumulative arrays with null slots.
                continue
            key = (row['beginCommandBufferIndex'], row['cmdIndex'], row['queueSubmitIndex'])
            if key in rows and rows[key] != row:
                raise ValueError('conflicting replay records for the same submission')
            rows[key] = row
    expected = {(r['begin'], r['copy'], r['submit']) for r in readbacks}
    if set(rows) != expected or len(expected) != len(readbacks):
        raise ValueError('replay readback/submission set differs from the request')
    for record in readbacks:
        row = rows[(record['begin'], record['copy'], record['submit'])]
        params = row['parameters']
        if (row['cmdType'] != 'vkCmdCopyImageToBuffer' or params['srcImage']['imageId'] != record['image'] or
                params['srcImage']['format'] != record['format'] or params['dstBuffer'] != record['buffer'] or
                len(params['regions']) != 1 or params['regions'][0]['size'] != 1024):
            raise ValueError('replay resource association or size mismatch')
        path = (evidence / params['regions'][0]['file']).resolve()
        if not path.is_relative_to((evidence / 'replay').resolve()):
            raise ValueError('replay resource path escaped its output directory')
        data = path.read_bytes()
        if len(data) != 1024 or record['format'] not in ('VK_FORMAT_R8G8B8A8_UNORM', 'VK_FORMAT_B8G8R8A8_UNORM'):
            raise ValueError('unexpected GL replay image layout')
        record['replay_sha256'] = sha(path)
        if record['format'] == 'VK_FORMAT_B8G8R8A8_UNORM':
            data = b''.join(data[i + 2:i + 3] + data[i + 1:i + 2] + data[i:i + 1] + data[i + 3:i + 4]
                            for i in range(0, len(data), 4))
        name = record['reference']
        record['comparison'] = 'NOT_SAVED_BY_PROBE' if name is None else (
            'MATCH' if data == (out / name).read_bytes() else 'MISMATCH')
    return readbacks
