/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for LoadStringA and LoadStringW as the api-ms-win-core-libraryloader contract has them
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64), where user32 forwards to the same code.
 * The strings are in the string table of this program.
 */

#include "precomp.h"

typedef int (WINAPI *PFN_LoadStringW)(HINSTANCE, UINT, LPWSTR, int);
typedef int (WINAPI *PFN_LoadStringA)(HINSTANCE, UINT, LPSTR, int);

#define PENDING_ERROR 0xdeadbeef

static PFN_LoadStringW pLoadStringW;
static PFN_LoadStringA pLoadStringA;

static const struct
{
    UINT Id;
    const char *Text;
} Strings[] =
{
    { 1, "first" },
    { 2, "second string" },
    { 3, "" },
    { 15, "last of block 1" },
    { 16, "block two first" },
    { 17, "ab" },
    { 0x12, "" },
    { 4660, "high id string" },
    { 65535, "id ffff" },
    { 0x10001, "first" },
    { 0xffff0001, "first" },
};

static void TestStrings(HINSTANCE Instance)
{
    static const int Sizes[] = { 64, 14, 13, 12, 6, 5, 2, 1 };
    WCHAR Wide[80], Expected[80];
    char Narrow[80];
    ULONG i, j, k;
    int Result, Length, Want;

    for (i = 0; i < RTL_NUMBER_OF(Strings); i++)
    {
        Length = (int)strlen(Strings[i].Text);
        for (k = 0; k <= (ULONG)Length; k++)
            Expected[k] = (WCHAR)(UCHAR)Strings[i].Text[k];

        for (j = 0; j < RTL_NUMBER_OF(Sizes); j++)
        {
            Want = min(Sizes[j] - 1, Length);

            memset(Wide, 0x55, sizeof(Wide));
            SetLastError(PENDING_ERROR);
            Result = pLoadStringW(Instance, Strings[i].Id, Wide, Sizes[j]);
            ok(Result == Want, "W %#x size %d: got %d, expected %d\n", Strings[i].Id, Sizes[j], Result, Want);
            ok(GetLastError() == PENDING_ERROR, "W %#x: the last error became %lu\n", Strings[i].Id, GetLastError());
            ok(!memcmp(Wide, Expected, Want * sizeof(WCHAR)) && Wide[Want] == 0, "W %#x size %d: bad text\n", Strings[i].Id, Sizes[j]);
            ok(Wide[Sizes[j]] == 0x5555, "W %#x size %d: wrote too much\n", Strings[i].Id, Sizes[j]);

            memset(Narrow, 0x55, sizeof(Narrow));
            Result = pLoadStringA(Instance, Strings[i].Id, Narrow, Sizes[j]);
            ok(Result == Want, "A %#x size %d: got %d, expected %d\n", Strings[i].Id, Sizes[j], Result, Want);
            ok(!memcmp(Narrow, Strings[i].Text, Want) && Narrow[Want] == 0, "A %#x size %d: bad text\n", Strings[i].Id, Sizes[j]);
            ok((UCHAR)Narrow[Sizes[j]] == 0x55, "A %#x size %d: wrote too much\n", Strings[i].Id, Sizes[j]);
        }

        /* a size of zero gives a pointer into the resource, through the buffer argument */
        {
            PCWSTR Pointer = NULL;

            SetLastError(PENDING_ERROR);
            Result = pLoadStringW(Instance, Strings[i].Id, (LPWSTR)&Pointer, 0);
            ok(Result == Length, "W %#x size 0: got %d, expected %d\n", Strings[i].Id, Result, Length);
            ok(Pointer != NULL, "W %#x size 0: no pointer\n", Strings[i].Id);
            if (Pointer && Length)
                ok(!memcmp(Pointer, Expected, Length * sizeof(WCHAR)), "W %#x size 0: bad text\n", Strings[i].Id);
        }
        /* the ANSI form says -1 for a string with characters; an empty string is 0 and gets its terminator */
        memset(Narrow, 0x55, sizeof(Narrow));
        Result = pLoadStringA(Instance, Strings[i].Id, Narrow, 0);
        ok(Result == (Length ? -1 : 0), "A %#x size 0: got %d\n", Strings[i].Id, Result);
        if (!Length)
            ok(Narrow[0] == 0 && (UCHAR)Narrow[1] == 0x55, "A %#x size 0: no terminator or too much written\n", Strings[i].Id);
    }
}

