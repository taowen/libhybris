#!/usr/bin/env python3
"""Run source-built Mesa/Zink desktop GL through the standard loader and hybris ICD."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
import uuid
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial',required=True)
p.add_argument('--hal',required=True)
p.add_argument('--api-version',required=True,help='actual ICD version from baseline version discovery')
p.add_argument('--mali-loader-quirk',action='store_true')
p.add_argument('--profile',choices=['core32','compat32','core33'],default='core32')
p.add_argument('--display',help='X11 DISPLAY for GLX; omit for surfaceless EGL')
a=p.parse_args()
if not re.fullmatch(r'\d+\.\d+\.\d+',a.api_version):p.error('invalid API version')
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from manifest import verify_manifest
build=root/'tests/desktop-gl/build'
baseline=root/'tests/baseline/build'
manifest=json.loads((build/'manifest.json').read_text())
hybris_manifest=json.loads((baseline/'manifest.json').read_text())
verify_manifest(hybris_manifest,baseline/'install/usr/lib/hybris',baseline/'runtime')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
if sha(build/'probe')!=manifest['probe_sha256']:raise RuntimeError('probe hash mismatch')
for name,digest in manifest['runtime'].items():
    if sha(build/'runtime'/name)!=digest:raise RuntimeError('runtime hash mismatch: '+name)
out=build/'results'/(time.strftime('%Y%m%dT%H%M%S')+'-'+uuid.uuid4().hex[:8]);out.mkdir(parents=True)
stage=out/'stage';stage.mkdir()
shutil.copytree(build/'runtime',stage/'runtime')
shutil.copytree(baseline/'install/usr/lib/hybris',stage/'hybris',symlinks=True)
shutil.copy2(build/'probe',stage/'probe')
adb=[os.environ.get('ADB','adb'),'-s',a.serial]
def shell(command,**kwargs):return subprocess.run(adb+['shell',command],**kwargs)
remote='/data/local/tmp/hybris-desktop-gl-'+out.name
(stage/'driver.json').write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':remote+'/hybris/libhybris-vulkan-icd.so.0','api_version':a.api_version}}))
sdk=shell('getprop ro.build.version.sdk',capture_output=True,text=True,check=True).stdout.strip()
env={'EGL_PLATFORM':'surfaceless','MESA_LOADER_DRIVER_OVERRIDE':'zink','GALLIUM_DRIVER':'zink',
 'MESA_DEBUG':'1','VK_DRIVER_FILES':remote+'/driver.json','VK_LAYER_PATH':remote+'/layers',
 'HYBRIS_LINKER_DIR':remote+'/hybris/libhybris/linker','HYBRIS_ANDROID_SDK_VERSION':sdk,
 'HYBRIS_VULKAN_HAL':a.hal,'XDG_RUNTIME_DIR':remote}
if a.display:
    env.pop('MESA_LOADER_DRIVER_OVERRIDE')
    env.update(DISPLAY=a.display, HYBRIS_GLX_PROBE='1', LIBGL_KOPPER_DISABLE='true',
               LIBGL_DRIVERS_PATH=remote+'/runtime/dri')
if a.mali_loader_quirk:env['HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK']='1'
command='env '+' '.join(k+'='+shlex.quote(v) for k,v in env.items())+' ./runtime/ld-linux-aarch64.so.1 --library-path ./runtime:./hybris ./probe '+a.profile
record={'serial':a.serial,'command':command,'mesa':manifest,'hybris':hybris_manifest,
 'staged_elf_sha256':{str(x.relative_to(stage)):sha(x) for x in stage.rglob('*') if x.is_file()},'runner_sha256':sha(Path(__file__))}
archive=out/'stage.tar'
with tarfile.open(archive,'w') as t:t.add(stage,arcname='.')
code=2
try:
    shell('mkdir -p '+shlex.quote(remote),check=True)
    with archive.open('rb') as f:shell('cd '+shlex.quote(remote)+' && tar xf -',stdin=f,check=True)
    launch='cd '+shlex.quote(remote)+' && echo $$ > runner.pid && exec '+command
    try:
        r=shell('sh -c '+shlex.quote(launch),capture_output=True,timeout=60);code=r.returncode
        (out/'probe.log').write_bytes(r.stdout+r.stderr)
    except subprocess.TimeoutExpired as error:
        code=124;(out/'probe.log').write_bytes((error.stdout or b'')+(error.stderr or b''))
    for name in ['maps.txt','image.rgba']:
        r=shell('cat '+shlex.quote(remote+'/'+name),capture_output=True)
        if not r.returncode:(out/name).write_bytes(r.stdout)
finally:
    script='cd '+shlex.quote(remote)+' || exit; p=$(cat runner.pid); case "$p" in ""|*[!0-9]*) exit;; esac; [ "$(readlink /proc/$p/cwd)" = '+shlex.quote(remote)+' ] && kill -KILL "$p"; true'
    shell('sh -c '+shlex.quote(script),capture_output=True)
    shell('rm -rf '+shlex.quote(remote),check=True)
if code==0:
    try:
        packed=re.findall(r'^PACKED_DRAW signed=(\d) normalized=(\d) bgra=(\d) divisor=(\d) bad=(\d+) error=0x([0-9a-f]+)$', (out/'probe.log').read_text(), re.MULTILINE)
        cases={(str(s),str(n),str(b),str(d)) for s in range(2) for n in range(2) for b in range(2 if n else 1) for d in (1,2)}
        if len(packed)!=12 or {row[:4] for row in packed}!=cases or any(row[4:]!=('0','0') for row in packed):
            raise ValueError('packed vertex draw matrix incomplete or failed')
        record['packed_vertex_cases']=12
        expected=b''.join(bytes((255,0,0,255) if x<8 else (0,255,0,255)) for y in range(16) for x in range(16))
        if (out/'image.rgba').read_bytes()!=expected:raise ValueError('full image mismatch')
        maps=(out/'maps.txt').read_text()
        for name in ['runtime/libgallium-', 'runtime/libvulkan.so.1', 'hybris/libhybris-vulkan-icd.so', 'vulkan.'+('mali' if 'mali' in a.hal else 'adreno')+'.so']:
            if name not in maps:raise ValueError('missing mapped backend '+name)
        if a.display and 'runtime/libGL.so.1' not in maps:raise ValueError('missing mapped GLX frontend')
        record['evidence']='256 exact pixels; Mesa, standard loader, hybris ICD and selected vendor mapped'
    except (OSError,ValueError) as error:
        code=2;record['evidence_error']=str(error)
record['exit_code']=code
record['status']='PASS' if code==0 else 'UNSUPPORTED' if code==3 else 'TIMEOUT' if code in (124,142) else 'CRASH' if code>=128 else 'FAIL'
(out/'result.json').write_text(json.dumps(record,indent=2))
print((out/'probe.log').read_text(errors='replace'));print(out,record['status'])
raise SystemExit(0 if code==0 else 1)
