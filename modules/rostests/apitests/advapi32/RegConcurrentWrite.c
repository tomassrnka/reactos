/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test concurrent writes to different keys of one hive
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/*
 * Writers to different keys of one hive run at the same time: each holds
 * only its own key's lock. Every thread here owns one key, fills it with
 * values large enough to make the hive grow, reads them back and deletes
 * the key, while other threads open keys of the same hive. On a
 * multiprocessor system a missing hive-level lock shows up as a value
 * that reads back with another thread's data, a failed call, or a crash.
 */

#define TEST_ROOT L"Software\\ReactOS RegConcurrentWrite"
#define WRITERS 6
#define READERS 2
#define MAX_DATA 16384
#define TEST_SECONDS 45

static HANDLE StartEvent;
static volatile LONG StopTest;
static volatile LONG WriterRounds[WRITERS];
static volatile LONG WriteFailures;
static volatile LONG DataMismatches;
static volatile LONG ReadFailures;
static volatile LONG Rounds;
static volatile LONG FirstError = ERROR_SUCCESS;

static
ULONG
NextRandom(
    _Inout_ PULONG Seed)
{
    *Seed = *Seed * 1103515245 + 12345;
    return (*Seed >> 16) & 0x7FFF;
}

static
VOID
NoteError(
    _In_ volatile LONG *Counter,
    _In_ LONG Error)
{
    InterlockedIncrement(Counter);
    InterlockedCompareExchange(&FirstError, Error, ERROR_SUCCESS);
}

static
DWORD
WINAPI
WriterThread(
    _In_ PVOID Parameter)
{
    ULONG Thread = (ULONG)(ULONG_PTR)Parameter;
    ULONG Seed = GetTickCount() ^ (Thread * 7919);
    BOOL Volatile = (Thread & 1) != 0;
    WCHAR Path[MAX_PATH];
    PUCHAR Data, ReadBack;
    ULONG Sizes[24];

    Data = HeapAlloc(GetProcessHeap(), 0, MAX_DATA);
    ReadBack = HeapAlloc(GetProcessHeap(), 0, MAX_DATA);
    if (!Data || !ReadBack)
    {
        NoteError(&WriteFailures, ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    StringCchPrintfW(Path, _countof(Path), TEST_ROOT L"\\W%lu", Thread);
    WaitForSingleObject(StartEvent, INFINITE);
    while (!StopTest)
    {
        HKEY Key;
        LONG Error;
        ULONG Values, i;

        Error = RegCreateKeyExW(HKEY_CURRENT_USER, Path, 0, NULL,
                                Volatile ? REG_OPTION_VOLATILE : REG_OPTION_NON_VOLATILE,
                                KEY_ALL_ACCESS, NULL, &Key, NULL);
        if (Error != ERROR_SUCCESS)
        {
            NoteError(&WriteFailures, Error);
            break;
        }

        Values = 1 + NextRandom(&Seed) % _countof(Sizes);
        for (i = 0; i < Values; i++)
        {
            WCHAR Name[16];
            ULONG j;

            Sizes[i] = 1 + (NextRandom(&Seed) * 2 + NextRandom(&Seed) % 2) % MAX_DATA;
            for (j = 0; j < Sizes[i]; j++)
                Data[j] = (UCHAR)(Thread * 31 + i + j);
            StringCchPrintfW(Name, _countof(Name), L"v%lu", i);
            Error = RegSetValueExW(Key, Name, 0, REG_BINARY, Data, Sizes[i]);
            if (Error != ERROR_SUCCESS)
                NoteError(&WriteFailures, Error);
        }

        for (i = 0; i < Values; i++)
        {
            WCHAR Name[16];
            DWORD Size = MAX_DATA, Type, j;

            StringCchPrintfW(Name, _countof(Name), L"v%lu", i);
            Error = RegQueryValueExW(Key, Name, NULL, &Type, ReadBack, &Size);
            if (Error != ERROR_SUCCESS)
            {
                NoteError(&WriteFailures, Error);
                continue;
            }
            if (Type != REG_BINARY || Size != Sizes[i])
            {
                NoteError(&DataMismatches, ERROR_INVALID_DATA);
                continue;
            }
            for (j = 0; j < Size; j++)
            {
                if (ReadBack[j] != (UCHAR)(Thread * 31 + i + j))
                {
                    NoteError(&DataMismatches, ERROR_INVALID_DATA);
                    break;
                }
            }
        }

        if ((NextRandom(&Seed) % 16) == 0 && !Volatile)
        {
            Error = RegFlushKey(Key);
            if (Error != ERROR_SUCCESS)
                NoteError(&WriteFailures, Error);
        }

        RegCloseKey(Key);
        Error = RegDeleteKeyW(HKEY_CURRENT_USER, Path);
        if (Error != ERROR_SUCCESS)
            NoteError(&WriteFailures, Error);
        InterlockedIncrement(&Rounds);
        InterlockedIncrement(&WriterRounds[Thread]);
    }

    HeapFree(GetProcessHeap(), 0, ReadBack);
    HeapFree(GetProcessHeap(), 0, Data);
    return 0;
}

static
DWORD
WINAPI
ReaderThread(
    _In_ PVOID Parameter)
{
    ULONG Seed = GetTickCount() ^ (ULONG)(ULONG_PTR)Parameter;

    WaitForSingleObject(StartEvent, INFINITE);
    while (!StopTest)
    {
        WCHAR Path[MAX_PATH];
        HKEY Key;
        LONG Error;

        StringCchPrintfW(Path, _countof(Path), TEST_ROOT L"\\W%lu", NextRandom(&Seed) % WRITERS);
        Error = RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_READ, &Key);
        if (Error == ERROR_SUCCESS)
        {
            DWORD Values = 0;

            /* The writer may delete the key at any time */
            Error = RegQueryInfoKeyW(Key, NULL, NULL, NULL, NULL, NULL, NULL,
                                     &Values, NULL, NULL, NULL, NULL);
            if (Error != ERROR_SUCCESS && Error != ERROR_KEY_DELETED)
                NoteError(&ReadFailures, Error);
            RegCloseKey(Key);
        }
        else if (Error != ERROR_FILE_NOT_FOUND)
        {
            NoteError(&ReadFailures, Error);
        }

        Error = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_READ, &Key);
        if (Error == ERROR_SUCCESS)
            RegCloseKey(Key);
        else
            NoteError(&ReadFailures, Error);
    }

    return 0;
}

