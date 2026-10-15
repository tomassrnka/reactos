/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Data written through mapped views past the valid data length, then cached writes and size changes
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on FAT32 and NTFS volumes of Windows Server 2008 R2 and Windows 10 22H2 (x64 and WOW64): every byte
 * written through a view stays readable, and a file that is cut and grown again reads zero in the grown range.
 * The test uses the volume of the temporary directory, or the directory named by MAPPEDFILEGROWTH_DIR. The 15 s soak
 * of four threads runs only when MAPPEDFILEGROWTH_SOAK is set.
 */

#include "precomp.h"

#define SOAK_THREADS    4
#define SOAK_SECONDS    15
#define SOAK_MAX_FILE   (2 * 1024 * 1024)

static WCHAR TestDir[MAX_PATH];

static BYTE Pattern(DWORD Offset, BYTE Seed)
{
    BYTE Value = (BYTE)(Offset * 7 + Offset / 251 + Seed);
    return Value ? Value : 1;
}

static HANDLE OpenTestFile(PCWSTR Name, DWORD Disposition)
{
    WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", TestDir, Name);
    return CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, Disposition, Disposition == CREATE_ALWAYS ? FILE_FLAG_DELETE_ON_CLOSE : 0, NULL);
}

static BOOL WriteFrom(HANDLE File, DWORD Offset, DWORD Length, BYTE Seed, PBYTE Buffer)
{
    LARGE_INTEGER Position;
    DWORD i, Written;

    for (i = 0; i < Length; i++)
        Buffer[i] = Pattern(Offset + i, Seed);
    Position.QuadPart = Offset;
    return SetFilePointerEx(File, Position, NULL, FILE_BEGIN) &&
           WriteFile(File, Buffer, Length, &Written, NULL) && Written == Length;
}

/* Single-threaded callers only */
static BOOL WriteAt(HANDLE File, DWORD Offset, DWORD Length, BYTE Seed)
{
    static BYTE Buffer[0x4000];

    return Length <= sizeof(Buffer) && WriteFrom(File, Offset, Length, Seed, Buffer);
}

static BOOL SetSize(HANDLE File, DWORD Size)
{
    LARGE_INTEGER Position;

    Position.QuadPart = Size;
    return SetFilePointerEx(File, Position, NULL, FILE_BEGIN) && SetEndOfFile(File);
}

/* Bytes of [Offset, Offset + Length) that ReadFile does not return as written (Seed 0: zero expected) */
static DWORD CountBad(HANDLE File, DWORD Offset, DWORD Length, BYTE Seed)
{
    static BYTE Buffer[0x10000];
    LARGE_INTEGER Position;
    DWORD i, Read = 0, Bad = 0;

    Position.QuadPart = Offset;
    if (Length > sizeof(Buffer) || !SetFilePointerEx(File, Position, NULL, FILE_BEGIN) ||
        !ReadFile(File, Buffer, Length, &Read, NULL))
    {
        return Length;
    }
    for (i = 0; i < Read; i++)
    {
        if (Buffer[i] != (Seed ? Pattern(Offset + i, Seed) : 0))
            Bad++;
    }
    return Bad + (Length - Read);
}

static PBYTE MapFile(HANDLE File, DWORD Size, PHANDLE Mapping)
{
    PBYTE View;

    *Mapping = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, Size, NULL);
    ok(*Mapping != NULL, "CreateFileMapping failed: %lu\n", GetLastError());
    if (!*Mapping)
        return NULL;
    View = MapViewOfFile(*Mapping, FILE_MAP_WRITE, 0, 0, 0);
    ok(View != NULL, "MapViewOfFile failed: %lu\n", GetLastError());
    if (!View)
        CloseHandle(*Mapping);
    return View;
}

static void FillView(PBYTE View, DWORD Offset, DWORD Length, BYTE Seed)
{
    DWORD i;

    for (i = 0; i < Length; i++)
        View[Offset + i] = Pattern(Offset + i, Seed);
}

