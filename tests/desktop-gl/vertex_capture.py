"""Index recorded vertex input state in GFXReconstruct JSONL; not a replay engine."""
import copy
import json
from pipeline_vertex import pipeline_vertex


def vertex_draws(path):
    pipelines, buffers, draws, unexpanded = {}, {}, [], []
    calls = 0
    for line in path.open():
        item = json.loads(line)
        function = item.get('function')
        if not function:
            continue
        calls += 1
        name, args = function['name'], function.get('args', {})
        index = item['index']
        if name == 'vkCreateGraphicsPipelines':
            for handle, info in zip(args['pPipelines'], args['pCreateInfos']):
                if handle:
                    pipelines[handle] = pipeline_vertex(handle, index, info, pipelines)
        elif name == 'vkDestroyPipeline':
            pipelines.pop(args['pipeline'], None)
        elif name == 'vkBeginCommandBuffer' and function.get('return') == 'VK_SUCCESS':
            buffers[args['commandBuffer']] = {'begin': index, 'vertices': {}, 'draws': []}
        elif name == 'vkCmdBindPipeline' and args['pipelineBindPoint'] == 'VK_PIPELINE_BIND_POINT_GRAPHICS':
            buffers[args['commandBuffer']]['pipeline'] = args['pipeline']
        elif name in ('vkCmdBindVertexBuffers', 'vkCmdBindVertexBuffers2', 'vkCmdBindVertexBuffers2EXT'):
            state = buffers[args['commandBuffer']]
            for i in range(args['bindingCount']):
                binding = args['firstBinding'] + i
                old = state['vertices'].get(binding, {})
                value = {'buffer': args['pBuffers'][i], 'offset': args['pOffsets'][i], 'bind_index': index}
                if args.get('pStrides') is not None:
                    value['dynamic_stride'] = args['pStrides'][i]
                elif 'dynamic_stride' in old:
                    value['dynamic_stride'] = old['dynamic_stride']
                state['vertices'][binding] = value
        elif name == 'vkCmdSetVertexInputEXT':
            buffers[args['commandBuffer']]['dynamic_input'] = args
        elif name == 'vkCmdBindShadersEXT':
            buffers[args['commandBuffer']]['pipeline'] = None
            unexpanded.append({'index': index, 'name': name})
        elif name == 'vkCmdExecuteCommands':
            unexpanded.append({'index': index, 'name': name})
        elif name.startswith('vkCmdDraw'):
            state = buffers[args['commandBuffer']]
            handle = state.get('pipeline')
            resolved = pipelines.get(handle, {})
            sources = resolved.get('vertex_sources', [])
            complete = len(sources) == 1 and not resolved.get('unresolved_libraries')
            source = sources[0] if complete else {}
            dynamic = source.get('dynamic', [])
            vi = source.get('state', {})
            descriptions = vi.get('pVertexBindingDescriptions') or []
            attributes = vi.get('pVertexAttributeDescriptions') or []
            divisors = {}
            node = vi.get('pNext')
            while node:
                if node['sType'].startswith('VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO'):
                    divisors.update((d['binding'], d['divisor']) for d in node['pVertexBindingDivisors'])
                node = node.get('pNext')
            if 'VK_DYNAMIC_STATE_VERTEX_INPUT_EXT' in dynamic:
                current = state.get('dynamic_input')
                if current is None:
                    raise ValueError('dynamic vertex input was not recorded before draw')
                descriptions = current.get('pVertexBindingDescriptions') or []
                attributes = current.get('pVertexAttributeDescriptions') or []
                divisors = {b['binding']: b['divisor'] for b in descriptions}
            bindings = []
            for description in descriptions:
                binding = description['binding']
                if binding not in state['vertices']:
                    complete = False
                value = copy.deepcopy(state['vertices'].get(binding, {}))
                value.update(binding=binding, input_rate=description['inputRate'],
                             divisor=divisors.get(binding, 1), stride=description['stride'])
                if any(d.startswith('VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE') for d in dynamic):
                    if 'dynamic_stride' not in value:
                        raise ValueError('dynamic binding stride was not recorded before draw')
                    value['stride'] = value['dynamic_stride']
                bindings.append(value)
            draw = {'index': index, 'name': name, 'parameters': args,
                    'begin_index': state['begin'], 'pipeline': handle, 'pipeline_create_index': resolved.get('create_index'),
                    'vertex_input_source': {k: source[k] for k in ('pipeline', 'create_index') if k in source},
                    'pipeline_libraries': resolved.get('libraries', []),
                    'unresolved_libraries': resolved.get('unresolved_libraries', []),
                    'vertex_input_source_count': len(sources),
                    'bindings': bindings, 'attributes': attributes, 'submit_indices': [],
                    'vertex_state_complete': complete}
            if 'Indirect' in name:
                draw['indirect_arguments'] = 'not decoded'
            elif complete:
                draw['nonzero_first_instance_with_divisor_not_one'] = bool(args.get('firstInstance')) and any(
                    b['input_rate'] == 'VK_VERTEX_INPUT_RATE_INSTANCE' and b['divisor'] != 1 for b in bindings)
            state['draws'].append(draw)
            draws.append(draw)
        elif name in ('vkQueueSubmit', 'vkQueueSubmit2', 'vkQueueSubmit2KHR') and function.get('return') == 'VK_SUCCESS':
            for submit in args.get('pSubmits') or []:
                command_buffers = submit.get('pCommandBuffers') or [v['commandBuffer'] for v in submit.get('pCommandBufferInfos') or []]
                for command_buffer in command_buffers:
                    for draw in buffers.get(command_buffer, {}).get('draws', []):
                        draw['submit_indices'].append(index)
    if not draws:
        raise ValueError('capture has no graphics draw commands')
    return {'function_calls': calls, 'recorded_draws': len(draws),
            'submitted_draws': sum(bool(d['submit_indices']) for d in draws),
            'unexpanded_commands': unexpanded, 'draws': draws,
            'scope': 'recorded vertex input state and direct submit references; pipeline-library vertex input is resolved at creation; indirect arguments and secondary execution are not decoded'}
