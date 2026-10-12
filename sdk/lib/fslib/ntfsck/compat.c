/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Native implementations of the C library calls of the imported sources that
 *              ntdll.dll does not export (see compat_ros.h)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsck_ros.h"

int ntfsck_errno;

/*
 * Every allocation of a check comes from a private heap that NtfsckRun creates before the
 * check and destroys after it, so an error path that leaks memory upstream leaks nothing here.
 */
HANDLE NtfsckHeap;

void *ntfsck_malloc(size_t size)
{
    void *p = RtlAllocateHeap(NtfsckHeap, 0, size ? size : 1);
    if (!p)
        ntfsck_errno = ENOMEM;
    return p;
}

void *ntfsck_calloc(size_t nmemb, size_t size)
{
    void *p;
    if (size && nmemb > (SIZE_MAX / size))
    {
        ntfsck_errno = ENOMEM;
        return NULL;
    }
    size *= nmemb;
    p = RtlAllocateHeap(NtfsckHeap, HEAP_ZERO_MEMORY, size ? size : 1);
    if (!p)
        ntfsck_errno = ENOMEM;
    return p;
}

void *ntfsck_realloc(void *ptr, size_t size)
{
    void *p;
    if (!ptr)
        return ntfsck_malloc(size);
    if (!size)
    {
        ntfsck_free(ptr);
        return NULL;
    }
    p = RtlReAllocateHeap(NtfsckHeap, 0, ptr, size);
    if (!p)
        ntfsck_errno = ENOMEM;
    return p;
}

void ntfsck_free(void *ptr)
{
    if (ptr)
        RtlFreeHeap(NtfsckHeap, 0, ptr);
}

char *ntfsck_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = ntfsck_malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

char *ntfsck_strndup(const char *s, size_t n)
{
    size_t len = ntfsck_strnlen(s, n);
    char *p = ntfsck_malloc(len + 1);
    if (p)
    {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

size_t ntfsck_strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n])
        n++;
    return n;
}

/* C99 vsnprintf on top of ntdll's _vsnprintf: always terminated, returns the full length. */
int ntfsck_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char stackbuf[512];
    char *tmp = stackbuf;
    size_t cap = sizeof(stackbuf);
    int n;

    for (;;)
    {
        va_list aq;
        va_copy(aq, ap);
        n = _vsnprintf(tmp, cap, fmt, aq);
        va_end(aq);
        if (n >= 0 && (size_t)n < cap)
            break;
        if (tmp != stackbuf)
            RtlFreeHeap(RtlGetProcessHeap(), 0, tmp);
        if (cap >= (1u << 24))
        {
            n = -1;
            tmp = NULL;
            break;
        }
        cap *= 4;
        tmp = RtlAllocateHeap(RtlGetProcessHeap(), 0, cap);
        if (!tmp)
        {
            n = -1;
            break;
        }
    }
    if (size)
    {
        size_t c = (n < 0) ? 0 : (((size_t)n < size) ? (size_t)n : size - 1);
        if (tmp && c)
            memcpy(buf, tmp, c);
        buf[c] = '\0';
    }
    if (tmp && tmp != stackbuf)
        RtlFreeHeap(RtlGetProcessHeap(), 0, tmp);
    if (n < 0)
        ntfsck_errno = EOVERFLOW;
    return n;
}

int ntfsck_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = ntfsck_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/*
 * The standard streams: text is collected per stream and each complete line goes to the
 * line sink of the running check. A carriage return ends a transient line (a progress
 * bar) that is dropped.
 */
#define NTFSCK_LINE_MAX 1024
static char LineBuf[2][NTFSCK_LINE_MAX];
static size_t LineLen[2];

static int StreamIndex(FILE *stream)
{
    if (stream == NTFSCK_STDOUT)
        return 0;
    if (stream == NTFSCK_STDERR)
        return 1;
    return -1;
}

static void EmitLine(int idx);

/*
 * The checker writes a step header to stdout and ends it only when the step is done; a
 * problem it reports to stderr meanwhile must not come out before the header.
 */
static void EmitPendingHeader(void)
{
    if (LineLen[0] >= 7 && !strncmp(LineBuf[0], "Parse #", 7))
        EmitLine(0);
}

static void EmitLine(int idx)
{
    LineBuf[idx][LineLen[idx]] = '\0';
    NtfsckLine(idx, LineBuf[idx]);
    LineLen[idx] = 0;
}

