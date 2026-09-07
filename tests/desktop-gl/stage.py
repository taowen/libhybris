"""Run inside the cross-builder after Mesa install to collect ELF dependencies."""
from pathlib import Path
import re
import shutil
import subprocess
root = Path('/work/third_party/libhybris/tests/desktop-gl/build')
lib = root / 'runtime'
lib.mkdir(exist_ok=True)
installed = root / 'install/usr/lib'
def needed(path):
    data = subprocess.check_output(['aarch64-linux-gnu-readelf', '-d', str(path)], text=True)
    return re.findall(r'\(NEEDED\).*\[(.*?)\]', data)
search = [installed, Path('/usr/lib/aarch64-linux-gnu'), Path('/lib/aarch64-linux-gnu')]
pending = ['libEGL.so.1', 'libgallium-26.3.0-devel.so', 'libvulkan.so.1', 'ld-linux-aarch64.so.1']
seen = set()
while pending:
    name = pending.pop()
    if name in seen: continue
    source = next((d / name for d in search if (d / name).is_file()), None)
    if source is None: raise RuntimeError('missing ELF dependency: ' + name)
    shutil.copy2(source, lib / name)
    seen.add(name)
    pending.extend(needed(source))
print('runtime:', ', '.join(sorted(seen)))
