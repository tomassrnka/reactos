/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Single-shot probes of documented behaviour that the
 *              workloads depend on; each prints PROBE name: PASS or FAIL
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

static NTSTATUS
TryWrite(volatile UCHAR *Address, UCHAR Value)
{
    NTSTATUS Status = STATUS_SUCCESS;

    _SEH2_TRY
    {
        *Address = Value;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

static NTSTATUS
TryRead(volatile UCHAR *Address, UCHAR *Value)
{
    NTSTATUS Status = STATUS_SUCCESS;

    _SEH2_TRY
    {
        *Value = *Address;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

/* Copy-on-write view of a section: write, then check the private copy and the shared data */
static VOID
ProbeCow(const char *Name, BOOL FileBacked, BOOL ReadViewFirst, BOOL ReadCowFirst, BOOL OpenByName)
{
    HANDLE File = INVALID_HANDLE_VALUE, Section, Section2 = NULL;
    PUCHAR Rw, Ro = NULL, Cow;
    UCHAR Value = 0;
    NTSTATUS Status;
    WCHAR Path[MAX_PATH];
    BOOL Pass = TRUE;

    if (FileBacked)
    {
        _snwprintf(Path, _countof(Path), L"C:\\mmt-probe-%lu.bin", GetCurrentProcessId());
        File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    }
    Section = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, 4 * PAGE_SIZE, OpenByName ? L"MmtProbeCow" : NULL);
    Rw = Section ? MapViewOfFile(Section, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (!Rw)
    {
        MmtLog("PROBE %s: FAIL (setup %lu)", Name, GetLastError());
        return;
    }
    memset(Rw, 0x11, 4 * PAGE_SIZE);
    if (OpenByName)
    {
        Section2 = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_COPY, FALSE, L"MmtProbeCow");
        if (!Section2)
        {
            MmtLog("PROBE %s: FAIL (open %lu)", Name, GetLastError());
            return;
        }
    }
    if (ReadViewFirst)
    {
        Ro = MapViewOfFile(OpenByName ? Section2 : Section, FILE_MAP_READ, 0, 0, 0);
        if (Ro)
            Value = Ro[8];
    }
    Cow = MapViewOfFile(OpenByName ? Section2 : Section, FILE_MAP_COPY, 0, 0, 0);
    if (!Cow)
    {
        MmtLog("PROBE %s: FAIL (copy view %lu)", Name, GetLastError());
        return;
    }
    if (ReadCowFirst)
    {
        Status = TryRead(Cow + 8, &Value);
        if (Status != STATUS_SUCCESS || Value != 0x11)
        {
            MmtLog("PROBE %s: read of the copy view %08lx value %02x", Name, Status, Value);
            Pass = FALSE;
        }
    }
    Status = TryWrite(Cow + 8, 0x22);
    if (Status != STATUS_SUCCESS)
    {
        MmtLog("PROBE %s: write to the copy view %08lx", Name, Status);
        Pass = FALSE;
    }
    else if (Cow[8] != 0x22 || Rw[8] != 0x11 || (Ro && Ro[8] != 0x11))
    {
        MmtLog("PROBE %s: copy %02x shared %02x", Name, Cow[8], Rw[8]);
        Pass = FALSE;
    }
    MmtLog("PROBE %s: %s", Name, Pass ? "PASS" : "FAIL");
    UnmapViewOfFile(Cow);
    if (Ro)
        UnmapViewOfFile(Ro);
    UnmapViewOfFile(Rw);
    if (Section2)
        CloseHandle(Section2);
    CloseHandle(Section);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
}

static ULONG
AvailablePages(VOID)
{
    SYSTEM_PERFORMANCE_INFORMATION Perf;

    if (!NT_SUCCESS(NtQuerySystemInformation(SystemPerformanceInformation, &Perf, sizeof(Perf), NULL)))
        return 0;
    return Perf.AvailablePages;
}

/* Physical pages lost per process lifetime: spawn children one at a time and compare */
static VOID
ProbeProcessLeak(const char *Mode, ULONG Count)
{
    WCHAR Args[64];
    ULONG Before, After, i;
    DWORD Code;

    _snwprintf(Args, _countof(Args), L"child %S", Mode);
    MmtSpawn(Args, TRUE, 60000, &Code, NULL);
    Sleep(5000);
    Before = AvailablePages();
    for (i = 0; i < Count; i++)
    {
        if (!MmtSpawn(Args, TRUE, 60000, &Code, NULL))
            break;
    }
    Sleep(10000);
    After = AvailablePages();
    MmtLog("PROBE leak-%s: %lu children, available pages %lu -> %lu (%ld per child x100): %s", Mode, i, Before, After,
           i ? (LONG)(Before - After) * 100 / (LONG)i : 0, (LONG)(Before - After) > (LONG)(i / 2 + 64) ? "FAIL" : "PASS");
}

/* Sequential cached write of a file larger than RAM, then a sequential read back */
static VOID
ProbeBigWrite(const char *DirA, ULONG Mb)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    PUCHAR Buffer = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    ULONG i, j, Start = GetTickCount();
    DWORD Done;
    BOOL Pass = TRUE;

    _snwprintf(Path, _countof(Path), L"%S\\mmt-big-%lu.bin", DirA, GetCurrentProcessId());
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (File == INVALID_HANDLE_VALUE || !Buffer)
    {
        MmtLog("PROBE bigwrite: FAIL (create %S: %lu)", Path, GetLastError());
        return;
    }
    for (i = 0; i < Mb && Pass; i++)
    {
        for (j = 0; j < 256; j++)
            MmtFillPage(Buffer + j * PAGE_SIZE, 0xB16B16ULL, i * 256 + j);
        if (!WriteFile(File, Buffer, 1 << 20, &Done, NULL) || Done != (1 << 20))
        {
            MmtLog("PROBE bigwrite: write failed at MB %lu: %lu", i, GetLastError());
            Pass = FALSE;
        }
        if ((i & 63) == 63)
            MmtLog("PROBE bigwrite: %lu MB written, %lu s", i + 1, (GetTickCount() - Start) / 1000);
    }
    SetFilePointer(File, 0, NULL, FILE_BEGIN);
    for (i = 0; i < Mb && Pass; i++)
    {
        if (!ReadFile(File, Buffer, 1 << 20, &Done, NULL) || Done != (1 << 20))
        {
            MmtLog("PROBE bigwrite: read failed at MB %lu: %lu", i, GetLastError());
            Pass = FALSE;
            break;
        }
        for (j = 0; j < 256 && Pass; j++)
            Pass = MmtCheckPage(Buffer + j * PAGE_SIZE, 0xB16B16ULL, i * 256 + j, FALSE, "bigwrite");
    }
    MmtLog("PROBE bigwrite %S %lu MB: %s in %lu s", Path, Mb, Pass ? "PASS" : "FAIL", (GetTickCount() - Start) / 1000);
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

/*
 * Copy-on-write copies of a file view larger than RAM live in the paging
 * file: write a pattern into every page of a WRITECOPY view, then read all
 * pages back, several rounds with a new pattern each round. With Remap,
 * every round maps a new view, so the paging file pages of the previous
 * round are freed and handed out again.
 */
static VOID
ProbeSwapCow(const char *DirA, ULONG Mb, ULONG Rounds, BOOL Remap)
{
    WCHAR Path[MAX_PATH];
    HANDLE File, Section;
    PUCHAR View, Buffer = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    ULONG i, Round, Pages = Mb * 256, Bad = 0, Start = GetTickCount();
    DWORD Done;

    _snwprintf(Path, _countof(Path), L"%S\\mmt-swapcow-%lu.bin", DirA, GetCurrentProcessId());
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (File == INVALID_HANDLE_VALUE || !Buffer)
    {
        MmtLog("PROBE swapcow: FAIL (create %S: %lu)", Path, GetLastError());
        return;
    }
    memset(Buffer, 0xEE, 1 << 20);
    for (i = 0; i < Mb; i++)
    {
        if (!WriteFile(File, Buffer, 1 << 20, &Done, NULL) || Done != (1 << 20))
        {
            MmtLog("PROBE swapcow: FAIL (write at MB %lu: %lu)", i, GetLastError());
            CloseHandle(File);
            return;
        }
    }
    Section = CreateFileMappingW(File, NULL, PAGE_WRITECOPY, 0, 0, NULL);
    View = Section ? MapViewOfFile(Section, FILE_MAP_COPY, 0, 0, 0) : NULL;
    if (!View)
    {
        MmtLog("PROBE swapcow: FAIL (map: %lu)", GetLastError());
        if (Section)
            CloseHandle(Section);
        CloseHandle(File);
        return;
    }
    for (Round = 0; Round < Rounds && Bad < 8; Round++)
    {
        if (Remap && Round > 0)
        {
            UnmapViewOfFile(View);
            View = MapViewOfFile(Section, FILE_MAP_COPY, 0, 0, 0);
            if (!View)
            {
                MmtLog("PROBE swapcow: FAIL (map in round %lu: %lu)", Round, GetLastError());
                CloseHandle(Section);
                CloseHandle(File);
                return;
            }
        }
        for (i = 0; i < Pages; i++)
            MmtFillPage(View + (SIZE_T)i * PAGE_SIZE, 0x5C0Bull + Round, i);
        for (i = 0; i < Pages && Bad < 8; i++)
        {
            if (!MmtCheckPage(View + (SIZE_T)i * PAGE_SIZE, 0x5C0Bull + Round, i, TRUE, "swapcow"))
                Bad++;
        }
        MmtLog("PROBE swapcow: round %lu, %lu bad pages, %lu s", Round, Bad, (GetTickCount() - Start) / 1000);
    }
    MmtLog("PROBE swapcow %lu MB x %lu%s: %s in %lu s", Mb, Rounds, Remap ? " remap" : "", Bad ? "FAIL" : "PASS", (GetTickCount() - Start) / 1000);
    UnmapViewOfFile(View);
    CloseHandle(Section);
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

/*
 * Load a renamed copy of a system DLL, free it and delete the file, many
 * times: once FreeLibrary returned, nothing maps the image any more and
 * the file must be deletable at once
 */
static VOID
ProbeDllCycle(const char *DirA, ULONG Count)
{
    WCHAR System[MAX_PATH], Copy[MAX_PATH];
    ULONG i, Late = 0, Never = 0, Start = GetTickCount();
    HMODULE Module;

    GetSystemDirectoryW(System, _countof(System));
    wcscat(System, L"\\shlwapi.dll");
    for (i = 0; i < Count; i++)
    {
        _snwprintf(Copy, _countof(Copy), L"%S\\mmt-dllcycle-%lu-%lu.dll", DirA, GetCurrentProcessId(), i);
        if (!CopyFileW(System, Copy, FALSE))
        {
            MmtLog("PROBE dllcycle: FAIL (copy %S: %lu)", Copy, GetLastError());
            return;
        }
        Module = LoadLibraryW(Copy);
        if (!Module)
        {
            MmtLog("PROBE dllcycle: FAIL (LoadLibrary %S: %lu)", Copy, GetLastError());
            DeleteFileW(Copy);
            return;
        }
        FreeLibrary(Module);
        if (i == 0 && GetModuleHandleW(Copy))
            MmtLog("PROBE dllcycle: %S still loaded after FreeLibrary", Copy);
        if (!DeleteFileW(Copy))
        {
            DWORD Error = GetLastError();
            ULONG Tries;

            for (Tries = 0; Tries < 20 && !DeleteFileW(Copy); Tries++)
                Sleep(100);
            if (Tries == 20)
            {
                Never++;
                if (Never <= 3)
                    MmtLog("PROBE dllcycle: %S not deletable after FreeLibrary (error %lu, still after 2 s)", Copy, Error);
            }
            else
            {
                Late++;
            }
        }
    }
    MmtLog("PROBE dllcycle x %lu: %s (%lu deletable only later, %lu never) in %lu s", Count,
           (Late || Never) ? "FAIL" : "PASS", Late, Never, (GetTickCount() - Start) / 1000);
}

/* VirtualLock, then PAGE_NOACCESS, then VirtualUnlock: report every step */
static VOID
ProbeVlockSeq(VOID)
{
    PVOID Base = NULL, Address;
    SIZE_T Size = PAGE_SIZE;
    ULONG Old;
    NTSTATUS Status;
    MEMORY_BASIC_INFORMATION Mbi;
    BOOL Ok;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    MmtLog("PROBE vlockseq: allocate 0x%lx at %p", Status, Base);
    if (!NT_SUCCESS(Status))
        return;
    *(volatile UCHAR *)Base = 1;
    Ok = VirtualLock(Base, PAGE_SIZE);
    MmtLog("PROBE vlockseq: VirtualLock %d error %lu", Ok, GetLastError());
    Address = Base;
    Size = PAGE_SIZE;
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_NOACCESS, &Old);
    MmtLog("PROBE vlockseq: protect NOACCESS 0x%lx old 0x%lx", Status, Old);
    VirtualQuery(Base, &Mbi, sizeof(Mbi));
    MmtLog("PROBE vlockseq: query state 0x%lx protect 0x%lx size 0x%Ix", Mbi.State, Mbi.Protect, Mbi.RegionSize);
    SetLastError(0xdeadbeef);
    Ok = VirtualUnlock(Base, PAGE_SIZE);
    MmtLog("PROBE vlockseq: VirtualUnlock %d error %lu", Ok, GetLastError());
    SetLastError(0xdeadbeef);
    Ok = VirtualUnlock(Base, PAGE_SIZE);
    MmtLog("PROBE vlockseq: VirtualUnlock again %d error %lu", Ok, GetLastError());
    Address = Base;
    Size = PAGE_SIZE;
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READWRITE, &Old);
    MmtLog("PROBE vlockseq: protect READWRITE 0x%lx old 0x%lx; byte %u", Status, Old, NT_SUCCESS(Status) ? *(volatile UCHAR *)Base : 0);
    Size = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
    MmtLog("PROBE vlockseq: release 0x%lx", Status);
}

/*
 * The same without the loader: map a renamed copy of a system DLL as an
 * image section, unmap it, close everything and delete the file
 */
static VOID
ProbeImageCycle(const char *DirA, ULONG Count)
{
    WCHAR System[MAX_PATH], Copy[MAX_PATH];
    ULONG i, Never = 0, Start = GetTickCount();
    HANDLE File, Section;
    PVOID View;
    SIZE_T ViewSize;
    NTSTATUS Status;

    GetSystemDirectoryW(System, _countof(System));
    wcscat(System, L"\\shlwapi.dll");
    for (i = 0; i < Count; i++)
    {
        _snwprintf(Copy, _countof(Copy), L"%S\\mmt-imgcycle-%lu-%lu.dll", DirA, GetCurrentProcessId(), i);
        if (!CopyFileW(System, Copy, FALSE))
        {
            MmtLog("PROBE imagecycle: FAIL (copy %S: %lu)", Copy, GetLastError());
            return;
        }
        File = CreateFileW(Copy, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (File == INVALID_HANDLE_VALUE)
        {
            MmtLog("PROBE imagecycle: FAIL (open %S: %lu)", Copy, GetLastError());
            return;
        }
        Status = NtCreateSection(&Section, SECTION_ALL_ACCESS, NULL, NULL, PAGE_EXECUTE, SEC_IMAGE, File);
        CloseHandle(File);
        if (!NT_SUCCESS(Status))
        {
            MmtLog("PROBE imagecycle: FAIL (section: 0x%lx)", Status);
            return;
        }
        View = NULL;
        ViewSize = 0;
        Status = NtMapViewOfSection(Section, NtCurrentProcess(), &View, 0, 0, NULL, &ViewSize, ViewShare, 0, PAGE_EXECUTE_READ);
        if (NT_SUCCESS(Status))
        {
            (void)*(volatile UCHAR *)View;
            NtUnmapViewOfSection(NtCurrentProcess(), View);
        }
        else
        {
            MmtLog("PROBE imagecycle: map 0x%lx", Status);
        }
        CloseHandle(Section);
        if (!DeleteFileW(Copy))
        {
            DWORD Error = GetLastError();
            ULONG Tries;

            for (Tries = 0; Tries < 20 && !DeleteFileW(Copy); Tries++)
                Sleep(100);
            if (Tries == 20)
            {
                if (Never++ < 3)
                    MmtLog("PROBE imagecycle: %S not deletable after unmap and close (error %lu)", Copy, Error);
            }
        }
    }
    MmtLog("PROBE imagecycle x %lu: %s (%lu never deletable) in %lu s", Count, Never ? "FAIL" : "PASS", Never,
           (GetTickCount() - Start) / 1000);
}

typedef struct _UNMAP_RACE
{
    PUCHAR volatile View;
    ULONG Pages;
    volatile LONG Stop;
    volatile LONG Reads;
    ULONG Seed;
} UNMAP_RACE;

static DWORD WINAPI
UnmapRaceReader(PVOID Param)
{
    UNMAP_RACE *Race = Param;
    MMT_RNG Rng;
    UCHAR Bytes[8];

    MmtRngInit(&Rng, Race->Seed ^ GetCurrentThreadId());
    while (!Race->Stop)
    {
        PUCHAR View = Race->View;

        /* The view may go away under the read: ReadProcessMemory then fails instead of faulting */
        if (View && ReadProcessMemory(GetCurrentProcess(), View + (SIZE_T)MmtRand(&Rng) % Race->Pages * PAGE_SIZE,
                                      Bytes, sizeof(Bytes), NULL))
        {
            InterlockedIncrement(&Race->Reads);
        }
    }
    return 0;
}

/*
 * Unmap a copy-on-write view while its private pages are paged in and out
 * of the paging file: every round dirties a view larger than RAM, so its
 * copies live partly in the paging file, starts readers that fault them
 * back in, and unmaps the view under them
 */
static VOID
ProbeUnmapRace(const char *DirA, ULONG Mb, ULONG Rounds)
{
    WCHAR Path[MAX_PATH];
    HANDLE File, Section, Threads[4];
    UNMAP_RACE Race = { NULL, Mb * 256, 0, 0, GetTickCount() };
    PUCHAR View, Buffer = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    ULONG i, Round, Start = GetTickCount();
    DWORD Done;
    MMT_RNG Rng;

    MmtRngInit(&Rng, GetTickCount());
    _snwprintf(Path, _countof(Path), L"%S\\mmt-unmaprace-%lu.bin", DirA, GetCurrentProcessId());
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (File == INVALID_HANDLE_VALUE || !Buffer)
    {
        MmtLog("PROBE unmaprace: FAIL (create %S: %lu)", Path, GetLastError());
        return;
    }
    memset(Buffer, 0x5A, 1 << 20);
    for (i = 0; i < Mb; i++)
    {
        if (!WriteFile(File, Buffer, 1 << 20, &Done, NULL) || Done != (1 << 20))
        {
            MmtLog("PROBE unmaprace: FAIL (write at MB %lu: %lu)", i, GetLastError());
            CloseHandle(File);
            return;
        }
    }
    Section = CreateFileMappingW(File, NULL, PAGE_WRITECOPY, 0, 0, NULL);
    if (!Section)
    {
        MmtLog("PROBE unmaprace: FAIL (section: %lu)", GetLastError());
        CloseHandle(File);
        return;
    }
    for (i = 0; i < _countof(Threads); i++)
        Threads[i] = CreateThread(NULL, 0, UnmapRaceReader, &Race, 0, NULL);
    for (Round = 0; Round < Rounds; Round++)
    {
        View = MapViewOfFile(Section, FILE_MAP_COPY, 0, 0, 0);
        if (!View)
        {
            MmtLog("PROBE unmaprace: FAIL (map in round %lu: %lu)", Round, GetLastError());
            break;
        }
        for (i = 0; i < Race.Pages; i++)
            View[(SIZE_T)i * PAGE_SIZE] = (UCHAR)(Round + i);
        MmtLog("PROBE unmaprace: round %lu dirtied, %lu s", Round + 1, (GetTickCount() - Start) / 1000);
        Race.View = View;
        Sleep(MmtRandRange(&Rng, 20, 2000));
        Race.View = NULL;
        UnmapViewOfFile(View);
        MmtLog("PROBE unmaprace: round %lu, %ld reads, %lu s", Round + 1, Race.Reads, (GetTickCount() - Start) / 1000);
    }
    Race.Stop = 1;
    WaitForMultipleObjects(_countof(Threads), Threads, TRUE, INFINITE);
    for (i = 0; i < _countof(Threads); i++)
        CloseHandle(Threads[i]);
    MmtLog("PROBE unmaprace %lu MB x %lu: %s in %lu s (%ld reads)", Mb, Round, Round == Rounds ? "PASS" : "FAIL",
           (GetTickCount() - Start) / 1000, Race.Reads);
    CloseHandle(Section);
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

static VOID
ProbeQuery(const char *Name, PUCHAR Address, DWORD State, DWORD Protect, SIZE_T Size)
{
    MEMORY_BASIC_INFORMATION Mbi;

    VirtualQuery(Address, &Mbi, sizeof(Mbi));
    MmtLog("PROBE %s: state %lx prot %lx size %lx (expected %lx %lx %lx): %s", Name, Mbi.State, Mbi.Protect,
           (ULONG)Mbi.RegionSize, State, Protect, (ULONG)Size,
           (Mbi.State == State && Mbi.Protect == Protect && Mbi.RegionSize == Size) ? "PASS" : "FAIL");
}

/* Protection changes of touched private pages, and how queries merge pages */
static VOID
ProbeProtect(VOID)
{
    PUCHAR P = VirtualAlloc(NULL, 8 * PAGE_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    DWORD Old;
    UCHAR Value;
    NTSTATUS Status;

    /* Three committed pages, only the middle one touched */
    VirtualAlloc(P, 3 * PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    P[PAGE_SIZE] = 1;
    ProbeQuery("query-touched-and-untouched", P, MEM_COMMIT, PAGE_READWRITE, 3 * PAGE_SIZE);

    /* Touched page to no access and back */
    VirtualProtect(P + PAGE_SIZE, PAGE_SIZE, PAGE_NOACCESS, &Old);
    VirtualProtect(P + PAGE_SIZE, PAGE_SIZE, PAGE_READWRITE, &Old);
    MmtLog("PROBE protect-old: %lx (expected 1): %s", Old, Old == PAGE_NOACCESS ? "PASS" : "FAIL");
    ProbeQuery("query-after-noaccess-rw", P + PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE, 2 * PAGE_SIZE);
    Status = TryRead(P + PAGE_SIZE, &Value);
    MmtLog("PROBE read-after-noaccess-rw: %08lx value %u: %s", Status, Value, (Status == 0 && Value == 1) ? "PASS" : "FAIL");
    Status = TryWrite(P + PAGE_SIZE, 2);
    MmtLog("PROBE write-after-noaccess-rw: %08lx: %s", Status, Status == 0 ? "PASS" : "FAIL");

    /* Touched page to guard, triggered, then queried */
    P[2 * PAGE_SIZE] = 3;
    VirtualProtect(P + 2 * PAGE_SIZE, PAGE_SIZE, PAGE_READWRITE | PAGE_GUARD, &Old);
    Status = TryRead(P + 2 * PAGE_SIZE, &Value);
    MmtLog("PROBE guard-first-touch: %08lx (expected 80000001): %s", Status, Status == STATUS_GUARD_PAGE_VIOLATION ? "PASS" : "FAIL");
    Status = TryRead(P + 2 * PAGE_SIZE, &Value);
    MmtLog("PROBE guard-second-touch: %08lx value %u: %s", Status, Value, (Status == 0 && Value == 3) ? "PASS" : "FAIL");
    ProbeQuery("query-after-guard", P, MEM_COMMIT, PAGE_READWRITE, 3 * PAGE_SIZE);

    /* Untouched page to read-only and back */
    VirtualAlloc(P + 4 * PAGE_SIZE, 2 * PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    VirtualProtect(P + 4 * PAGE_SIZE, PAGE_SIZE, PAGE_READONLY, &Old);
    VirtualProtect(P + 4 * PAGE_SIZE, PAGE_SIZE, PAGE_READWRITE, &Old);
    ProbeQuery("query-untouched-ro-rw", P + 4 * PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE, 2 * PAGE_SIZE);
    VirtualFree(P, 0, MEM_RELEASE);
}

/* Commit after various decommit histories: the new protection must apply */
static VOID
ProbeRecommit(VOID)
{
    PUCHAR P = VirtualAlloc(NULL, 16 * PAGE_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    DWORD Old;
    ULONG i;

    for (i = 0; i < 6; i++)
    {
        PUCHAR Q = P + i * 2 * PAGE_SIZE;
        char Name[48];
        VirtualAlloc(Q, PAGE_SIZE, MEM_COMMIT, (i == 4) ? (PAGE_READWRITE | PAGE_GUARD) : PAGE_READWRITE);
        if (i == 1 || i == 2 || i == 5)
            Q[0] = 1;
        if (i == 1)
            VirtualProtect(Q, PAGE_SIZE, PAGE_NOACCESS, &Old);
        if (i == 2)
            VirtualLock(Q, PAGE_SIZE);
        if (i == 5)
            VirtualProtect(Q, PAGE_SIZE, PAGE_READONLY, &Old);
        VirtualFree(Q, PAGE_SIZE, MEM_DECOMMIT);
        VirtualAlloc(Q, PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
        _snprintf(Name, sizeof(Name), "recommit-%s", (const char *[]){ "untouched", "noaccess", "locked", "plain", "guard", "readonly" }[i]);
        ProbeQuery(Name, Q, MEM_COMMIT, PAGE_READWRITE, PAGE_SIZE);
    }
    /* Commit a range that is partly committed already, all with the same protection */
    VirtualAlloc(P + 13 * PAGE_SIZE, PAGE_SIZE, MEM_COMMIT, PAGE_READONLY);
    P[13 * PAGE_SIZE] = P[13 * PAGE_SIZE];
    VirtualAlloc(P + 12 * PAGE_SIZE, 3 * PAGE_SIZE, MEM_COMMIT, PAGE_READONLY);
    ProbeQuery("recommit-partial", P + 12 * PAGE_SIZE, MEM_COMMIT, PAGE_READONLY, 3 * PAGE_SIZE);
    VirtualFree(P, 0, MEM_RELEASE);
}

/* Every pair of protection changes on a touched and an untouched page: old value, query and access */
static VOID
ProbeProtectPairs(VOID)
{
    static const DWORD Prot[] = { PAGE_READWRITE, PAGE_READONLY, PAGE_NOACCESS, PAGE_READWRITE | PAGE_GUARD,
                                  PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_READ, PAGE_READONLY | PAGE_GUARD };
    ULONG a, b, touched, Bad = 0;

    for (touched = 0; touched < 2; touched++)
    for (a = 0; a < _countof(Prot); a++)
    for (b = 0; b < _countof(Prot); b++)
    {
        PUCHAR P = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        MEMORY_BASIC_INFORMATION Mbi;
        DWORD Old1 = 0, Old2 = 0;
        if (!P)
            continue;
        if (touched)
            P[0] = 7;
        VirtualProtect(P, PAGE_SIZE, Prot[a], &Old1);
        VirtualProtect(P, PAGE_SIZE, Prot[b], &Old2);
        VirtualQuery(P, &Mbi, sizeof(Mbi));
        if (Old1 != PAGE_READWRITE || Old2 != Prot[a] || Mbi.Protect != Prot[b])
        {
            if (Bad++ < 40)
                MmtLog("PROBE pair touched=%lu %lx->%lx: old1 %lx old2 %lx query %lx: FAIL", touched, Prot[a], Prot[b], Old1, Old2, Mbi.Protect);
        }
        VirtualFree(P, 0, MEM_RELEASE);
    }
    MmtLog("PROBE protect-pairs: %lu mismatches: %s", Bad, Bad ? "FAIL" : "PASS");
}

/* Access a page the way the vmapi model expects: a guard page fires once, then has its plain protection */
static VOID
ChainAccess(PUCHAR P, DWORD *Cur, DWORD *Bad, const char *Step, DWORD a, DWORD b, DWORD c)
{
    UCHAR V;
    NTSTATUS R = TryRead(P, &V), W;
    BOOL Writable = ((*Cur & 0xFF) == PAGE_READWRITE || (*Cur & 0xFF) == PAGE_EXECUTE_READWRITE);
    NTSTATUS ExpectR = STATUS_SUCCESS;

    if (*Cur & PAGE_GUARD)
    {
        ExpectR = STATUS_GUARD_PAGE_VIOLATION;
        *Cur &= ~PAGE_GUARD;
    }
    else if (*Cur == PAGE_NOACCESS)
        ExpectR = STATUS_ACCESS_VIOLATION;
    if (R != ExpectR && (*Bad)++ < 40)
        MmtLog("PROBE chain %lx->%lx->%lx %s: read %lx expected %lx: FAIL", a, b, c, Step, R, ExpectR);
    if (ExpectR != STATUS_SUCCESS)
        return;
    W = TryWrite(P, 9);
    if ((W == STATUS_SUCCESS) != Writable && (*Bad)++ < 40)
        MmtLog("PROBE chain %lx->%lx->%lx %s: write %lx with %lx: FAIL", a, b, c, Step, W, *Cur);
}

/* Three protection changes with an access after each, as the vmapi workload does: old protections and query */
static VOID
ProbeProtectChains(VOID)
{
    static const DWORD Prot[] = { PAGE_READWRITE, PAGE_READONLY, PAGE_NOACCESS, PAGE_READWRITE | PAGE_GUARD,
                                  PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_READ, PAGE_READONLY | PAGE_GUARD };
    ULONG a, b, c, i;
    DWORD Bad = 0;

    for (a = 0; a < _countof(Prot); a++)
    for (b = 0; b < _countof(Prot); b++)
    for (c = 0; c < _countof(Prot); c++)
    {
        PUCHAR P = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        const DWORD Seq[3] = { Prot[a], Prot[b], Prot[c] };
        MEMORY_BASIC_INFORMATION Mbi;
        DWORD Cur = PAGE_READWRITE, Old;
        if (!P)
            continue;
        P[0] = 7;
        for (i = 0; i < 3; i++)
        {
            Old = 0;
            if (!VirtualProtect(P, PAGE_SIZE, Seq[i], &Old))
            {
                if (Bad++ < 40)
                    MmtLog("PROBE chain %lx->%lx->%lx step %lu: VirtualProtect error %lu: FAIL", Seq[0], Seq[1], Seq[2], i, GetLastError());
                break;
            }
            if (Old != Cur && Bad++ < 40)
                MmtLog("PROBE chain %lx->%lx->%lx step %lu: old %lx expected %lx: FAIL", Seq[0], Seq[1], Seq[2], i, Old, Cur);
            Cur = Seq[i];
            VirtualQuery(P, &Mbi, sizeof(Mbi));
            if (Mbi.Protect != Cur && Bad++ < 40)
                MmtLog("PROBE chain %lx->%lx->%lx step %lu: query %lx expected %lx: FAIL", Seq[0], Seq[1], Seq[2], i, Mbi.Protect, Cur);
            ChainAccess(P, &Cur, &Bad, i == 0 ? "access 0" : i == 1 ? "access 1" : "access 2", Seq[0], Seq[1], Seq[2]);
            VirtualQuery(P, &Mbi, sizeof(Mbi));
            if (Mbi.Protect != Cur && Bad++ < 40)
                MmtLog("PROBE chain %lx->%lx->%lx after access %lu: query %lx expected %lx: FAIL", Seq[0], Seq[1], Seq[2], i, Mbi.Protect, Cur);
        }
        VirtualFree(P, 0, MEM_RELEASE);
    }
    MmtLog("PROBE protect-chains: %lu mismatches: %s", Bad, Bad ? "FAIL" : "PASS");
}


int
MmtProbeMain(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[2], "bigwrite"))
    {
        ProbeBigWrite(argv[3], MmtArgUlong(argc, argv, 4, 1024));
        MmtLog("PROBES DONE");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[2], "swapcow"))
    {
        ProbeSwapCow(argv[3], MmtArgUlong(argc, argv, 4, 512), MmtArgUlong(argc, argv, 5, 3),
                     argc > 6 && !strcmp(argv[6], "remap"));
        MmtLog("PROBES DONE");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "vlockseq"))
    {
        ProbeVlockSeq();
        MmtLog("PROBES DONE");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[2], "imagecycle"))
    {
        ProbeImageCycle(argv[3], MmtArgUlong(argc, argv, 4, 50));
        MmtLog("PROBES DONE");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[2], "dllcycle"))
    {
        ProbeDllCycle(argv[3], MmtArgUlong(argc, argv, 4, 200));
        MmtLog("PROBES DONE");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[2], "unmaprace"))
    {
        ProbeUnmapRace(argv[3], MmtArgUlong(argc, argv, 4, 512), MmtArgUlong(argc, argv, 5, 100));
        MmtLog("PROBES DONE");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "chains"))
    {
        ProbeProtectChains();
        MmtLog("PROBES DONE");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "leak"))
    {
        ProbeProcessLeak("quick", 200);
        ProbeProcessLeak("alloc", 100);
        ProbeProcessLeak("threads", 100);
        ProbeProcessLeak("map", 100);
        ProbeProcessLeak("dll", 100);
        ProbeProcessLeak("stack", 100);
        MmtLog("PROBES DONE");
        return 0;
    }
    ProbeProtect();
    ProbeProtectPairs();
    ProbeProtectChains();
    ProbeRecommit();
    ProbeCow("cow-pagefile", FALSE, FALSE, FALSE, FALSE);
    ProbeCow("cow-pagefile-readcow", FALSE, FALSE, TRUE, FALSE);
    ProbeCow("cow-pagefile-readview", FALSE, TRUE, TRUE, FALSE);
    ProbeCow("cow-pagefile-byname", FALSE, TRUE, TRUE, TRUE);
    ProbeCow("cow-file", TRUE, FALSE, FALSE, FALSE);
    ProbeCow("cow-file-readcow", TRUE, FALSE, TRUE, FALSE);
    ProbeCow("cow-file-readview", TRUE, TRUE, TRUE, FALSE);
    MmtLog("PROBES DONE");
    return 0;
}