static void StreamPut(int idx, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
    {
        char c = s[i];
        if (c == '\n')
        {
            if (idx == 1)
                EmitPendingHeader();
            EmitLine(idx);
        }
        else if (c == '\r')
        {
            LineLen[idx] = 0;
        }
        else
        {
            if (LineLen[idx] >= NTFSCK_LINE_MAX - 1)
                EmitLine(idx);
            LineBuf[idx][LineLen[idx]++] = c;
        }
    }
}

void NtfsckFlushStreams(void)
{
    int i;
    for (i = 0; i < 2; i++)
    {
        if (LineLen[i])
            EmitLine(i);
    }
}

void NtfsckResetStreams(void)
{
    LineLen[0] = LineLen[1] = 0;
}

int ntfsck_vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    char buf[NTFSCK_LINE_MAX];
    int idx = StreamIndex(stream);
    int n;

    if (idx < 0)
    {
        ntfsck_errno = EBADF;
        return -1;
    }
    n = ntfsck_vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0)
        return n;
    StreamPut(idx, buf, ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1);
    return n;
}

int ntfsck_fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = ntfsck_vfprintf(stream, fmt, ap);
    va_end(ap);
    return n;
}

int ntfsck_vprintf(const char *fmt, va_list ap)
{
    return ntfsck_vfprintf(NTFSCK_STDOUT, fmt, ap);
}

int ntfsck_printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = ntfsck_vfprintf(NTFSCK_STDOUT, fmt, ap);
    va_end(ap);
    return n;
}

int ntfsck_fputs(const char *s, FILE *stream)
{
    int idx = StreamIndex(stream);
    if (idx < 0)
        return EOF;
    StreamPut(idx, s, strlen(s));
    return 0;
}

int ntfsck_puts(const char *s)
{
    StreamPut(0, s, strlen(s));
    StreamPut(0, "\n", 1);
    return 0;
}

int ntfsck_fputc(int c, FILE *stream)
{
    char ch = (char)c;
    int idx = StreamIndex(stream);
    if (idx < 0)
        return EOF;
    StreamPut(idx, &ch, 1);
    return (unsigned char)c;
}

size_t ntfsck_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    int idx = StreamIndex(stream);
    if (idx < 0 || !size)
        return 0;
    StreamPut(idx, ptr, size * nmemb);
    return nmemb;
}

int ntfsck_fflush(FILE *stream)
{
    UNREFERENCED_PARAMETER(stream);
    return 0;
}

FILE *ntfsck_fopen(const char *path, const char *mode)
{
    UNREFERENCED_PARAMETER(path);
    UNREFERENCED_PARAMETER(mode);
    ntfsck_errno = ENOENT;
    return NULL;
}

int ntfsck_fclose(FILE *stream)
{
    UNREFERENCED_PARAMETER(stream);
    return 0;
}

/* There is no terminal: a question to the user reads end of input. */
char *ntfsck_fgets(char *s, int size, FILE *stream)
{
    UNREFERENCED_PARAMETER(s);
    UNREFERENCED_PARAMETER(size);
    UNREFERENCED_PARAMETER(stream);
    return NULL;
}

char *ntfsck_strerror(int errnum)
{
    switch (errnum)
    {
        case 0: return "Success";
        case EPERM: return "Operation not permitted";
        case ENOENT: return "No such file or directory";
        case EIO: return "Input/output error";
        case ENXIO: return "No such device or address";
        case EBADF: return "Bad file descriptor";
        case ENOMEM: return "Cannot allocate memory";
        case EACCES: return "Permission denied";
        case EBUSY: return "Device or resource busy";
        case EEXIST: return "File exists";
        case ENODEV: return "No such device";
        case ENOTDIR: return "Not a directory";
        case EISDIR: return "Is a directory";
        case EINVAL: return "Invalid argument";
        case ENOSPC: return "No space left on device";
        case EROFS: return "Read-only file system";
        case ERANGE: return "Numerical result out of range";
        case ENAMETOOLONG: return "File name too long";
        case ENOTEMPTY: return "Directory not empty";
        case EOVERFLOW: return "Value too large for defined data type";
        case EOPNOTSUPP: return "Operation not supported";
        case ENODATA: return "No data available";
        default: return "Unknown error";
    }
}