/* A mapping grows the file and its tail is written through the view; then a cached append */
static void TestGrowTailThenAppend(BOOL KeepMapped)
{
    HANDLE File, Mapping;
    PBYTE View;
    DWORD i, BadView = 0;

    File = OpenTestFile(L"tail.bin", CREATE_ALWAYS);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteAt(File, 0, 0x1000, 1), "WriteFile failed: %lu\n", GetLastError());
    View = MapFile(File, 0x3000, &Mapping);
    if (View)
    {
        FillView(View, 0x1800, 0x1000, 2);
        if (!KeepMapped)
        {
            UnmapViewOfFile(View);
            CloseHandle(Mapping);
        }
        ok(WriteAt(File, 0x3000, 0x100, 3), "WriteFile failed: %lu\n", GetLastError());
        if (KeepMapped)
        {
            for (i = 0; i < 0x1000; i++)
                BadView += View[0x1800 + i] != Pattern(0x1800 + i, 2);
            ok(BadView == 0, "kept view: %lu bytes changed\n", BadView);
            UnmapViewOfFile(View);
            CloseHandle(Mapping);
        }
        ok(CountBad(File, 0x1800, 0x1000, 2) == 0, "keep %d: view-written bytes read back: %lu bad\n",
           KeepMapped, CountBad(File, 0x1800, 0x1000, 2));
        ok(CountBad(File, 0, 0x1000, 1) == 0, "keep %d: first write: %lu bad\n", KeepMapped, CountBad(File, 0, 0x1000, 1));
        ok(CountBad(File, 0x3000, 0x100, 3) == 0, "keep %d: append: %lu bad\n", KeepMapped, CountBad(File, 0x3000, 0x100, 3));
    }
    CloseHandle(File);
}

/* Two ranges written through a view that stays mapped, a cached write past both; then close and reopen */
static void TestTwoRangesThenWrite(void)
{
    HANDLE File, Mapping;
    PBYTE View;

    File = OpenTestFile(L"ranges.bin", CREATE_ALWAYS);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteAt(File, 0, 0x200, 1), "WriteFile failed: %lu\n", GetLastError());
    ok(SetSize(File, 0x20000), "SetEndOfFile failed: %lu\n", GetLastError());
    View = MapFile(File, 0, &Mapping);
    if (View)
    {
        FillView(View, 0x5000, 0x3000, 2);
        FillView(View, 0xC000, 0x1000, 4);
        ok(WriteAt(File, 0x10000, 0x100, 3), "WriteFile failed: %lu\n", GetLastError());
        ok(CountBad(File, 0x5000, 0x3000, 2) == 0, "first range: %lu bad\n", CountBad(File, 0x5000, 0x3000, 2));
        ok(CountBad(File, 0xC000, 0x1000, 4) == 0, "second range: %lu bad\n", CountBad(File, 0xC000, 0x1000, 4));
        ok(CountBad(File, 0x200, 0x4E00, 0) == 0, "gap before the first range: %lu not zero\n", CountBad(File, 0x200, 0x4E00, 0));
        ok(CountBad(File, 0x8000, 0x4000, 0) == 0, "gap between the ranges: %lu not zero\n", CountBad(File, 0x8000, 0x4000, 0));
        UnmapViewOfFile(View);
        CloseHandle(Mapping);
    }
    CloseHandle(File);
}

/* A view writes past the valid data length, every handle closes, the file is opened again */
static void TestCloseReopen(void)
{
    HANDLE File, Mapping;
    PBYTE View;
    WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, _countof(Path), L"%s\\reopen.bin", TestDir);
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteAt(File, 0, 0x200, 1), "WriteFile failed: %lu\n", GetLastError());
    View = MapFile(File, 0x6000, &Mapping);
    if (View)
    {
        FillView(View, 0x3000, 0x1000, 2);
        UnmapViewOfFile(View);
        CloseHandle(Mapping);
    }
    CloseHandle(File);
    File = CreateFileW(Path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(CountBad(File, 0x3000, 0x1000, 2) == 0, "view-written bytes: %lu bad\n", CountBad(File, 0x3000, 0x1000, 2));
    ok(CountBad(File, 0x200, 0x2E00, 0) == 0, "gap: %lu not zero\n", CountBad(File, 0x200, 0x2E00, 0));
    CloseHandle(File);
}

/* A file cut inside cached data and grown again reads zero where the old bytes were */
static void TestCutAndGrow(void)
{
    HANDLE File;

    File = OpenTestFile(L"cut.bin", CREATE_ALWAYS);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteAt(File, 0, 0x3000, 1), "WriteFile failed: %lu\n", GetLastError());
    ok(SetSize(File, 0x1800), "SetEndOfFile failed: %lu\n", GetLastError());
    ok(SetSize(File, 0x3000), "SetEndOfFile failed: %lu\n", GetLastError());
    ok(CountBad(File, 0x1800, 0x1800, 0) == 0, "grown range: %lu bytes not zero\n", CountBad(File, 0x1800, 0x1800, 0));
    ok(WriteAt(File, 0x3000, 0x100, 3), "WriteFile failed: %lu\n", GetLastError());
    ok(CountBad(File, 0x1800, 0x1800, 0) == 0, "after an append: %lu bytes not zero\n", CountBad(File, 0x1800, 0x1800, 0));
    ok(CountBad(File, 0, 0x1800, 1) == 0, "kept range: %lu bad\n", CountBad(File, 0, 0x1800, 1));
    CloseHandle(File);
}

