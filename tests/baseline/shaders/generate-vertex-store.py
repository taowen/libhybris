#!/usr/bin/env python3
"""Regenerate the vertex-store probe's checked-in SPIR-V."""
from pathlib import Path
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='hybris-vertex-store-') as directory:
    output = Path(directory) / 'vertex-store.spv'
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.1',
                    str(root / 'vertex-store.vert'), '-o', str(output)], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(output)], check=True)
    data = output.read_bytes()
words = struct.unpack('<%dI' % (len(data) // 4), data)
(root / 'vertex-store.inc').write_text(
    '/* Generated from vertex-store.vert with glslangValidator -V --target-env vulkan1.1. */\n'
    'static const uint32_t kVertexStore[] = {\n' + ''.join(
        '    ' + ', '.join('0x%08xu' % word for word in words[i:i + 8]) + ',\n'
        for i in range(0, len(words), 8)) + '};\n')
