#!/usr/bin/env python3
"""Generate result-ID and definite-literal positions from the pinned SPIR-V grammar."""
import argparse
import hashlib
import json
from pathlib import Path

SHA256 = 'db8581272b63d232268094a47b68d18a0464fc911e06004d57419924fe660ba4'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('grammar', type=Path)
p.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'hybris/vulkan/compat/spirv_results.inc')
p.add_argument('--literal-output', type=Path)
a = p.parse_args()
raw = a.grammar.read_bytes()
if hashlib.sha256(raw).hexdigest() != SHA256:
    raise SystemExit('Expected SPIR-V 1.6 revision 7 grammar SHA256 ' + SHA256)
grammar = json.loads(raw)
positions = {}
for instruction in grammar['instructions']:
    result = next((i + 1 for i, operand in enumerate(instruction.get('operands', [])) if operand['kind'] == 'IdResult'), 0)
    assert result in (0, 1, 2)
    opcode = instruction['opcode']
    assert opcode not in positions or positions[opcode] == result + 1
    positions[opcode] = result + 1
entries = [f'[{op}]={position}' for op, position in sorted(positions.items())]
text = '/* Generated from Khronos SPIR-V 1.6 revision 7 grammar.\n * Copyright 2014-2024 The Khronos Group Inc.; SPDX-License-Identifier: MIT\n * Grammar SHA256: ' + SHA256 + '\n * Zero is unknown; other values encode result word position plus one.\n */\n'
text += '\n'.join('    ' + ', '.join(entries[i:i+8]) + ',' for i in range(0, len(entries), 8)) + '\n'
a.output.write_text(text)

# Stop at variable-width or ambiguous operands. A missing classification keeps
# the runtime scan conservative; never classify a possible ID as a literal.
kinds = {kind['kind']: kind for kind in grammar['operand_kinds']}
literals = {}
for instruction in grammar['instructions']:
    position, mask, tail = 1, 0, 0
    operands = instruction.get('operands', [])
    for index, operand in enumerate(operands):
        kind = operand['kind']
        quantifier = operand.get('quantifier')
        last = index == len(operands) - 1
        if quantifier and not last:
            break
        if kind in ('LiteralInteger', 'LiteralExtInstInteger', 'LiteralSpecConstantOpInteger'):
            if quantifier == '*':
                tail = position
                break
            mask |= 1 << position
        elif kind == 'LiteralContextDependentNumber' and last:
            tail = position
            break
        elif kind.startswith('Id'):
            if quantifier == '*':
                break
        elif kinds[kind]['category'] in ('ValueEnum', 'BitEnum'):
            mask |= 1 << position
            if any(value.get('parameters') for value in kinds[kind]['enumerants']):
                break
        else:
            break
        position += 1
        if position >= 32:
            break
    opcode = instruction['opcode']
    assert opcode not in literals or literals[opcode] == (mask, tail)
    literals[opcode] = (mask, tail)
rows = [f'{{{opcode}, {tail}, 0x{mask:08x}}}' for opcode, (mask, tail) in sorted(literals.items()) if mask or tail]
header = '/* Generated from the same pinned grammar as spirv_results.inc.\n * Copyright 2014-2024 The Khronos Group Inc.; SPDX-License-Identifier: MIT\n * Fields: opcode, all-literal tail (zero if absent), fixed literal word mask.\n */\n'
(a.literal_output or a.output.with_name('spirv_literals.inc')).write_text(header + '\n'.join('    ' + ', '.join(rows[i:i+4]) + ',' for i in range(0, len(rows), 4)) + '\n')
