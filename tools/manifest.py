#!/usr/bin/env python3
"""Record and verify ELF provenance for a hybris install + glibc runtime."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

# Platform plugins that current source can emit. Anything else is stale staging.
ALLOWED_VULKAN_PLATFORMS = {'vulkanplatform_null.so', 'vulkanplatform_wayland.so'}
ALLOWED_EGL_PLATFORMS = {
    'eglplatform_fbdev.so',
    'eglplatform_hwcomposer.so',
    'eglplatform_null.so',
    'eglplatform_wayland.so',
    'eglplatform_x11.so',
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def build_id(path: Path) -> str | None:
    try:
        out = subprocess.check_output(['file', str(path)], text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    marker = 'BuildID[sha1]='
    if marker not in out:
        return None
    return out.split(marker, 1)[1].split(',', 1)[0].split()[0]


def is_elf(path: Path) -> bool:
    if not path.is_file():
        return False
    with path.open('rb') as handle:
        return handle.read(4) == b'\x7fELF'


def collect_elfs(root: Path) -> list[Path]:
    files = []
    for path in sorted(root.rglob('*')):
        if path.suffix in {'.la', '.pc'} or path.name.endswith('.py'):
            continue
        if is_elf(path):
            files.append(path)
    return files


def unexpected_platforms(hybris_lib: Path) -> list[str]:
    plugin_dir = hybris_lib / 'libhybris'
    if not plugin_dir.is_dir():
        return ['missing libhybris/ plugin directory']
    unexpected = []
    for path in sorted(plugin_dir.glob('*.so')):
        name = path.name
        if name.startswith('vulkanplatform_') and name not in ALLOWED_VULKAN_PLATFORMS:
            unexpected.append(str(path))
        if name.startswith('eglplatform_') and name not in ALLOWED_EGL_PLATFORMS:
            unexpected.append(str(path))
    return unexpected


def describe_tree(root: Path, label: str) -> list[dict]:
    entries = []
    for path in collect_elfs(root):
        entries.append({
            'path': str(path.relative_to(root)),
            'tree': label,
            'sha256': sha256_file(path),
            'build_id': build_id(path),
            'size': path.stat().st_size,
        })
    return entries


def verify_manifest(payload: dict, hybris_lib: Path, runtime: Path) -> None:
    """Verify the complete deployable ELF set, including SONAME aliases."""
    expected = {(e['tree'], e['path']): e['sha256'] for e in payload['elfs']}
    actual = {(label, str(path.relative_to(root))): sha256_file(path)
              for label, root in [('hybris', hybris_lib), ('runtime', runtime)]
              for path in collect_elfs(root)}
    if not expected or actual != expected:
        changed = sorted(k for k in actual.keys() | expected.keys()
                         if actual.get(k) != expected.get(k))
        raise ValueError('manifest does not match staged ELF files: ' + repr(changed))


def write_manifest(out: Path, payload: dict) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2) + '\n')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hybris-lib', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--source-commit')
    parser.add_argument('--source-dirty', action='store_true')
    parser.add_argument('--inputs', type=Path)
    parser.add_argument('--packages', type=Path)
    parser.add_argument('--headers')
    parser.add_argument('--compiler')
    parser.add_argument('--configure-args', default='')
    args = parser.parse_args()

    unexpected = unexpected_platforms(args.hybris_lib)
    if unexpected:
        print('stale or unknown platform plugins:', *unexpected, sep='\n  ', file=sys.stderr)
        return 2

    payload = {
        'source_commit': args.source_commit,
        'source_dirty': bool(args.source_dirty),
        'headers': args.headers,
        'compiler': args.compiler,
        'configure_args': [part for part in args.configure_args.split() if part],
        'hybris_lib': str(args.hybris_lib.resolve()),
        'runtime': str(args.runtime.resolve()),
        'elfs': describe_tree(args.hybris_lib, 'hybris') + describe_tree(args.runtime, 'runtime'),
        'note': 'Build script supplies source provenance; ELF hashes verify artifacts, not source-to-binary reproducibility.',
    }
    if args.inputs:
        payload['build_inputs'] = json.loads(args.inputs.read_text())
    if args.packages:
        payload['builder_packages'] = args.packages.read_text().splitlines()
    write_manifest(args.out, payload)
    print(args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
