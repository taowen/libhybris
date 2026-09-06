"""Check GFXReconstruct resource evidence for the single-draw widget fixture."""
import hashlib
import struct
from attachment_evidence import check_attachment


def check_draw(calls, report, local, evidence, binding, indices, expected, dynamic=False):
    begin, draw_index, submit = indices
    draws = [draw for item in report for draw in item.get('drawCallCommands', [])]
    if len(draws) != 1:
        raise ValueError('expected one resource-backed draw')
    draw = draws[0]
    if (draw['beginCommandBufferIndex'], draw['drawIndex'], draw['queueSubmitIndex']) != indices:
        raise ValueError('draw resource command association mismatch')
    if draw['parameters']['drawCallType'] != 'vkCmdDrawIndexed' or draw['parameters']['indexCount'] != 18:
        raise ValueError('unexpected widget draw parameters')

    def one(name):
        matches = [c for c in calls if c.get('function', {}).get('name') == name]
        if len(matches) != 1:
            raise ValueError('fixture requires exactly one ' + name)
        return matches[0]

    def read(path):
        file = evidence.parent / path
        if not file.resolve().is_relative_to((local / 'replay').resolve()):
            raise ValueError('draw dump path escapes replay output')
        return file.read_bytes()

    bind = one('vkCmdBindDescriptorSets')
    pipeline = one('vkCmdBindPipeline')
    update = one('vkUpdateDescriptorSets')
    if not update['index'] < begin < pipeline['index'] < bind['index'] < draw_index < submit:
        raise ValueError('unexpected fixture binding order')
    args = bind['function']['args']
    writes = update['function']['args']['pDescriptorWrites']
    if len(writes) != 1 or args['firstSet'] != 0 or args['dynamicOffsetCount'] != int(dynamic):
        raise ValueError('unexpected fixture descriptor structure')
    write = writes[0]
    if args['pDescriptorSets'] != [write['dstSet']] or write['dstBinding'] != 0:
        raise ValueError('updated set does not match the draw binding')
    descriptor_type = ('VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC' if dynamic
                       else 'VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER')
    if (write['descriptorType'] != descriptor_type or write['descriptorCount'] != 1
            or len(write['pBufferInfo']) != 1):
        raise ValueError('unexpected widget descriptor type/count')
    info = write['pBufferInfo'][0]
    dynamic_offsets = args.get('pDynamicOffsets') or []
    if len(dynamic_offsets) != int(dynamic):
        raise ValueError('dynamic offset payload does not match count')
    dynamic_offset = dynamic_offsets[0] if dynamic else 0
    if dynamic and (info['offset'] <= 0 or dynamic_offset <= 0):
        raise ValueError('dynamic fixture requires nonzero base and dynamic offsets')
    effective_offset = info['offset'] + dynamic_offset
    reference = bytearray(272)
    struct.pack_into('<f', reference, 0, 1.0 if binding == 'good' else 0.0)
    for offset in (192, 212, 232, 252):
        struct.pack_into('<f', reference, offset, 1.0)
    struct.pack_into('<f', reference, 256, 0.0 if binding == 'good' else 1.0)
    struct.pack_into('<i', reference, 268, 1 if binding == 'good' else 0)
    for stage in ('vertex', 'fragment'):
        descriptors = draw['descriptors'][stage]
        if len(descriptors) != 1:
            raise ValueError('missing draw descriptor evidence')
        desc = descriptors[0]
        if (desc['set'], desc['binding'], desc['arrayIndex'], desc['bufferId'], desc['offset'], desc['size']) != (
                0, 0, 0, info['buffer'], effective_offset, info['range']):
            raise ValueError('draw descriptor does not match effective update/binding')
        if desc['type'] != descriptor_type or read(desc['file']) != reference:
            raise ValueError('draw UBO content mismatch')
    attachments = draw['colorAttachments']
    if len(attachments) != 1 or attachments[0]['format'] != 'VK_FORMAT_R8G8B8A8_UNORM':
        raise ValueError('unexpected draw attachment')
    attachment = attachments[0]
    lineage = check_attachment(calls, attachment)
    subresources = attachment['subresources']
    if len(subresources) != 1 or subresources[0]['dimensions'] != [16, 16, 1]:
        raise ValueError('unexpected attachment subresource')
    sub = subresources[0]
    before, after = read(sub['beforeFile']), read(sub['file'])
    if len(before) != 1024 or after != expected:
        raise ValueError('draw attachment does not match final readback')
    transfer = next(cmd for item in report for cmd in item.get('transferCommands', []))
    if transfer['parameters']['srcImage']['imageId'] != attachment['imageId']:
        raise ValueError('draw attachment is not the copied image')
    return {'draw_index': draw_index, 'submit_index': submit,
            'pipeline': pipeline['function']['args']['pipeline'], 'layout': args['layout'],
            'set': write['dstSet'], 'buffer': info['buffer'], 'range': info['range'],
            'descriptor_type': descriptor_type, 'descriptor_offset': info['offset'],
            'dynamic_offset': dynamic_offset, 'effective_offset': effective_offset,
            'image': attachment['imageId'], 'attachment_lineage': lineage,
            'ubo_sha256': hashlib.sha256(reference).hexdigest(),
            'before_sha256': hashlib.sha256(before).hexdigest(),
            'after_sha256': hashlib.sha256(after).hexdigest()}