START_TEST(LoadString)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    HINSTANCE Self = GetModuleHandleW(NULL);
    WCHAR Wide[16];
    char Narrow[16];
    int Result;

    pLoadStringW = (PFN_LoadStringW)GetProcAddress(Kernel32, "LoadStringW");
    pLoadStringA = (PFN_LoadStringA)GetProcAddress(Kernel32, "LoadStringA");
    if (!pLoadStringW || !pLoadStringA)
    {
        /* Windows has them in kernelbase, which user32 and the API set forward to; ReactOS has them in kernel32 */
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")), "kernel32 does not export LoadStringA and LoadStringW\n");
        pLoadStringW = (PFN_LoadStringW)GetProcAddress(LoadLibraryW(L"user32.dll"), "LoadStringW");
        pLoadStringA = (PFN_LoadStringA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "LoadStringA");
        if (!pLoadStringW || !pLoadStringA)
        {
            skip("no LoadString\n");
            return;
        }
    }

    TestStrings(Self);

    /* no buffer: nothing, and the invalid parameter error (measured on kernelbase of Windows Server 2008 R2 and Windows 10) */
    SetLastError(PENDING_ERROR);
    ok_dec(pLoadStringW(Self, 1, NULL, 0), 0);
    ok_dec(GetLastError(), ERROR_INVALID_PARAMETER);
    SetLastError(PENDING_ERROR);
    ok_dec(pLoadStringW(Self, 1, NULL, 5), 0);
    ok_dec(GetLastError(), ERROR_INVALID_PARAMETER);

    /* a null instance is the program itself */
    memset(Wide, 0x55, sizeof(Wide));
    Result = pLoadStringW(NULL, 1, Wide, RTL_NUMBER_OF(Wide));
    ok_dec(Result, 5);
    ok(!wcscmp(Wide, L"first"), "bad text\n");
    memset(Narrow, 0x55, sizeof(Narrow));
    Result = pLoadStringA(NULL, 1, Narrow, sizeof(Narrow));
    ok_dec(Result, 5);
    ok(!strcmp(Narrow, "first"), "bad text\n");

    /* a string that is not in the table: nothing, the buffer is made empty and the resource lookup sets the error */
    memset(Wide, 0x55, sizeof(Wide));
    SetLastError(PENDING_ERROR);
    Result = pLoadStringW(Self, 9999, Wide, RTL_NUMBER_OF(Wide));
    ok_dec(Result, 0);
    ok_dec(GetLastError(), ERROR_RESOURCE_NAME_NOT_FOUND);
    ok_hex(Wide[0], 0);
    memset(Narrow, 0x55, sizeof(Narrow));
    SetLastError(PENDING_ERROR);
    Result = pLoadStringA(Self, 9999, Narrow, sizeof(Narrow));
    ok_dec(Result, 0);
    ok_dec(GetLastError(), ERROR_RESOURCE_NAME_NOT_FOUND);
    ok_hex((UCHAR)Narrow[0], 0);
    SetLastError(PENDING_ERROR);
    Result = pLoadStringW(Self, 9999, (LPWSTR)&Wide[8], 0);
    ok_dec(Result, 0);
    memset(Narrow, 0x55, sizeof(Narrow));
    SetLastError(PENDING_ERROR);
    Result = pLoadStringA(Self, 9999, Narrow, 0);
    ok_dec(Result, 0);
    ok_dec(GetLastError(), ERROR_RESOURCE_NAME_NOT_FOUND);
    ok(Narrow[0] == 0 && (UCHAR)Narrow[1] == 0x55, "no terminator or too much written\n");

    /* not a module */
    SetLastError(PENDING_ERROR);
    Result = pLoadStringW((HINSTANCE)(ULONG_PTR)0x12340000, 1, Wide, RTL_NUMBER_OF(Wide));
    ok_dec(Result, 0);
    ok_dec(GetLastError(), ERROR_RESOURCE_DATA_NOT_FOUND);
    SetLastError(PENDING_ERROR);
    Result = pLoadStringA((HINSTANCE)(ULONG_PTR)0x12340000, 1, Narrow, sizeof(Narrow));
    ok_dec(Result, 0);
    ok_dec(GetLastError(), ERROR_RESOURCE_DATA_NOT_FOUND);
}
