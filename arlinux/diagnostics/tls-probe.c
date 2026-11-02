/* Device diagnostic for the real glibc bridge, independent of GPU work.
 * Built by the Linux libhybris component. Never load a vendor driver here.
 * Hook timings are not rendering or game-FPS acceptance. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *(*get_tls)(void);
static pthread_barrier_t barrier;
static const unsigned iterations = 10000000;
static uint64_t elapsed[16];
static uint64_t cpu_elapsed[16];
static void (*register_tls)(size_t, const void *, size_t, size_t);
static size_t first_offset = 256, second_offset = 272, segment_size = 16;
static pthread_key_t reentry_key;
static unsigned destructor_calls;

static void *verify_worker(void *unused)
{
    (void)unused;
    unsigned char *tls = get_tls();
    unsigned char *original = tls;
    if (tls[first_offset - 8] != 17) abort();
    tls[first_offset - 8] = 41;
    pthread_barrier_wait(&barrier);
    pthread_barrier_wait(&barrier);
    tls = get_tls();
    if (tls != original || tls[first_offset - 8] != 41 ||
            tls[second_offset - 8] != 29 || tls[second_offset + segment_size - 9]) abort();
    tls[second_offset - 8] = 53;
    for (unsigned i = 0; i < 100000; i++) {
        tls = get_tls();
        if (tls != original || tls[first_offset - 8] != 41 || tls[second_offset - 8] != 53) abort();
    }
    return NULL;
}

static uint64_t now(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *worker(void *arg)
{
    unsigned index = (unsigned)(uintptr_t)arg;
    void *expected = get_tls();
    pthread_barrier_wait(&barrier);
    uint64_t start = now(CLOCK_MONOTONIC);
    uint64_t cpu_start = now(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned i = 0; i < iterations; i++)
        if (get_tls() != expected) abort();
    cpu_elapsed[index] = now(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    elapsed[index] = now(CLOCK_MONOTONIC) - start;
    return NULL;
}

static void destructor_reentry(void *unused)
{
    (void)unused;
    if (!get_tls()) abort();
    __atomic_fetch_add(&destructor_calls, 1, __ATOMIC_RELAXED);
}

static void *churn_worker(void *arg)
{
    unsigned char value = (unsigned char)(uintptr_t)arg + 1;
    unsigned char *tls = get_tls();
    tls[first_offset - 8] = value;
    if (pthread_setspecific(reentry_key, (void *)1)) abort();
    pthread_barrier_wait(&barrier);
    if (get_tls() != tls || tls[first_offset - 8] != value) abort();
    return NULL;
}

static unsigned long memory_kib(const char *field)
{
    FILE *file = fopen("/proc/self/status", "r");
    if (!file) abort();
    char line[256], key[64];
    unsigned long value;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%63s %lu kB", key, &value) == 2 && !strcmp(key, field)) {
            fclose(file);
            return value;
        }
    }
    abort();
}

static void churn_wave(void)
{
    pthread_t threads[16];
    for (unsigned i = 0; i < 16; i++)
        if (pthread_create(&threads[i], NULL, churn_worker, (void *)(uintptr_t)i)) abort();
    for (unsigned i = 0; i < 16; i++)
        if (pthread_join(threads[i], NULL)) abort();
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int large = strcmp(argv[2], "--verify-large") == 0;
    int verify = large || strcmp(argv[2], "--verify") == 0;
    int churn = strcmp(argv[2], "--churn") == 0;
    int count = verify || churn ? 1 : atoi(argv[2]);
    if (count < 1 || count > 16) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    *(void **)(&get_tls) = dlsym(library, "_hybris_hook___get_tls_hooks");
    if (!get_tls) return 1;
    if (verify || churn) {
        *(void **)(&register_tls) = dlsym(library, "_hybris_init_static_tls_for_thread");
        if (!register_tls) return 1;
    }
    if (churn) {
        unsigned char initial = 0;
        register_tls(first_offset, &initial, 1, segment_size);
        get_tls();
        if (pthread_key_create(&reentry_key, destructor_reentry) ||
                pthread_barrier_init(&barrier, NULL, 16)) abort();
        churn_wave();
        unsigned long before = memory_kib("VmSize:"), rss_before = memory_kib("VmRSS:");
        for (unsigned wave = 0; wave < 64; wave++) churn_wave();
        unsigned long after = memory_kib("VmSize:"), rss_after = memory_kib("VmRSS:");
        if (destructor_calls != 65 * 16 || after > before + 32768) abort();
        printf("{\"mode\":\"churn\",\"threads_completed\":%u,\"destructor_reentries\":%u,"
               "\"virtual_kib_before\":%lu,\"virtual_kib_after\":%lu,"
               "\"rss_kib_before\":%lu,\"rss_kib_after\":%lu}\n",
               65 * 16, destructor_calls, before, after, rss_before, rss_after);
        pthread_barrier_destroy(&barrier);
        pthread_key_delete(reentry_key);
        dlclose(library);
        return 0;
    }
    if (verify) {
        if (large) { first_offset = 8192; second_offset = 2 * 1024 * 1024 + 256; segment_size = 4096; }
        unsigned char first = 17, second = 29;
        register_tls(first_offset, &first, 1, segment_size);
        unsigned char *original = get_tls();
        pthread_t thread;
        if (pthread_barrier_init(&barrier, NULL, 2)) abort();
        if (pthread_create(&thread, NULL, verify_worker, NULL)) abort();
        pthread_barrier_wait(&barrier);
        register_tls(second_offset, &second, 1, segment_size);
        pthread_barrier_wait(&barrier);
        if (pthread_join(thread, NULL)) abort();
        unsigned char *tls = get_tls();
        if (tls != original || tls[first_offset - 8] != 17 || tls[second_offset - 8] != 29) abort();
        printf("PASS cross-thread TLS publication, zero fill and stable addresses%s\n", large ? " beyond 2 MiB" : "");
        pthread_barrier_destroy(&barrier);
        dlclose(library);
        return 0;
    }
    pthread_t threads[16];
    if (pthread_barrier_init(&barrier, NULL, count)) return 1;
    for (int i = 0; i < count; i++)
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i)) abort();
    uint64_t maximum = 0, cpu_total = 0;
    for (int i = 0; i < count; i++) {
        if (pthread_join(threads[i], NULL)) abort();
        if (elapsed[i] > maximum) maximum = elapsed[i];
        cpu_total += cpu_elapsed[i];
    }
    printf("{\"threads\":%d,\"calls_per_thread\":%u,\"max_elapsed_ns\":%llu,"
           "\"aggregate_ns_per_call\":%.3f,\"cpu_ns_per_call\":%.3f}\n", count, iterations,
           (unsigned long long)maximum, (double)maximum / iterations / count,
           (double)cpu_total / iterations / count);
    pthread_barrier_destroy(&barrier);
    dlclose(library);
    return 0;
}
