#!/usr/bin/env python3
"""Build a disposable compositor APK from an explicitly supplied backend APK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--backend-apk', type=Path, required=True)
p.add_argument('--backend-library', type=Path, help='replace libanlabwc.so with a locally rebuilt backend; dependencies/assets still come from the APK')
p.add_argument('--x11-build', type=Path, default=Path(__file__).resolve().parents[2] / 'x11/build', help='built Xwayland bundle to include in the test APK')
p.add_argument('--sdk', type=Path, default=Path(os.environ.get('ANDROID_HOME', str(Path.home() / 'Android/Sdk'))))
p.add_argument('--ndk', default='29.0.14206865')
p.add_argument('--build-tools', default='36.0.0')
a = p.parse_args()
source = Path(__file__).resolve().parent
out = source.parent / 'build/compositor'
work = out / 'work'
if work.exists(): shutil.rmtree(work)
work.mkdir(parents=True)
libs = work / 'lib/arm64-v8a'; libs.mkdir(parents=True)
def run(*command): subprocess.run([str(x) for x in command], check=True)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
with zipfile.ZipFile(a.backend_apk) as archive:
    names = {Path(n).name: n for n in archive.namelist() if n.startswith('lib/arm64-v8a/') and n.endswith('.so')}
    pending = ['libanlabwc.so']; copied = set(); external = set()
    while pending:
        name = pending.pop()
        if name in copied: continue
        if name not in names: raise ValueError('missing backend library ' + name)
        path = libs / name
        data = a.backend_library.read_bytes() if name == 'libanlabwc.so' and a.backend_library else archive.read(names[name])
        path.write_bytes(data); copied.add(name)
        needed = subprocess.check_output(['patchelf', '--print-needed', str(path)], text=True).splitlines()
        for dependency in needed:
            if dependency in names: pending.append(dependency)
            else: external.add(dependency)
    for name in archive.namelist():
        if name.startswith('assets/xkb/') and not name.endswith('/'):
            path = work / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(archive.read(name))
if not (work / 'assets/xkb').is_dir(): raise ValueError('backend APK has no xkb assets')
# One fixed server bundle in the APK; only the glibc client/ICD are deployed
# per run. Verify every input before packaging, including transitive ELF deps.
x11 = json.loads((a.x11_build / 'manifest.json').read_text())
server = work / 'assets/x11'; server.mkdir(parents=True)
server_files = {}
for name, digest in x11['files'].items():
    if not name.startswith('server/'): continue
    if Path(name).parent != Path('server'): raise ValueError('invalid server manifest path')
    original = a.x11_build / name
    if sha(original) != digest: raise ValueError('Xwayland bundle hash mismatch: ' + name)
    shutil.copy2(original, server / original.name)
    server_files[original.name] = digest
if 'Xwayland' not in server_files: raise ValueError('bundle has no Xwayland')
server_manifest = {'files': server_files, 'build': x11['server_inputs']}
(server / 'manifest.json').write_text(json.dumps(server_manifest, indent=2) + '\n')
bt = a.sdk / 'build-tools' / a.build_tools
android = a.sdk / 'platforms/android-35/android.jar'
cc = a.sdk / 'ndk' / a.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang'
run(cc, '-shared', '-fPIC', '-Wall', '-Wextra', source / 'host.c', '-landroid', '-llog', '-ldl', '-o', libs / 'libwsihost.so')
classes = work / 'classes'; classes.mkdir()
run('javac', '-source', '8', '-target', '8', '-bootclasspath', android, '-d', classes, source / 'CompositorActivity.java')
run(bt / 'd8', '--lib', android, '--output', work, *classes.rglob('*.class'))
run(bt / 'aapt2', 'link', '-I', android, '--manifest', source / 'AndroidManifest.xml', '-A', work / 'assets', '-o', work / 'base.apk')
with zipfile.ZipFile(work / 'base.apk', 'a', compression=zipfile.ZIP_STORED) as archive:
    archive.write(work / 'classes.dex', 'classes.dex')
    for path in sorted(libs.glob('*.so')): archive.write(path, 'lib/arm64-v8a/' + path.name)
key = out / 'debug.keystore'
if not key.exists():
    run('keytool', '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass', 'android', '-alias', 'androiddebugkey', '-keyalg', 'RSA', '-validity', '10000', '-dname', 'CN=Hybris WSI Test')
run(bt / 'zipalign', '-f', '4', work / 'base.apk', out / 'unsigned.apk')
run(bt / 'apksigner', 'sign', '--ks', key, '--ks-pass', 'pass:android', '--out', out / 'hybris-wsi-test.apk', out / 'unsigned.apk')
run(bt / 'apksigner', 'verify', out / 'hybris-wsi-test.apk')
(out / 'manifest.json').write_text(json.dumps({'backend_apk': str(a.backend_apk.resolve()), 'backend_sha256': sha(a.backend_apk),
    'backend_library_override': ({'path': str(a.backend_library.resolve()), 'sha256': sha(a.backend_library)} if a.backend_library else None),
    'xwayland': server_manifest, 'apk_sha256': sha(out / 'hybris-wsi-test.apk'), 'libraries': {x.name: sha(x) for x in libs.glob('*.so')},
    'external_dependencies': sorted(external), 'sources': {x.name: sha(x) for x in source.iterdir() if x.is_file()},
    'ndk': a.ndk, 'build_tools': a.build_tools,
    'note': 'APK dependencies with optional local backend replacement; hashes identify inputs, not source reproducibility.'}, indent=2))
print(out / 'hybris-wsi-test.apk')
