"""Verify point-size fixtures and the actual transformed vertex modules."""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
from builtin_evidence import builtin_decorations


def point_size_evidence(directory, log, source, unused_builtins=False):
    expected_pixels = [(str(v), str(r), '0' if r == 1 else '3',
                        str((25 if v == 2 else 1) if r == 1 else 256), '0')
                       for v in range(3) for r in range(3)]
    actual = re.findall(r'^POINT_SIZE_READBACK variant=(\d+) round=(\d+) topology=(\d+) white=(\d+) bad=(\d+)$', log, re.M)
    if actual != expected_pixels:
        raise ValueError('point/triangle reuse or point-size pixel evidence differs')
    conversions = re.findall(r'^HYBRIS_POINT_SIZE removed=(\d+) topology=(\d+) original=(\d+) converted=(\d+)$', log, re.M)
    if len(conversions) != 2 or any(row[:2] != ('1', '3') for row in conversions):
        raise ValueError('only the two constant-one triangle pipelines may be changed')
    records = re.findall(r'^HYBRIS_SCALED_DUMP id=(\d+) original=(\d) converted=(\d) attributes=(\d+)$', log, re.M)
    if records != [('0', '1', '1', '0'), ('1', '1', '1', '0')]:
        raise ValueError('point-size shader artifacts missing or unexpected')
    body = re.search(r'kPointVertex\[\] = \{(.*?)\};', source.read_text(), re.S)[1]
    words = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{8}', body)]
    reference = hashlib.sha256(struct.pack('<' + 'I' * len(words), *words)).hexdigest()
    evidence = []
    for sequence in range(2):
        paths = [directory / f'{sequence:03d}-{kind}.spv' for kind in ('original', 'converted')]
        modules = []
        for path in paths:
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(path)], check=True, capture_output=True)
            text = subprocess.check_output(['spirv-dis', '--raw-id', str(path)], text=True)
            path.with_suffix('.spvasm').write_text(text)
            modules.append(text)
        if hashlib.sha256(paths[0].read_bytes()).hexdigest() != reference:
            raise ValueError('transformed source is not the constant-one fixture')
        before, after = modules
        ids = dict(re.findall(r'^\s*(%\d+) = (.+)$', before, re.M))
        members = re.findall(r'OpMemberDecorate (%\d+) (\d+) BuiltIn PointSize', before)
        if len(members) != 1 or 'BuiltIn PointSize' in after:
            raise ValueError('PointSize declaration was not removed as expected')
        structure, member = members[0]
        pointers = {id for id, value in ids.items() if value == 'OpTypePointer Output ' + structure}
        variables = {id for id, value in ids.items() if value.split()[:2] == ['OpVariable', next(iter(pointers))]} if len(pointers) == 1 else set()
        point_accesses = set()
        for id, value in ids.items():
            fields = value.split()
            if fields[0] in {'OpAccessChain', 'OpInBoundsAccessChain'} and len(fields) == 4 and fields[2] in variables:
                constant = ids[fields[3]].split()
                if constant[0] == 'OpConstant' and constant[2] == member:
                    point_accesses.add(id)
        if len(point_accesses) != 1:
            raise ValueError('constant-one fixture has an unexpected PointSize pointer shape')
        kept = []
        stores = 0
        for line in before.splitlines():
            tokens = line.split()
            if any(id in tokens for id in point_accesses):
                operation = tokens[2] if len(tokens) > 2 and tokens[1] == '=' else tokens[0]
                if operation == 'OpStore' and tokens[1] in point_accesses and len(tokens) == 3:
                    constant = ids[tokens[2]].split()
                    if len(constant) != 3 or constant[0] != 'OpConstant' or ids[constant[1]] != 'OpTypeFloat 32' or float(constant[2]) != 1.0:
                        raise ValueError('PointSize removal erased a non-one store')
                    stores += 1
                elif operation in {'OpAccessChain', 'OpInBoundsAccessChain'} and tokens[0] in point_accesses:
                    pass
                elif operation in {'OpName', 'OpDecorate'}:
                    pass
                else:
                    raise ValueError('PointSize removal erased a read or other pointer use')
                continue
            kept.append(line)
        if stores != 1:
            raise ValueError('fixture constant-one store was not independently identified')
        stripped = '\n'.join(kept)
        allowed = {'PointSize'} | ({'ClipDistance', 'CullDistance'} if unused_builtins else set())
        expected, changes = builtin_decorations(stripped, after, removable=allowed)
        if sorted(expected) != sorted(re.findall(r'^\s*(Op(?:Member)?Decorate .+)$', after, re.M)):
            raise ValueError('PointSize cleanup changed unrelated decorations')
        def functions(text):
            return [line.strip() for line in text.splitlines()[next(i for i, line in enumerate(text.splitlines()) if ' = OpFunction ' in line):]]
        if functions(stripped) != functions(after):
            raise ValueError('PointSize cleanup changed other function instructions')
        evidence.append({'source_sha256': reference, 'converted_sha256': hashlib.sha256(paths[1].read_bytes()).hexdigest(),
                         'spirv_val': 'PASS', 'removed_constant_stores': stores, 'structures': changes})
    return {'readbacks': 9, 'transformed_triangle_pipelines': 2, 'modules': evidence}
