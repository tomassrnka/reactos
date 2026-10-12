/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     C library functions of the imported sources that ntdll.dll does not export,
 *              redirected to reactos/compat.c
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _NTFSPROGS_REACTOS_COMPAT_H
#define _NTFSPROGS_REACTOS_COMPAT_H

/* The checker runs under one lock (reactos/ntfschk.c), so these are plain globals. */
extern int ntfsck_errno;
#undef errno
#define errno ntfsck_errno

/*
 * Standard streams. Output goes to the sink of the running check; there is no input and
 * no file: fopen fails, fgets returns NULL.
 */
#define NTFSCK_STDIN  ((FILE *)(uintptr_t)1)
#define NTFSCK_STDOUT ((FILE *)(uintptr_t)2)
#define NTFSCK_STDERR ((FILE *)(uintptr_t)3)
#undef stdin
#undef stdout
#undef stderr
#define stdin  NTFSCK_STDIN
#define stdout NTFSCK_STDOUT
#define stderr NTFSCK_STDERR

#undef malloc
#define malloc(...) ntfsck_malloc(__VA_ARGS__)
#undef calloc
#define calloc(...) ntfsck_calloc(__VA_ARGS__)
#undef realloc
#define realloc(...) ntfsck_realloc(__VA_ARGS__)
#undef free
#define free(...) ntfsck_free(__VA_ARGS__)
#undef strdup
#define strdup(...) ntfsck_strdup(__VA_ARGS__)
#undef strndup
#define strndup(...) ntfsck_strndup(__VA_ARGS__)
#undef snprintf
#define snprintf(...) ntfsck_snprintf(__VA_ARGS__)
#undef vsnprintf
#define vsnprintf(...) ntfsck_vsnprintf(__VA_ARGS__)
#undef printf
#define printf(...) ntfsck_printf(__VA_ARGS__)
#undef vprintf
#define vprintf(...) ntfsck_vprintf(__VA_ARGS__)
#undef fprintf
#define fprintf(...) ntfsck_fprintf(__VA_ARGS__)
#undef vfprintf
#define vfprintf(...) ntfsck_vfprintf(__VA_ARGS__)
#undef puts
#define puts(...) ntfsck_puts(__VA_ARGS__)
#undef fputs
#define fputs(...) ntfsck_fputs(__VA_ARGS__)
#undef fputc
#define fputc(...) ntfsck_fputc(__VA_ARGS__)
#undef putc
#define putc(...) ntfsck_fputc(__VA_ARGS__)
#undef putchar
#define putchar(c) ntfsck_fputc((c), NTFSCK_STDOUT)
#undef fwrite
#define fwrite(...) ntfsck_fwrite(__VA_ARGS__)
#undef fflush
#define fflush(...) ntfsck_fflush(__VA_ARGS__)
#undef fopen
#define fopen(...) ntfsck_fopen(__VA_ARGS__)
#undef fclose
#define fclose(...) ntfsck_fclose(__VA_ARGS__)
#undef fgets
#define fgets(...) ntfsck_fgets(__VA_ARGS__)
#undef strerror
#define strerror(...) ntfsck_strerror(__VA_ARGS__)
#undef strtoll
#define strtoll(...) ntfsck_strtoll(__VA_ARGS__)
#undef strtoull
#define strtoull(...) ntfsck_strtoull(__VA_ARGS__)
#undef atoll
#define atoll(...) ntfsck_strtoll_dec(__VA_ARGS__)
#undef strcasecmp
#define strcasecmp(...) _stricmp(__VA_ARGS__)
#undef strncasecmp
#define strncasecmp(...) _strnicmp(__VA_ARGS__)
#undef strsep
#define strsep(...) ntfsck_strsep(__VA_ARGS__)
#undef strnlen
#define strnlen(...) ntfsck_strnlen(__VA_ARGS__)
#undef ffs
#define ffs(...) ntfsck_ffs(__VA_ARGS__)
#undef ffsl
#define ffsl(...) ntfsck_ffsl(__VA_ARGS__)
#undef getenv
#define getenv(...) ntfsck_getenv(__VA_ARGS__)
#undef setenv
#define setenv(...) ntfsck_setenv(__VA_ARGS__)
#undef time
#define time(...) ntfsck_time(__VA_ARGS__)
#undef getpid
#define getpid(...) ntfsck_getpid(__VA_ARGS__)
#undef random
#define random(...) ntfsck_random(__VA_ARGS__)
#undef srandom
#define srandom(...) ntfsck_srandom(__VA_ARGS__)
#undef setlocale
#define setlocale(...) ntfsck_setlocale(__VA_ARGS__)
#undef realpath
#define realpath(...) ntfsck_realpath(__VA_ARGS__)
#undef stat
#define stat(p, s) ntfsck_stat((p), (s))
#undef exit
#define exit(...) ntfsck_exit(__VA_ARGS__)
#undef abort
#define abort(...) ntfsck_abort(__VA_ARGS__)
#undef sleep
#define sleep(...) ntfsck_sleep(__VA_ARGS__)
#undef atexit
#define atexit(...) ntfsck_atexit(__VA_ARGS__)

