/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for RtlUnicodeToUTF8N and RtlUTF8ToUnicodeN, which Windows 7 and later export from ntdll
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The vectors are the ones of Wine's ntdll rtlstr test, which passes on Windows 10; the whole of the test ran on
 * Windows Server 2008 R2 and Windows 10 22H2 as well.
 */

#include "precomp.h"

typedef NTSTATUS (WINAPI *PFN_UnicodeToUTF8N)(CHAR *, ULONG, ULONG *, const WCHAR *, ULONG);
typedef NTSTATUS (WINAPI *PFN_UTF8ToUnicodeN)(WCHAR *, ULONG, ULONG *, const CHAR *, ULONG);

START_TEST(RtlUTF8)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_UnicodeToUTF8N pRtlUnicodeToUTF8N = (PFN_UnicodeToUTF8N)GetProcAddress(Ntdll, "RtlUnicodeToUTF8N");
    PFN_UTF8ToUnicodeN pRtlUTF8ToUnicodeN = (PFN_UTF8ToUnicodeN)GetProcAddress(Ntdll, "RtlUTF8ToUnicodeN");
    const WCHAR Empty[] = { 0 };
    const WCHAR Special[] = { 'X', 0x80, 0xd800, 0 };
    const UCHAR SpecialExpected[] = { 'X', 0xc2, 0x80, 0xef, 0xbf, 0xbd, 0 };
    const CHAR Utf8[] = { 'X', (CHAR)0xc2, (CHAR)0x80, (CHAR)0xe2, (CHAR)0x82, (CHAR)0xac, (CHAR)0xf0, (CHAR)0x9f, (CHAR)0x98, (CHAR)0x80 };
    const WCHAR Utf16[] = { 'X', 0x80, 0x20ac, 0xd83d, 0xde00 };
    CHAR Buffer[32];
    WCHAR Wide[16];
    ULONG BytesOut, i;
    NTSTATUS Status;

    /* Windows Vista and older have neither; ReactOS has them from export level 0x601 */
    if (!pRtlUnicodeToUTF8N || !pRtlUTF8ToUnicodeN)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "RtlUnicodeToUTF8N or RtlUTF8ToUnicodeN is not exported\n");
        skip("the UTF-8 functions are not exported\n");
        return;
    }

    /* parameter checks: the source and the result are looked at in this order */
    Status = pRtlUnicodeToUTF8N(NULL, 0, NULL, NULL, 0);
    ok_hex(Status, STATUS_INVALID_PARAMETER_4);
    Status = pRtlUnicodeToUTF8N(NULL, 0, NULL, Empty, 0);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    Status = pRtlUTF8ToUnicodeN(NULL, 0, NULL, NULL, 0);
    ok_hex(Status, STATUS_INVALID_PARAMETER_4);
    Status = pRtlUTF8ToUnicodeN(NULL, 0, NULL, "", 0);
    ok_hex(Status, STATUS_INVALID_PARAMETER);

    /* a size query needs no buffer */
    BytesOut = 0x55555555;
    Status = pRtlUnicodeToUTF8N(NULL, 0, &BytesOut, Special, sizeof(Special));
    ok_hex(Status, STATUS_SOME_NOT_MAPPED);
    ok_dec(BytesOut, 7);

    /* a lone surrogate becomes U+FFFD, the rest of the string is converted */
    memset(Buffer, 0x55, sizeof(Buffer));
    BytesOut = 0xdeadbeef;
    Status = pRtlUnicodeToUTF8N(Buffer, 7, &BytesOut, Special, sizeof(Special));
    ok_hex(Status, STATUS_SOME_NOT_MAPPED);
    ok_dec(BytesOut, 7);
    ok(!memcmp(Buffer, SpecialExpected, 7), "bad conversion\n");

    /* a buffer that is too small is filled with whole characters as far as it goes */
    for (i = 0; i <= 6; i++)
    {
        static const ULONG Fits[] = { 0, 1, 1, 3, 3, 3, 6 };

        memset(Buffer, 0x55, sizeof(Buffer));
        BytesOut = 0xdeadbeef;
        Status = pRtlUnicodeToUTF8N(Buffer, i, &BytesOut, Special, sizeof(Special));
        ok(Status == STATUS_BUFFER_TOO_SMALL, "%lu: status %#lx\n", i, Status);
        ok(BytesOut == Fits[i], "%lu: got %lu, expected %lu\n", i, BytesOut, Fits[i]);
        ok(!memcmp(Buffer, SpecialExpected, Fits[i]), "%lu: bad conversion\n", i);
    }

    /* a valid string of one to four bytes per character, in both directions */
    memset(Buffer, 0x55, sizeof(Buffer));
    BytesOut = 0;
    Status = pRtlUnicodeToUTF8N(Buffer, sizeof(Buffer), &BytesOut, Utf16, sizeof(Utf16));
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(BytesOut, sizeof(Utf8));
    ok(!memcmp(Buffer, Utf8, sizeof(Utf8)), "bad conversion\n");

    memset(Wide, 0x55, sizeof(Wide));
    BytesOut = 0;
    Status = pRtlUTF8ToUnicodeN(Wide, sizeof(Wide), &BytesOut, Utf8, sizeof(Utf8));
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(BytesOut, sizeof(Utf16));
    ok(!memcmp(Wide, Utf16, sizeof(Utf16)), "bad conversion\n");

    /* an invalid byte becomes U+FFFD and is reported */
    {
        const CHAR Bad[] = { 'a', (CHAR)0xff, 'b' };

        BytesOut = 0;
        Status = pRtlUTF8ToUnicodeN(Wide, sizeof(Wide), &BytesOut, Bad, sizeof(Bad));
        ok_hex(Status, STATUS_SOME_NOT_MAPPED);
        ok_dec(BytesOut, 3 * sizeof(WCHAR));
        ok(Wide[0] == 'a' && Wide[1] == 0xfffd && Wide[2] == 'b', "bad conversion %x %x %x\n", Wide[0], Wide[1], Wide[2]);
    }

    /* a lead byte takes the continuation bytes that follow it; a sequence cut short by the end of the string is one character (measured) */
    {
        static const struct
        {
            UCHAR Bytes[4];
            ULONG Length;
            ULONG Count;
            WCHAR Expected[4];
        } Cases[] =
        {
            { { 0xe2, 0x41 },             2, 2, { 0xfffd, 0x41 } },
            { { 0xe2, 0x82 },             2, 1, { 0xfffd } },
            { { 0xe2, 0x82, 0x41 },       3, 2, { 0xfffd, 0x41 } },
            { { 0xf0, 0x9f, 0x41 },       3, 2, { 0xfffd, 0x41 } },
            { { 0x41, 0xc3 },             2, 2, { 0x41, 0xfffd } },
            { { 0xc3 },                   1, 1, { 0xfffd } },
            { { 0xf0, 0x9f, 0x98 },       3, 1, { 0xfffd } },
            { { 0xe2, 0x41, 0x42 },       3, 3, { 0xfffd, 0x41, 0x42 } },
            { { 0xc3, 0x41, 0x42 },       3, 3, { 0xfffd, 0x41, 0x42 } },
            { { 0xf0, 0x41, 0x42, 0x43 }, 4, 4, { 0xfffd, 0x41, 0x42, 0x43 } },
            { { 0xe0, 0x80 },             2, 1, { 0xfffd } },
            { { 0xed, 0xa0 },             2, 1, { 0xfffd } },
            { { 0xf4, 0x90 },             2, 1, { 0xfffd } },
        };

        for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
        {
            ULONG SizeOnly = 0;

            memset(Wide, 0x55, sizeof(Wide));
            BytesOut = 0;
            Status = pRtlUTF8ToUnicodeN(Wide, sizeof(Wide), &BytesOut, (const CHAR *)Cases[i].Bytes, Cases[i].Length);
            ok(Status == STATUS_SOME_NOT_MAPPED, "%lu: status %#lx\n", i, Status);
            ok(BytesOut == Cases[i].Count * sizeof(WCHAR), "%lu: got %lu bytes\n", i, BytesOut);
            ok(!memcmp(Wide, Cases[i].Expected, Cases[i].Count * sizeof(WCHAR)), "%lu: bad conversion\n", i);
            Status = pRtlUTF8ToUnicodeN(NULL, 0, &SizeOnly, (const CHAR *)Cases[i].Bytes, Cases[i].Length);
            ok(Status == STATUS_SOME_NOT_MAPPED, "%lu: size query status %#lx\n", i, Status);
            ok(SizeOnly == Cases[i].Count * sizeof(WCHAR), "%lu: size query %lu\n", i, SizeOnly);
        }
    }
}
