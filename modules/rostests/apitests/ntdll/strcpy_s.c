/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ntdll!strcpy_s (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows Server 2008 R2 and Windows 10 22H2.
 */

#include "precomp.h"

typedef int (__cdecl *PFN_strcpy_s)(char *, size_t, const char *);

static PFN_strcpy_s pstrcpy_s;

#define EINVAL_ 22
#define ERANGE_ 34

static void Check(size_t Length, const char *Source, int Expected, const UCHAR *ExpectedBytes, int Line)
{
    char Buffer[16];
    int Result;

    memset(Buffer, 'x', sizeof(Buffer));
    Result = pstrcpy_s(Buffer, Length, Source);
    ok(Result == Expected, "line %d: result %d, expected %d\n", Line, Result, Expected);
    ok(!memcmp(Buffer, ExpectedBytes, 6), "line %d: bytes %02x %02x %02x %02x %02x %02x\n", Line,
       (UCHAR)Buffer[0], (UCHAR)Buffer[1], (UCHAR)Buffer[2], (UCHAR)Buffer[3], (UCHAR)Buffer[4], (UCHAR)Buffer[5]);
}

#define CHECK(l, s, r, ...) do { static const UCHAR Bytes[6] = { __VA_ARGS__ }; Check(l, s, r, Bytes, __LINE__); } while (0)

START_TEST(strcpy_s)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    char Buffer[16];

    pstrcpy_s = (PFN_strcpy_s)GetProcAddress(Ntdll, "strcpy_s");
    /* Windows XP and Windows Server 2003 have no such function; ReactOS has it from export level 0x601 */
    if (!pstrcpy_s)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "ntdll does not export strcpy_s\n");
        skip("strcpy_s is not exported\n");
        return;
    }

    /* the string and its terminator fit */
    CHECK(16, "abc", 0, 'a', 'b', 'c', 0, 'x', 'x');
    CHECK(4, "abc", 0, 'a', 'b', 'c', 0, 'x', 'x');
    CHECK(8, "", 0, 0, 'x', 'x', 'x', 'x', 'x');
    CHECK(1, "", 0, 0, 'x', 'x', 'x', 'x', 'x');

    /* too small: the string is copied as far as the size goes and the first byte is zeroed afterwards */
    CHECK(3, "abc", ERANGE_, 0, 'b', 'c', 'x', 'x', 'x');
    CHECK(2, "ab", ERANGE_, 0, 'b', 'x', 'x', 'x', 'x');
    CHECK(1, "abc", ERANGE_, 0, 'x', 'x', 'x', 'x', 'x');
    CHECK(1, "a", ERANGE_, 0, 'x', 'x', 'x', 'x', 'x');

    /* invalid arguments */
    CHECK(0, "abc", EINVAL_, 'x', 'x', 'x', 'x', 'x', 'x');
    CHECK(4, NULL, EINVAL_, 0, 'x', 'x', 'x', 'x', 'x');
    ok_dec(pstrcpy_s(NULL, 4, "abc"), EINVAL_);
    ok_dec(pstrcpy_s(NULL, 0, "abc"), EINVAL_);

    /* the size is a size_t: a big one that is a 32-bit value is not a limit */
    memset(Buffer, 'x', sizeof(Buffer));
    ok_dec(pstrcpy_s(Buffer, 0xffffffffU, "abc"), 0);
    ok(!strcmp(Buffer, "abc"), "got %s\n", Buffer);

    /* the loop copies from the source as it goes, so an overlap is not a copy of the original */
    memset(Buffer, 'x', sizeof(Buffer));
    strcpy(Buffer, "abcdef");
    ok_dec(pstrcpy_s(Buffer + 1, 3, Buffer), ERANGE_);
    ok(!memcmp(Buffer, "\x61\x00\x61\x61\x65", 5), "bytes %02x %02x %02x %02x %02x\n",
       (UCHAR)Buffer[0], (UCHAR)Buffer[1], (UCHAR)Buffer[2], (UCHAR)Buffer[3], (UCHAR)Buffer[4]);
}
