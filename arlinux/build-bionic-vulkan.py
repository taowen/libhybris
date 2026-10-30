#!/usr/bin/env python3
"""Build the shared desktop Vulkan HAL adapter with Android's native libc.

The libhybris checkout supplies the existing ICD/WSI, not its libc bridge.
The output uses the system Vulkan HAL and the public AHardwareBuffer allocator.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import re
import importlib.util


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['framework', 'source', 'headers', 'ndk', 'native-prefix', 'graphics-prefix', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.framework.resolve()
    component = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = args.source.resolve() / 'hybris'
    stage = output / 'source'
    # Refresh only the selected files; do not alter the shared source checkout.
    selected = ['vulkan/icd', 'vulkan/layer', 'vulkan/compat', 'platforms/common', 'platforms/wayland']
    for subdir in selected:
        for path in (source / subdir).rglob('*'):
            if path.suffix not in {'.c', '.cpp', '.h', '.inc', '.json'}:
                continue
            target = stage / subdir / path.relative_to(source / subdir)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    # Correct the HAL's float-control capability at the shared Vulkan layer.
    # Applications such as DXVK then use the ordinary capability query path.
    patch = component / 'libhybris-adreno-float-properties.patch'
    with patch.open('rb') as patch_input:
        check = subprocess.run(['patch', '-d', str(stage), '-p1', '--batch', '--fuzz=0', '--dry-run'],
                               stdin=patch_input, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with patch.open('rb') as patch_input:
        command = ['patch', '-d', str(stage), '-p1', '--batch', '--fuzz=0']
        if check.returncode:
            command += ['--reverse', '--dry-run']
        subprocess.run(command, stdin=patch_input, check=True)
    generated = output / 'include'
    (generated / 'hybris/common').mkdir(parents=True, exist_ok=True)
    (generated / 'config.h').write_text('#define WANT_WAYLAND 1\n')
    (generated / 'android-config.h').write_text('#define ANDROID_VERSION_MAJOR 10\n#define ANDROID_VERSION_MINOR 0\n#define ANDROID_VERSION_PATCH 0\n')
    (generated / 'logging.h').write_text('#define TRACE(...)\n#define HYBRIS_TRACE_BEGIN(...)\n#define HYBRIS_TRACE_END(...)\n#define HYBRIS_TRACE_COUNTER(...)\n')
    (generated / 'vulkanhybris.h').write_text('')
    (generated / 'hybris/common/dlfcn.h').write_text('''#pragma once
#include <dlfcn.h>
#include <android/dlext.h>
#include <hardware/hardware.h>
#ifdef __cplusplus
extern "C" {
#endif
void* bionic_get_exported_namespace(const char*);
void* bionic_dlopen_ext(const char*, int, const android_dlextinfo*);
int bionic_hw_get_module(const char*, const hw_module_t**);
#ifdef __cplusplus
}
#endif
#define hybris_get_exported_namespace bionic_get_exported_namespace
#define hybris_dlopen_ext bionic_dlopen_ext
#define hybris_dlopen dlopen
#define hybris_dlsym dlsym
#define hybris_dlerror dlerror
#define hw_get_module bionic_hw_get_module
''')
    # Diagnostics dumping belongs to libhybris and is not part of native WSI.
    path = stage / 'vulkan/icd/wayland_window.cpp'
    path.write_text(path.read_text().replace('hybris_dump_buffer_to_file(wnb->getNativeBuffer());', '(void)wnb;'))
    path = stage / 'vulkan/icd/swapchain.c'
    text = path.read_text()
    text = text.replace('    struct hybris_icd_device context;\n    if (!info || !swapchain', '''    if (getenv("ARLINUX_VULKAN_TRACE") && info)
        fprintf(stderr, "Bionic WSI: flags=0x%x extent=%ux%u usage=0x%x mode=%d alpha=0x%x layers=%u transform=0x%x colorspace=%d images=%u\\n",
            info->flags, info->imageExtent.width, info->imageExtent.height, info->imageUsage, info->presentMode,
            info->compositeAlpha, info->imageArrayLayers, info->preTransform, info->imageColorSpace, info->minImageCount);
    struct hybris_icd_device context;
    if (!info || !swapchain''')
    text = text.replace('    if (checked != VK_SUCCESS) return checked;', '''    if (checked != VK_SUCCESS) return checked;
    if (getenv("ARLINUX_VULKAN_TRACE"))
        fprintf(stderr, "Bionic WSI: caps extent=%ux%u max=%ux%u usage=0x%x alpha=0x%x\\n",
            capabilities.currentExtent.width, capabilities.currentExtent.height, capabilities.maxImageExtent.width,
            capabilities.maxImageExtent.height, capabilities.supportedUsageFlags, capabilities.supportedCompositeAlpha);''')
    path.write_text(text)
    # RGBX provides the format-independent OPAQUE contract required by Wine's
    # swapchain. BGRA has no native RGBX counterpart in this shared transport.
    path = stage / 'vulkan/icd/wsi.c'
    text = path.read_text()
    original = 'const VkFormat candidates[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};'
    if original not in text:
        raise RuntimeError('Shared WSI format query changed; review the native opaque backing contract')
    text = text.replace(original, 'const VkFormat candidates[] = {VK_FORMAT_R8G8B8A8_UNORM};')
    text = text.replace('for (uint32_t i = 0; i < 2; ++i) {\n        VkImageFormatProperties properties;',
                        'for (uint32_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {\n        VkImageFormatProperties properties;')
    path.write_text(text)
    ndk = args.ndk.resolve() / 'toolchains/llvm/prebuilt/linux-x86_64'
    compiler = ndk / 'bin/aarch64-linux-android29-clang'
    includes = [generated, args.graphics_prefix / 'include', args.native_prefix / 'include',
                component / 'graphics-protocols/include', args.headers, source / 'include',
                stage / 'platforms/common', stage / 'platforms/wayland', stage / 'vulkan/icd']
    flags = ['-fPIC', '-O2', '-fvisibility=hidden', '-D_GNU_SOURCE', '-DANDROID_BUILD', '-DHYBRIS_NO_SERVER_SIDE_BUFFERS']
    flags += ['-I' + str(path.resolve()) for path in includes]
    xml = component / 'graphics-protocols/wayland-android.xml'
    subprocess.run(['wayland-scanner', 'client-header', str(xml), str(generated / 'wayland-android-client-protocol.h')], check=True)
    protocol = output / 'wayland-android-protocol.c'
    subprocess.run(['wayland-scanner', 'private-code', str(xml), str(protocol)], check=True)
    files = [stage / 'vulkan/icd' / name for name in ['icd.c', 'instance.c', 'device.c', 'wsi.c', 'swapchain.c',
             'native_window_wayland.c', 'native_window_headless.cpp', 'window_owner.cpp', 'wayland_window.cpp']]
    files += [stage / 'platforms/common/nativewindowbase.cpp', stage / 'platforms/wayland/wayland_window_common.cpp',
              component / 'bionic-vulkan/native.cpp', protocol]
    # Use the AOSP usage conversion already shipped with the shared adapter.
    files.append(source / 'gralloc/GrallocUsageConversion.cpp')
    flags += ['-I' + str(source / 'libsync/include')]
    objects = []
    for index, path in enumerate(files):
        obj = output / f'{index}-{path.stem}.o'
        cpp = path.suffix == '.cpp'
        cmd = [str(compiler) + ('++' if cpp else ''), *flags]
        if cpp: cmd += ['-std=gnu++17']
        subprocess.run([*cmd, '-c', str(path), '-o', str(obj)], check=True)
        objects.append(str(obj))
    library = output / 'libvulkan_android_desktop.so'
    subprocess.run([str(compiler) + '++', '-shared', '-static-libstdc++', *objects,
                    '-L' + str(args.native_prefix.resolve() / 'lib'), '-landroid', '-lnativewindow', '-ldl',
                    '-lwayland-client', '-lwayland-egl',
                    '-Wl,--no-undefined,-z,nodelete,-z,max-page-size=16384',
                    '-Wl,-soname,libvulkan_android_desktop.so', '-o', str(library)], check=True)
    print(library)
    # The standard compatibility layer is independent of the libc bridge and
    # the HAL. Reuse its maintained source list so capability queries and the
    # matching shader/resource transformations are ported together.
    makefile = (source / 'vulkan/Makefile.am').read_text()
    layer_sources = makefile.split('libVkLayer_hybris_compat_la_SOURCES =', 1)[1].split(
        'libVkLayer_hybris_compat_la_CFLAGS', 1)[0]
    files = [stage / 'vulkan' / path for path in re.findall(r'\b(?:layer|compat)/[\w/]+\.c\b', layer_sources)]
    if not files:
        raise RuntimeError('Shared Vulkan compatibility source list changed')
    objects = []
    for index, path in enumerate(files):
        obj = output / f'compat-{index}-{path.stem}.o'
        subprocess.run([str(compiler), *flags, '-std=gnu11', '-c', str(path), '-o', str(obj)], check=True)
        objects.append(str(obj))
    layer = output / 'libVkLayer_hybris_compat.so'
    subprocess.run([str(compiler), '-shared', *objects, '-lm',
                    '-Wl,--no-undefined,-z,nodelete,-z,max-page-size=16384',
                    '-Wl,-soname,libVkLayer_hybris_compat.so', '-o', str(layer)], check=True)
    shutil.copyfile(stage / 'vulkan/layer/VkLayer_hybris_compat.json', output / 'VkLayer_hybris_compat.json')
    notices = output / 'licenses/libhybris-vulkan'
    notices.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.source / 'LICENSE.GPL3', notices / 'LICENSE.GPL3')
    for name in ('MALIBRIDGE.md', 'NOTICE.MaliBridge', 'LICENSE.bcdec'):
        shutil.copyfile(source / 'vulkan/compat/shaders' / name, notices / name)
    commit = subprocess.check_output(['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
    (notices / 'SOURCE').write_text('https://github.com/taowen/libhybris/tree/' + commit + '\n')
    print(layer)
    spec = importlib.util.spec_from_file_location('hybris_diagnostics_build', component/'diagnostics/build.py')
    diagnostics = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(diagnostics)
    diagnostics.build_diagnostics(args.source.resolve(), output, compiler, flags, layer, args.ndk.resolve())


if __name__ == '__main__':
    main()
