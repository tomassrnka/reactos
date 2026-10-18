/*
 * PROJECT:   SORT - Adapted for ReactOS
 * LICENSE:   GPL - See COPYING in the top level directory
 * PURPOSE:   Reads line of a file and sorts them in order
 * COPYRIGHT: Copyright 1995 Jim Lynch
 *
 */

#include <stdlib.h>
#include <errno.h>
#include <wctype.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <io.h>
#include <fcntl.h>

#include <windef.h>
#include <winbase.h>
#include <winnls.h>

typedef struct _LINE
{
    const char *Text;       /* the original bytes, written out unchanged */
    size_t Length;
    const WCHAR *Key;       /* the line in the OEM code page, compared */
    int KeyLength;
    size_t Index;
} LINE;

/* Reverse flag */
static int rev;

/* Sort column, counted from 0 */
static int sortcol;

/* The "C" locale: ordinal comparison */
static int ordinal;

static void usage(FILE *stream)
{
    fputs("Sorts input and writes the result to the standard output or a file.\n\n", stream);
    fputs("SORT [/R] [/+n] [/M kilobytes] [/L locale] [/REC characters]\n", stream);
    fputs("     [[drive:][path]filename] [/T [drive:][path]] [/O [drive:][path]filename]\n\n", stream);
    fputs("  /R[EVERSE]            Sort in reverse order.\n", stream);
    fputs("  /+n                   Compare from character n of each line; shorter lines\n", stream);
    fputs("                        sort first.\n", stream);
    fputs("  /L[OCALE] locale      Use the \"C\" locale (ordinal order) instead of the\n", stream);
    fputs("                        system locale. Sorting ignores case.\n", stream);
    fputs("  /M[EMORY] kilobytes   Accepted for compatibility; not used.\n", stream);
    fputs("  /REC[ORD_MAXIMUM] n   Accepted for compatibility; not used.\n", stream);
    fputs("  /T[EMPORARY] path     Accepted for compatibility; not used.\n", stream);
    fputs("  /O[UTPUT] filename    Write the result to this file.\n", stream);
    fputs("  filename              The file to sort; standard input if none is given.\n", stream);
}

static void fail(const char *message)
{
    fputs(message, stderr);
    fputs("\n\n", stderr);
    exit(1);
}

static void fail_system(const WCHAR *name, DWORD error)
{
    WCHAR *text = NULL;

    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, error, 0, (LPWSTR)&text, 0, NULL);
    /* The system message ends its own line */
    fwprintf(stderr, L"%s: %s", name, text ? text : L"\n");
    LocalFree(text);
    exit(1);
}

static void fail_crt(const WCHAR *name, int error)
{
    /* A failed stdio call reports through errno, not GetLastError */
    if (error == 0)
        fail_system(name, ERROR_WRITE_FAULT);
    fwprintf(stderr, L"%s: %hs\n", name, strerror(error));
    exit(1);
}

static void *xrealloc(void *block, size_t size)
{
    void *p = realloc(block, size);
    if (p == NULL)
        fail("SORT: Insufficient memory");
    return p;
}

/* Matches /NAME by a prefix of at least minimum characters */
static int is_option(const WCHAR *arg, const WCHAR *name, size_t minimum)
{
    size_t length = wcslen(arg);
    return length >= minimum && length <= wcslen(name) && !_wcsnicmp(arg, name, length);
}

static char *read_handle(HANDLE handle, const WCHAR *name, size_t *size)
{
    char *data = NULL;
    size_t used = 0, allocated = 0;
    DWORD got;

    for (;;)
    {
        if (allocated - used < 65536)
        {
            allocated = allocated ? allocated * 2 : 65536;
            data = xrealloc(data, allocated);
        }
        if (!ReadFile(handle, data + used, (DWORD)min(allocated - used, 0x10000000), &got, NULL))
        {
            if (GetLastError() == ERROR_BROKEN_PIPE)
                break;
            fail_system(name, GetLastError());
        }
        if (got == 0)
            break;
        used += got;
    }

    *size = used;
    return data;
}

static char *read_console(size_t *size)
{
    char *data = NULL;
    size_t used = 0, allocated = 0, got;

    /* Text mode ends console input at Ctrl+Z */
    for (;;)
    {
        if (allocated - used < 4096)
        {
            allocated = allocated ? allocated * 2 : 65536;
            data = xrealloc(data, allocated);
        }
        got = fread(data + used, 1, allocated - used, stdin);
        if (got == 0)
        {
            if (ferror(stdin))
                fail_crt(L"SORT", errno);
            break;
        }
        used += got;
    }

    *size = used;
    return data;
}

static int compare_lines(const void *a, const void *b)
{
    const LINE *A = (const LINE *)a;
    const LINE *B = (const LINE *)b;
    const WCHAR *ka = A->Key + min(sortcol, A->KeyLength);
    const WCHAR *kb = B->Key + min(sortcol, B->KeyLength);
    int la = A->KeyLength - min(sortcol, A->KeyLength);
    int lb = B->KeyLength - min(sortcol, B->KeyLength);
    int result;

    /* Lines that end before the sort column stay in the reverse of their input order, even for /R */
    if (la == 0 && lb == 0)
        return (A->Index < B->Index) - (A->Index > B->Index);

    if (ordinal)
    {
        /* Ordinal, without case, and a NUL is a character like any other */
        int i, n = min(la, lb);

        result = 0;
        for (i = 0; i < n && result == 0; i++)
        {
            WCHAR ca = towlower(ka[i]), cb = towlower(kb[i]);
            result = (ca > cb) - (ca < cb);
        }
        if (result == 0)
            result = (la > lb) - (la < lb);
    }
    else
    {
        result = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, ka, la, kb, lb);
        if (result == 0)
            fail_system(L"SORT", GetLastError());
        result -= CSTR_EQUAL;
    }

    /* Equal keys come out in the reverse of their input order */
    if (result == 0)
        result = (A->Index < B->Index) - (A->Index > B->Index);

    return rev ? -result : result;
}

