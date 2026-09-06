#!/usr/bin/env python3
"""Fingerprint the snapshots supplied to the compiler, before generated files."""
import argparse
import hashlib
import json
from pathlib import Path


def tree_identity(root):
    root = root.resolve()
    entries = []
    for path in sorted(root.rglob('*')):
        if '.git' in path.relative_to(root).parts:
            continue
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            # The snapshot must contain every input it names.
            if not path.resolve().is_relative_to(root):
                raise ValueError(f'input symlink escapes snapshot: {path}')
            entries.append({'path': name, 'link': str(path.readlink())})
        elif path.is_file():
            entries.append({'path': name,
                            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                            'executable': bool(path.stat().st_mode & 0o111)})
    encoded = json.dumps(entries, sort_keys=True, separators=(',', ':')).encode()
    return {'sha256': hashlib.sha256(encoded).hexdigest(), 'files': entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--recipe', type=Path, required=True)
    parser.add_argument('--image-id', required=True)
    parser.add_argument('--build-script', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    payload = {
        'source': tree_identity(args.source),
        'headers': tree_identity(args.headers),
        'builder': {'image_id': args.image_id,
                    'repository_recipe_sha256': hashlib.sha256(args.recipe.read_bytes()).hexdigest(),
                    'build_script_sha256': hashlib.sha256(args.build_script.read_bytes()).hexdigest()},
    }
    args.out.write_text(json.dumps(payload, indent=2) + '\n')


if __name__ == '__main__':
    main()
