"""Audit the mixed-format aggregate shaders submitted to the vendor driver."""
import hashlib
import re
import struct
import subprocess


def aggregate_evidence(directory, log, source):
    words = [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]{8}', source.read_text())]
    specialized = source.name.startswith('scaled.spec')
    direct = source.name == 'scaled.spec-direct.inc'
    reference = hashlib.sha256(struct.pack('<' + 'I' * len(words), *words)).hexdigest()
    records = re.findall(r'^HYBRIS_SCALED_DUMP id=(\d+) original=(\d) converted=(\d) attributes=(\d+)$', log, re.M)
    summary = re.findall(r'^SCALED tested=(\d+) unsupported=(\d+) failures=(\d+) validation_errors=(\d+)$', log, re.M)
    masks = re.findall(r'^HYBRIS_SCALED_VERTEX experimental=1 force=[01] fallback_mask=0x([0-9a-f]+)$', log, re.M)
    if len(masks) != 1:
        raise ValueError('missing aggregate fallback mask')
    mask = int(masks[0], 16)
    pipelines = [(c, r) for c in range(12) for r in range(4 if specialized else 1)
                 if mask & ((1 << c) | (1 << (((c + 6) % 12) ^ 1)))]
    if summary != [('48' if specialized else '12', '0', '0', '0')] or len(records) != len(pipelines):
        raise ValueError('aggregate fixture must complete all twelve mixed-format pipelines')
    if specialized:
        expected_rounds = [(str(c), str(r), str((2, 4, 3, 2)[r]), str(((2, 4, 3, 2) if direct else (0, 3, 2, 0))[r]),
                            '1' if r == 2 else '0.5', '1' if r in (0, 3) else '0')
                           for c in range(12) for r in range(4)]
        if re.findall(r'^SCALED SPEC case=(\d+) round=(\d+) columns=(\d+) base=(\d+) tint=(\S+) invert=([01])$', log, re.M) != expected_rounds:
            raise ValueError('specialization round sequence differs from fixture')
        restored = re.findall(r'^SCALED cache restored bytes=(\d+)$', log, re.M)
        if len(restored) != 1 or int(restored[0]) < 32:
            raise ValueError('pipeline cache serialization/restoration did not complete')
    evidence = {'modules': []}
    for sequence, (index, original, converted, count) in enumerate(records):
        case, round_index = pipelines[sequence]
        columns = (2, 4, 3, 2)[round_index] if specialized else 4
        expected_signs = {}
        if mask & (1 << case):
            expected_signs['1'] = str(case % 2)
        if mask & (1 << (((case + 6) % 12) ^ 1)):
            expected_signs['2'] = str(1 - case % 2)
        if int(index) != sequence or (original, converted, count) != ('1', '1', str(len(expected_signs))):
            raise ValueError('incomplete aggregate shader dump')
        signs = dict(re.findall(r'^HYBRIS_SCALED_ATTRIBUTE id=' + index + r' location=([12]) signed=([01])$', log, re.M))
        if signs != expected_signs:
            raise ValueError('aggregate format signedness differs from fixture')
        modules, files = [], []
        for kind in ('original', 'converted'):
            path = directory / f'{sequence:03d}-{kind}.spv'
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(path)], check=True, capture_output=True)
            text = subprocess.check_output(['spirv-dis', '--raw-id', str(path)], text=True)
            path.with_suffix('.spvasm').write_text(text)
            modules.append(text)
            files.append({'name': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'spirv_val': 'PASS'})
        if files[0]['sha256'] != reference or files[0]['sha256'] == files[1]['sha256']:
            raise ValueError('aggregate source identity or conversion mismatch')
        before, after = [dict(re.findall(r'^\s*(%\d+) = (.+)$', text, re.M)) for text in modules]
        root, = [var for var in re.findall(r'OpDecorate (%\d+) Location 0$', modules[0], re.M)
                 if before[var].endswith(' Input')]
        old_pointer = before[root].split()[1]
        new_pointer = after[root].split()[1]
        aggregate_type = before[old_pointer].split()[2]
        if after[root] != 'OpVariable ' + new_pointer + ' Private' or after[new_pointer] != 'OpTypePointer Private ' + aggregate_type:
            raise ValueError('original aggregate must retain its float type in Private storage')
        if re.search(r'OpDecorate ' + root + r' (Location|Component)\b', modules[1]):
            raise ValueError('Private aggregate retains an interface decoration')
        entry, = re.findall(r'^\s*OpEntryPoint Vertex %\d+ "main"(.*)$', modules[1], re.M)
        interfaces = set(re.findall(r'%\d+', entry))
        if root in interfaces:
            raise ValueError('SPIR-V 1.0 entry retains the Private aggregate')
        locations = {}
        for var, location in re.findall(r'OpDecorate (%\d+) Location (\d+)$', modules[1], re.M):
            if not after[var].endswith(' Input'):
                continue
            if var not in interfaces or location in locations:
                raise ValueError('invalid aggregate leaf interface')
            pointer = after[var].split()[1]
            vector = after[after[pointer].split()[2]].split()
            if vector[0] != 'OpTypeVector' or vector[2] != '4':
                raise ValueError('aggregate fixture leaf must be vec4')
            locations[location] = after[vector[1]]
        expected = {str(i): 'OpTypeFloat 32' for i in range(columns)}
        expected.update({loc: 'OpTypeInt 32 ' + sign for loc, sign in signs.items() if int(loc) < columns})
        conversions = [('OpConvertSToF' if sign == '1' else 'OpConvertUToF') for loc, sign in signs.items() if int(loc) < columns]
        if locations != expected or any(op not in modules[1] for op in conversions):
            raise ValueError('aggregate mixed float/signed/unsigned interface mismatch')
        item = {'index': sequence, 'files': files, 'locations': locations, 'private_root': root}
        if specialized:
            length_id = after[aggregate_type].split()[2]
            length = after[length_id].split()
            if length[0] != 'OpConstant' or int(length[2]) != columns:
                raise ValueError('aggregate array length was not frozen to this pipeline specialization')
            decorations = r'OpDecorate %\d+ SpecId (\d+)'
            if sorted(re.findall(decorations, modules[0])) != ['19', '23', '7'] or sorted(re.findall(decorations, modules[1])) != (['19', '23'] if direct else ['19', '23', '7']):
                raise ValueError('array expression rewrite changed independent specialization constants')
            specs = re.findall(r'^HYBRIS_SCALED_SPECIALIZATION id=' + index + r' entries=(\d+) data_size=(\d+) saved=(\d)$', log, re.M)
            if round_index == 2:
                if specs:
                    raise ValueError('default pipeline unexpectedly supplied a specialization map')
                item['specialization'] = None
            else:
                if specs != [('4', '24', '1')]:
                    raise ValueError('missing specialization input dump')
                maps = re.findall(r'^HYBRIS_SCALED_MAP id=' + index + r' constant=(\d+) offset=(\d+) size=(\d+)$', log, re.M)
                if maps != [('23', '17', '4'), ('999', '0', '1'), ('7', '3', '4'), ('19', '9', '4')]:
                    raise ValueError('specialization mapping differs from fixture input')
                path = directory / f'{sequence:03d}-specialization.bin'
                data = path.read_bytes()
                expected_data = bytearray(24)
                struct.pack_into('<i', expected_data, 3, columns if direct else columns - 1 - int(round_index in (0, 3)))
                struct.pack_into('<f', expected_data, 9, 0.5)
                struct.pack_into('<I', expected_data, 17, int(round_index in (0, 3)))
                if data != expected_data:
                    raise ValueError('specialization bytes differ from pipeline inputs')
                item['specialization'] = {'maps': maps, 'sha256': hashlib.sha256(data).hexdigest()}
            if round_index == 3 and files[1]['sha256'] != evidence['modules'][-3]['files'][1]['sha256']:
                raise ValueError('repeated specialization produced a different module')
        evidence['modules'].append(item)
    return evidence
