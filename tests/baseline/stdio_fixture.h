#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int stdio_flush_lifecycle(unsigned memory) {
    char *buffer = NULL;
    size_t length = 0;
    const char expected[] = "hybris buffered output";
    FILE *stream = memory ? open_memstream(&buffer, &length) : fopen("./stdio-flush.data", "w+");
    if (!stream) return 2;
    char file_buffer[256];
    int failed = 0;
    if (!memory && setvbuf(stream, file_buffer, _IOFBF, sizeof(file_buffer))) failed = 1;
    if (fwrite(expected, 1, sizeof(expected)-1, stream) != sizeof(expected)-1) failed = 1;
    if (fflush(memory ? stream : NULL)) failed = 1;
    if (memory) {
        if (!buffer || length != sizeof(expected)-1 || memcmp(buffer, expected, length)) failed = 1;
    } else {
        char actual[sizeof(expected)] = {0};
        if (pread(fileno(stream), actual, sizeof(expected)-1, 0) != sizeof(expected)-1 ||
            memcmp(actual, expected, sizeof(expected)-1)) failed = 1;
    }
    if (fclose(stream)) failed = 1;
    free(buffer);
    if (!memory && unlink("./stdio-flush.data")) failed = 1;
    return failed ? 2 : 0;
}
