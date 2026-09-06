#ifndef HYBRIS_BIONIC_STDIO_H
#define HYBRIS_BIONIC_STDIO_H

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <wchar.h>
#include <mntent.h>
#include <hybris/common/floating_point_abi.h>

#if defined(__LP64__)
typedef int64_t bionic_off_t;
#else
// bionic uses 32-bit off_t on 32-bit architectures
typedef __kernel_off_t bionic_off_t;
#endif
typedef bionic_off_t bionic_fpos_t;
typedef off64_t bionic_fpos64_t;

__attribute__((visibility("hidden"))) extern int _hybris_hook___isthreaded;
__attribute__((visibility("hidden"))) extern char _hybris_hook_sF[];

__attribute__((visibility("hidden")))
void _hybris_hook_clearerr(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fclose(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_feof(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_ferror(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fflush(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fgetc(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fgetpos(FILE *fp, bionic_fpos_t *pos);
__attribute__((visibility("hidden")))
int _hybris_hook_fgetpos64(FILE *fp, bionic_fpos64_t *pos);
__attribute__((visibility("hidden")))
char* _hybris_hook_fgets(char *s, int n, FILE *fp);
__attribute__((visibility("hidden")))
FP_ATTRIB int _hybris_hook_fprintf(FILE *fp, const char *fmt, ...);
__attribute__((visibility("hidden")))
int _hybris_hook_fputc(int c, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fputs(const char *s, FILE *fp);
__attribute__((visibility("hidden")))
size_t _hybris_hook_fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
__attribute__((visibility("hidden")))
FILE* _hybris_hook_freopen(const char *filename, const char *mode, FILE *fp);
__attribute__((visibility("hidden")))
FILE* _hybris_hook_freopen64(const char *filename, const char *mode, FILE *fp);
__attribute__((visibility("hidden")))
FP_ATTRIB int _hybris_hook_fscanf(FILE *fp, const char *fmt, ...);
__attribute__((visibility("hidden")))
int _hybris_hook_fseek(FILE *fp, long offset, int whence);
__attribute__((visibility("hidden")))
int _hybris_hook_fseeko(FILE *fp, bionic_off_t offset, int whence);
__attribute__((visibility("hidden")))
int _hybris_hook_fseeko64(FILE *fp, off64_t offset, int whence);
__attribute__((visibility("hidden")))
int _hybris_hook_fsetpos(FILE *fp, const bionic_fpos_t *pos);
__attribute__((visibility("hidden")))
int _hybris_hook_fsetpos64(FILE *fp, const bionic_fpos64_t *pos);
__attribute__((visibility("hidden")))
long _hybris_hook_ftell(FILE *fp);
__attribute__((visibility("hidden")))
bionic_off_t _hybris_hook_ftello(FILE *fp);
__attribute__((visibility("hidden")))
off64_t _hybris_hook_ftello64(FILE *fp);
__attribute__((visibility("hidden")))
size_t _hybris_hook_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_getc(FILE *fp);
__attribute__((visibility("hidden")))
ssize_t _hybris_hook_getdelim(char ** lineptr, size_t *n, int delimiter, FILE * fp);
__attribute__((visibility("hidden")))
ssize_t _hybris_hook_getline(char **lineptr, size_t *n, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_putc(int c, FILE *fp);
__attribute__((visibility("hidden")))
void _hybris_hook_rewind(FILE *fp);
__attribute__((visibility("hidden")))
void _hybris_hook_setbuf(FILE *fp, char *buf);
__attribute__((visibility("hidden")))
int _hybris_hook_setvbuf(FILE *fp, char *buf, int mode, size_t size);
__attribute__((visibility("hidden")))
int _hybris_hook_ungetc(int c, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_vfprintf(FILE *fp, const char *fmt, va_list arg);
__attribute__((visibility("hidden")))
int _hybris_hook_vfscanf(FILE *fp, const char *fmt, va_list arg);
__attribute__((visibility("hidden")))
int _hybris_hook_fileno(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_pclose(FILE *fp);
__attribute__((visibility("hidden")))
void _hybris_hook_flockfile(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_ftrylockfile(FILE *fp);
__attribute__((visibility("hidden")))
void _hybris_hook_funlockfile(FILE *fp);
__attribute__((visibility("hidden")))
void _hybris_hook_clearerr_unlocked(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_feof_unlocked(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_ferror_unlocked(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fflush_unlocked(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fgetc_unlocked(FILE *fp);
__attribute__((visibility("hidden")))
char* _hybris_hook_fgets_unlocked(char *s, int n, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fputc_unlocked(int c, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fputs_unlocked(const char *s, FILE *fp);
__attribute__((visibility("hidden")))
size_t _hybris_hook_fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *fp);
__attribute__((visibility("hidden")))
size_t _hybris_hook_fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_getc_unlocked(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_putc_unlocked(int c, FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fileno_unlocked(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fpurge(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_getw(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_putw(int w, FILE *fp);
__attribute__((visibility("hidden")))
void _hybris_hook_setbuffer(FILE *fp, char *buf, int size);
__attribute__((visibility("hidden")))
int _hybris_hook_setlinebuf(FILE *fp);
__attribute__((visibility("hidden")))
FILE* _hybris_hook_setmntent(const char *filename, const char *type);
__attribute__((visibility("hidden")))
struct mntent* _hybris_hook_getmntent(FILE *fp);
__attribute__((visibility("hidden")))
struct mntent* _hybris_hook_getmntent_r(FILE *fp, struct mntent *e, char *buf, int buf_len);
int _hybris_hook_endmntent(FILE *fp);
__attribute__((visibility("hidden")))
int _hybris_hook_fputws(const wchar_t *ws, FILE *stream);
__attribute__((visibility("hidden")))
int _hybris_hook_vfwprintf(FILE *stream, const wchar_t *format, va_list args);
__attribute__((visibility("hidden")))
wint_t _hybris_hook_fputwc(wchar_t wc, FILE *stream);
__attribute__((visibility("hidden")))
wint_t _hybris_hook_putwc(wchar_t wc, FILE *stream);
__attribute__((visibility("hidden")))
wint_t _hybris_hook_fgetwc(FILE *stream);
__attribute__((visibility("hidden")))
wint_t _hybris_hook_getwc(FILE *stream);
__attribute__((visibility("hidden")))
size_t _hybris_hook___fbufsize(FILE *stream);
__attribute__((visibility("hidden")))
size_t _hybris_hook___fpending(FILE *stream);
__attribute__((visibility("hidden")))
int _hybris_hook___flbf(FILE *stream);
__attribute__((visibility("hidden")))
int _hybris_hook___freadable(FILE *stream);
__attribute__((visibility("hidden")))
int _hybris_hook___fwritable(FILE *stream);
__attribute__((visibility("hidden")))
int _hybris_hook___freading(FILE *stream);
__attribute__((visibility("hidden")))
int _hybris_hook___fwriting(FILE *stream);
__attribute__((visibility("hidden")))
int _hybris_hook___fsetlocking(FILE *stream, int type);
__attribute__((visibility("hidden")))
void _hybris_hook__flushlbf(void);
__attribute__((visibility("hidden")))
void _hybris_hook___fpurge(FILE *stream);

#endif
