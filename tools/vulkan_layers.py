"""One compatibility-layer deployment and ordering for both Vulkan backends."""
import json
import shutil
from manifest import sha256_file

COMPAT_LAYER = 'VK_LAYER_HYBRIS_compat'
COMPAT_LIBRARY = 'libVkLayer_hybris_compat.so'


def add_layer(env, name, directory, *, before=False):
    paths = [p for p in env.get('VK_LAYER_PATH', '').split(':') if p]
    if directory not in paths:
        paths.append(directory)
    layers = [p for p in env.get('VK_INSTANCE_LAYERS', '').split(':') if p and p != name]
    layers.insert(0 if before else len(layers), name)
    env.update(VK_LAYER_PATH=':'.join(paths), VK_INSTANCE_LAYERS=':'.join(layers))


def stage_compatibility(library_dir, provenance, stage, remote):
    source = library_dir / COMPAT_LIBRARY
    expected = next((item['sha256'] for item in provenance['elfs']
                     if item['tree'] == 'hybris' and item['path'] == COMPAT_LIBRARY), None)
    digest = sha256_file(source)
    if not expected or digest != expected:
        raise ValueError('compatibility layer does not match the selected build manifest')
    original = library_dir / 'VkLayer_hybris_compat.json'
    layer = json.loads(original.read_text())
    if layer['layer']['name'] != COMPAT_LAYER:
        raise ValueError('unexpected compatibility layer manifest')
    dest = stage / 'compat'
    dest.mkdir()
    shutil.copy2(source, dest / COMPAT_LIBRARY)
    layer['layer']['library_path'] = './' + COMPAT_LIBRARY
    (dest / original.name).write_text(json.dumps(layer, indent=2) + '\n')
    env = {}
    add_layer(env, COMPAT_LAYER, remote + '/compat')
    return {'library_sha256': digest, 'manifest_sha256': sha256_file(original), 'env': env}


def verify_compatibility_maps(maps):
    if '/compat/' + COMPAT_LIBRARY not in maps:
        raise ValueError('shared compatibility layer is missing from live mappings')


def stage_validation(library, original, stage, remote, env):
    layer = json.loads(original.read_text())
    if layer['layer']['name'] != 'VK_LAYER_KHRONOS_validation':
        raise ValueError('expected Khronos validation layer manifest')
    dest = stage / 'layers'
    dest.mkdir()
    shutil.copy2(library, dest / 'libVkLayer_khronos_validation.so')
    shutil.copy2(original, stage.parent / 'validation-original.json')
    layer['layer']['library_path'] = './libVkLayer_khronos_validation.so'
    (dest / 'validation.json').write_text(json.dumps(layer, indent=2) + '\n')
    add_layer(env, 'VK_LAYER_KHRONOS_validation', remote + '/layers')
    return {'validation_layer_sha256': sha256_file(library),
            'validation_manifest_sha256': sha256_file(original)}
