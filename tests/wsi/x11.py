"""XCB/Xlib probe staging and evidence; invoked only by run.py."""
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time
from manifest import sha256_file
from vulkan_backend import stage_backend, verify_backend_maps
from vulkan_layers import stage_validation
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
    remote = host.files + '/hybris-wsi-' + out.parent.name + '-' + out.name
    backend = stage_backend(a, stage, remote)
    for name in ('probe-xcb', 'x11-session'): shutil.copy2(a.probe / name, stage / name)
    for name, digest in probe['runtime'].items():
        source = a.probe / 'runtime' / name
        if sha256_file(source) != digest: raise ValueError('client runtime hash mismatch: ' + name)
        # The selected backend owns its coherent loader/libc pair, whose
        # exact hashes are already verified by stage_backend.
        if a.backend == 'turnip' and name in ('libc.so.6', 'ld-linux-aarch64.so.1'):
            continue
        destination = stage / 'glibc' / name
        if destination.exists():
            if sha256_file(destination) != digest: raise ValueError('client/hybris runtime conflict: ' + name)
        else:
            shutil.copy2(source, destination)
    libraries = backend['library_path']
    env = dict(backend['env'], DISPLAY=a.display, XDG_RUNTIME_DIR=a.runtime_dir,
               WAYLAND_DISPLAY=a.wayland,
               HYBRIS_ANDROID_SDK_VERSION=prop('ro.build.version.sdk'))
    env['ARDESK_WSI_TRACE' if a.backend == 'turnip' else 'HYBRIS_X11_TRACE'] = '1'
    if a.surface_format is not None: env['WSI_SURFACE_FORMAT'] = str(a.surface_format)
    if a.xauthority: env['XAUTHORITY'] = a.xauthority
    server = {'source': 'external-service', 'display': a.display, 'xauthority': a.xauthority}
    layer_meta = {}
    if a.validation_layer:
        layer_meta = stage_validation(a.validation_layer, a.validation_manifest, stage, remote, env)
        env['HYBRIS_X11_VALIDATION'] = '1'
    if a.api == 'xlib': env['HYBRIS_X11_XLIB'] = '1'
    prefix = ' '.join(k + '=' + shlex.quote(v) for k, v in env.items())
    client = './glibc/ld-linux-aarch64.so.1 --library-path ' + libraries + ' ./probe-xcb '
    command = prefix + ' ./x11-session ' + client + a.case
    record = {'case': a.case, 'api': a.api, 'serial': a.serial, 'fingerprint': prop('ro.build.fingerprint'),
              'package': package, 'command': command, 'remote': remote, 'probe': probe, 'backend': backend,
              'server': server, 'apk_sha256': host.record['apk_sha256'], 'runner_sha256': sha256_file(Path(__file__)), 'checker_sha256': sha256_file(ROOT / 'tests/wsi/screen_evidence.py')}
    record['requested_surface_format'] = a.surface_format
    record.update(layer_meta)
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
            driver = json.dumps({'file_format_version': '1.0.0', 'ICD': {'library_path': remote + '/' + backend['driver'], 'api_version': versions[0]}})
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
    if code == 0 and a.case != 'control':
        try:
            maps = (out / 'maps.txt').read_text()
            verify_backend_maps(backend, maps)
            if a.validation_layer:
                log = (out / 'probe.log').read_text()
                if re.findall(r'^X11_VALIDATION errors=(\d+)$', log, re.M) != ['0'] or re.search(r'^VALIDATION ', log, re.M):
                    raise ValueError('validation failed')
                if 'libVkLayer_khronos_validation.so' not in maps:
                    raise ValueError('validation layer mapping missing')
        except (ValueError, OSError) as error:
            record['validation_or_mapping_error'] = str(error); code = 2
    if code == 0 and a.case in ('present', 'resize'):
        try:
            if a.surface_format is not None and not re.search(r'^X11_SURFACE .* format=' + str(a.surface_format) + r'$', (out / 'probe.log').read_text(), re.M):
                raise ValueError('selected surface format does not match request')
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
                transitions = re.findall(r'^X11_RESIZE epoch=(\d+) size=(\d+)x(\d+) status=(SUBOPTIMAL|OUT_OF_DATE) acquire_sync_checked=1 present_wait_idle=1 semaphore_reused=1 old_images_preserved=1$', log, re.M)
                if [row[:3] for row in transitions] != [('1', '160', '120'), ('2', '256', '192')]: raise ValueError('missing resize/synchronization verdicts')
                if a.backend == 'hybris' and any(row[3] != 'SUBOPTIMAL' for row in transitions):
                    raise ValueError('hybris retired a still-presentable resized pool')
                record['resize'] = {'transitions': 2, 'frames': 24, 'statuses': [row[3] for row in transitions], 'acquire_sync_checked': True, 'present_wait_idle': True, 'semaphore_reused': True}

            active = {}; serials = set(); presents = releases = 0
            for event, window, serial, buffer in re.findall(r'^X11_WSI event=(present|release) window=(\d+) serial=(\d+) buffer=(0x[0-9a-f]+)$', log, re.M):
                if event == 'present':
                    if buffer in active or serial in serials: raise ValueError('buffer reused before release or duplicate serial')
                    active[buffer] = serial; serials.add(serial); presents += 1
                else:
                    if active.pop(buffer, None) != serial: raise ValueError('release has no matching present')
                    releases += 1
            resized_presents = sum(row[3] == 'SUBOPTIMAL' for row in transitions) if a.case == 'resize' else 0
            if presents != 8 * len(sizes) + resized_presents or releases < 5 * len(sizes): raise ValueError('missing TAWC-DRI presentation/release evidence')
            record['protocol'] = {'presents': presents, 'releases': releases, 'reuse_after_release': True}
        except (ValueError, OSError) as error: record['screen_error'] = str(error); code = 2
    if code == 0 and a.case == 'missing-protocol':
        log = (out / 'probe.log').read_text()
        if re.findall(r'^X11_REJECT attempt=(\d+) result=-13$', log, re.M) != [str(i) for i in range(8)]:
            record['rejection_error'] = 'missing eight exact rejections'; code = 2
    if code == 0 and a.case == 'acquire-timeout':
        if not re.search(r'^X11_ACQUIRE held=3 zero=NOT_READY finite=TIMEOUT elapsed_ns=\d+ index_unchanged=1 fence_unsignaled=1$', (out / 'probe.log').read_text(), re.M):
            record['acquire_error'] = 'missing timeout verdict'; code = 2
    if code == 0 and a.case == 'surface-lost':
        log = (out / 'probe.log').read_text()
        verdict = 'X11_SURFACE_LOST capabilities=1 acquire=1 present=1 index_unchanged=1 fence_unsignaled=1 present_wait_idle=1 semaphore_reused=1'
        if log.splitlines().count(verdict) != 1:
            record['surface_lost_error'] = 'missing surface loss/synchronization verdict'; code = 2
    record['status'] = 'PASS'  if code == 0 else 'UNSUPPORTED' if code == 3 else 'TIMEOUT' if code in (124, 142) else 'FAIL'
    record['exit_code'] = code
    record['elapsed_seconds'] = round(time.monotonic() - started, 3)
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(out); print(record['status'], code)
    return 0 if code == 0 else 1
