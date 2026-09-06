/*
 * Copyright (c) 2012 Carsten Munk <carsten.munk@gmail.com>
 * Copyright (c) 2012 Canonical Ltd
 * Copyright (c) 2013 Christophe Chapuis <chris.chapuis@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "config.h"
#include "bionic_stdio.h"
#include "logging.h"
#include <stdio_ext.h>
#include <string.h>
#include <errno.h>

#define TRACE_HOOK(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

/*
 * __isthreaded is used in bionic's stdio.h to choose between a fast internal implementation
 * and a more classic stdio function call.
 * For example:
 * #define  __sfeof(p)  (((p)->_flags & __SEOF) != 0)
 * #define  feof(p)     (!__isthreaded ? __sfeof(p) : (feof)(p))
 *
 * We see here that if __isthreaded is false, then it will use directly the bionic's FILE structure
 * instead of calling one of the hooked methods.
 * Therefore we need to set __isthreaded to true, even if we are not in a multi-threaded context.
 */
int _hybris_hook___isthreaded = 1;

/* "struct __sbuf" from bionic/libc/include/stdio.h */
#if defined(__LP64__)
struct bionic_sbuf {
    unsigned char* _base;
    size_t _size;
};
#else
struct bionic_sbuf {
    unsigned char *_base;
    int _size;
};
#endif

/* "struct __sFILE" from bionic/libc/include/stdio.h */
struct bionic_file {
    unsigned char *_p;      /* current position in (some) buffer */
    int _r;                 /* read space left for getc() */
    int _w;                 /* write space left for putc() */
#if defined(__LP64__)
    int _flags;             /* flags, below; this FILE is free if 0 */
    int _file;              /* fileno, if Unix descriptor, else -1 */
#else
    short _flags;           /* flags, below; this FILE is free if 0 */
    short _file;            /* fileno, if Unix descriptor, else -1 */
#endif
    struct bionic_sbuf _bf; /* the buffer (at least 1 byte, if !NULL) */
    int _lbfsize;           /* 0 or -_bf._size, for inline putc */

    /* operations */
    void *_cookie;          /* cookie passed to io functions */
    int (*_close)(void *);
    int (*_read)(void *, char *, int);
    bionic_fpos_t (*_seek)(void *, bionic_fpos_t, int);
    int (*_write)(void *, const char *, int);

    /* extension data, to avoid further ABI breakage */
    struct bionic_sbuf _ext;
    /* data for long sequences of ungetc() */
    unsigned char *_up;     /* saved _p when _p is doing ungetc data */
    int _ur;                /* saved _r when _r is counting ungetc data */

    /* tricks to meet minimum requirements even when malloc() fails */
    unsigned char _ubuf[3]; /* guarantee an ungetc() buffer */
    unsigned char _nbuf[1]; /* guarantee a getc() buffer */

    /* separate buffer for fgetln() when line crosses buffer boundary */
    struct bionic_sbuf _lb; /* buffer for fgetln() */

    /* Unix stdio files get aligned to block boundaries on fseek() */
    int _blksize;           /* stat.st_blksize (may be != _bf._size) */
    bionic_fpos_t _offset;         /* current lseek offset */
};

/*
 * redirection for bionic's __sF, which is defined as:
 *   FILE __sF[3];
 *   #define stdin  &__sF[0];
 *   #define stdout &__sF[1];
 *   #define stderr &__sF[2];
 *   So the goal here is to catch the call to file methods where the FILE* pointer
 *   is either stdin, stdout or stderr, and translate that pointer to a valid glibc
 *   pointer.
 *   Currently, only fputs is managed.
 */
char _hybris_hook_sF[3 * sizeof(struct bionic_file)] = {0};
static FILE *_get_actual_fp(FILE *fp)
{
    char *c_fp = (char*)fp;
    if (c_fp == &_hybris_hook_sF[0])
        return stdin;
    else if (c_fp == &_hybris_hook_sF[sizeof(struct bionic_file)])
        return stdout;
    else if (c_fp == &_hybris_hook_sF[sizeof(struct bionic_file) * 2])
        return stderr;

    return fp;
}

void _hybris_hook_clearerr(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    clearerr(_get_actual_fp(fp));
}

int _hybris_hook_fclose(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fclose(_get_actual_fp(fp));
}

int _hybris_hook_feof(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return feof(_get_actual_fp(fp));
}

int _hybris_hook_ferror(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ferror(_get_actual_fp(fp));
}

