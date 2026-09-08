"""Verify and stage the pinned GFXReconstruct layer and command-line tools."""
import json
import shutil
from pathlib import Path


def stage_tools(install, stage, metadata, sha256):
    provenance = json.loads((install / 'manifest.json').read_text())
    for name, expected in provenance['files'].items():
        if sha256(install / name) != expected:
            raise ValueError('capture tool manifest mismatch: ' + name)
    metadata['capture_build'] = provenance
    metadata['capture_stage_sha256'] = sha256(Path(__file__))
    dest = stage / 'capture-tools'
    dest.mkdir()
    for name in ('gfxrecon-replay', 'gfxrecon-convert'):
        shutil.copy2(install / 'bin' / name, dest / name)
    layers = list(install.glob('lib*/**/libVkLayer_gfxreconstruct.so'))
    if len(layers) != 1:
        raise ValueError('expected one installed GFXReconstruct layer')
    shutil.copy2(layers[0], dest / layers[0].name)
    shutil.copytree(install / 'runtime', dest / 'runtime')
    original = install / 'share/vulkan/explicit_layer.d/VkLayer_gfxreconstruct.json'
    manifest = json.loads(original.read_text())
    if manifest['layer']['name'] != 'VK_LAYER_LUNARG_gfxreconstruct':
        raise ValueError('unexpected capture layer name')
    manifest['layer']['library_path'] = './libVkLayer_gfxreconstruct.so'
    (dest / 'capture.json').write_text(json.dumps(manifest))
    metadata['capture_tools'] = {
        str(path.relative_to(dest)): sha256(path)
        for path in dest.rglob('*') if path.is_file()}

