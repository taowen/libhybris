/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"
#include "mali_quirks.h"
#if defined(MALI_QUIRKS) && defined(__aarch64__)
#include "linker_bridge.h"
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

/* Limited to one inspected driver build. This knob skips MMUD's Android-loader
 * InstanceData inspection, not Vulkan validation. EGL and Vulkan share its
 * once-only decoder, so install it before either API initializes the driver.
 * Do not apply it to other driver versions without inspecting their encoding. */
static const unsigned char driver_build_id[] = {
    0x5a,0xc4,0xef,0xe8,0xd6,0x17,0x52,0x98,0xb2,0x73,
    0xdb,0xae,0xb8,0xf9,0xd2,0x8e,0x5e,0x50,0x8e,0x72
};
static pthread_once_t property_once = PTHREAD_ONCE_INIT;
static int32_t (*original_property_get_int32)(const char *, int32_t);

static int matches_driver(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    Elf64_Ehdr eh;
    int matched = 0;
    if (fread(&eh, 1, sizeof(eh), file) != sizeof(eh) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_AARCH64 ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum > 128 ||
        eh.e_phoff > INT64_MAX - 128 * sizeof(Elf64_Phdr)) goto done;
    for (unsigned i = 0; i < eh.e_phnum && !matched; ++i) {
        Elf64_Phdr ph;
        if (fseeko(file, eh.e_phoff + i * sizeof(ph), SEEK_SET) ||
            fread(&ph, 1, sizeof(ph), file) != sizeof(ph)) break;
        unsigned char notes[4096];
        if (ph.p_type != PT_NOTE || ph.p_filesz > sizeof(notes) || ph.p_offset > INT64_MAX)
            continue;
        if (fseeko(file, ph.p_offset, SEEK_SET) ||
            fread(notes, 1, ph.p_filesz, file) != ph.p_filesz) break;
        for (size_t offset = 0; offset + sizeof(Elf64_Nhdr) <= ph.p_filesz;) {
            Elf64_Nhdr note;
            memcpy(&note, notes + offset, sizeof(note));
            offset += sizeof(note);
            size_t name_size = ((size_t)note.n_namesz + 3) & ~(size_t)3;
            size_t desc_size = ((size_t)note.n_descsz + 3) & ~(size_t)3;
            if (name_size > ph.p_filesz - offset || desc_size > ph.p_filesz - offset - name_size)
                break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                note.n_descsz == sizeof(driver_build_id) &&
                !memcmp(notes + offset, "GNU", 4) &&
                !memcmp(notes + offset + name_size, driver_build_id, sizeof(driver_build_id)))
                matched = 1;
            offset += name_size + desc_size;
        }
    }
done:
    fclose(file);
    return matched;
}

static void resolve_property(void)
{
    /* Hook calls occur after linker initialization. Avoid reentering common's
     * public initialization gate, including during vendor constructors. */
    void *utils = _android_dlopen("libcutils.so", RTLD_NOW | RTLD_LOCAL);
    original_property_get_int32 = utils ? _android_dlsym(utils, "property_get_int32") : NULL;
    if (!original_property_get_int32) {
        fprintf(stderr, "HYBRIS: fatal: cannot resolve original Mali property_get_int32\n");
        abort();
    }
    /* Keep the reference alive with the driver, which retains this hook. */
}

static int32_t mali_property_get_int32(const char *name, int32_t fallback)
{
    pthread_once(&property_once, resolve_property);
    int32_t value = original_property_get_int32(name, fallback);
    if (strcmp(name, "vendor.debug.gpu.easy_check")) return value;
    /* Inspected 0x1db11d0 decoder: N*1000000+322126, N[1:0] log level,
     * N[2] another control, N[3] bypasses the loader inspection. Preserve
     * all existing controls. Unknown positive encodings remain untouched. */
    int32_t controls = value > 0 ? value / 1000000 : 0;
    if ((value > 0 && value % 1000000 != 322126) ||
        (controls | 8) > (INT32_MAX - 322126) / 1000000) {
        fprintf(stderr, "HYBRIS_MALI_MMUD rejected property encoding=%d\n", value);
        return value;
    }
    int32_t adjusted = (controls | 8) * 1000000 + 322126;
    fprintf(stderr, "HYBRIS_MALI_MMUD build_id=5ac4efe8d6175298b273dbaeb8f9d28e5e508e72 "
                    "skip_loader_check=1 property_before=%d property_after=%d\n", value, adjusted);
    return adjusted;
}
#endif

void *hybris_mali_hook(const char *symbol, const char *requester)
{
#if defined(MALI_QUIRKS) && defined(__aarch64__)
    if (strcmp(symbol, "property_get_int32") || !requester || getauxval(AT_SECURE)) return NULL;
    const char *enabled = getenv("HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK");
    if (enabled && strcmp(enabled, "1")) return NULL;
    const char *base = strrchr(requester, '/');
    if (!base || strcmp(base + 1, "libGLES_mali.so")) return NULL;
    int saved_errno = errno;
    int match = matches_driver(requester);
    errno = saved_errno;
    if (match) return mali_property_get_int32;
#else
    (void)symbol;
    (void)requester;
#endif
    return NULL;
}
