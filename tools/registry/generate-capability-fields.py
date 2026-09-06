#!/usr/bin/env python3
"""Generate named core capability fields from the pinned Vulkan registry."""
import argparse
import hashlib
from pathlib import Path
import re
import xml.etree.ElementTree as ET

REVISION = '952f776f6573aafbb62ea717d871cd1d6816c387'
SHA256 = '1adbeb17c00be04771aac41bbb33850ff83c3503b42dcb2a25a96b4fd79c7e07'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('registry', type=Path)
a = p.parse_args()
raw = a.registry.read_bytes()
if hashlib.sha256(raw).hexdigest() != SHA256:
    raise SystemExit('registry hash mismatch; expected ' + REVISION)
r = ET.fromstring(raw)
lines = ['/* Generated from Vulkan-Headers ' + REVISION + '; do not edit. */']
property_checks = [lines[0]]
for struct, prefix, expr in [('VkPhysicalDeviceFeatures', 'features', 'features'),
                              ('VkPhysicalDeviceLimits', 'limits', 'props.limits'),
                              ('VkPhysicalDeviceSparseProperties', 'sparse', 'props.sparseProperties')]:
    for member in r.find("./types/type[@name='" + struct + "']").findall('member'):
        name = member.findtext('name')
        kind = member.findtext('type')
        array = re.search(r'\[(\d+)\]', member.find('name').tail or '')
        for suffix in (['[' + str(i) + ']' for i in range(int(array[1]))] if array else ['']):
            field = name + suffix
            macro = 'CAP_FLOAT' if kind == 'float' else 'CAP_SIGNED' if kind == 'int32_t' else 'CAP_UINT'
            lines.append(f'{macro}("{prefix}.{field}", {expr}.{field});')
            if expr.startswith('props.'):
                property_checks.append('COMPARE_PROPERTY(' + expr[6:] + '.' + field + ');')
out = Path(__file__).resolve().parents[2] / 'tests/baseline/capability_fields.inc'
out.write_text('\n'.join(lines) + '\n')
print(len(lines)-1, 'named capability values')
(out.parent / 'property_compare.inc').write_text('\n'.join(property_checks) + '\n')

features = r.find("./types/type[@name='VkPhysicalDeviceFeatures']")
checks = ['/* Generated from Vulkan-Headers ' + REVISION + '; do not edit. */']
for member in features.findall('member'):
    name = member.findtext('name')
    checks.append('COMPARE_FEATURE(' + name + ');')
(out.parent / 'feature_compare.inc').write_text('\n'.join(checks) + '\n')
