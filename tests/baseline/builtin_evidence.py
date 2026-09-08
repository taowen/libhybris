"""Audit output-member pruning in actual SPIRV-Tools disassemblies.

This verifier is intentionally specific to the recorded rendering fixtures;
it does not reuse the production parser or authorize arbitrary shader edits.
"""
import re


def builtin_decorations(before, after, removable=frozenset({'ClipDistance', 'CullDistance'})):
    old = dict(re.findall(r'^\s*(%\d+) = (.+)$', before, re.M))
    new = dict(re.findall(r'^\s*(%\d+) = (.+)$', after, re.M))
    decorations = re.findall(r'^\s*(Op(?:Member)?Decorate .+)$', before, re.M)
    old_builtins = {(s, int(m)): b for s, m, b in re.findall(r'OpMemberDecorate (%\d+) (\d+) BuiltIn (\w+)', before)}
    new_builtins = {(s, int(m)): b for s, m, b in re.findall(r'OpMemberDecorate (%\d+) (\d+) BuiltIn (\w+)', after)}
    mappings = {}
    for struct_id, definition in old.items():
        if not definition.startswith('OpTypeStruct ') or struct_id not in new or definition == new[struct_id]:
            continue
        if not new[struct_id].startswith('OpTypeStruct '):
            raise ValueError('builtin cleanup changed a structure into another type')
        old_types, new_types = definition.split()[1:], new[struct_id].split()[1:]
        mapping, removed, cursor = {}, [], 0
        for index, member_type in enumerate(old_types):
            builtin = old_builtins.get((struct_id, index))
            if cursor < len(new_types) and (member_type, builtin) == (new_types[cursor], new_builtins.get((struct_id, cursor))):
                mapping[index] = cursor
                cursor += 1
            elif builtin in removable:
                removed.append(index)
            else:
                raise ValueError('cleanup removed or changed a member outside the allowed builtins')
        if cursor != len(new_types) or not removed:
            raise ValueError('cleanup introduced an unexpected output member')
        mappings[struct_id] = {'members': mapping, 'removed': removed, 'access_chains': 0}
    # Check all source uses of each affected output variable, not only the
    # declarations. Whole-value/pointer aliases are outside these fixtures.
    for variable, definition in old.items():
        fields = definition.split()
        if fields[0] != 'OpVariable' or fields[2] != 'Output':
            continue
        pointer = old[fields[1]].split()
        struct_id = pointer[2]
        if struct_id not in mappings:
            continue
        mapping = mappings[struct_id]
        for line in before.splitlines():
            tokens = line.split()
            if variable not in tokens:
                continue
            operation = tokens[2] if len(tokens) > 2 and tokens[1] == '=' else tokens[0]
            if operation in {'OpName', 'OpDecorate', 'OpEntryPoint', 'OpVariable'}:
                continue
            if operation not in {'OpAccessChain', 'OpInBoundsAccessChain'} or tokens[4] != variable:
                raise ValueError('cleanup fixture contains an unanalyzed output use')
            index_id = tokens[5]
            constant = old[index_id].split()
            if constant[0] != 'OpConstant' or old[constant[1]] not in {'OpTypeInt 32 0', 'OpTypeInt 32 1'}:
                raise ValueError('cleanup source index is not a 32-bit integer constant')
            index = int(constant[2])
            if index not in mapping['members']:
                raise ValueError('cleanup removed an accessed output member')
            result_id = tokens[0]
            if result_id not in new:
                # Entry extraction can remove a different entry's function.
                continue
            actual = new[result_id].split()
            if actual[:3] != tokens[2:5] or actual[4:] != tokens[6:]:
                raise ValueError('cleanup changed an output access beyond its first member index')
            actual_constant = new[actual[3]].split()
            if actual_constant != ['OpConstant', constant[1], str(mapping['members'][index])]:
                raise ValueError('cleanup output member index is incorrect')
            if index_id in new and old[index_id] != new[index_id]:
                raise ValueError('cleanup changed a shared source constant')
            mapping['access_chains'] += 1
    expected = []
    for decoration in decorations:
        fields = decoration.split()
        if fields[1] not in new:
            continue
        if fields[0] == 'OpMemberDecorate' and fields[1] in mappings:
            mapping = mappings[fields[1]]
            member = int(fields[2])
            if member in mapping['removed']:
                continue
            fields[2] = str(mapping['members'][member])
        expected.append(' '.join(fields))
    old_caps = set(re.findall(r'OpCapability (\w+)', before))
    new_caps = set(re.findall(r'OpCapability (\w+)', after))
    if new_caps - old_caps or old_caps - new_caps - {'ClipDistance', 'CullDistance'}:
        raise ValueError('cleanup changed an unrelated capability')
    for capability in old_caps - new_caps:
        if re.search(r'BuiltIn ' + capability + r'\b', after):
            raise ValueError('cleanup removed a capability still declared by the module')
    return expected, mappings
