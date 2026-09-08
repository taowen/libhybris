"""Stage either product Vulkan backend for the shared window clients."""
import json
from pathlib import Path
import shutil
from host import stage_runtime
from manifest import sha256_file


def stage_backend(a, stage, remote):
    if a.backend == 'hybris':
        inputs = stage_runtime(a.build, stage)
        driver = 'hybris/libhybris-vulkan-icd.so.0'
        loader = a.vulkan_loader
        env = {'HYBRIS_LINKER_DIR': remote + '/hybris/libhybris/linker'}
        if a.icd_hal: env['HYBRIS_VULKAN_HAL'] = a.icd_hal
        if a.icd_mali_loader_quirk: env['HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK'] = '1'
        library_path = './standard:./hybris:./glibc'
    else:
        inputs = json.loads((a.mesa_build / 'manifest.json').read_text())
        if not inputs.get('product_mesa'):
            raise ValueError('rebuild desktop-gl using the product Mesa build')
        stage.mkdir(parents=True)
        (stage / 'glibc').mkdir()
        for name, digest in inputs['runtime'].items():
            source = a.mesa_build / 'runtime' / name
            if sha256_file(source) != digest: raise ValueError('Mesa runtime hash mismatch: ' + name)
            destination = stage / 'glibc' / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        driver = 'glibc/libvulkan_freedreno.so'
        loader = stage / 'glibc/libvulkan.so.1'
        env = {}
        library_path = './standard:./glibc'
    if not (stage / driver).is_file(): raise ValueError('selected ICD is missing: ' + driver)
    if loader:
        (stage / 'standard').mkdir()
        shutil.copy2(loader, stage / 'standard/libvulkan.so.1')
        env.update(VK_DRIVER_FILES=remote + '/driver.json', WSI_ICD_LIBRARY=remote + '/' + driver)
    return {'backend': a.backend, 'inputs': inputs, 'driver': driver,
            'loader_sha256': sha256_file(stage / 'standard/libvulkan.so.1') if loader else None,
            'library_path': library_path, 'env': env,
            'stager_sha256': sha256_file(Path(__file__))}


def verify_backend_maps(stage_info, maps):
    if '/standard/libvulkan.so.1' not in maps or stage_info['driver'] not in maps or '/system/lib64/libvulkan.so' in maps:
        raise ValueError('unexpected Vulkan loader/ICD mappings')
    if stage_info['backend'] == 'turnip' and 'libhybris-vulkan-icd' in maps:
        raise ValueError('unexpected hybris ICD in Turnip run')
