/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for SetProcessPreferredUILanguages and GetProcessPreferredUILanguages (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The vectors were measured on Windows Server 2008 R2 (x64 and WOW64).
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_Set)(DWORD, PCZZWSTR, PULONG);
typedef BOOL (WINAPI *PFN_Get)(DWORD, PULONG, PZZWSTR, PULONG);

#define PENDING_ERROR 0xdeadbeef
#define MUI_ID   0x4
#define MUI_NAME 0x8

static PFN_Set pSet;
static PFN_Get pGet;

/* A multi-string is written with "|" in place of its zeros to keep the vectors readable */
static void ToMulti(const char *Text, WCHAR *Out)
{
    while (*Text)
    {
        *Out++ = *Text == '|' ? 0 : (WCHAR)*Text;
        Text++;
    }
    *Out++ = 0;
}

static void CheckSet(DWORD Flags, const char *Text, BOOL Expected, DWORD ExpectedError, ULONG ExpectedCount, int Line)
{
    WCHAR Buffer[256];
    ULONG Count = 0xdead;
    BOOL Result;
    DWORD Error;

    ToMulti(Text, Buffer);
    SetLastError(PENDING_ERROR);
    Result = pSet(Flags, Buffer, &Count);
    Error = GetLastError();
    ok(Result == Expected, "line %d: result %d, expected %d\n", Line, Result, Expected);
    ok(Error == ExpectedError, "line %d: error %lu, expected %lu\n", Line, Error, ExpectedError);
    ok(Count == ExpectedCount, "line %d: count %lu, expected %lu\n", Line, Count, ExpectedCount);
}

static void CheckGet(DWORD Flags, const char *Expected, ULONG ExpectedCount, int Line)
{
    WCHAR Buffer[256], Wanted[256];
    ULONG Count = 0xdead, Size = RTL_NUMBER_OF(Buffer), WantedSize;
    BOOL Result;

    ToMulti(Expected, Wanted);
    WantedSize = 0;
    while (Wanted[WantedSize] || Wanted[WantedSize + 1])
        WantedSize++;
    WantedSize += 2;

    memset(Buffer, 0x55, sizeof(Buffer));
    SetLastError(PENDING_ERROR);
    Result = pGet(Flags, &Count, Buffer, &Size);
    ok(Result, "line %d: failed with %lu\n", Line, GetLastError());
    ok(GetLastError() == PENDING_ERROR, "line %d: the last error changed to %lu\n", Line, GetLastError());
    ok(Count == ExpectedCount, "line %d: count %lu, expected %lu\n", Line, Count, ExpectedCount);
    ok(Size == WantedSize, "line %d: size %lu, expected %lu\n", Line, Size, WantedSize);
    if (Result && Size == WantedSize)
        ok(!memcmp(Buffer, Wanted, WantedSize * sizeof(WCHAR)), "line %d: the list is not the expected one\n", Line);
}

#define SETOK(f, t, n) CheckSet(f, t, TRUE, PENDING_ERROR, n, __LINE__)
#define SETFAIL(f, t, e) CheckSet(f, t, FALSE, e, 0xdead, __LINE__)
#define GETOK(f, t, n) CheckGet(f, t, n, __LINE__)

/* The names of a language list, from the multi-string; the thread list comes from the thread function */
typedef BOOL (WINAPI *PFN_GetThread)(DWORD, PULONG, PZZWSTR, PULONG);

static ULONG ThreadNames(PFN_GetThread GetThread, DWORD Flags, WCHAR Names[16][32])
{
    WCHAR Buffer[256];
    ULONG Count = 0, Size = RTL_NUMBER_OF(Buffer), n = 0;
    PCWSTR Entry;

    memset(Buffer, 0, sizeof(Buffer));
    if (!GetThread(Flags, &n, Buffer, &Size))
        return 0;
    for (Entry = Buffer; *Entry && Count < 16; Entry += wcslen(Entry) + 1)
        lstrcpynW(Names[Count++], Entry, 32);
    return Count;
}

static BOOL ListHas(WCHAR Names[16][32], ULONG Count, PCWSTR Name)
{
    ULONG i;

    for (i = 0; i < Count; i++)
        if (!lstrcmpiW(Names[i], Name))
            return TRUE;
    return FALSE;
}

