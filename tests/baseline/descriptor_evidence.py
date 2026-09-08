"""Reconstruct ordinary/dynamic buffer descriptors for the fixed widget draw.

One allocation, one update and one bind are required. This does not reconstruct
arbitrary descriptor history, templates, copied descriptors or handle reuse.
"""
import hashlib
import struct


def one(calls, name):
    matches = [call for call in calls if call.get('function', {}).get('name') == name]
    if len(matches) != 1:
        raise ValueError('descriptor fixture requires exactly one ' + name)
    return matches[0]


def check_layout(calls, bound_layout, descriptor_type, multi=False):
    layout_call = one(calls, 'vkCreatePipelineLayout')
    layout = layout_call['function']['args']
    info = layout['pCreateInfo']
    bind_call = one(calls, 'vkCmdBindDescriptorSets')
    bind = bind_call['function']['args']
    allocation_call = one(calls, 'vkAllocateDescriptorSets')
    allocation = allocation_call['function']['args']
    count = 2 if multi else 1
    if (layout_call['function']['return'] != 'VK_SUCCESS' or layout['pPipelineLayout'] != bound_layout
            or info['setLayoutCount'] != count or len(info['pSetLayouts']) != count
            or info['pushConstantRangeCount'] != 0 or bind['layout'] != bound_layout
            or bind['pipelineBindPoint'] != 'VK_PIPELINE_BIND_POINT_GRAPHICS'
            or bind['firstSet'] != 0 or bind['descriptorSetCount'] != count
            or allocation_call['function']['return'] != 'VK_SUCCESS'
            or allocation['pAllocateInfo']['descriptorSetCount'] != count
            or allocation['pAllocateInfo']['pSetLayouts'] != info['pSetLayouts']
            or allocation['pDescriptorSets'] != bind['pDescriptorSets']
            or len(bind['pDescriptorSets']) != count
            or not layout_call['index'] < allocation_call['index'] < bind_call['index']):
        raise ValueError('draw descriptor allocation/pipeline layout mismatch')
    layouts = [call for call in calls if call.get('function', {}).get('name') == 'vkCreateDescriptorSetLayout']
    if len(layouts) != count:
        raise ValueError('unexpected descriptor set layout count')
    entries = []
    for set_index, handle in enumerate(info['pSetLayouts']):
        matches = [call for call in layouts if call['function']['args']['pSetLayout'] == handle]
        if len(matches) != 1 or matches[0]['index'] >= layout_call['index']:
            raise ValueError('descriptor set layout creation is missing or ambiguous')
        call = matches[0]
        declared = call['function']['args']['pCreateInfo']
        bindings = declared['pBindings']
        expected = [(0, 1), (3, 2)] if multi and set_index == 0 else [(1 if multi else 0, 1)]
        if (call['function']['return'] != 'VK_SUCCESS' or declared['bindingCount'] != len(expected)
                or sorted((b['binding'], b['descriptorCount']) for b in bindings) != expected):
            raise ValueError('unexpected widget descriptor interface')
        for binding in sorted(bindings, key=lambda b: b['binding']):
            if binding['descriptorType'] != descriptor_type or int(binding['stageFlags'], 16) != 0x11:
                raise ValueError('shader descriptor type/stages mismatch')
            for element in range(binding['descriptorCount']):
                entries.append({'set': set_index, 'binding': binding['binding'], 'array_index': element,
                                'set_handle': bind['pDescriptorSets'][set_index], 'set_layout': handle})
    return entries