static HANDLE OpenNamed(PCWSTR Name, DWORD Disposition, DWORD Flags)
{
    WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", TestDir, Name);
    return CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, Disposition, Flags, NULL);
}

/*
 * A file cut at an unaligned size, with every handle closed before it grows again, so that the pages past the cut
 * are no longer in memory: the grown range reads zero, from the cut on and again after another reopen.
 */
static void TestCutCloseGrow(BOOL Flush)
{
    HANDLE File;
    DWORD Cut = 0x1801;

    File = OpenNamed(L"cutclose.bin", CREATE_ALWAYS, 0);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteAt(File, 0, 0x3000, 5), "WriteFile failed: %lu\n", GetLastError());
    if (Flush)
        ok(FlushFileBuffers(File), "FlushFileBuffers failed: %lu\n", GetLastError());
    ok(SetSize(File, Cut), "SetEndOfFile failed: %lu\n", GetLastError());
    CloseHandle(File);

    File = OpenNamed(L"cutclose.bin", OPEN_EXISTING, 0);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(SetSize(File, 0x3000), "SetEndOfFile failed: %lu\n", GetLastError());
    ok(CountBad(File, Cut, 0x3000 - Cut, 0) == 0, "flush %d: grown range: %lu bytes not zero\n", Flush, CountBad(File, Cut, 0x3000 - Cut, 0));
    ok(CountBad(File, 0, Cut, 5) == 0, "flush %d: kept range: %lu bad\n", Flush, CountBad(File, 0, Cut, 5));
    CloseHandle(File);

    File = OpenNamed(L"cutclose.bin", OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(CountBad(File, Cut, 0x3000 - Cut, 0) == 0, "flush %d: after reopen: %lu bytes not zero\n", Flush, CountBad(File, Cut, 0x3000 - Cut, 0));
    CloseHandle(File);
}

/*
 * Pages read into the cache from disk, then cut away at an unaligned size; the file grows again and a write past
 * the grown range makes the file system zero the gap: the old contents must not come back, through ReadFile or a
 * view, before or after a reopen.
 */
static void TestCachedCutGrowWrite(BOOL ThroughView)
{
    HANDLE File, Mapping;
    PBYTE View;
    DWORD Cut = 0x2345, i, Bad;

    File = OpenNamed(L"cutgrow.bin", CREATE_ALWAYS, 0);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    if (ThroughView)
    {
        ok(SetSize(File, 0x6000), "SetEndOfFile failed: %lu\n", GetLastError());
        View = MapFile(File, 0, &Mapping);
        if (View)
        {
            FillView(View, 0, 0x6000, 6);
            UnmapViewOfFile(View);
            CloseHandle(Mapping);
        }
    }
    else
    {
        ok(WriteAt(File, 0, 0x3000, 6) && WriteAt(File, 0x3000, 0x3000, 6), "WriteFile failed: %lu\n", GetLastError());
    }
    ok(FlushFileBuffers(File), "FlushFileBuffers failed: %lu\n", GetLastError());
    CloseHandle(File);

    File = OpenNamed(L"cutgrow.bin", OPEN_EXISTING, 0);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(CountBad(File, 0, 0x6000, 6) == 0, "view %d: contents: %lu bad\n", ThroughView, CountBad(File, 0, 0x6000, 6));
    ok(SetSize(File, Cut), "SetEndOfFile failed: %lu\n", GetLastError());
    ok(SetSize(File, 0x6000), "SetEndOfFile failed: %lu\n", GetLastError());
    ok(WriteAt(File, 0x6000, 0x100, 7), "WriteFile failed: %lu\n", GetLastError());
    ok(CountBad(File, Cut, 0x6000 - Cut, 0) == 0, "view %d: gap: %lu bytes not zero\n", ThroughView, CountBad(File, Cut, 0x6000 - Cut, 0));
    ok(CountBad(File, 0, Cut, 6) == 0, "view %d: kept range: %lu bad\n", ThroughView, CountBad(File, 0, Cut, 6));
    View = MapFile(File, 0, &Mapping);
    if (View)
    {
        for (i = Cut, Bad = 0; i < 0x6000; i++)
            Bad += View[i] != 0;
        ok(Bad == 0, "view %d: gap through a view: %lu bytes not zero\n", ThroughView, Bad);
        UnmapViewOfFile(View);
        CloseHandle(Mapping);
    }
    ok(FlushFileBuffers(File), "FlushFileBuffers failed: %lu\n", GetLastError());
    CloseHandle(File);

    File = OpenNamed(L"cutgrow.bin", OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(CountBad(File, Cut, 0x6000 - Cut, 0) == 0, "view %d: gap after reopen: %lu bytes not zero\n", ThroughView, CountBad(File, Cut, 0x6000 - Cut, 0));
    ok(CountBad(File, 0x6000, 0x100, 7) == 0, "view %d: append after reopen: %lu bad\n", ThroughView, CountBad(File, 0x6000, 0x100, 7));
    CloseHandle(File);
}

/* View data past the valid data length written to disk by a flush, then every handle closed; a cached append after the reopen */
static void TestViewFlushReopenAppend(void)
{
    HANDLE File, Mapping;
    PBYTE View;

    File = OpenNamed(L"vflush.bin", CREATE_ALWAYS, 0);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteAt(File, 0, 0x200, 1), "WriteFile failed: %lu\n", GetLastError());
    View = MapFile(File, 0x9000, &Mapping);
    if (View)
    {
        FillView(View, 0x4000, 0x2800, 2);
        ok(FlushViewOfFile(View, 0), "FlushViewOfFile failed: %lu\n", GetLastError());
        UnmapViewOfFile(View);
        CloseHandle(Mapping);
    }
    CloseHandle(File);

    File = OpenNamed(L"vflush.bin", OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteAt(File, 0x9000, 0x100, 3), "WriteFile failed: %lu\n", GetLastError());
    ok(CountBad(File, 0x4000, 0x2800, 2) == 0, "view-written bytes: %lu bad\n", CountBad(File, 0x4000, 0x2800, 2));
    ok(CountBad(File, 0x200, 0x3E00, 0) == 0, "gap: %lu not zero\n", CountBad(File, 0x200, 0x3E00, 0));
    CloseHandle(File);
}

