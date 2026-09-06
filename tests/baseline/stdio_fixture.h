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

#include <errno.h>
static int stdio_position_lifecycle(unsigned wide_offset) {
    FILE *stream = fopen("./stdio-position.data", "w+");
    if (!stream) return 2;
    int failed = fputs("0123456789", stream) < 0 || fseek(stream, 3, SEEK_SET);
    fpos_t position = 0;
    fpos64_t position64 = 0;
    if (wide_offset ? fgetpos64(stream, &position64) : fgetpos(stream, &position)) failed = 1;
    if ((wide_offset ? position64 : position) != 3 || fgetc(stream) != '3') failed = 1;
    if (wide_offset ? fsetpos64(stream, &position64) : fsetpos(stream, &position)) failed = 1;
    if (fgetc(stream) != '3') failed = 1;
    if (fclose(stream) || unlink("./stdio-position.data")) failed = 1;
    int ends[2];
    if (pipe(ends)) return 2;
    stream = fdopen(ends[0], "r");
    close(ends[1]);
    if (!stream) { close(ends[0]); return 2; }
    position = 12345;
    position64 = 12345;
    errno = 0;
    int result = wide_offset ? fgetpos64(stream, &position64) : fgetpos(stream, &position);
    int saved_errno = errno;
    long long observed = wide_offset ? position64 : position;
    printf("STDIO_POSITION wide=%u result=%d errno=%d position=%lld\n",
           wide_offset, result, saved_errno, observed);
    if (result != -1 || saved_errno != ESPIPE || observed != -1) failed = 1;
    if (fclose(stream)) failed = 1;
    return failed ? 2 : 0;
}