int _hybris_hook_fflush(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fflush(_get_actual_fp(fp));
}

int _hybris_hook_fgetc(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fgetc(_get_actual_fp(fp));
}

int _hybris_hook_fgetpos(FILE *fp, bionic_fpos_t *pos)
{
    TRACE_HOOK("fp %p pos %p", fp, pos);

    fpos_t my_fpos;
    int ret = fgetpos(_get_actual_fp(fp), &my_fpos);

    *pos = my_fpos.__pos;

    return ret;
}

int _hybris_hook_fgetpos64(FILE *fp, bionic_fpos64_t *pos)
{
    TRACE_HOOK("fp %p pos %p", fp, pos);

    fpos64_t my_fpos;
    int ret = fgetpos64(_get_actual_fp(fp), &my_fpos);

    *pos = my_fpos.__pos;

    return ret;
}

char* _hybris_hook_fgets(char *s, int n, FILE *fp)
{
    TRACE_HOOK("s %s n %d fp %p", s, n, fp);

    return fgets(s, n, _get_actual_fp(fp));
}

FP_ATTRIB int _hybris_hook_fprintf(FILE *fp, const char *fmt, ...)
{
    int ret = 0;

    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    va_list args;
    va_start(args,fmt);
    ret = vfprintf(_get_actual_fp(fp), fmt, args);
    va_end(args);

    return ret;
}

int _hybris_hook_fputc(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return fputc(c, _get_actual_fp(fp));
}

int _hybris_hook_fputs(const char *s, FILE *fp)
{
    TRACE_HOOK("s '%s' fp %p", s, fp);

    return fputs(s, _get_actual_fp(fp));
}

size_t _hybris_hook_fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    TRACE_HOOK("ptr %p size %zu nmemb %zu fp %p", ptr, size, nmemb, fp);

    return fread(ptr, size, nmemb, _get_actual_fp(fp));
}

FILE* _hybris_hook_freopen(const char *filename, const char *mode, FILE *fp)
{
    TRACE_HOOK("filename '%s' mode '%s' fp %p", filename, mode, fp);

    return freopen(filename, mode, _get_actual_fp(fp));
}

FILE* _hybris_hook_freopen64(const char *filename, const char *mode, FILE *fp)
{
    TRACE_HOOK("filename '%s' mode '%s' fp %p", filename, mode, fp);

    return freopen64(filename, mode, _get_actual_fp(fp));
}

FP_ATTRIB int _hybris_hook_fscanf(FILE *fp, const char *fmt, ...)
{
    int ret = 0;

    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    va_list args;
    va_start(args,fmt);
    ret = vfscanf(_get_actual_fp(fp), fmt, args);
    va_end(args);

    return ret;
}

int _hybris_hook_fseek(FILE *fp, long offset, int whence)
{
    TRACE_HOOK("fp %p offset %ld whence %d", fp, offset, whence);

    return fseek(_get_actual_fp(fp), offset, whence);
}

int _hybris_hook_fseeko(FILE *fp, bionic_off_t offset, int whence)
{
    TRACE_HOOK("fp %p offset %ld whence %d", fp, offset, whence);

    return fseeko(_get_actual_fp(fp), offset, whence);
}

int _hybris_hook_fseeko64(FILE *fp, off64_t offset, int whence)
{
    TRACE_HOOK("fp %p offset %ld whence %d", fp, offset, whence);

    return fseeko64(_get_actual_fp(fp), offset, whence);
}

int _hybris_hook_fsetpos(FILE *fp, const bionic_fpos_t *pos)
{
    TRACE_HOOK("fp %p pos %p", fp, pos);

    fpos_t my_fpos;
    my_fpos.__pos = *pos;
    memset(&my_fpos.__state, 0, sizeof(mbstate_t));

    return fsetpos(_get_actual_fp(fp), &my_fpos);
}

int _hybris_hook_fsetpos64(FILE *fp, const bionic_fpos64_t *pos)
{
    TRACE_HOOK("fp %p pos %p", fp, pos);

    fpos64_t my_fpos;
    my_fpos.__pos = *pos;
    memset(&my_fpos.__state, 0, sizeof(mbstate_t));

    return fsetpos64(_get_actual_fp(fp), &my_fpos);
}

long _hybris_hook_ftell(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ftell(_get_actual_fp(fp));
}

bionic_off_t _hybris_hook_ftello(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ftello(_get_actual_fp(fp));
}

off64_t _hybris_hook_ftello64(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ftello64(_get_actual_fp(fp));
}