/* Several threads grow their own files by cached appends and by mappings whose tail they write; every byte is checked */
typedef struct _SOAK
{
    DWORD Id;
    DWORD Seed;
    DWORD Failures;
    DWORD Operations;
} SOAK, *PSOAK;

static volatile LONG SoakStop;

static DWORD Random(PDWORD State)
{
    *State = *State * 1103515245 + 12345;
    return (*State >> 8) & 0xFFFFFF;
}

static DWORD WINAPI SoakThread(PVOID Parameter)
{
    PSOAK Soak = Parameter;
    PBYTE Model = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, SOAK_MAX_FILE + 0x10000);
    PBYTE Buffer = HeapAlloc(GetProcessHeap(), 0, 0x10000);
    DWORD Round = 0;

    if (!Model || !Buffer)
        return 1;
    while (!SoakStop && !Soak->Failures)
    {
        WCHAR Name[32];
        HANDLE File;
        DWORD Size = 0, Seed = (Soak->Id + 1) * 16 + Round % 8;

        StringCchPrintfW(Name, _countof(Name), L"soak%lu_%lu.bin", Soak->Id, Round++);
        File = OpenTestFile(Name, CREATE_ALWAYS);
        if (File == INVALID_HANDLE_VALUE)
        {
            Soak->Failures++;
            break;
        }
        ZeroMemory(Model, SOAK_MAX_FILE + 0x10000);
        while (!SoakStop && !Soak->Failures && Size < SOAK_MAX_FILE)
        {
            DWORD Op = Random(&Soak->Seed) % 10, Length, i, Offset;

            Soak->Operations++;
            if (Op < 5)
            {
                /* cached append */
                Length = 1 + Random(&Soak->Seed) % 0x3000;
                if (!WriteFrom(File, Size, Length, (BYTE)Seed, Buffer))
                    Soak->Failures++;
                for (i = 0; i < Length; i++)
                    Model[Size + i] = Pattern(Size + i, (BYTE)Seed);
                Size += Length;
            }
            else if (Op < 7)
            {
                /* a mapping grows the file; every third byte of its tail is written through the view */
                HANDLE Mapping;
                PBYTE View;

                Length = 1 + Random(&Soak->Seed) % 0x3000;
                Mapping = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, Size + Length, NULL);
                View = Mapping ? MapViewOfFile(Mapping, FILE_MAP_WRITE, 0, 0, 0) : NULL;
                if (!View)
                {
                    Soak->Failures++;
                    if (Mapping)
                        CloseHandle(Mapping);
                    break;
                }
                for (i = 0; i < Length; i += 3)
                {
                    View[Size + i] = Pattern(Size + i, (BYTE)Seed);
                    Model[Size + i] = View[Size + i];
                }
                UnmapViewOfFile(View);
                CloseHandle(Mapping);
                Size += Length;
            }
            else
            {
                /* read back a range near the end */
                Offset = Size > 0x8000 ? Size - 0x8000 + Random(&Soak->Seed) % 0x4000 : 0;
                Length = min(0x10000, Size - Offset);
                if (!Length)
                    continue;
                {
                    LARGE_INTEGER Position;
                    DWORD Read = 0;

                    Position.QuadPart = Offset;
                    if (!SetFilePointerEx(File, Position, NULL, FILE_BEGIN) ||
                        !ReadFile(File, Buffer, Length, &Read, NULL) || Read != Length)
                    {
                        Soak->Failures++;
                        break;
                    }
                    for (i = 0; i < Length; i++)
                    {
                        if (Buffer[i] != Model[Offset + i])
                        {
                            trace("thread %lu: byte at 0x%lx reads 0x%02x, written 0x%02x (file size 0x%lx)\n",
                                  Soak->Id, Offset + i, Buffer[i], Model[Offset + i], Size);
                            Soak->Failures++;
                            break;
                        }
                    }
                }
            }
        }
        CloseHandle(File);
    }
    HeapFree(GetProcessHeap(), 0, Model);
    HeapFree(GetProcessHeap(), 0, Buffer);
    return 0;
}

