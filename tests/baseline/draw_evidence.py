"""Check GFXReconstruct resource evidence for the single-draw widget fixture."""
import hashlib
from descriptor_evidence import check_descriptors
from attachment_evidence import check_attachment


def check_draw(calls, report, local, evidence, binding, indices, expected, dynamic=False, multi=False):
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
    descriptor_type, descriptors = check_descriptors(
        calls, draw, binding, read, begin, draw_index, submit, dynamic, multi)
    selected = descriptors[2 if multi else 0]
    args = bind['function']['args']
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
    if before != bytes(1024) or after != expected:
        raise ValueError('draw attachment does not match final readback')
    transfer = next(cmd for item in report for cmd in item.get('transferCommands', []))
    if transfer['parameters']['srcImage']['imageId'] != attachment['imageId']:
        raise ValueError('draw attachment is not the copied image')
    return {'draw_index': draw_index, 'submit_index': submit,
            'pipeline': pipeline['function']['args']['pipeline'], 'layout': args['layout'],
            'set': selected['set_handle'], 'buffer': selected['buffer'], 'range': selected['range'],
            'descriptors': descriptors,
            'descriptor_type': descriptor_type, 'descriptor_offset': selected['descriptor_offset'],
            'dynamic_offset': selected['dynamic_offset'], 'effective_offset': selected['effective_offset'],
            'image': attachment['imageId'], 'attachment_lineage': lineage,
            'ubo_sha256': selected['ubo_sha256'],
            'before_sha256': hashlib.sha256(before).hexdigest(),
            'after_sha256': hashlib.sha256(after).hexdigest()}