size_t _hybris_hook_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    TRACE_HOOK("ptr %p size %zu nmemb %zu fp %p", ptr, size, nmemb, fp);

    return fwrite(ptr, size, nmemb, _get_actual_fp(fp));
}

int _hybris_hook_getc(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return getc(_get_actual_fp(fp));
}

ssize_t _hybris_hook_getdelim(char ** lineptr, size_t *n, int delimiter, FILE * fp)
{
    TRACE_HOOK("lineptr %p n %p delimiter %d fp %p", lineptr, n, delimiter, fp);

    return getdelim(lineptr, n, delimiter, _get_actual_fp(fp));
}

ssize_t _hybris_hook_getline(char **lineptr, size_t *n, FILE *fp)
{
    TRACE_HOOK("lineptr %p n %p fp %p", lineptr, n, fp);

    return getline(lineptr, n, _get_actual_fp(fp));
}

int _hybris_hook_putc(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return putc(c, _get_actual_fp(fp));
}

void _hybris_hook_rewind(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    rewind(_get_actual_fp(fp));
}

void _hybris_hook_setbuf(FILE *fp, char *buf)
{
    TRACE_HOOK("fp %p buf '%s'", fp, buf);

    setbuf(_get_actual_fp(fp), buf);
}

int _hybris_hook_setvbuf(FILE *fp, char *buf, int mode, size_t size)
{
    TRACE_HOOK("fp %p buf '%s' mode %d size %zu", fp, buf, mode, size);

    return setvbuf(_get_actual_fp(fp), buf, mode, size);
}

int _hybris_hook_ungetc(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return ungetc(c, _get_actual_fp(fp));
}

int _hybris_hook_vfprintf(FILE *fp, const char *fmt, va_list arg)
{
    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    return vfprintf(_get_actual_fp(fp), fmt, arg);
}

int _hybris_hook_vfscanf(FILE *fp, const char *fmt, va_list arg)
{
    TRACE_HOOK("fp %p fmt '%s'", fp, fmt);

    return vfscanf(_get_actual_fp(fp), fmt, arg);
}

int _hybris_hook_fileno(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fileno(_get_actual_fp(fp));
}

int _hybris_hook_pclose(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return pclose(_get_actual_fp(fp));
}

void _hybris_hook_flockfile(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return flockfile(_get_actual_fp(fp));
}

int _hybris_hook_ftrylockfile(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ftrylockfile(_get_actual_fp(fp));
}

void _hybris_hook_funlockfile(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return funlockfile(_get_actual_fp(fp));
}

void _hybris_hook_clearerr_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    clearerr_unlocked(_get_actual_fp(fp));
}

int _hybris_hook_feof_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return feof_unlocked(_get_actual_fp(fp));
}

int _hybris_hook_ferror_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return ferror_unlocked(_get_actual_fp(fp));
}

int _hybris_hook_fflush_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fflush_unlocked(_get_actual_fp(fp));
}

int _hybris_hook_fgetc_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fgetc_unlocked(_get_actual_fp(fp));
}

char* _hybris_hook_fgets_unlocked(char *s, int n, FILE *fp)
{
    TRACE_HOOK("s %s n %d fp %p", s, n, fp);

    return fgets_unlocked(s, n, _get_actual_fp(fp));
}

int _hybris_hook_fputc_unlocked(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return fputc_unlocked(c, _get_actual_fp(fp));
}

int _hybris_hook_fputs_unlocked(const char *s, FILE *fp)
{
    TRACE_HOOK("s '%s' fp %p", s, fp);

    return fputs_unlocked(s, _get_actual_fp(fp));
}

size_t _hybris_hook_fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    TRACE_HOOK("ptr %p size %zu nmemb %zu fp %p", ptr, size, nmemb, fp);

    return fread_unlocked(ptr, size, nmemb, _get_actual_fp(fp));
}

size_t _hybris_hook_fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    TRACE_HOOK("ptr %p size %zu nmemb %zu fp %p", ptr, size, nmemb, fp);

    return fwrite_unlocked(ptr, size, nmemb, _get_actual_fp(fp));
}

int _hybris_hook_getc_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return getc_unlocked(_get_actual_fp(fp));
}

int _hybris_hook_putc_unlocked(int c, FILE *fp)
{
    TRACE_HOOK("c %d fp %p", c, fp);

    return putc_unlocked(c, _get_actual_fp(fp));
}