static void TestSoak(void)
{
    HANDLE Threads[SOAK_THREADS];
    SOAK Soak[SOAK_THREADS];
    DWORD i, Failures = 0, Operations = 0;

    SoakStop = 0;
    for (i = 0; i < SOAK_THREADS; i++)
    {
        Soak[i].Id = i;
        Soak[i].Seed = 0x1234 + i * 7919;
        Soak[i].Failures = 0;
        Soak[i].Operations = 0;
        Threads[i] = CreateThread(NULL, 0, SoakThread, &Soak[i], 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed: %lu\n", GetLastError());
    }
    Sleep(SOAK_SECONDS * 1000);
    InterlockedExchange(&SoakStop, 1);
    for (i = 0; i < SOAK_THREADS; i++)
    {
        if (!Threads[i])
            continue;
        ok(WaitForSingleObject(Threads[i], 120000) == WAIT_OBJECT_0, "thread %lu did not finish\n", i);
        CloseHandle(Threads[i]);
        Failures += Soak[i].Failures;
        Operations += Soak[i].Operations;
    }
    ok(Failures == 0, "soak: %lu failures in %lu operations\n", Failures, Operations);
    trace("soak: %lu threads, %lu operations\n", (ULONG)SOAK_THREADS, Operations);
}

START_TEST(MappedFileGrowth)
{
    WCHAR Temp[MAX_PATH], Volume[MAX_PATH], FsName[32] = L"?";
    DWORD Length;

    Length = GetEnvironmentVariableW(L"MAPPEDFILEGROWTH_DIR", Temp, _countof(Temp));
    if (!Length || Length >= _countof(Temp))
        GetTempPathW(_countof(Temp), Temp);
    StringCchPrintfW(TestDir, _countof(TestDir), L"%s\\mfg%lu", Temp, GetCurrentProcessId());
    if (!CreateDirectoryW(TestDir, NULL))
    {
        skip("cannot create %ls: %lu\n", TestDir, GetLastError());
        return;
    }
    if (GetVolumePathNameW(TestDir, Volume, _countof(Volume)))
        GetVolumeInformationW(Volume, NULL, 0, NULL, NULL, NULL, FsName, _countof(FsName));
    trace("directory %ls, file system %ls\n", TestDir, FsName);

    TestGrowTailThenAppend(FALSE);
    TestGrowTailThenAppend(TRUE);
    TestTwoRangesThenWrite();
    TestCloseReopen();
    TestCutAndGrow();
    TestCutCloseGrow(FALSE);
    TestCutCloseGrow(TRUE);
    TestCachedCutGrowWrite(FALSE);
    TestCachedCutGrowWrite(TRUE);
    TestViewFlushReopenAppend();
    if (GetEnvironmentVariableW(L"MAPPEDFILEGROWTH_SOAK", Temp, _countof(Temp)))
        TestSoak();
    else
        skip("soak not run: set MAPPEDFILEGROWTH_SOAK to run it\n");

    RemoveDirectoryW(TestDir);
}