/* The list of a thread starts with the languages of the process, then its own fallbacks without repeating one */
static void CheckThreadList(PFN_GetThread GetThread, DWORD Flags, WCHAR Base[16][32], ULONG BaseCount, PCWSTR const *Head,
                            ULONG HeadCount, int Line)
{
    WCHAR Now[16][32], Expected[16][32];
    ULONG NowCount, ExpectedCount = 0, i;

    for (i = 0; i < HeadCount; i++)
        lstrcpyW(Expected[ExpectedCount++], Head[i]);
    for (i = 0; i < BaseCount && ExpectedCount < 16; i++)
        if (!ListHas(Expected, ExpectedCount, Base[i]))
            lstrcpyW(Expected[ExpectedCount++], Base[i]);

    NowCount = ThreadNames(GetThread, Flags, Now);
    ok(NowCount == ExpectedCount, "line %d: thread flags %#lx: %lu languages, expected %lu\n", Line, Flags, NowCount, ExpectedCount);
    for (i = 0; i < min(NowCount, ExpectedCount); i++)
        ok(!lstrcmpiW(Now[i], Expected[i]), "line %d: thread flags %#lx: entry %lu is %ls, expected %ls\n", Line, Flags, i, Now[i], Expected[i]);
}

START_TEST(PreferredUILanguages)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    static const DWORD BadGet[] = { 1, 2, 0x10, 0x20, 0x40, 0x100, 0x200, 0x300, 0x80000, 0x80000000 };
    static const DWORD BadSet[] = { 1, 2, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 5, 9, 0xc, 0xd, 0x14, 0x18, 0x28, 0x108, 0x80000000 };
    WCHAR Buffer[256];
    WCHAR Base8[16][32], Base10[16][32], Base40[16][32];
    ULONG Count, Size, i, Base8Count = 0, Base10Count = 0, Base40Count = 0;
    PFN_GetThread pGetThread = NULL;
    BOOL Result;

    /* Windows Vista and older do not have them: they came with Windows 7 */
    if (!GetProcAddress(Kernel32, "GetTickCount64"))
    {
        skip("this system is older than Windows Vista\n");
        return;
    }
    pSet = (PFN_Set)GetProcAddress(Kernel32, "SetProcessPreferredUILanguages");
    pGet = (PFN_Get)GetProcAddress(Kernel32, "GetProcessPreferredUILanguages");
    if (!pSet || !pGet)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")), "the functions are not exported\n");
        skip("the process preferred UI language functions are not exported\n");
        return;
    }

    /* the list of the thread before the process has languages is the base of the lists that follow */
    pGetThread = (PFN_GetThread)GetProcAddress(Kernel32, "GetThreadPreferredUILanguages");
    if (pGetThread)
    {
        Base8Count = ThreadNames(pGetThread, MUI_NAME, Base8);
        Base10Count = ThreadNames(pGetThread, 0x10, Base10);
        Base40Count = ThreadNames(pGetThread, 0x40, Base40);
    }

    /* a process starts with no language of its own, and the count is not written for an empty list (measured) */
    GETOK(0, "|", 0xdead);
    GETOK(MUI_ID, "|", 0xdead);
    GETOK(MUI_NAME, "|", 0xdead);
    for (i = 0; i < RTL_NUMBER_OF(BadGet); i++)
    {
        Count = 0xdead;
        Size = RTL_NUMBER_OF(Buffer);
        SetLastError(PENDING_ERROR);
        Result = pGet(BadGet[i], &Count, Buffer, &Size);
        ok(!Result && GetLastError() == ERROR_INVALID_PARAMETER, "get %lx: %d, error %lu\n", BadGet[i], Result, GetLastError());
    }
    for (i = 0; i < RTL_NUMBER_OF(BadSet); i++)
        SETFAIL(BadSet[i], "en-US|", ERROR_INVALID_PARAMETER);
    GETOK(MUI_NAME, "|", 0xdead);

    /* names, a flag of 0 reads names as well */
    SETOK(MUI_NAME, "en-US|", 1);
    GETOK(MUI_NAME, "en-US|", 1);
    GETOK(0, "en-US|", 1);
    GETOK(MUI_ID, "0409|", 1);
    SETOK(0, "de-DE|", 1);
    GETOK(MUI_NAME, "de-DE|", 1);

    /* identifiers: four hexadecimal digits */
    SETOK(MUI_ID, "0409|", 1);
    GETOK(MUI_ID, "0409|", 1);
    GETOK(MUI_NAME, "en-US|", 1);
    GETOK(0, "en-US|", 1);
    SETOK(MUI_ID, "0407|", 1);
    GETOK(MUI_NAME, "de-DE|", 1);
    SETFAIL(MUI_NAME, "0409|", ERROR_GEN_FAILURE);
    SETFAIL(0, "0409|", ERROR_GEN_FAILURE);

    /* neutral languages are fine and have a short identifier */
    SETOK(MUI_NAME, "fr|de|", 2);
    GETOK(MUI_NAME, "fr|de|", 2);
    GETOK(MUI_ID, "000C|0007|", 2);

    /* at most five languages are kept, duplicates once */
    SETOK(MUI_NAME, "cs-CZ|sk-SK|pl-PL|hu-HU|ru-RU|ja-JP|ko-KR|zh-CN|el-GR|tr-TR|nl-NL|", 5);
    GETOK(MUI_NAME, "cs-CZ|sk-SK|pl-PL|hu-HU|ru-RU|", 5);
    GETOK(MUI_ID, "0405|041B|0415|040E|0419|", 5);
    SETOK(MUI_NAME, "en-US|en-US|", 1);
    GETOK(MUI_NAME, "en-US|", 1);

    /* something that is not a language is dropped from a list and refused alone */
    SETOK(MUI_NAME, "en-US|nonsense|", 1);
    GETOK(MUI_NAME, "en-US|", 1);
    SETOK(MUI_NAME, "fr-FR|de-DE|", 2);
    SETFAIL(MUI_NAME, "x|", ERROR_GEN_FAILURE);
    SETFAIL(MUI_NAME, "zz-ZZ|", ERROR_GEN_FAILURE);
    GETOK(MUI_NAME, "fr-FR|de-DE|", 2);

    /* the languages of the process lead the list of the thread, with their parents when the fallbacks are merged */
    if (pGetThread)
    {
        static const PCWSTR Plain[] = { L"fr-FR", L"de-DE" };
        static const PCWSTR Parents[] = { L"fr-FR", L"fr", L"de-DE", L"de" };

        CheckThreadList(pGetThread, MUI_NAME, Base8, Base8Count, Plain, 2, __LINE__);
        CheckThreadList(pGetThread, 0x20, Base8, Base8Count, Plain, 2, __LINE__);
        CheckThreadList(pGetThread, 0x10, Base10, Base10Count, Parents, 4, __LINE__);
        CheckThreadList(pGetThread, 0x40, Base40, Base40Count, NULL, 0, __LINE__);
    }

    /* the size of the buffer */
    Count = 0xdead;
    Size = 5;
    SetLastError(PENDING_ERROR);
    Result = pGet(MUI_NAME, &Count, Buffer, &Size);
    ok(!Result && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "small buffer: %d, error %lu\n", Result, GetLastError());
    ok_dec(Count, 2);
    ok_dec(Size, 13);
    Size = 12;
    SetLastError(PENDING_ERROR);
    Result = pGet(MUI_NAME, &Count, Buffer, &Size);
    ok(!Result && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "buffer of 12: %d, error %lu\n", Result, GetLastError());
    ok_dec(Size, 13);
    Size = 13;
    Result = pGet(MUI_NAME, &Count, Buffer, &Size);
    ok(Result, "buffer of 13: failed with %lu\n", GetLastError());
    Count = 0xdead;
    Size = 0;
    SetLastError(PENDING_ERROR);
    Result = pGet(MUI_NAME, &Count, NULL, &Size);
    ok(Result, "size query: failed with %lu\n", GetLastError());
    ok_dec(Count, 2);
    ok_dec(Size, 13);

    /* null counters */
    Size = RTL_NUMBER_OF(Buffer);
    SetLastError(PENDING_ERROR);
    Result = pGet(MUI_NAME, &Count, Buffer, NULL);
    ok(!Result && GetLastError() == ERROR_INVALID_PARAMETER, "null size: %d, error %lu\n", Result, GetLastError());
    SetLastError(PENDING_ERROR);
    Result = pGet(MUI_NAME, NULL, Buffer, &Size);
    ok(!Result && GetLastError() == ERROR_INVALID_PARAMETER, "null count: %d, error %lu\n", Result, GetLastError());
    ToMulti("en-US|", Buffer);
    SetLastError(PENDING_ERROR);
    Result = pSet(MUI_NAME, Buffer, NULL);
    ok(Result, "set without a count: failed with %lu\n", GetLastError());
    GETOK(MUI_NAME, "en-US|", 1);

    /* no list, or an empty one, resets the languages; the count is not written */
    Count = 0xdead;
    SetLastError(PENDING_ERROR);
    Result = pSet(MUI_NAME, NULL, &Count);
    ok(Result, "reset: failed with %lu\n", GetLastError());
    ok_dec(Count, 0xdead);
    GETOK(MUI_NAME, "|", 0xdead);
    SETOK(MUI_NAME, "fr-FR|", 1);
    Buffer[0] = 0;
    Buffer[1] = 0;
    Count = 0xdead;
    Result = pSet(MUI_NAME, Buffer, &Count);
    ok(Result, "empty list: failed with %lu\n", GetLastError());
    ok_dec(Count, 0xdead);
    GETOK(MUI_NAME, "|", 0xdead);
    Size = 0;
    SetLastError(PENDING_ERROR);
    Result = pGet(MUI_NAME, &Count, Buffer, &Size);
    ok(!Result && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "empty list, no room: %d, error %lu\n", Result, GetLastError());
    ok_dec(Size, 2);

    /* with no language left the list of the thread is its own again */
    if (pGetThread)
        CheckThreadList(pGetThread, MUI_NAME, Base8, Base8Count, NULL, 0, __LINE__);
}
