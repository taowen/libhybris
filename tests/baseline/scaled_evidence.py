"""Validate the modules actually submitted by experimental scaled conversion."""
import hashlib
import re
import shutil
import struct
import subprocess
from decoration_evidence import normalize_decorations


def vertex_input(text, entry):
    definitions = dict(re.findall(r'^\s*(%\d+) = (.+)$', text, re.M))
    variables = re.findall(r'OpDecorate (%\d+) Location 0$', text, re.M)
    interfaces = set(re.findall(r'%\d+', entry)[1:])
    inputs = []
    for variable in variables:
        if variable not in interfaces:
            continue
        declaration = definitions[variable].split()
        if declaration[0] != 'OpVariable' or declaration[2] != 'Input':
            continue
        pointer = definitions[declaration[1]].split()
        vector = definitions[pointer[2]].split()
        if vector[0] != 'OpTypeVector':
            raise ValueError('fixture vertex input is not a vector')
        inputs.append({'id': variable, 'location': 0, 'components': int(vector[2]),
                       'scalar': definitions[vector[1]]})
    if len(inputs) != 1:
        raise ValueError('expected one vertex input at location zero')
    return inputs[0]


def scaled_evidence(directory, log, forced, source, unused_builtins=False, dynamic_stride=False):
    validator = shutil.which('spirv-val')
    if not validator:
        raise ValueError('spirv-val is required to audit converted modules')
    disassembler = shutil.which('spirv-dis')
    if not disassembler:
        raise ValueError('spirv-dis is required to audit vertex interfaces')
    words = [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]{8}', source.read_text())]
    reference = hashlib.sha256(struct.pack('<' + 'I' * len(words), *words)).hexdigest()
    multiple = source.name in ('scaled.multi.inc', 'scaled.group-multi.inc')
    grouped = source.name.startswith('scaled.group')
    if dynamic_stride:
        stride_caps = re.findall(r'^SCALED STRIDE extension=([01]) feature=([01])$', log, re.M)
        if 'SCALED STRIDE UNSUPPORTED' in log:
            if len(stride_caps) != 1 or stride_caps[0] == ('1', '1') or 'SCALED INSTANCE' in log or list(directory.glob('*.spv')):
                raise ValueError('invalid unsupported stride evidence')
            return {'forced': forced, 'unsupported': 'extended dynamic state', 'capabilities': stride_caps, 'modules': []}
        if stride_caps != [('1', '1')]:
            raise ValueError('missing stride capabilities')
        strides = re.findall(r'^SCALED STRIDE case=(\d+) round=(\d+) phase=(\d+) offset=4 overwritten=(\d+) stride=(\d+)$', log, re.M)
        expected_strides = [(str(c), str(r), str(p), str(width + (8 if p == 1 else 4) + 4), str(width + (8 if p == 1 else 4)))
                            for c, width in enumerate((1,1,2,2,4,4,2,2,4,4,8,8)) for r in range(3) for p in range(3)]
        if strides != expected_strides:
            raise ValueError('dynamic stride sequence incomplete or incorrect')
    rounds = 1
    divisor_evidence = None
    if source.name == 'scaled.divisor.inc':
        caps = re.findall(r'^SCALED DIVISOR extension=(\S+) max=(\d+) divisor=([01]) zero=([01]) nonzero_first=([01]) mode=(\d+)$', log, re.M)
        unsupported = re.findall(r'^SCALED DIVISOR UNSUPPORTED (.+)$', log, re.M)
        instances = re.findall(r'^SCALED INSTANCE case=(\d+) round=(\d+) divisor=(\d+) first=(\d+)$', log, re.M)
        if unsupported:
            if len(unsupported) != 1 or instances or 'HYBRIS_SCALED_DUMP' in log or 'SCALED tested=' in log:
                raise ValueError('unsupported divisor case contains execution evidence')
            if caps:
                if len(caps) != 1:
                    raise ValueError('ambiguous divisor capabilities')
                extension, maximum, divisor, zero, first, mode = caps[0]
                maximum, divisor, zero, first, mode = map(int, (maximum, divisor, zero, first, mode))
                if divisor and ((mode & 2 and zero) or (not mode & 2 and maximum >= 3)) and (not mode & 4 or first):
                    raise ValueError('supported divisor combination reported unsupported')
            elif not re.fullmatch(r'extension khr=0 ext=[012]', unsupported[0]):
                raise ValueError('missing divisor capability evidence')
            return {'forced': forced, 'unsupported': unsupported[0], 'capabilities': caps, 'modules': []}
        if len(caps) != 1:
            raise ValueError('missing divisor capability record')
        extension, maximum, divisor, zero, first, mode = caps[0]
        maximum, divisor, zero, first, mode = map(int, (maximum, divisor, zero, first, mode))
        if mode not in (1, 3, 5, 7) or not divisor or (mode & 2 and not zero) or (not mode & 2 and maximum < 3) or (mode & 4 and not first):
            raise ValueError('divisor execution exceeds queried capabilities')
        rounds = 1 if mode & 2 else 3
        expected_instances = [(str(c), str(r), str(0 if mode & 2 else r + 1), str(3 if mode & 4 else 0))
                              for c in range(12) for r in range(rounds)]
        if instances != expected_instances:
            raise ValueError('divisor draw sequence incomplete or repeated')
        pixels = re.findall(r'^SCALED format=(\S+) entry=main phase=([012]) expected=[^\n]+ pixel=(\d+),(\d+),(\d+),(\d+) bad=(\d+)$', log, re.M)
        formats = [base + '_' + sign for base in ('R8', 'R8G8', 'R8G8B8A8', 'R16', 'R16G16', 'R16G16B16A16')
                   for sign in ('USCALED', 'SSCALED')]
        expected_pixels = [(fmt, str(phase), '0' if phase == 2 else '255', '255', '255', '255', '0')
                           for fmt in formats for _ in range(rounds) for phase in range(3)]
        if pixels != expected_pixels:
            raise ValueError('divisor pixel readbacks incomplete or incorrect')
        divisor_evidence = {'extension': extension, 'max': maximum, 'zero': zero,
                            'nonzero_first': first, 'mode': mode, 'pipelines': len(instances)}
    stages_per_pipeline = 2 if multiple else 1
    records = re.findall(r'^HYBRIS_SCALED_DUMP id=(\d+) original=(\d) converted=(\d) attributes=(\d+)$', log, re.M)
    masks = re.findall(r'^HYBRIS_SCALED_VERTEX experimental=1 force=[01] fallback_mask=0x([0-9a-f]+)$', log, re.M)
    if len(masks) != 1 or len(records) != int(masks[0], 16).bit_count() * stages_per_pipeline * rounds:
        raise ValueError('module dump count differs from the active format fallback mask')
    if forced:
        summary = re.findall(r'^SCALED tested=(\d+) unsupported=(\d+) failures=(\d+) validation_errors=(\d+)$', log, re.M)
        if len(summary) != 1 or len(records) != int(summary[0][0]) * stages_per_pipeline:
            raise ValueError('converted pipeline count differs from the completed fixture')
    evidence = {'forced': forced, 'validator': subprocess.check_output([validator, '--version'], text=True), 'modules': []}
    if divisor_evidence is not None:
        evidence['divisor'] = divisor_evidence
    for sequence, (index, original, converted, count) in enumerate(records):
        if int(index) != sequence:
            raise ValueError('module dump sequence is incomplete or repeated')
        vertex = not multiple or sequence % 2 == 0
        if (original, converted, count) != ('1', '1', '1' if vertex else '0'):
            raise ValueError('module dump failed or stage attribute count differs from fixture')
        signs = re.findall(r'^HYBRIS_SCALED_ATTRIBUTE id=' + index + r' location=0 signed=([01])$', log, re.M)
        if len(signs) != (1 if vertex else 0):
            raise ValueError('signedness records differ from the pipeline stage')
        entry = {'index': int(index), 'stage': 'vertex' if vertex else 'fragment',
                 'signed': int(signs[0]) if vertex else None, 'files': []}
        modules = []
        for kind in ('original', 'converted'):
            path = directory / f'{int(index):03d}-{kind}.spv'
            subprocess.run([validator, '--target-env', 'vulkan1.1', str(path)], check=True, capture_output=True)
            disassembly = subprocess.check_output([disassembler, '--raw-id', str(path)], text=True)
            path.with_suffix('.spvasm').write_text(disassembly)
            normalized = None
            if grouped:
                disassembly, normalized = normalize_decorations(path, disassembly, kind == 'original')
            modules.append(disassembly)
            entry['files'].append({'name': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'spirv_val': 'PASS', 'normalized_decorations': normalized})
        if entry['files'][0]['sha256'] == entry['files'][1]['sha256']:
            raise ValueError('converted module is identical to the original')
        if entry['files'][0]['sha256'] != reference:
            raise ValueError('original module differs from the probe build input')
        entries = re.findall(r'^\s*(OpEntryPoint .+)$', modules[1], re.M)
        if len(entries) != 1 or not entries[0].startswith('OpEntryPoint ' + ('Vertex' if vertex else 'Fragment') + ' '):
            raise ValueError('converted module must expose only its selected stage entry')
        selected = entries[0]
        if selected not in re.findall(r'^\s*(OpEntryPoint .+)$', modules[0], re.M):
            raise ValueError('converted entry differs from the original declaration')
        selected_name = re.search(r'"([^"]+)"', selected).group(1)
        expected_name = ('alternate_vertex' if sequence // 2 % 2 else 'scaled_vertex') if multiple and vertex else 'main'
        if selected_name != expected_name:
            raise ValueError('pipeline selected the wrong vertex entry')
        entry['entry_point'] = selected_name
        before, after = [vertex_input(module, selected) for module in modules]
        if source.name == 'scaled.literal.inc':
            if before['id'] != '%3' or not re.search(r'OpVectorShuffle .+ 3 2 1 0$', modules[0], re.M):
                raise ValueError('literal collision fixture lost its ID/index overlap')
        if before['scalar'] != 'OpTypeFloat 32' or before['components'] != 4:
            raise ValueError('unexpected original vertex interface')
        expected = dict(before, scalar='OpTypeInt 32 ' + signs[0]) if vertex else before
        if after != expected:
            raise ValueError('converted interface differs from expected integer fetch')
        stable = r'^\s*(Op(?:Member)?Decorate .+)$'
        retained_ids = set(re.findall(r'^\s*(%\d+) = ', modules[1], re.M))
        original_decorations = [line for line in re.findall(stable, modules[0], re.M)
                                if re.search(r'%\d+', line).group() in retained_ids]
        if unused_builtins:
            from builtin_evidence import builtin_decorations
            original_decorations, entry["builtin_cleanup"] = builtin_decorations(*modules)
            if source.name == 'scaled.builtins.inc':
                changes = list(entry['builtin_cleanup'].values())
                if len(changes) != 1 or changes[0]['removed'] != [0, 1] or not changes[0]['access_chains']:
                    raise ValueError('reordered builtin fixture did not exercise pruning and access remapping')
        if sorted(original_decorations) != sorted(re.findall(stable, modules[1], re.M)):
            raise ValueError('conversion changed decorations of retained declarations')
        if vertex:
            opcode = 'OpConvertSToF' if entry['signed'] else 'OpConvertUToF'
            if opcode not in modules[1]:
                raise ValueError('converted module lacks the required conversion')
        entry['interface'] = {'original': before, 'converted': after}
        evidence['modules'].append(entry)
    return evidence
