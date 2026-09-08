"""XCB/Xlib probe staging and evidence; invoked only by run.py."""
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time
from manifest import sha256_file
from host import stage_runtime
from screen_evidence import verify_epoch

ROOT = Path(__file__).resolve().parents[2]

def run(a, host, out):
    started = time.monotonic()
    package = a.package
    shell, app, prop = host.shell, host.app, host.prop
    probe = json.loads((a.probe / 'manifest.json').read_text())
    for name in ('probe-xcb', 'x11-session'):
        if sha256_file(a.probe / name) != probe['files'][name]: raise ValueError('probe hash mismatch: ' + name)
    stage = out / 'stage'
    hybris = stage_runtime(a.build, stage)
    for name in ('probe-xcb', 'x11-session'): shutil.copy2(a.probe / name, stage / name)
    for name, digest in probe['runtime'].items():
        source = a.probe / 'runtime' / name
        if sha256_file(source) != digest: raise ValueError('client runtime hash mismatch: ' + name)
        destination = stage / 'glibc' / name
        if destination.exists():
            if sha256_file(destination) != digest: raise ValueError('client/hybris runtime conflict: ' + name)
        else:
            shutil.copy2(source, destination)
    files = host.files
    remote = files + '/hybris-wsi-' + out.parent.name + '-' + out.name
    libraries = './standard:./hybris:./glibc'
    env = {'DISPLAY': a.display,
           'HYBRIS_X11_TRACE': '1', 'HYBRIS_ANDROID_SDK_VERSION': prop('ro.build.version.sdk'),
           'HYBRIS_LINKER_DIR': remote + '/hybris/libhybris/linker'}
    if a.xauthority: env['XAUTHORITY'] = a.xauthority
    server = {'source': 'external-service', 'display': a.display, 'xauthority': a.xauthority}
    if a.validation_layer:
        (stage / 'layers').mkdir()
        shutil.copy2(a.validation_layer, stage / 'layers/libVkLayer_khronos_validation.so')
        layer = json.loads(a.validation_manifest.read_text())
        if layer['layer']['name'] != 'VK_LAYER_KHRONOS_validation': raise ValueError('unexpected layer')
        layer['layer']['library_path'] = './libVkLayer_khronos_validation.so'
        (stage / 'layers/validation.json').write_text(json.dumps(layer))
        shutil.copy2(a.validation_manifest, out / 'validation-original.json')
        env['VK_LAYER_PATH'] = remote + '/layers'; env['HYBRIS_X11_VALIDATION'] = '1'
    if a.api == 'xlib': env['HYBRIS_X11_XLIB'] = '1'
    if a.icd_hal: env['HYBRIS_VULKAN_HAL'] = a.icd_hal
    if a.icd_mali_loader_quirk: env['HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK'] = '1'
    if a.vulkan_loader:
        (stage / 'standard').mkdir(); shutil.copy2(a.vulkan_loader, stage / 'standard/libvulkan.so.1')
        env['VK_DRIVER_FILES'] = remote + '/driver.json'
    prefix = ' '.join(k + '=' + shlex.quote(v) for k, v in env.items())
    client = './glibc/ld-linux-aarch64.so.1 --library-path ' + libraries + ' ./probe-xcb '
    command = prefix + ' ./x11-session ' + client + a.case
    record = {'case': a.case, 'api': a.api, 'serial': a.serial, 'fingerprint': prop('ro.build.fingerprint'),
              'package': package, 'command': command, 'remote': remote, 'probe': probe, 'hybris': hybris,
              'server': server, 'apk_sha256': host.record['apk_sha256'], 'runner_sha256': sha256_file(Path(__file__)), 'checker_sha256': sha256_file(ROOT / 'tests/wsi/screen_evidence.py')}
    if a.validation_layer: record['validation_layer_sha256'] = sha256_file(stage / 'layers/libVkLayer_khronos_validation.so')
    code = 2
    try:
        host.upload(stage, remote)
        if a.case != 'control':
            version = app('cd ' + shlex.quote(remote) + ' && ' + prefix + ' ' + client + 'version',
                          capture_output=True, text=True, timeout=20)
            (out / 'version.log').write_text(version.stdout + version.stderr)
            if version.returncode: raise ValueError('ICD version query failed; see version.log')
            versions = re.findall(r'^X11_ICD_VERSION (\d+\.\d+\.\d+)$', version.stdout, re.M)
            if len(versions) != 1: raise ValueError('missing ICD version')
            driver = json.dumps({'file_format_version': '1.0.0', 'ICD': {'library_path': remote + '/hybris/libhybris-vulkan-icd.so.0', 'api_version': versions[0]}})
            (stage / 'driver.json').write_text(driver)
            app('cat > ' + shlex.quote(remote + '/driver.json'), input=driver, text=True, check=True, timeout=10)
            record['standard_loader_sha256'] = sha256_file(stage / 'standard/libvulkan.so.1')
        code = host.execute(command, remote, out, a.timeout)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        record['error'] = str(error); code = 124 if isinstance(error, subprocess.TimeoutExpired) else 2
    finally:
        cleanup_errors = []
        def cleanup(action):
            nonlocal code
            try: return action()
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                cleanup_errors.append(str(error)); code = 2
        evidence = ['maps.txt'] + [f'image-{epoch}-{frame}.rgba' for epoch in range(3 if a.case == 'resize' else 1) for frame in (0, 7)]
        for name in evidence:
            copied = cleanup(lambda: app('cat ' + shlex.quote(remote + '/' + name), capture_output=True, timeout=10))
            if copied is not None and not copied.returncode: (out / name).write_bytes(copied.stdout)
        if (out / 'maps.txt').is_file():
            paths = set()
            for line in (out / 'maps.txt').read_text().splitlines():
                fields = line.split(maxsplit=5)
                if len(fields) == 6 and fields[5].startswith(('/vendor/', '/system/', '/system_ext/', '/apex/')) and '.so' in fields[5]: paths.add(fields[5])
            if paths:
                hashes = cleanup(lambda: app('sha256sum ' + shlex.join(sorted(paths)), capture_output=True, text=True, check=True, timeout=10))
                if hashes is not None: (out / 'android-library-hashes.txt').write_text(hashes.stdout)
        cleanup(lambda: app('rm -rf ' + shlex.quote(remote), check=True, timeout=10))
        record['cleanup_errors'] = cleanup_errors
    if code == 0 and a.case in ('present', 'resize'):
        try:
            if a.validation_layer:
                log = (out / 'probe.log').read_text()
                if re.findall(r'^X11_VALIDATION errors=(\d+)$', log, re.M) != ['0'] or re.search(r'^VALIDATION ', log, re.M):
                    raise ValueError('validation failed')
                record['validation_layer_sha256'] = sha256_file(stage / 'layers/libVkLayer_khronos_validation.so')
            sizes = [(320, 240), (160, 120), (256, 192)] if a.case == 'resize' else [(320, 240)]
            record['screen'] = [verify_epoch(out, epoch, size) for epoch, size in enumerate(sizes)]
            log = (out / 'probe.log').read_text()
            frames = re.findall(r'^X11_FRAME frame=(\d+) image=(\d+) pixels=76800 exact=1$', log, re.M)
            if a.case == 'present':
                if [int(f[0]) for f in frames] != list(range(8)): raise ValueError('missing exact eight-frame sequence')
            else:
                frames = re.findall(r'^X11_RESIZE_FRAME epoch=(\d+) frame=(\d+) image=\d+ size=(\d+)x(\d+) pixels=(\d+) exact=1$', log, re.M)
                expected = [(str(epoch), str(frame), str(w), str(h), str(w*h)) for epoch, (w,h) in enumerate(sizes) for frame in range(8)]
                if frames != expected: raise ValueError('missing exact resized frame sequence')
                transitions = re.findall(r'^X11_RESIZE epoch=(\d+) size=(\d+)x(\d+) out_of_date=1 fence_unsignaled=1 semaphore_reused=1 old_images_preserved=1$', log, re.M)
                if transitions != [('1', '160', '120'), ('2', '256', '192')]: raise ValueError('missing resize/synchronization verdicts')
                record['resize'] = {'transitions': 2, 'frames': 24, 'out_of_date': True, 'semaphore_reused': True}

            active = {}; serials = set(); presents = releases = 0
            for event, window, serial, buffer in re.findall(r'^X11_WSI event=(present|release) window=(\d+) serial=(\d+) buffer=(0x[0-9a-f]+)$', log, re.M):
                if event == 'present':
                    if buffer in active or serial in serials: raise ValueError('buffer reused before release or duplicate serial')
                    active[buffer] = serial; serials.add(serial); presents += 1
                else:
                    if active.pop(buffer, None) != serial: raise ValueError('release has no matching present')
                    releases += 1
            if presents != 8 * len(sizes) or releases < 5 * len(sizes): raise ValueError('missing TAWC-DRI presentation/release evidence')
            record['protocol'] = {'presents': presents, 'releases': releases, 'reuse_after_release': True}
            maps = (out / 'maps.txt').read_text()
            if a.validation_layer and 'libVkLayer_khronos_validation.so' not in maps: raise ValueError('validation layer mapping missing')
            if '/standard/libvulkan.so.1' not in maps or 'libhybris-vulkan-icd.so.0' not in maps or '/system/lib64/libvulkan.so' in maps:
                raise ValueError('unexpected Vulkan loader mappings')
        except (ValueError, OSError) as error: record['screen_error'] = str(error); code = 2
    if code == 0 and a.case == 'missing-protocol':
        log = (out / 'probe.log').read_text()
        if re.findall(r'^X11_REJECT attempt=(\d+) result=-13$', log, re.M) != [str(i) for i in range(8)]:
            record['rejection_error'] = 'missing eight exact rejections'; code = 2
    if code == 0 and a.case == 'acquire-timeout':
        if not re.search(r'^X11_ACQUIRE held=3 zero=NOT_READY finite=TIMEOUT elapsed_ns=\d+ index_unchanged=1 fence_unsignaled=1$', (out / 'probe.log').read_text(), re.M):
            record['acquire_error'] = 'missing timeout verdict'; code = 2
    record['status'] = 'PASS'  if code == 0 else 'UNSUPPORTED' if code == 3 else 'TIMEOUT' if code in (124, 142) else 'FAIL'
    record['exit_code'] = code
    record['elapsed_seconds'] = round(time.monotonic() - started, 3)
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(out); print(record['status'], code)
    return 0 if code == 0 else 1