def check_descriptors(calls, draw, binding, read, begin, draw_index, submit, dynamic=False, multi=False):
    bind_call = one(calls, 'vkCmdBindDescriptorSets')
    pipeline = one(calls, 'vkCmdBindPipeline')
    update_call = one(calls, 'vkUpdateDescriptorSets')
    allocation = one(calls, 'vkAllocateDescriptorSets')
    bind = bind_call['function']['args']
    update = update_call['function']['args']
    if not allocation['index'] < update_call['index'] < begin < pipeline['index'] < bind_call['index'] < draw_index < submit:
        raise ValueError('unexpected fixture binding order')
    descriptor_type = 'VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC' if dynamic else 'VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER'
    entries = check_layout(calls, bind['layout'], descriptor_type, multi)
    offsets = bind.get('pDynamicOffsets') or []
    expected_offsets = len(entries) if dynamic else 0
    if bind['dynamicOffsetCount'] != expected_offsets or len(offsets) != expected_offsets:
        raise ValueError('dynamic offset payload does not match layout')
    writes = update['pDescriptorWrites']
    if (update['descriptorWriteCount'] != len(writes) or len(writes) != (3 if multi else 1)
            or update['descriptorCopyCount'] != 0):
        raise ValueError('unexpected fixture descriptor updates')
    values = {}
    for write in writes:
        buffers = write['pBufferInfo']
        if write['descriptorType'] != descriptor_type or write['descriptorCount'] != len(buffers):
            raise ValueError('unexpected descriptor update type/count')
        for element, info in enumerate(buffers, write['dstArrayElement']):
            key = (write['dstSet'], write['dstBinding'], element)
            if key in values:
                raise ValueError('ambiguous descriptor update')
            values[key] = info
    if len(values) != len(entries):
        raise ValueError('updated descriptor count does not match the layout')
    result = []
    for index, entry in enumerate(entries):
        key = (entry['set_handle'], entry['binding'], entry['array_index'])
        if key not in values:
            raise ValueError('draw descriptor has no matching update')
        info = values[key]
        offset = offsets[index] if dynamic else 0
        if dynamic and (info['offset'] <= 0 or offset <= 0):
            raise ValueError('dynamic fixture requires nonzero base and dynamic offsets')
        effective = info['offset'] + offset
        buffers = [c for c in calls if c.get('function', {}).get('name') == 'vkCreateBuffer'
                   and c['function']['args']['pBuffer'] == info['buffer']]
        binds = [c for c in calls if c.get('function', {}).get('name') == 'vkBindBufferMemory'
                 and c['function']['args']['buffer'] == info['buffer']]
        if (len(buffers) != 1 or len(binds) != 1 or buffers[0]['function']['return'] != 'VK_SUCCESS'
                or binds[0]['function']['return'] != 'VK_SUCCESS'
                or not buffers[0]['index'] < binds[0]['index'] < update_call['index']
                or info['range'] != 272 or effective < 0
                or effective + info['range'] > buffers[0]['function']['args']['pCreateInfo']['size']):
            raise ValueError('draw descriptor buffer/range provenance mismatch')
        memory_bind = binds[0]['function']['args']
        allocations = [c for c in calls if c.get('function', {}).get('name') == 'vkAllocateMemory'
                       and c['function']['args']['pMemory'] == memory_bind['memory']]
        if (len(allocations) != 1 or allocations[0]['function']['return'] != 'VK_SUCCESS'
                or allocations[0]['index'] >= binds[0]['index']):
            raise ValueError('descriptor backing allocation is missing or ambiguous')
        memory_allocation = allocations[0]['function']['args']
        if (len({update['device'], allocation['function']['args']['device'],
                 buffers[0]['function']['args']['device'], memory_bind['device'], memory_allocation['device']}) != 1
                or memory_bind['memoryOffset'] < 0
                or memory_bind['memoryOffset'] + buffers[0]['function']['args']['pCreateInfo']['size'] >
                   memory_allocation['pAllocateInfo']['allocationSize']):
            raise ValueError('descriptor buffer/memory ownership or bounds mismatch')
        reference = bytearray(272)
        alternate = binding == 'bad' and (not multi or index == 2)
        struct.pack_into('<f', reference, 0, 0.0 if alternate else 1.0)
        for matrix_offset in (192, 212, 232, 252):
            struct.pack_into('<f', reference, matrix_offset, 1.0)
        struct.pack_into('<f', reference, 256, 1.0 if alternate else 0.0)
        struct.pack_into('<i', reference, 268, 0 if alternate else 1)
        if multi:
            struct.pack_into('<f', reference, 16, 101.0 + index)
        for stage in ('vertex', 'fragment'):
            descriptors = draw['descriptors'][stage]
            matches = [d for d in descriptors if (d['set'], d['binding'], d['arrayIndex']) ==
                       (entry['set'], entry['binding'], entry['array_index'])]
            if len(descriptors) != len(entries) or len(matches) != 1:
                raise ValueError('missing or ambiguous draw descriptor evidence')
            desc = matches[0]
            if (desc['bufferId'], desc['offset'], desc['size'], desc['type']) != (
                    info['buffer'], effective, info['range'], descriptor_type):
                raise ValueError('draw descriptor does not match effective update/binding')
            if read(desc['file']) != reference:
                raise ValueError('draw UBO content mismatch')
        result.append(dict(entry, buffer=info['buffer'], range=info['range'],
                           descriptor_offset=info['offset'], dynamic_offset=offset, effective_offset=effective,
                           memory=binds[0]['function']['args']['memory'],
                           buffer_memory_offset=memory_bind['memoryOffset'],
                           buffer_creation_index=buffers[0]['index'], memory_allocation_index=allocations[0]['index'],
                           memory_bind_index=binds[0]['index'], set_allocation_index=allocation['index'],
                           descriptor_update_index=update_call['index'], descriptor_bind_index=bind_call['index'],
                           ubo_sha256=hashlib.sha256(reference).hexdigest()))
    return descriptor_type, result
