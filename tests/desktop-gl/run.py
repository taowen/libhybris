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
p.add_argument('--backend',choices=['hybris','turnip'],default='hybris')
p.add_argument('--hal',help='vendor HAL path, required for the hybris backend')
p.add_argument('--api-version',required=True,help='actual ICD version from baseline version discovery')
p.add_argument('--mali-loader-quirk',action='store_true')
p.add_argument('--profile',choices=['core32','compat32','core33'],default='core32')
p.add_argument('--display',help='X11 DISPLAY for GLX; omit for surfaceless EGL')
p.add_argument('--vertex-prepass',action='store_true',help='exercise explicit compute vertex prepass feasibility workload')
p.add_argument('--vertex-draws',action='store_true',help='run ordinary procedural, attribute, indexed and multidraw GL cases')
p.add_argument('--validation-layer',type=Path,help='glibc AArch64 Khronos validation layer')
p.add_argument('--validation-manifest',type=Path,help='matching original validation JSON')
a=p.parse_args()
if a.backend=='hybris' and not a.hal:p.error('--hal is required for hybris')
if a.backend=='turnip' and (a.hal or a.mali_loader_quirk):p.error('Turnip does not use a vendor HAL or Mali loader quirk')
if bool(a.validation_layer)!=bool(a.validation_manifest):p.error('provide both validation layer and manifest')
if not re.fullmatch(r'\d+\.\d+\.\d+',a.api_version):p.error('invalid API version')
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from manifest import verify_manifest
build=root/'tests/desktop-gl/build'
baseline=root/'tests/baseline/build'
manifest=json.loads((build/'manifest.json').read_text())
hybris_manifest=None
if a.backend=='hybris':
    hybris_manifest=json.loads((baseline/'manifest.json').read_text())
    verify_manifest(hybris_manifest,baseline/'install/usr/lib/hybris',baseline/'runtime')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
if sha(build/'probe')!=manifest['probe_sha256']:raise RuntimeError('probe hash mismatch')
for name,digest in manifest['runtime'].items():
    if sha(build/'runtime'/name)!=digest:raise RuntimeError('runtime hash mismatch: '+name)
out=build/'results'/(time.strftime('%Y%m%dT%H%M%S')+'-'+uuid.uuid4().hex[:8]);out.mkdir(parents=True)
stage=out/'stage';stage.mkdir()
shutil.copytree(build/'runtime',stage/'runtime')
if a.backend=='hybris':
    shutil.copytree(baseline/'install/usr/lib/hybris',stage/'hybris',symlinks=True)
    # The ICD has dependencies beyond Mesa's closure (for example wayland-egl).
    # Keep Mesa's selected runtime for shared SONAMEs and add the verified ICD
    # dependencies that are absent from it.
    for source in (baseline/'runtime').iterdir():
        destination=stage/'runtime'/source.name
        if not destination.exists():shutil.copy2(source,destination)