void *ntfsck_malloc(size_t size);
void *ntfsck_calloc(size_t nmemb, size_t size);
void *ntfsck_realloc(void *ptr, size_t size);
void ntfsck_free(void *ptr);
char *ntfsck_strdup(const char *s);
char *ntfsck_strndup(const char *s, size_t n);
int ntfsck_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ntfsck_snprintf(char *buf, size_t size, const char *fmt, ...);
int ntfsck_vfprintf(FILE *stream, const char *fmt, va_list ap);
int ntfsck_fprintf(FILE *stream, const char *fmt, ...);
int ntfsck_vprintf(const char *fmt, va_list ap);
int ntfsck_printf(const char *fmt, ...);
int ntfsck_puts(const char *s);
int ntfsck_fputs(const char *s, FILE *stream);
int ntfsck_fputc(int c, FILE *stream);
size_t ntfsck_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int ntfsck_fflush(FILE *stream);
FILE *ntfsck_fopen(const char *path, const char *mode);
int ntfsck_fclose(FILE *stream);
char *ntfsck_fgets(char *s, int size, FILE *stream);
char *ntfsck_strerror(int errnum);
long long ntfsck_strtoll(const char *nptr, char **endptr, int base);
unsigned long long ntfsck_strtoull(const char *nptr, char **endptr, int base);
long long ntfsck_strtoll_dec(const char *nptr);
char *ntfsck_strsep(char **stringp, const char *delim);
size_t ntfsck_strnlen(const char *s, size_t maxlen);
int ntfsck_ffs(int i);
int ntfsck_ffsl(long i);
char *ntfsck_getenv(const char *name);
int ntfsck_setenv(const char *name, const char *value, int overwrite);
time_t ntfsck_time(time_t *t);
int ntfsck_getpid(void);
long ntfsck_random(void);
void ntfsck_srandom(unsigned int seed);
char *ntfsck_setlocale(int category, const char *locale);
char *ntfsck_realpath(const char *path, char *resolved);
int ntfsck_stat(const char *path, struct stat *st);
void ntfsck_exit(int status);
void ntfsck_abort(void);
unsigned int ntfsck_sleep(unsigned int seconds);
int ntfsck_atexit(void (*fn)(void));

/* The mount-table and syslog interfaces have no meaning here; the sources keep their calls. */
#define LOG_NOTICE 5
#define LOG_ERR 3
#define LOG_PID 1
#define LOG_USER 8
#define openlog(i, o, f) ((void)0)
#undef syslog
#define syslog(p, ...) ((void)0)
#undef vsyslog
#define vsyslog(p, f, a) ((void)0)
#undef closelog
#define closelog() ((void)0)

#endif /* _NTFSPROGS_REACTOS_COMPAT_H */
