#!/usr/bin/env python3
"""Generate the ICD physical-device resolver's scope table from pinned vk.xml."""
import argparse
import hashlib
from pathlib import Path
import xml.etree.ElementTree as ET

REVISION = '952f776f6573aafbb62ea717d871cd1d6816c387'
SHA256 = '1adbeb17c00be04771aac41bbb33850ff83c3503b42dcb2a25a96b4fd79c7e07'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('registry', type=Path)
parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] /
                    'hybris/vulkan/icd/physical_commands.inc')
args = parser.parse_args()
raw = args.registry.read_bytes()
if hashlib.sha256(raw).hexdigest() != SHA256:
    raise SystemExit('registry hash mismatch; use Vulkan-Headers v1.4.309 commit ' + REVISION)
commands = ET.fromstring(raw).findall('./commands/command')
types = {}
aliases = {}
for command in commands:
    if command.get('alias'):
        aliases[command.get('name')] = command.get('alias')
    else:
        types[command.findtext('proto/name')] = command.findtext('param/type')
for alias, target in aliases.items():
    while target in aliases:
        target = aliases[target]
    types[alias] = types[target]
names = sorted(name for name, kind in types.items() if kind == 'VkPhysicalDevice')
args.output.write_text('/* Generated from Vulkan-Headers ' + REVISION +
                       '; do not edit. */\n' +
                       ''.join('    "' + name + '",\n' for name in names))
print(len(names), 'physical-device commands')