static unsigned long long StrToU64(const char *nptr, char **endptr, int base, int *neg, int *overflow)
{
    const char *s = nptr;
    unsigned long long v = 0;
    int any = 0;

    *neg = 0;
    *overflow = 0;
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '-' || *s == '+')
        *neg = (*s++ == '-');
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        s += 2;
        base = 16;
    }
    else if (base == 0)
    {
        base = (s[0] == '0') ? 8 : 10;
    }
    for (;; s++)
    {
        int d;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            d = *s - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        if (v > (ULLONG_MAX - d) / base)
            *overflow = 1;
        v = v * base + d;
        any = 1;
    }
    if (endptr)
        *endptr = (char *)(any ? s : nptr);
    return v;
}

long long ntfsck_strtoll(const char *nptr, char **endptr, int base)
{
    int neg, overflow;
    unsigned long long v = StrToU64(nptr, endptr, base, &neg, &overflow);
    if (overflow || (!neg && v > (unsigned long long)LLONG_MAX) ||
        (neg && v > (unsigned long long)LLONG_MAX + 1))
    {
        ntfsck_errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? -(long long)v : (long long)v;
}

unsigned long long ntfsck_strtoull(const char *nptr, char **endptr, int base)
{
    int neg, overflow;
    unsigned long long v = StrToU64(nptr, endptr, base, &neg, &overflow);
    if (overflow)
    {
        ntfsck_errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? (unsigned long long)(-(long long)v) : v;
}

long long ntfsck_strtoll_dec(const char *nptr)
{
    return ntfsck_strtoll(nptr, NULL, 10);
}

char *ntfsck_strsep(char **stringp, const char *delim)
{
    char *s = *stringp, *p;
    if (!s)
        return NULL;
    for (p = s; *p; p++)
    {
        if (strchr(delim, *p))
        {
            *p = '\0';
            *stringp = p + 1;
            return s;
        }
    }
    *stringp = NULL;
    return s;
}

int ntfsck_ffsl(long i)
{
    int bit;
    unsigned long u = (unsigned long)i;
    if (!u)
        return 0;
    for (bit = 1; !(u & 1); bit++)
        u >>= 1;
    return bit;
}

int ntfsck_ffs(int i)
{
    int bit;
    if (!i)
        return 0;
    for (bit = 1; !(i & 1); bit++)
        i = (int)((unsigned int)i >> 1);
    return bit;
}

/* No environment: the upstream knobs read from it (NTFSCK_SCRATCH_DIR) keep their defaults. */
char *ntfsck_getenv(const char *name)
{
    UNREFERENCED_PARAMETER(name);
    return NULL;
}

int ntfsck_setenv(const char *name, const char *value, int overwrite)
{
    UNREFERENCED_PARAMETER(name);
    UNREFERENCED_PARAMETER(value);
    UNREFERENCED_PARAMETER(overwrite);
    return 0;
}

time_t ntfsck_time(time_t *t)
{
    LARGE_INTEGER Now;
    ULONG Seconds = 0;
    time_t r;

    NtQuerySystemTime(&Now);
    RtlTimeToSecondsSince1970(&Now, &Seconds);
    r = (time_t)Seconds;
    if (t)
        *t = r;
    return r;
}

int ntfsck_getpid(void)
{
    return (int)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess;
}

static unsigned long RandomState = 1;

long ntfsck_random(void)
{
    RandomState = RandomState * 1103515245UL + 12345UL;
    return (long)((RandomState >> 1) & 0x7fffffff);
}

void ntfsck_srandom(unsigned int seed)
{
    RandomState = seed;
}

char *ntfsck_setlocale(int category, const char *locale)
{
    UNREFERENCED_PARAMETER(category);
    UNREFERENCED_PARAMETER(locale);
    return "C";
}

char *ntfsck_realpath(const char *path, char *resolved)
{
    size_t n = strlen(path) + 1;
    if (!resolved)
        return ntfsck_strdup(path);
    if (n > PATH_MAX)
    {
        ntfsck_errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(resolved, path, n);
    return resolved;
}

int ntfsck_stat(const char *path, struct stat *st)
{
    UNREFERENCED_PARAMETER(path);
    UNREFERENCED_PARAMETER(st);
    ntfsck_errno = ENOENT;
    return -1;
}

unsigned int ntfsck_sleep(unsigned int seconds)
{
    LARGE_INTEGER Interval;
    Interval.QuadPart = -10000000LL * seconds;
    NtDelayExecution(FALSE, &Interval);
    return 0;
}

int ntfsck_atexit(void (*fn)(void))
{
    UNREFERENCED_PARAMETER(fn);
    return 0;
}
