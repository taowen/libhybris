"""Audit the packed vertex probe's actual shader dumps and pixel readbacks."""
import hashlib
import re
import struct
import subprocess


def packed_evidence(directory, log, source, width):
    if width not in range(1, 5):
        raise ValueError('invalid packed fixture width')
    arrays = re.findall(r'kPacked(\d)\[\] = \{(.*?)\};', source.read_text(), re.S)
    fixture = dict(arrays).get(str(width))
    if fixture is None:
        raise ValueError('packed fixture is missing')
    words = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{8}', fixture)]
    reference = struct.pack('<' + 'I' * len(words), *words)
    masks = re.findall(r'^HYBRIS_SCALED_VERTEX experimental=1 force=[01] fallback_mask=0x([0-9a-f]+)$', log, re.M)
    if len(masks) != 1:
        raise ValueError('missing unique vertex format decision')
    converted = bool(int(masks[0], 16) & (1 << 12))
    records = re.findall(r'^HYBRIS_SCALED_DUMP id=(\d+) original=(\d) converted=(\d) attributes=(\d+)$', log, re.M)
    if records != ([('0', '1', '1', '1')] if converted else []):
        raise ValueError('packed shader dumps differ from the active format decision')
    if re.findall(r'^HYBRIS_PACKED_ATTRIBUTE (.+)$', log, re.M) != (['id=0 location=0 swizzle=bgra'] if converted else []):
        raise ValueError('packed conversion attribute differs from fixture')
    if re.findall(r'^SCALED tested=(.+)$', log, re.M) != ['2 unsupported=0 failures=0 validation_errors=0']:
        raise ValueError('packed source and native control did not both complete')
    pixels = re.findall(r'^SCALED format=(\S+) entry=main phase=([012]) expected=[^\n]+ pixel=(\d+,\d+,\d+,\d+) bad=(\d+)$', log, re.M)
    expected = [(fmt, str(phase), '0,255,255,255' if phase == 2 else '255,255,255,255', '0')
                for fmt in ('A2R10G10B10_SNORM_PACK32', 'A2B10G10R10_SNORM_PACK32') for phase in range(3)]
    if pixels != expected:
        raise ValueError('packed readbacks or negative control differ from fixture')
    if not converted:
        if list(directory.glob('*.spv')):
            raise ValueError('native packed fetch unexpectedly dumped a conversion')
        return {'width': width, 'converted': False, 'readbacks': len(pixels), 'modules': []}
    paths = [directory / ('000-' + kind + '.spv') for kind in ('original', 'converted')]
    if set(directory.glob('*.spv')) != set(paths):
        raise ValueError('unexpected or missing packed shader dumps')
    if paths[0].read_bytes() != reference:
        raise ValueError('original shader differs from generated packed fixture')
    for path in paths:
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(path)], check=True,
                       capture_output=True, text=True)
    text = subprocess.check_output(['spirv-dis', '--raw-id', str(paths[1])], text=True)
    definitions = dict(re.findall(r'^\s*(%\d+) = (.+)$', text, re.M))
    inputs = []
    for variable in re.findall(r'^\s*OpDecorate (%\d+) Location 0$', text, re.M):
        declaration = definitions[variable].split()
        if declaration[0] == 'OpVariable' and declaration[2] == 'Input':
            inputs.append((variable, definitions[declaration[1]].split()[2]))
    if len(inputs) != 1:
        raise ValueError('expected one replacement input at location zero')
    variable, vector = inputs[0]
    vector_type = definitions[vector].split()
    if vector_type[0] != 'OpTypeVector' or vector_type[2] != '4' or definitions[vector_type[1]] != 'OpTypeFloat 32':
        raise ValueError('replacement fetch is not float32 vec4')
    loads = [identifier for identifier, instruction in definitions.items()
             if instruction == 'OpLoad ' + vector + ' ' + variable]
    if len(loads) != 1:
        raise ValueError('expected one load of replacement input')
    selectors = [2, 1, 0, 3][:width]
    rewrites = []
    for identifier, instruction in definitions.items():
        parts = instruction.split()
        if width == 1 and parts[0] == 'OpCompositeExtract' and parts[2:] == [loads[0], '2']:
            rewrites.append(identifier)
        elif width > 1 and parts[0] == 'OpVectorShuffle' and parts[2:] == [loads[0], loads[0], *map(str, selectors)]:
            result_type = definitions[parts[1]].split()
            if result_type == ['OpTypeVector', vector_type[1], str(width)]:
                rewrites.append(identifier)
    if len(rewrites) != 1:
        raise ValueError('missing unique packed component selection')
    stores = re.findall(r'OpStore (%\d+) ' + re.escape(rewrites[0]) + r'$', text, re.M)
    if len(stores) != 1:
        raise ValueError('missing packed component selection and initialization store')
    address = definitions[stores[0]].split()
    if address[0] != 'OpAccessChain' or definitions[address[1]].split()[1] != 'Private':
        raise ValueError('packed initialization does not use a Private pointer')
    root = definitions[address[2]].split()
    if root[0] != 'OpVariable' or root[2] != 'Private':
        raise ValueError('original input was not materialized as Private storage')
    return {'width': width, 'selectors': selectors, 'readbacks': len(pixels),
            'modules': [{'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                        for path in paths]}
