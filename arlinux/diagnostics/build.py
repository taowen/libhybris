#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Internal APK stage of arlinux/build-bionic-vulkan.py; not another builder."""
from pathlib import Path
import hashlib
import json
import os
import secrets
import shutil
import struct
import subprocess
import tempfile
import zipfile


def build_diagnostics(source, output, compiler, flags, layer, ndk):
    recipe = Path(__file__).resolve().parent
    sdk = Path(os.environ['ANDROID_HOME'])
    platform = sdk / 'platforms/android-34/android.jar'
    tools = sdk / 'build-tools/36.0.0'
    if not platform.is_file() or not (tools/'aapt2').is_file():
        manager = sdk / 'cmdline-tools/latest/bin/sdkmanager'
        subprocess.run([str(manager), 'platforms;android-34', 'build-tools;36.0.0'], check=True)
    target = output / 'diagnostics'
    target.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='diagnostics-', dir=output) as temporary:
        work = Path(temporary)
        assets = work/'assets'
        assets.mkdir()
        shader_tools = ndk/'shader-tools/linux-x86_64'
        for name in ('sample.comp', 'robust_null.comp', 'robust_buffer.comp', 'robust_image.comp', 'viewport.vert', 'viewport.frag', 'multidraw.vert', 'multidraw.frag', 'multidraw_drawid.vert', 'dual_single.frag', 'dual_pair.frag', 'dual_extra.frag', 'dual_full.frag', 'dual_array.frag'):
            filename = name
            name = name.replace('.', '_') if not name.endswith('.comp') else name[:-5]
            spv = work/(name+'.spv')
            subprocess.run([str(shader_tools/'glslc'), '--target-env=vulkan1.1', '-O',
                            str(recipe/filename), '-o', str(spv)], check=True)
            subprocess.run([str(shader_tools/'spirv-val'), '--target-env', 'vulkan1.1', str(spv)], check=True)
            words = struct.unpack('<'+'I'*(spv.stat().st_size//4), spv.read_bytes())
            (work/(name+'.inc')).write_text('static const uint32_t '+name+'_spv[] = {'+
                ','.join(hex(word) for word in words)+'};\n')
        spec = work/'spec_constants.spv'
        subprocess.run([str(shader_tools/'spirv-as'), '--target-env', 'vulkan1.1',
                        str(recipe/'spec_constants.spvasm'), '-o', str(spec)], check=True)
        subprocess.run([str(shader_tools/'spirv-val'), '--target-env', 'vulkan1.1', str(spec)], check=True)
        reference = list(struct.unpack('<'+'I'*(spec.stat().st_size//4), spec.read_bytes()))
        application = list(reference)
        at = 5
        while at < len(application):
            length, opcode = application[at] >> 16, application[at] & 0xffff
            # Historical application form, consumed only by the enabled repair
            # route. Native/disabled routes use the validator-checked reference.
            if opcode == 51:
                application[at] = (length << 16) | 44
            at += length
        (work/'spec_constants.inc').write_text(''.join(
            'static const uint32_t '+name+'[] = {'+','.join(hex(word) for word in words)+'};\n'
            for name, words in (('spec_reference', reference), ('spec_application', application))))
        strip = ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
        sha = lambda file: hashlib.sha256(file.read_bytes()).hexdigest()
        probes = {}
        executables = []
        for name in ('bc', 'robustness', 'spec_constants', 'multiviewport', 'multidraw', 'dual_source'):
            executable = work/('libhybris_'+name+'_probe.so')
            subprocess.run([str(compiler)+'++', *flags, '-std=gnu++17', '-I'+str(work),
                            '-pie', '-static-libstdc++', str(recipe/(name+'_probe.cpp')), '-ldl', '-llog',
                            '-Wl,--no-undefined,-z,max-page-size=16384', '-o', str(executable)], check=True)
            subprocess.run([str(strip), '--strip-debug', str(executable)], check=True)
            executables.append(executable)
            probes[name] = dict(executable=executable.name, sha256=sha(executable))
        commit = subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'], text=True).strip()
        identity = dict(libhybrisCommit=commit, mode='bionic', minSdk=29,
                        androidPlatform=34, androidBuildTools='36.0.0',
                        compatibilityLayerSha256=sha(layer), probes=probes)
        (assets/'build.json').write_text(json.dumps(identity, indent=2)+'\n')
        licenses = assets/'licenses'
        licenses.mkdir()
        shutil.copyfile(source/'LICENSE.GPL3', licenses/'LICENSE.GPL3')
        for name in ('MALIBRIDGE.md','NOTICE.MaliBridge','LICENSE.bcdec','LICENSE.PanVK'):
            shutil.copyfile(source/'hybris/vulkan/compat/shaders'/name, licenses/name)
        classes = work/'classes'
        classes.mkdir()
        java_home = Path(os.environ['JAVA_HOME'])
        subprocess.run([str(java_home/'bin/javac'), '--release','8','-classpath',str(platform),
                        '-d',str(classes), *map(str,sorted((recipe/'java').rglob('*.java')))], check=True)
        jar = work/'classes.jar'
        subprocess.run([str(java_home/'bin/jar'),'cf',str(jar),'-C',str(classes),'.'], check=True)
        dex = work/'dex'
        dex.mkdir()
        subprocess.run([str(tools/'d8'),'--release','--min-api','29','--lib',str(platform),
                        '--output',str(dex),str(jar)], check=True)
        apk = work/'unsigned.apk'
        resources = work/'resources.zip'
        subprocess.run([str(tools/'aapt2'),'compile','--dir',str(recipe/'res'),'-o',str(resources)], check=True)
        subprocess.run([str(tools/'aapt2'),'link','-I',str(platform),'--manifest',str(recipe/'AndroidManifest.xml'),
                        '-A',str(assets),'-o',str(apk),str(resources)], check=True)
        with zipfile.ZipFile(apk,'a',compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(dex/'classes.dex','classes.dex')
            archive.write(layer,'lib/arm64-v8a/'+layer.name)
            for executable in executables:
                archive.write(executable,'lib/arm64-v8a/'+executable.name)
        aligned = work/'aligned.apk'
        subprocess.run([str(tools/'zipalign'),'-P','16','-f','4',str(apk),str(aligned)], check=True)
        # A disposable diagnostics-only key: never use or distribute the
        # production application's signing key. Rebuilds may require reinstall.
        key = work/'diagnostics.jks'
        env = dict(os.environ, HYBRIS_DIAGNOSTIC_KEY_PASS=secrets.token_hex(24))
        subprocess.run([str(java_home/'bin/keytool'),'-genkeypair','-keystore',str(key),'-alias','diagnostics',
                        '-storepass:env','HYBRIS_DIAGNOSTIC_KEY_PASS','-keypass:env','HYBRIS_DIAGNOSTIC_KEY_PASS',
                        '-keyalg','RSA','-keysize','2048','-validity','3650',
                        '-dname','CN=libhybris disposable diagnostics'], env=env, check=True)
        final = target/'libhybris-diagnostics.apk'
        subprocess.run([str(tools/'apksigner'),'sign','--ks',str(key),'--ks-key-alias','diagnostics',
                        '--ks-pass','env:HYBRIS_DIAGNOSTIC_KEY_PASS','--key-pass','env:HYBRIS_DIAGNOSTIC_KEY_PASS',
                        '--v2-signing-enabled','true',
                        '--out',str(final),str(aligned)], env=env, check=True)
        subprocess.run([str(tools/'apksigner'),'verify','--verbose',str(final)], check=True)
        subprocess.run([str(tools/'zipalign'),'-c','-P','16','4',str(final)], check=True)
        identity.update(apkSha256=sha(final), sizeBytes=final.stat().st_size,
                        package='org.arlinux.hybrisdiagnostics',
                        sourceUrl='https://github.com/taowen/libhybris/tree/'+commit)
        (target/'libhybris-diagnostics.json').write_text(json.dumps(identity, indent=2)+'\n')
    print(final)