int wmain(int argc, WCHAR **argv)
{
    const WCHAR *input = NULL, *output = NULL;
    char *data, *p, *end;
    WCHAR *keys, *key;
    LINE *lines = NULL;
    size_t size, count = 0, allocated = 0, keychars = 0, i;
    HANDLE handle;
    FILE *out;
    int n;

    for (n = 1; n < argc; n++)
    {
        const WCHAR *arg = argv[n];

        if (arg[0] != L'/')
        {
            if (input)
                fail("Input file specified two times.");
            input = arg;
            continue;
        }

        arg++;
        if (!wcscmp(arg, L"?"))
        {
            usage(stdout);
            return 0;
        }
        else if (arg[0] == L'+')
        {
            long column = wcstol(arg + 1, NULL, 10);
            if (column < 1)
                fail("Sort position must be greater than zero.");
            sortcol = (column > INT_MAX) ? INT_MAX : (int)column - 1;
        }
        else if (is_option(arg, L"RECORD_MAXIMUM", 3) || is_option(arg, L"TEMPORARY", 1))
        {
            /* All records are kept in memory, whatever their length */
            if (++n == argc)
                fail("Invalid switch.");
        }
        else if (is_option(arg, L"MEMORY", 1))
        {
            if (++n == argc)
                fail("Invalid switch.");
            if (wcstol(argv[n], NULL, 10) < 160)
                fputs("Warning: the specified memory size is too low and will be adjusted to the minimum.\n\n", stderr);
        }
        else if (is_option(arg, L"REVERSE", 1))
        {
            rev = 1;
        }
        else if (is_option(arg, L"LOCALE", 1))
        {
            if (++n == argc)
                fail("Invalid switch.");
            if (_wcsicmp(argv[n], L"C"))
                fail("Invalid locale.");
            ordinal = 1;
        }
        else if (is_option(arg, L"OUTPUT", 1))
        {
            if (++n == argc)
                fail("Invalid switch.");
            output = argv[n];
        }
        else
        {
            fail("Invalid switch.");
        }
    }

    if (input)
    {
        handle = CreateFileW(input, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE)
            fail_system(input, GetLastError());
        data = read_handle(handle, input, &size);
        CloseHandle(handle);
    }
    else if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR)
    {
        data = read_console(&size);
    }
    else
    {
        data = read_handle(GetStdHandle(STD_INPUT_HANDLE), L"SORT", &size);
    }

    /* Lines end in LF or CR LF; a last line without an end still counts */
    for (p = data, end = data + size; p < end; )
    {
        char *eol = memchr(p, '\n', end - p);
        size_t length = (eol ? eol : end) - p;

        if (eol && length && p[length - 1] == '\r')
            length--;
        if (length > INT_MAX)
            fail("SORT: A line is too long.");
        if (count == allocated)
        {
            allocated = allocated ? allocated * 2 : 1024;
            lines = xrealloc(lines, allocated * sizeof(LINE));
        }
        lines[count].Text = p;
        lines[count].Length = length;
        lines[count].KeyLength = length ? MultiByteToWideChar(CP_OEMCP, 0, p, (int)length, NULL, 0) : 0;
        if (length && !lines[count].KeyLength)
            fail_system(input ? input : L"SORT", GetLastError());
        lines[count].Index = count;
        keychars += lines[count].KeyLength;
        count++;
        p = eol ? eol + 1 : end;
    }

    /* Windows reads text in the OEM code page, whatever the console uses */
    keys = xrealloc(NULL, (keychars + 1) * sizeof(WCHAR));
    for (i = 0, key = keys; i < count; i++)
    {
        if (lines[i].KeyLength)
            MultiByteToWideChar(CP_OEMCP, 0, lines[i].Text, (int)lines[i].Length, key, lines[i].KeyLength);
        lines[i].Key = key;
        key += lines[i].KeyLength;
    }

    qsort(lines, count, sizeof(LINE), compare_lines);

    if (output)
    {
        /* Opened only now, so the output may replace the input file */
        handle = CreateFileW(output, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE)
            fail_system(output, GetLastError());
        n = _open_osfhandle((intptr_t)handle, 0);
        if (n == -1)
        {
            CloseHandle(handle);
            fail_crt(output, errno);
        }
        out = _fdopen(n, "wb");
        if (out == NULL)
        {
            int error = errno;

            _close(n);
            fail_crt(output, error);
        }
    }
    else
    {
        out = stdout;
        _setmode(_fileno(stdout), _O_BINARY);
    }

    for (i = 0; i < count; i++)
    {
        if (fwrite(lines[i].Text, 1, lines[i].Length, out) != lines[i].Length ||
            fwrite("\r\n", 1, 2, out) != 2)
        {
            fail_crt(output ? output : L"SORT", errno);
        }
    }

    if (fflush(out) || ferror(out) || (out != stdout && fclose(out)))
        fail_crt(output ? output : L"SORT", errno);

    free(keys);
    free(lines);
    free(data);
    return 0;
}
/* EOF */