shutil.copy2(build/'probe',stage/'probe')
if a.validation_layer:
    (stage/'layers').mkdir()
    shutil.copy2(a.validation_layer,stage/'layers/libVkLayer_khronos_validation.so')
    layer=json.loads(a.validation_manifest.read_text())
    if layer['layer']['name']!='VK_LAYER_KHRONOS_validation':raise ValueError('expected Khronos validation manifest')
    layer['layer']['library_path']='./libVkLayer_khronos_validation.so'
    (stage/'layers/validation.json').write_text(json.dumps(layer))
    (stage/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\nkhronos_validation.report_flags = error,warn,info\n')
adb=[os.environ.get('ADB','adb'),'-s',a.serial]
def shell(command,**kwargs):return subprocess.run(adb+['shell',command],**kwargs)
remote='/data/local/tmp/hybris-desktop-gl-'+out.name
icd=remote+('/runtime/libvulkan_freedreno.so' if a.backend=='turnip' else '/hybris/libhybris-vulkan-icd.so.0')
(stage/'driver.json').write_text(json.dumps({'file_format_version':'1.0.0','ICD':{'library_path':icd,'api_version':a.api_version}}))
sdk=shell('getprop ro.build.version.sdk',capture_output=True,text=True,check=True).stdout.strip()
env={'EGL_PLATFORM':'surfaceless','MESA_LOADER_DRIVER_OVERRIDE':'zink','GALLIUM_DRIVER':'zink',
 'MESA_DEBUG':'1','VK_DRIVER_FILES':remote+'/driver.json','VK_LAYER_PATH':remote+'/layers',
 'XDG_RUNTIME_DIR':remote}
if a.backend=='hybris':env.update(HYBRIS_LINKER_DIR=remote+'/hybris/libhybris/linker',HYBRIS_ANDROID_SDK_VERSION=sdk,HYBRIS_VULKAN_HAL=a.hal)
if a.vertex_prepass:env['HYBRIS_VERTEX_PREPASS']='1'
if a.vertex_draws:
    env['HYBRIS_PROCEDURAL_VERTEX']='1'
    env['ZINK_DEBUG']='spirv'
if a.validation_layer:
    env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
    env['VK_LAYER_ENABLES']='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
    env['VK_LAYER_SETTINGS_PATH']=remote
    env['ZINK_DEBUG']=env.get('ZINK_DEBUG','')+',validation'
if a.display:
    env.pop('MESA_LOADER_DRIVER_OVERRIDE')
    env.update(DISPLAY=a.display, HYBRIS_GLX_PROBE='1', LIBGL_KOPPER_DISABLE='true',
               LIBGL_DRIVERS_PATH=remote+'/runtime/dri')
if a.mali_loader_quirk:env['HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK']='1'
command='env '+' '.join(k+'='+shlex.quote(v) for k,v in env.items())+' ./runtime/ld-linux-aarch64.so.1 --library-path ./runtime:./hybris ./probe '+a.profile
record={'backend':a.backend,'serial':a.serial,'command':command,'mesa':manifest,'hybris':hybris_manifest,
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
    artifacts=['maps.txt','image.rgba']
    if a.vertex_draws:artifacts += [f'procedural-{phase}.rgba' for phase in range(3)] + [f'attributes-{phase}.rgba' for phase in range(11)] + [f'indexed-{phase}.rgba' for phase in range(9)] + [f'resources-{phase}.rgba' for phase in range(6)] + [f'multidraw-{phase}.rgba' for phase in range(5)]
    if a.vertex_prepass:artifacts += [f'vertex-prepass-{phase}.rgba' for phase in range(3)]
    if a.vertex_draws:
        listing=shell('cd '+shlex.quote(remote)+' && ls dump*.spv',capture_output=True,text=True)
        artifacts += [name for name in listing.stdout.splitlines() if re.fullmatch(r'dump[0-9]+\.spv', name)]
    for name in artifacts:
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
        if a.vertex_prepass:
            for phase in range(3):
                marker=f'VERTEX_PREPASS phase={phase} PASS bad_pixels=0 guards=0 count={6*(phase+1)} ids=63 error=0x0'
                if marker not in (out/'probe.log').read_text():raise ValueError('vertex prepass failed or missing')
            record['vertex_prepass']='PASS'
        expected=b''.join(bytes((255,0,0,255) if x<8 else (0,255,0,255)) for y in range(16) for x in range(16))
        if (out/'image.rgba').read_bytes()!=expected:raise ValueError('full image mismatch')
        if a.vertex_prepass:
            for phase in range(3):
                wanted=bytes((255,0,255,255))*256 if phase==1 else expected
                if (out/f'vertex-prepass-{phase}.rgba').read_bytes()!=wanted:raise ValueError('prepass image mismatch')
        if a.vertex_draws:
            attributes=bytes(c for y in range(16) for x in range(16) for c in (255*((x//4)&1),255*((x//8)&1),0,255))
            for phase in range(11):
                marker=f'ATTRIBUTE_VERTEX phase={phase} PASS bad_pixels=0 error=0x0'
                if marker not in (out/'probe.log').read_text() or (out/f'attributes-{phase}.rgba').read_bytes()!=attributes:
                    raise ValueError('attribute vertex image failed or missing')
            for phase in range(11):
                if f'ATTRIBUTE_COMPUTE_RESTORE phase={phase} PASS rgba=17,34,51,255' not in (out/'probe.log').read_text():
                    raise ValueError('compute sampler restoration failed or missing')
            for phase in range(9):
                wanted=bytes(c for y in range(16) for x in range(16) for c in ((0,0,255,255) if phase==6 or (phase==8 and x+y>15) or x+y==15 else (255,0,0,255) if x+y<15 else (0,255,0,255)))
                if f'INDEXED_VERTEX phase={phase} PASS bad_pixels=0 error=0x0' not in (out/'probe.log').read_text() or (out/f'indexed-{phase}.rgba').read_bytes()!=wanted:
                    raise ValueError('indexed vertex image failed or missing')
            record['indexed_vertex_cases']=9
            for phase in range(5):
                wanted=bytes(c for y in range(16) for x in range(16) for c in ((255,0,0,255) if x<5 else (0,0,0,255) if phase==4 else (0,255,0,255) if x<10 else (0,0,255,255)))
                if f'MULTIDRAW_VERTEX phase={phase} PASS bad_pixels=0 error=0x0' not in (out/'probe.log').read_text() or (out/f'multidraw-{phase}.rgba').read_bytes()!=wanted:
                    raise ValueError('multidraw vertex image failed or missing')
            record['multidraw_vertex_cases']=5
            for phase in range(6):
                if f'RESOURCE_VERTEX phase={phase} PASS bad_pixels=0 error=0x0' not in (out/'probe.log').read_text() or (out/f'resources-{phase}.rgba').read_bytes()!=expected:
                    raise ValueError('resource pressure image failed or missing')
            record['resource_vertex_cases']=6
            record['attribute_vertex_cases']=11
            record['compute_sampler_restore_cases']=11
            for phase in range(3):
                marker=f'PROCEDURAL_VERTEX phase={phase} PASS bad_pixels=0 error=0x0'
                if marker not in (out/'probe.log').read_text():raise ValueError('procedural vertex case failed or missing')
                wanted=bytes((255,0,255,255))*256 if phase==1 else expected
                if (out/f'procedural-{phase}.rgba').read_bytes()!=wanted:raise ValueError('procedural image mismatch')
        maps=(out/'maps.txt').read_text()
        if a.validation_layer:
            if 'layers/libVkLayer_khronos_validation.so' not in maps:raise ValueError('validation layer not mapped')
            if re.search(r'Validation Error|VUID-|SYNC-HAZARD', (out/'probe.log').read_text()):raise ValueError('Vulkan validation reported errors')
            if 'Current Enables: VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT' not in (out/'probe.log').read_text():raise ValueError('SyncVal activation not confirmed')
            record['synchronization_validation']='enabled and no reported errors'
            record['validation_layer_sha256']=sha(a.validation_layer)
            record['validation_manifest_sha256']=sha(a.validation_manifest)
        backends=['runtime/libvulkan_freedreno.so'] if a.backend=='turnip' else ['hybris/libhybris-vulkan-icd.so', 'vulkan.'+('mali' if 'mali' in a.hal else 'adreno')+'.so']
        for name in ['runtime/libgallium-', 'runtime/libvulkan.so.1']+backends:
            if name not in maps:raise ValueError('missing mapped backend '+name)
        if a.display and 'runtime/libGL.so.1' not in maps:raise ValueError('missing mapped GLX frontend')
        record['evidence']='256 exact pixels; Mesa, standard loader and selected '+a.backend+' backend mapped'
    except (OSError,ValueError) as error:
        code=2;record['evidence_error']=str(error)
record['exit_code']=code
record['status']='PASS' if code==0 else 'UNSUPPORTED' if code==3 else 'TIMEOUT' if code in (124,142) else 'CRASH' if code>=128 else 'FAIL'
(out/'result.json').write_text(json.dumps(record,indent=2))
print((out/'probe.log').read_text(errors='replace'));print(out,record['status'])
raise SystemExit(0 if code==0 else 1)
