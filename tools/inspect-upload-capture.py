#!/usr/bin/env python3
"""Report mapped upload buffers with no observed flush in a GFXReconstruct JSONL.

This is capture evidence, not a Vulkan validator or a reconstruction of CPU
write times. FillMemoryCommand records a snapshot, which may lag the write.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path

WHOLE = (1 << 64) - 1


def integer(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def uncovered(begin, end, ranges):
    cursor = begin
    for low, high in sorted(ranges):
        if high <= cursor:
            continue
        if low > cursor:
            return True
        cursor = max(cursor, high)
        if cursor >= end:
            return False
    return cursor < end


def inspect(path):
    physical = {}
    devices = {}
    memories = {}
    buffers = {}
    commands = {}
    findings = {}
    examined = set()
    counted_submissions = set()
    limitations = set()
    counts = collections.Counter()

    def consume(command, handle, kind, call):
        buffer = buffers.get(handle)
        if buffer and buffer['usage'] == (0x82 if kind == 'immediate_vertex' else 1):
            commands.setdefault(command, []).append((buffer, kind, call))

    def submit(command, call, seen=None):
        seen = set() if seen is None else seen
        if command in seen:
            limitations.add('cyclic command-buffer execution reference')
            return
        for entry in commands.get(command, []):
            if entry[0] == 'secondary':
                submit(entry[1], call, seen | {command})
                continue
            buffer, kind, use = entry
            memory = buffer.get('binding')
            if not memory or not memory['mapped']:
                continue
            flags = memory['flags']
            if flags is None:
                limitations.add('some allocations have no captured physical memory properties')
                continue
            if not flags & 2 or flags & 4:
                continue
            low, high = buffer['offset'], buffer['offset'] + buffer['size']
            writes = [(max(low, a), min(high, b), index) for a, b, index in memory['writes']
                      if a < high and b > low]
            if not writes:
                continue
            key = (buffer['created'], kind)
            examined.add(key)
            missing = [(a, b, index) for a, b, index in writes
                       if uncovered(a, b, memory['flushes'])]
            if not missing:
                continue
            if key not in findings:
                findings[key] = {
                    'buffer': buffer['handle'], 'buffer_create_call': buffer['created'],
                    'usage': hex(buffer['usage']), 'size': buffer['size'], 'kind': kind,
                    'memory': memory['handle'], 'memory_allocate_call': memory['created'],
                    'memory_type_index': memory['type'], 'memory_flags': hex(flags),
                    'binding_offset': low, 'first_consumer_call': use,
                    'first_submit_call': call, 'submissions_with_uncovered_snapshots': 0,
                    'uncovered_snapshots': [{'offset': a, 'size': b - a, 'capture_call': index}
                                            for a, b, index in missing[:8]],
                }
            if (key, call) not in counted_submissions:
                findings[key]['submissions_with_uncovered_snapshots'] += 1
                counted_submissions.add((key, call))

    with path.open() as source:
        for line in source:
            row = json.loads(line)
            index = row.get('index')
            meta = row.get('meta', {})
            if meta.get('name') == 'FillMemoryCommand':
                args = meta['args']
                memory = memories.get(args['memory_id'])
                if memory:
                    begin = integer(args['offset'])
                    memory['writes'].append((begin, begin + integer(args['size']), index))
                continue
            function = row.get('function', {})
            name, args = function.get('name'), function.get('args', {})
            if function.get('return', 'VK_SUCCESS') != 'VK_SUCCESS':
                continue
            if name == 'vkCreateDevice':
                devices[args['pDevice']] = args['physicalDevice']
            elif name in ('vkGetPhysicalDeviceMemoryProperties', 'vkGetPhysicalDeviceMemoryProperties2',
                          'vkGetPhysicalDeviceMemoryProperties2KHR'):
                props = args['pMemoryProperties']
                physical[args['physicalDevice']] = props.get('memoryProperties', props)
            elif name == 'vkAllocateMemory':
                info = args['pAllocateInfo']
                props = physical.get(devices.get(args['device']), {})
                types = props.get('memoryTypes', [])
                slot = integer(info['memoryTypeIndex'])
                memories[args['pMemory']] = {
                    'handle': args['pMemory'], 'created': index, 'type': slot,
                    'flags': integer(types[slot]['propertyFlags']) if slot < len(types) else None,
                    'size': integer(info['allocationSize']), 'mapped': False,
                    'writes': [], 'flushes': [],
                }
            elif name == 'vkFreeMemory':
                memory = memories.pop(args['memory'], None)
                if memory:
                    memory['mapped'] = False
            elif name in ('vkMapMemory', 'vkUnmapMemory'):
                memory = memories.get(args['memory'])
                if memory:
                    memory['mapped'] = name == 'vkMapMemory'
            elif name == 'vkCreateBuffer':
                info = args['pCreateInfo']
                buffers[args['pBuffer']] = {
                    'handle': args['pBuffer'], 'created': index,
                    'size': integer(info['size']), 'usage': integer(info['usage']),
                }
            elif name == 'vkDestroyBuffer':
                buffers.pop(args['buffer'], None)
            elif name in ('vkBindBufferMemory', 'vkBindBufferMemory2', 'vkBindBufferMemory2KHR'):
                for binding in args.get('pBindInfos', [args]):
                    buffer = buffers.get(binding['buffer'])
                    if buffer:
                        buffer['binding'] = memories.get(binding['memory'])
                        buffer['offset'] = integer(binding['memoryOffset'])
            elif name == 'vkFlushMappedMemoryRanges':
                for span in args['pMemoryRanges']:
                    memory = memories.get(span['memory'])
                    if memory:
                        begin, size = integer(span['offset']), integer(span['size'])
                        end = memory['size'] if size == WHOLE else begin + size
                        memory['flushes'].append((begin, end))
            elif name in ('vkBeginCommandBuffer', 'vkResetCommandBuffer'):
                commands[args['commandBuffer']] = []
            elif name == 'vkFreeCommandBuffers':
                for command in args['pCommandBuffers']:
                    commands.pop(command, None)
            elif name == 'vkCmdExecuteCommands':
                for command in args['pCommandBuffers']:
                    commands.setdefault(args['commandBuffer'], []).append(('secondary', command))
            elif name in ('vkCmdCopyBufferToImage', 'vkCmdCopyBufferToImage2', 'vkCmdCopyBufferToImage2KHR'):
                info = args.get('pCopyBufferToImageInfo', args)
                consume(args['commandBuffer'], info['srcBuffer'], 'texture_staging', index)
            elif name in ('vkCmdCopyBuffer', 'vkCmdCopyBuffer2', 'vkCmdCopyBuffer2KHR'):
                info = args.get('pCopyBufferInfo', args)
                consume(args['commandBuffer'], info['srcBuffer'], 'buffer_staging', index)
            elif name in ('vkCmdBindVertexBuffers', 'vkCmdBindVertexBuffers2', 'vkCmdBindVertexBuffers2EXT'):
                for buffer in args['pBuffers']:
                    consume(args['commandBuffer'], buffer, 'immediate_vertex', index)
            elif name in ('vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR'):
                counts['successful_submit_calls'] += 1
                for info in args['pSubmits'] or []:
                    commands_to_submit = info.get('pCommandBuffers') or [
                        entry['commandBuffer'] for entry in info.get('pCommandBufferInfos') or []]
                    for command in commands_to_submit:
                        submit(command, index)
    return {
        'source': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'status': 'CAPTURE_OBSERVATIONS',
        'scope': 'pure transfer sources and Blender immediate vertex usage (0x82)',
        'examined_host_written_noncoherent_buffers': len(examined),
        'counts': dict(counts), 'buffers_with_uncovered_snapshots': list(findings.values()),
        'limitations': sorted(limitations) + [
            'FillMemoryCommand is a captured snapshot, not an exact CPU write timestamp.',
            'Only absence of any covering flush before submission is reported; later unflushed rewrites can be missed.',
            'Buffer-wide observations do not prove which bytes a shader or copy actually consumes.',
            'Capture layers cannot observe flushes added below the layer by the ICD.',
            'Sparse bindings, map-memory2, external/alias writes and all lifetime/synchronization rules are not modeled.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = inspect(args.capture)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"{len(result['buffers_with_uncovered_snapshots'])} buffers with uncovered snapshots; {args.output}")


if __name__ == '__main__':
    main()
