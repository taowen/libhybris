"""Wayland probe staging and evidence; invoked only by run.py."""
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
from manifest import sha256_file
from host import stage_runtime
from screen_evidence import verify_screen
from diagnostics import Diagnostics
from capture import preserve_capture, stage_tools, verify_window_capture


def run(a, host, out):
    if not a.icd_hal or not a.vulkan_loader:
        raise ValueError('frontend windows are retired; standard loader and ICD are required')
    adb, shell, app, prop = host.adb, host.shell, host.app, host.prop
    probe_provenance = json.loads((a.probe / 'probe-manifest.json').read_text())
    probe_name = 'probe-wayland'
    probe_hash = 'binary_sha256'
    if sha256_file(a.probe / probe_name) != probe_provenance[probe_hash]:
        raise SystemExit('probe hash mismatch; rebuild it')
    run_id = out.parent.name + '-' + out.name
    stage = out / 'stage'
    provenance = stage_runtime(a.build, stage)
    shutil.copy2(a.probe / probe_name, stage / probe_name)
    files = '/data/user/0/' + a.package + '/files'
    remote = files + '/hybris-wsi-' + run_id
    sdk = prop('ro.build.version.sdk')
    env = {'HYBRIS_LINKER_DIR': remote + '/hybris/libhybris/linker',
           'HYBRIS_ANDROID_SDK_VERSION': sdk, 'XDG_RUNTIME_DIR': a.runtime_dir,
           'WAYLAND_DISPLAY': a.wayland}
    layer_meta = {}
    adapter = stage / 'hybris/libhybris-vulkan-icd.so.0'
    if not adapter.is_file():
        raise SystemExit('ICD adapter missing from hybris install')
    (stage / 'standard').mkdir()
    shutil.copy2(a.vulkan_loader, stage / 'standard/libvulkan.so.1')
    env['HYBRIS_VULKAN_HAL'] = a.icd_hal
    if a.swapchain_review: env['HYBRIS_WSI_SWAPCHAIN_REVIEW'] = '1'
    if a.icd_mali_loader_quirk:
        env['HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK'] = '1'
    env['VK_DRIVER_FILES'] = remote + '/driver.json'
    libraries = './standard:./hybris:./glibc'
    layer_dirs = []
    if a.validation_layer:
        (stage / 'layers').mkdir()
        shutil.copy2(a.validation_layer, stage / 'layers/libVkLayer_khronos_validation.so')
        layer_meta['validation_layer_sha256'] = sha256_file(a.validation_layer)
        layer_meta['validation_manifest_sha256'] = sha256_file(a.validation_manifest)
        shutil.copy2(a.validation_manifest, out / 'validation-original.json')
        layer_json = json.loads(a.validation_manifest.read_text())
        if layer_json['layer']['name'] != 'VK_LAYER_KHRONOS_validation':
            raise SystemExit('expected Khronos validation layer manifest')
        layer_json['layer']['library_path'] = './libVkLayer_khronos_validation.so'
        (stage / 'layers/validation.json').write_text(json.dumps(layer_json))
        env['HYBRIS_WSI_VALIDATION'] = '1'
        layer_dirs.append(remote + '/layers')
    if a.capture_tools:
        stage_tools(a.capture_tools, stage, layer_meta, sha256_file)
        env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_LUNARG_gfxreconstruct'
        env['GFXRECON_CAPTURE_FILE'] = remote + '/window.gfxr'
        env['GFXRECON_CAPTURE_FILE_TIMESTAMP'] = 'false'
        layer_dirs.append(remote + '/capture-tools')
        libraries += ':./capture-tools:./capture-tools/runtime'
    if layer_dirs:
        env['VK_LAYER_PATH'] = ':'.join(layer_dirs)
    if a.trace: env.update(HYBRIS_TRACE='1', HYBRIS_LOGGING_LEVEL='warn')
    command = ' '.join(k + '=' + shlex.quote(v) for k, v in env.items())
    command += ' ./glibc/ld-linux-aarch64.so.1 --library-path ' + libraries + ' ./' + probe_name
    metadata = {'run_id': run_id, 'serial': a.serial, 'package': a.package,
                'fingerprint': prop('ro.build.fingerprint'), 'sdk': sdk,
                'command': command, 'remote': remote, 'host_timeout_seconds': a.timeout, 'runner_sha256': sha256_file(Path(__file__)), 'hybris': provenance, 'probe': probe_provenance,
                'path': 'icd'}
    metadata['helper_sha256'] = {name: sha256_file(Path(__file__).with_name(name))
                               for name in ('capture.py', 'screen_evidence.py', 'diagnostics.py')}
    metadata['icd_hal'] = a.icd_hal
    metadata['standard_loader_sha256'] = sha256_file(stage / 'standard/libvulkan.so.1')
    metadata.update(layer_meta)
    metadata['apk_sha256'] = host.record['apk_sha256']
    (out / 'device.json').write_text(json.dumps(metadata, indent=2))
    code = 2
    diagnostics = Diagnostics(adb, a.package, out, app)
    def stop_owned_process(pid_file='runner.pid'): host.stop_process(remote, pid_file)

    try:
        host.upload(stage, remote)
        # Query the staged adapter directly before creating a loader manifest.
        # A fixed 1.3 declaration would misrepresent a HAL reporting 1.1.
        version_command = 'cd ' + shlex.quote(remote) + ' && env ' + command + ' --icd-version'
        version_run = app(version_command, capture_output=True, text=True, timeout=30)
        (out / 'icd-version.log').write_text(version_run.stdout + version_run.stderr)
        versions = re.findall(r'^WSI_ICD_VERSION (\d+\.\d+\.\d+)$', version_run.stdout, re.MULTILINE)
        if version_run.returncode or len(versions) != 1:
            raise RuntimeError('staged ICD version query failed; see icd-version.log')
        driver = json.dumps({'file_format_version': '1.0.0', 'ICD': {
            'library_path': remote + '/hybris/libhybris-vulkan-icd.so.0',
            'api_version': versions[0]}})
        (stage / 'driver.json').write_text(driver)
        app('cat > ' + shlex.quote(remote + '/driver.json'), input=driver, text=True, check=True)
        metadata['icd_api_version'] = versions[0]
        metadata['icd_version_command'] = version_command
        (out / 'device.json').write_text(json.dumps(metadata, indent=2))
        code = host.execute(command, remote, out, a.timeout, diagnostics)
        if code not in (0, 3):
            diagnostics.snapshot(remote, 'client exited unsuccessfully; client may already be gone')
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        code = 2
        (out / 'runner-error.txt').write_text(str(error))
        diagnostics.snapshot(remote, 'runner operation failed')
    except subprocess.TimeoutExpired:
        code = 124
        diagnostics.snapshot(remote, 'host timeout before terminating owned client')
    finally:
        for name in ['maps-instance.txt', 'maps-surface.txt', 'maps-frame.txt'] + [
                f'image-{epoch}-{frame}.rgba' for epoch in range(3) for frame in (0, 7)]:
            saved = app('cat ' + shlex.quote(remote + '/' + name), capture_output=True)
            if saved.returncode == 0: (out / name).write_bytes(saved.stdout)
        paths = set()
        for mapping in out.glob('maps-*.txt'):
            for line in mapping.read_text().splitlines():
                fields = line.split(maxsplit=5)
                if len(fields) == 6 and fields[5].startswith(('/vendor/', '/system/', '/system_ext/', '/apex/')) and '.so' in fields[5]:
                    paths.add(fields[5])
        if paths:
            hashes = app('sha256sum ' + shlex.join(sorted(paths)), capture_output=True, text=True)
            (out / 'android-library-hashes.json').write_text(json.dumps({
                'exit_code': hashes.returncode, 'output': hashes.stdout, 'errors': hashes.stderr}, indent=2))
            if hashes.returncode and code == 0: code = 2
        if a.capture_tools:
            try:
                metadata['capture_sha256'] = preserve_capture(app, remote, out)
                tool_env = {k: v for k, v in env.items()
                            if k not in ('VK_INSTANCE_LAYERS', 'GFXRECON_CAPTURE_FILE',
                                         'GFXRECON_CAPTURE_FILE_TIMESTAMP', 'HYBRIS_WSI_VALIDATION',
                                         'VK_LAYER_PATH')}
                tool_prefix = ' '.join(k + '=' + shlex.quote(v) for k, v in tool_env.items())
                tool_prefix += ' ./glibc/ld-linux-aarch64.so.1 --library-path ' + libraries + ' '
                log_text = (out / 'probe.log').read_text()
                if code == 0:
                    metadata['window_capture'] = verify_window_capture(
                        app, remote, tool_prefix, out, log_text, stop_owned_process)
                (out / 'device.json').write_text(json.dumps(metadata, indent=2))
            except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
                (out / 'capture-error.txt').write_text(str(error))
                if code == 0: code = 124 if isinstance(error, subprocess.TimeoutExpired) else 2
            finally:
                try:
                    stop_owned_process('capture-tool.pid')
                except (OSError, subprocess.SubprocessError) as error:
                    (out / 'capture-cleanup-error.txt').write_text(str(error))
                    if code == 0: code = 2
        app('rm -rf ' + shlex.quote(remote), check=True)
        diagnostics.finish()
    if code == 0:
        try:
            evidence = verify_screen(out)
        except (ValueError, OSError) as error:
            evidence = {'status': 'FAIL', 'error': str(error)}
            code = 2
        (out / 'screen-evidence.json').write_text(json.dumps(evidence, indent=2))
    if code == 0 and a.validation_layer:
        log_text = (out / 'probe.log').read_text()
        counts = re.findall(r'^WSI_VALIDATION errors=(\d+)$', log_text, re.M)
        if len(counts) != 1 or int(counts[0]) != 0 or re.search(r'^VALIDATION ', log_text, re.M):
            code = 2
            (out / 'validation-error.txt').write_text(
                'expected one WSI_VALIDATION errors=0 record and no ERROR callback anywhere in the log')
    if code not in (0, 3):
        diagnostics.screen('failure-screen.png')
        (out / 'diagnostics.json').write_text(json.dumps(diagnostics.records, indent=2))
    status = 'PASS' if code == 0 else 'UNSUPPORTED' if code == 3 else 'TIMEOUT' if code in (124, 142) else 'CRASH' if code >= 128 else 'FAIL'
    (out / 'result.json').write_text(json.dumps({'status': status, 'exit_code': code,
        'scope': ('icd-presentation' +
                  ('-validation' if a.validation_layer else '') +
                  ('-capture' if a.capture_tools else ''))}, indent=2))
    print(out)
    print(status, code)
    return 0 if code in (0, 3) else 1
