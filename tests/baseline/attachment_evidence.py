"""Creation-to-copy attachment lineage for the fixed headless widget capture."""


def check_attachment(calls, attachment):
    def one(name):
        matches = [c for c in calls if c.get('function', {}).get('name') == name]
        if len(matches) != 1:
            raise ValueError('attachment fixture requires exactly one ' + name)
        return matches[0]

    image = one('vkCreateImage')
    view = one('vkCreateImageView')
    framebuffer = one('vkCreateFramebuffer')
    for creation in (image, view, framebuffer):
        if creation['function']['return'] != 'VK_SUCCESS':
            raise ValueError('attachment resource creation did not succeed')
    ia, va, fa = (c['function']['args'] for c in (image, view, framebuffer))
    ii, vi, fi = (a['pCreateInfo'] for a in (ia, va, fa))
    begin = one('vkBeginCommandBuffer')
    render = one('vkCmdBeginRenderPass')
    draw = one('vkCmdDrawIndexed')
    end = one('vkCmdEndRenderPass')
    copy = one('vkCmdCopyImageToBuffer')
    submit = one('vkQueueSubmit')
    sequence = (image, view, framebuffer, begin, render, draw, end, copy, submit)
    if any(a['index'] >= b['index'] for a, b in zip(sequence, sequence[1:])):
        raise ValueError('attachment creation/use order mismatch')
    command_buffer = begin['function']['args']['commandBuffer']
    for call in (render, draw, end, copy, one('vkCmdBindPipeline'), one('vkCmdBindDescriptorSets')):
        if call['function']['args']['commandBuffer'] != command_buffer:
            raise ValueError('attachment draw commands belong to different command buffers')
    submissions = submit['function']['args']['pSubmits']
    if len(submissions) != 1 or submissions[0]['pCommandBuffers'] != [command_buffer]:
        raise ValueError('attachment command buffer is not the submitted buffer')
    ri = render['function']['args']['pRenderPassBegin']
    ca = copy['function']['args']
    if (fa['pFramebuffer'] != ri['framebuffer'] or fi['renderPass'] != ri['renderPass']
            or fi['attachmentCount'] != 1 or fi['pAttachments'] != [va['pView']]
            or vi['image'] != ia['pImage'] or ia['pImage'] != attachment['imageId']
            or ca['srcImage'] != ia['pImage'] or len({ia['device'], va['device'], fa['device']}) != 1):
        raise ValueError('framebuffer/view/image/copy lineage mismatch')
    if (ii['format'] != attachment['format'] or vi['format'] != ii['format']
            or ii['extent'] != {'width': 16, 'height': 16, 'depth': 1}
            or ii['mipLevels'] != 1 or ii['arrayLayers'] != 1
            or (fi['width'], fi['height'], fi['layers']) != (16, 16, 1)
            or vi['viewType'] != 'VK_IMAGE_VIEW_TYPE_2D'
            or any(v != 'VK_COMPONENT_SWIZZLE_IDENTITY' for v in vi['components'].values())):
        raise ValueError('unexpected widget attachment format/dimensions/view')
    sub = vi['subresourceRange']
    if (int(sub['aspectMask'], 16), sub['baseMipLevel'], sub['levelCount'],
            sub['baseArrayLayer'], sub['layerCount']) != (1, 0, 1, 0, 1):
        raise ValueError('unexpected widget view subresource')
    if ca['regionCount'] != 1 or len(ca['pRegions']) != 1:
        raise ValueError('unexpected attachment copy count')
    region = ca['pRegions'][0]
    copied = region['imageSubresource']
    if (int(copied['aspectMask'], 16), copied['mipLevel'], copied['baseArrayLayer'],
            copied['layerCount']) != (1, 0, 0, 1) or region['imageOffset'] != {'x': 0, 'y': 0, 'z': 0}:
        raise ValueError('attachment copy subresource differs from the view')
    if region['imageExtent'] != ii['extent']:
        raise ValueError('attachment copy extent differs from the image')
    return {'command_buffer': command_buffer, 'framebuffer': fa['pFramebuffer'],
            'view': va['pView'], 'image': ia['pImage'], 'device': ia['device'],
            'render_pass': ri['renderPass'], 'view_subresource': sub,
            'image_creation_index': image['index'], 'view_creation_index': view['index'],
            'framebuffer_creation_index': framebuffer['index'], 'copy_index': copy['index']}
