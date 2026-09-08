"""Resolve captured pipeline-library vertex input at pipeline creation time."""
import copy


def pipeline_vertex(handle, index, info, pipelines):
    libraries, subsets = [], None
    flags = int(info['flags'], 0)
    node = info.get('pNext')
    while node:
        if node['sType'] == 'VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR':
            libraries = node.get('pLibraries') or []
            if len(libraries) != node['libraryCount']:
                raise ValueError('pipeline library count differs from captured handles')
        if node['sType'].startswith('VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO'):
            flags = int(node['flags'], 0)
        if node['sType'] == 'VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT':
            subsets = int(node['flags'], 0)
        node = node.get('pNext')
    if subsets is None:
        subsets = 0 if libraries or flags & 0x800 else 0xf
    sources, dependencies, unresolved = [], [], []
    if subsets & 1:  # VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT
        sources.append({'pipeline': handle, 'create_index': index,
                        'state': copy.deepcopy(info.get('pVertexInputState') or {}),
                        'dynamic': copy.deepcopy((info.get('pDynamicState') or {}).get('pDynamicStates') or [])})
    for library in libraries:
        resolved = pipelines.get(library)
        if resolved is None:
            unresolved.append(library)
            continue
        dependencies.append({'pipeline': library, 'create_index': resolved['create_index']})
        dependencies.extend(resolved['libraries'])
        sources.extend(resolved['vertex_sources'])
        unresolved.extend(resolved['unresolved_libraries'])
    # Copy resolved state now: linked libraries can be destroyed before drawing.
    return copy.deepcopy({'create_index': index, 'vertex_sources': sources,
                          'libraries': dependencies, 'unresolved_libraries': unresolved})
