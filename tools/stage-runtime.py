#!/usr/bin/env python3
"""Stage the transitive DT_NEEDED closure of installed glibc ELF libraries."""
import argparse
from collections import deque
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--hybris', type=Path, required=True)
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--search', type=Path, action='append', required=True)
p.add_argument('--require', action='append', default=[])
a = p.parse_args()
a.runtime.mkdir(parents=True, exist_ok=True)

def elf(path):
    if not path.is_file(): return False
    with path.open('rb') as f: return f.read(4) == b'\x7fELF'

def needed(path):
    output = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    return re.findall(r'\(NEEDED\).*\[([^]]+)\]', output)

installed = [path for path in a.hybris.rglob('*') if elf(path)]
provided = {path.name for path in installed}
pending = deque((name, '<required>') for name in a.require)
for path in installed:
    pending.extend((name, str(path)) for name in needed(path))
staged = set()
while pending:
    name, parent = pending.popleft()
    if name in provided or name in staged: continue
    if Path(name).name != name:
        raise SystemExit(f'non-SONAME dependency {name!r} in {parent}')
    source = next((directory / name for directory in a.search if elf(directory / name)), None)
    if source is None:
        raise SystemExit(f'missing runtime dependency {name} required by {parent}')
    destination = a.runtime / name
    shutil.copyfile(source, destination, follow_symlinks=True)
    shutil.copymode(source, destination, follow_symlinks=True)
    staged.add(name)
    pending.extend((dependency, str(source)) for dependency in needed(source))
print(f'Staged {len(staged)} runtime ELFs; checked {len(installed)} installed ELF paths')