int _hybris_hook_fileno_unlocked(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return fileno_unlocked(_get_actual_fp(fp));
}

/* exists only on the BSD platform
static char* _hybris_hook_fgetln(FILE *fp, size_t *len)
{
    return fgetln(_get_actual_fp(fp), len);
}
*/

int _hybris_hook_fpurge(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    __fpurge(_get_actual_fp(fp));

    return 0;
}

int _hybris_hook_getw(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return getw(_get_actual_fp(fp));
}

int _hybris_hook_putw(int w, FILE *fp)
{
    TRACE_HOOK("w %d fp %p", w, fp);

    return putw(w, _get_actual_fp(fp));
}

void _hybris_hook_setbuffer(FILE *fp, char *buf, int size)
{
    TRACE_HOOK("fp %p buf '%s' size %d", fp, buf, size);

    setbuffer(_get_actual_fp(fp), buf, size);
}

int _hybris_hook_setlinebuf(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    setlinebuf(_get_actual_fp(fp));

    return 0;
}


FILE* _hybris_hook_setmntent(const char *filename, const char *type)
{
    TRACE_HOOK("filename %s type %s", filename, type);

    return setmntent(filename, type);
}

struct mntent* _hybris_hook_getmntent(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    /* glibc doesn't allow NULL fp here, but bionic does. */
    if (fp == NULL)
        return NULL;

    return getmntent(_get_actual_fp(fp));
}

struct mntent* _hybris_hook_getmntent_r(FILE *fp, struct mntent *e, char *buf, int buf_len)
{
    TRACE_HOOK("fp %p e %p buf '%s' buf len %i",
               fp, e, buf, buf_len);

    /* glibc doesn't allow NULL fp here, but bionic does. */
    if (fp == NULL)
        return NULL;

    return getmntent_r(_get_actual_fp(fp), e, buf, buf_len);
}

int _hybris_hook_endmntent(FILE *fp)
{
    TRACE_HOOK("fp %p", fp);

    return endmntent(_get_actual_fp(fp));
}

int _hybris_hook_fputws(const wchar_t *ws, FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return fputws(ws, _get_actual_fp(stream));
}

int _hybris_hook_vfwprintf(FILE *stream, const wchar_t *format, va_list args)
{
    TRACE_HOOK("stream %p", stream);

    return vfwprintf(_get_actual_fp(stream), format, args);
}

wint_t _hybris_hook_fputwc(wchar_t wc, FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return fputwc(wc, _get_actual_fp(stream));
}

wint_t _hybris_hook_putwc(wchar_t wc, FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return putwc(wc, _get_actual_fp(stream));
}

wint_t _hybris_hook_fgetwc(FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return fgetwc(_get_actual_fp(stream));
}

wint_t _hybris_hook_getwc(FILE *stream)
{
    TRACE_HOOK("stream %p", stream);

    return getwc(_get_actual_fp(stream));
}

size_t _hybris_hook___fbufsize(FILE *stream)
{
    TRACE_HOOK("__fbufsize");
    return __fbufsize(_get_actual_fp(stream));
}

size_t _hybris_hook___fpending(FILE *stream)
{
    TRACE_HOOK("__fpending");
    return __fpending(_get_actual_fp(stream));
}

int _hybris_hook___flbf(FILE *stream)
{
    TRACE_HOOK("__flbf");
    return __flbf(_get_actual_fp(stream));
}

int _hybris_hook___freadable(FILE *stream)
{
    TRACE_HOOK("__freadable");
    return __freadable(_get_actual_fp(stream));
}

int _hybris_hook___fwritable(FILE *stream)
{
    TRACE_HOOK("__fwritable");
    return __fwritable(_get_actual_fp(stream));
}

int _hybris_hook___freading(FILE *stream)
{
    TRACE_HOOK("__freading");
    return __freading(_get_actual_fp(stream));
}

int _hybris_hook___fwriting(FILE *stream)
{
    TRACE_HOOK("__fwriting");
    return __fwriting(_get_actual_fp(stream));
}

int _hybris_hook___fsetlocking(FILE *stream, int type)
{
    TRACE_HOOK("__fsetlocking");
    return __fsetlocking(_get_actual_fp(stream), type);
}

void _hybris_hook__flushlbf(void)
{
    TRACE_HOOK("_flushlbf");
    _flushlbf();
}

void _hybris_hook___fpurge(FILE *stream)
{
    TRACE_HOOK("__fpurge");
    __fpurge(_get_actual_fp(stream));
}
