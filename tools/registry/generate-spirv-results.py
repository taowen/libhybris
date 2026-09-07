#!/usr/bin/env python3
"""Generate result-ID positions from the pinned Khronos SPIR-V grammar."""
import argparse
import hashlib
import json
from pathlib import Path

SHA256 = 'db8581272b63d232268094a47b68d18a0464fc911e06004d57419924fe660ba4'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('grammar', type=Path)
p.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'hybris/vulkan/compat/spirv_results.inc')
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
