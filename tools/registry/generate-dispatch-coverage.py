#!/usr/bin/env python3
"""Generate Vulkan command scope/alias metadata and the baseline query table."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

REVISION = '952f776f6573aafbb62ea717d871cd1d6816c387'
SHA256 = '1adbeb17c00be04771aac41bbb33850ff83c3503b42dcb2a25a96b4fd79c7e07'
ROOT = Path(__file__).resolve().parents[2]


def vulkan(node, attribute='api'):
    return 'vulkan' in node.get(attribute, 'vulkan').split(',')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('registry', type=Path)
    args = parser.parse_args()
    raw = args.registry.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise SystemExit('registry hash mismatch; expected Vulkan-Headers ' + REVISION)
    registry = ET.fromstring(raw)
    commands = {node.get('name') or node.findtext('proto/name'): node
                for node in registry.findall('./commands/command') if vulkan(node)}
    providers = {}
    for feature in registry.findall('./feature') + registry.findall('./extensions/extension'):
        attribute = 'supported' if feature.tag == 'extension' else 'api'
        if not vulkan(feature, attribute):
            continue
        for require in feature.findall('require'):
            if not vulkan(require):
                continue
            for command in require.findall('command'):
                if vulkan(command):
                    providers.setdefault(command.get('name'), []).append({
                        'name': feature.get('name'),
                        'kind': feature.get('type', 'core'),
                        'version': feature.get('number'),
                        'depends': require.get('depends') or feature.get('depends'),
                        'platform': feature.get('platform'),
                        'provisional': feature.get('provisional') == 'true'})
    rows = []
    scopes = {'VkInstance': 'instance', 'VkPhysicalDevice': 'physical',
              'VkDevice': 'device', 'VkQueue': 'device', 'VkCommandBuffer': 'device'}
    for name in sorted(providers):
        node = commands[name]
        target = node
        while target.get('alias'):
            target = commands[target.get('alias')]
        first = target.findtext('param/type')
        scope = scopes.get(first, 'global')
        # GIPA is a resolver with an instance parameter and special NULL rules.
        if name == 'vkGetInstanceProcAddr':
            scope = 'gipa'
        core = [p['version'] for p in providers[name] if p['kind'] == 'core']
        version = min(core, key=lambda v: tuple(map(int, v.split('.')))) if core else None
        rows.append({'name': name, 'scope': scope, 'first_parameter': first,
                     'alias': node.get('alias'), 'core_version': version,
                     'providers': providers[name]})
    payload = ('{\n  "registry_revision": ' + json.dumps(REVISION) +
               ',\n  "registry_sha256": ' + json.dumps(SHA256) + ',\n  "commands": [\n' +
               ',\n'.join('    ' + json.dumps(row) for row in rows) + '\n  ]\n}\n')
    (ROOT / 'tools/registry/dispatch-coverage.json').write_text(payload)
    lines = ['/* Generated from Vulkan-Headers ' + REVISION + '; do not edit. */']
    for row in rows:
        link = 'LINK(' + row['name'] + ')' if row['core_version'] == '1.0' else 'NULL'
        lines.append('ENTRY(' + json.dumps(row['name']) + ', SCOPE_' + row['scope'].upper() +
                     ', ' + ('1' if row['core_version'] == '1.0' else '0') + ', ' + link + '),')
    (ROOT / 'tests/baseline/dispatch_commands.inc').write_text('\n'.join(lines) + '\n')
    print(len(rows), 'Vulkan commands;', sum(r['core_version'] == '1.0' for r in rows), 'core 1.0')


if __name__ == '__main__':
    main()
