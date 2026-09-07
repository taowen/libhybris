/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "scaled_vertex.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

static pthread_mutex_t dump_guard = PTHREAD_MUTEX_INITIALIZER;
static unsigned sequence;
static int write_module(const char *directory, unsigned id, const char *kind,
                        const uint32_t *code, size_t size)
{
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%03u-%s.spv", directory, id, kind);
    if (length < 0 || (size_t)length >= sizeof(path)) return 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return 0;
    const char *bytes = (const char *)code;
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, bytes + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    int closed = close(fd);
    return written == size && !closed;
}
void hybris_scaled_dump(const uint32_t *original, size_t original_size,
    const uint32_t *converted, size_t converted_size,
    const struct hybris_scaled_attribute *attributes, uint32_t count)
{
    const char *directory = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_SCALED_DUMP_DIR");
    if (!directory || !*directory) return;
    pthread_mutex_lock(&dump_guard);
    unsigned id = sequence;
    if (sequence < 128) ++sequence;
    pthread_mutex_unlock(&dump_guard);
    if (id >= 128) return;
    int before = write_module(directory, id, "original", original, original_size);
    int after = write_module(directory, id, "converted", converted, converted_size);
    fprintf(stderr, "HYBRIS_SCALED_DUMP id=%u original=%d converted=%d attributes=%u\n", id, before, after, count);
    for (uint32_t i = 0; i < count; ++i)
        fprintf(stderr, "HYBRIS_SCALED_ATTRIBUTE id=%u location=%u signed=%d\n", id, attributes[i].location, attributes[i].is_signed);
}