START_TEST(RegConcurrentWrite)
{
    HANDLE Threads[WRITERS + READERS];
    SYSTEM_INFO SystemInfo;
    HKEY Root;
    LONG Error;
    ULONG i, Count = 0;

    GetSystemInfo(&SystemInfo);
    if (SystemInfo.dwNumberOfProcessors < 2)
        trace("Single processor: the race this test looks for needs several\n");

    Error = RegCreateKeyExW(HKEY_CURRENT_USER, TEST_ROOT, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_ALL_ACCESS, NULL, &Root, NULL);
    ok(Error == ERROR_SUCCESS, "RegCreateKeyExW returned %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    StartEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(StartEvent != NULL, "CreateEventW failed with %lu\n", GetLastError());
    if (!StartEvent)
        return;

    for (i = 0; i < WRITERS + READERS; i++)
    {
        Threads[Count] = CreateThread(NULL, 0,
                                      i < WRITERS ? WriterThread : ReaderThread,
                                      (PVOID)(ULONG_PTR)i, 0, NULL);
        ok(Threads[Count] != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (Threads[Count])
            Count++;
    }

    SetEvent(StartEvent);
    Sleep(TEST_SECONDS * 1000);
    InterlockedExchange(&StopTest, 1);
    WaitForMultipleObjects(Count, Threads, TRUE, INFINITE);
    for (i = 0; i < Count; i++)
        CloseHandle(Threads[i]);
    CloseHandle(StartEvent);

    for (i = 0; i < WRITERS; i++)
        ok(WriterRounds[i] > 0, "Writer %lu finished no round\n", i);
    ok(WriteFailures == 0, "%ld writes failed, first error %ld\n", WriteFailures, FirstError);
    ok(DataMismatches == 0, "%ld values read back with wrong data\n", DataMismatches);
    ok(ReadFailures == 0, "%ld opens failed, first error %ld\n", ReadFailures, FirstError);
    trace("%ld rounds on %lu processors\n", Rounds, SystemInfo.dwNumberOfProcessors);

    for (i = 0; i < WRITERS; i++)
    {
        WCHAR Path[MAX_PATH];
        StringCchPrintfW(Path, _countof(Path), TEST_ROOT L"\\W%lu", i);
        RegDeleteKeyW(HKEY_CURRENT_USER, Path);
    }
    RegCloseKey(Root);
    Error = RegDeleteKeyW(HKEY_CURRENT_USER, TEST_ROOT);
    ok(Error == ERROR_SUCCESS, "RegDeleteKeyW returned %ld\n", Error);
}
