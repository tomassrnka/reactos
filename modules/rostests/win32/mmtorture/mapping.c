/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Workload (c): data and image file mappings. Pages of a large
 *              file are written and read through mapped views, cached file
 *              I/O and long-lived views at once; every page carries its
 *              index and version so any reader can check it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

#define SLICE_PAGES 256
#define SLICE_BYTES (SLICE_PAGES * PAGE_SIZE)
#define PAGE_MAGIC 0x4D4D5450ULL
#define FILE_SEED 0xF11EF11E12345678ULL

typedef struct _MAPFILE
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    ULONG Slices;
    CRITICAL_SECTION *Locks;
    PUCHAR LongView;
    ULONG LongSlices;
    HANDLE LongSection;
} MAPFILE;

static MAPFILE Map;
static WCHAR Dir[MAX_PATH];
static LONG Slot;
static volatile LONG ImageCounter;

static VOID
WriteVersionedPage(PUCHAR Page, ULONGLONG PageIndex, ULONGLONG Version)
{
    ULONGLONG *P = (ULONGLONG *)Page;
    SIZE_T i;

    P[0] = (PAGE_MAGIC << 32) | PageIndex;
    P[1] = Version;
    for (i = 2; i < PAGE_SIZE / sizeof(ULONGLONG); i++)
        P[i] = MmtPatternWord(FILE_SEED ^ Version, (SIZE_T)PageIndex, i);
}

static BOOL
CheckVersionedPage(const UCHAR *Page, ULONGLONG PageIndex, const char *What)
{
    const ULONGLONG *P = (const ULONGLONG *)Page;
    ULONGLONG Version = P[1];
    SIZE_T i;

    if (P[0] != ((PAGE_MAGIC << 32) | PageIndex))
    {
        MmtFail("%s: page %lu header %08lx%08lx (wrong page or lost data) at %p",
                What, (ULONG)PageIndex, (ULONG)(P[0] >> 32), (ULONG)P[0], Page);
        return FALSE;
    }
    for (i = 2; i < PAGE_SIZE / sizeof(ULONGLONG); i += 37)
    {
        if (P[i] != MmtPatternWord(FILE_SEED ^ Version, (SIZE_T)PageIndex, i))
        {
            MmtFail("%s: page %lu version %lu word %lu torn at %p", What, (ULONG)PageIndex, (ULONG)Version, (ULONG)i, Page);
            return FALSE;
        }
    }
    if (P[PAGE_SIZE / sizeof(ULONGLONG) - 1] != MmtPatternWord(FILE_SEED ^ Version, (SIZE_T)PageIndex, PAGE_SIZE / sizeof(ULONGLONG) - 1))
    {
        MmtFail("%s: page %lu version %lu last word torn", What, (ULONG)PageIndex, (ULONG)Version);
        return FALSE;
    }
    return TRUE;
}

/* Positional I/O: the threads share one handle, so never use its file pointer */
static BOOL
ReadAt(HANDLE File, ULONGLONG Offset, PVOID Buffer, DWORD Length, PDWORD Done)
{
    OVERLAPPED Ov;

    ZeroMemory(&Ov, sizeof(Ov));
    Ov.Offset = (DWORD)Offset;
    Ov.OffsetHigh = (DWORD)(Offset >> 32);
    *Done = 0;
    return ReadFile(File, Buffer, Length, Done, &Ov);
}

static BOOL
WriteAt(HANDLE File, ULONGLONG Offset, PVOID Buffer, DWORD Length, PDWORD Done)
{
    OVERLAPPED Ov;

    ZeroMemory(&Ov, sizeof(Ov));
    Ov.Offset = (DWORD)Offset;
    Ov.OffsetHigh = (DWORD)(Offset >> 32);
    *Done = 0;
    return WriteFile(File, Buffer, Length, Done, &Ov);
}

static BOOL
CreateDataFile(ULONG FileMb)
{
    PUCHAR Buffer;
    ULONG Slice, i;
    DWORD Done;

    _snwprintf(Map.Path, _countof(Map.Path), L"%s\\mmt-map-%lu.dat", Dir, GetCurrentProcessId());
    Map.File = CreateFileW(Map.Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Map.File == INVALID_HANDLE_VALUE)
    {
        MmtFail("mapping: cannot create %S: %lu", Map.Path, GetLastError());
        return FALSE;
    }
    Map.Slices = FileMb * 1024 * 1024 / SLICE_BYTES;
    Map.Locks = HeapAlloc(GetProcessHeap(), 0, Map.Slices * sizeof(CRITICAL_SECTION));
    Buffer = VirtualAlloc(NULL, SLICE_BYTES, MEM_COMMIT, PAGE_READWRITE);
    if (!Map.Locks || !Buffer)
        return FALSE;
    for (Slice = 0; Slice < Map.Slices; Slice++)
    {
        InitializeCriticalSection(&Map.Locks[Slice]);
        for (i = 0; i < SLICE_PAGES; i++)
            WriteVersionedPage(Buffer + i * PAGE_SIZE, (ULONGLONG)Slice * SLICE_PAGES + i, 0);
        if (!WriteFile(Map.File, Buffer, SLICE_BYTES, &Done, NULL) || Done != SLICE_BYTES)
        {
            MmtFail("mapping: writing %S failed at slice %lu: %lu", Map.Path, Slice, GetLastError());
            return FALSE;
        }
        if ((Slice & 63) == 0)
            MmtProgress(Slot);
    }
    VirtualFree(Buffer, 0, MEM_RELEASE);
    FlushFileBuffers(Map.File);
    MmtLog("mapping: created %S, %lu MB", Map.Path, FileMb);
    return TRUE;
}

static PUCHAR
MapSlice(HANDLE Section, ULONG Slice, BOOL Write)
{
    ULONGLONG Offset = (ULONGLONG)Slice * SLICE_BYTES;

    return MapViewOfFile(Section, Write ? FILE_MAP_WRITE : FILE_MAP_READ,
                         (DWORD)(Offset >> 32), (DWORD)Offset, SLICE_BYTES);
}

static BOOL
CheckSliceBuffer(PUCHAR Base, ULONG Slice, const char *What)
{
    ULONG i;

    for (i = 0; i < SLICE_PAGES; i++)
    {
        if (!CheckVersionedPage(Base + i * PAGE_SIZE, (ULONGLONG)Slice * SLICE_PAGES + i, What))
            return FALSE;
    }
    return TRUE;
}

static VOID
ImageAction(MMT_RNG *Rng)
{
    WCHAR Copy[MAX_PATH], Args[MAX_PATH + 32], DllCopy[MAX_PATH], System[MAX_PATH];
    LONG n = InterlockedIncrement(&ImageCounter);
    DWORD Code;
    HMODULE Module;
    HANDLE Process;

    /* Run a copy of ourselves from the test volume: a new image section each time */
    _snwprintf(Copy, _countof(Copy), L"%s\\mmtimg-%lu-%ld.exe", Dir, GetCurrentProcessId(), n);
    if (!CopyFileW(MmtExePath, Copy, FALSE))
    {
        MmtLog("mapping: CopyFile to %S failed %lu", Copy, GetLastError());
        return;
    }
    _snwprintf(Args, _countof(Args), L"!\"%s\" child %S", Copy, (MmtRand(Rng) & 1) ? "threads" : "quick");
    if (MmtSpawn(Args, FALSE, 0, NULL, &Process))
    {
        /* The running image must not be deletable or writable */
        if (DeleteFileW(Copy))
            MmtFail("mapping: deleted the image %S of a running process", Copy);
        if (WaitForSingleObject(Process, 180000) != WAIT_OBJECT_0)
        {
            MmtFail("mapping: image child did not end");
            TerminateProcess(Process, 0xDEAD);
            WaitForSingleObject(Process, 60000);
        }
        GetExitCodeProcess(Process, &Code);
        if (Code != 0)
            MmtFail("mapping: image child exited %08lx", Code);
        CloseHandle(Process);
    }
    else
    {
        MmtFail("mapping: cannot start %S: %lu", Copy, GetLastError());
    }
    if (!DeleteFileW(Copy))
    {
        /* The image section can outlive the process for a moment */
        Sleep(1000);
        if (!DeleteFileW(Copy))
            MmtFail("mapping: cannot delete %S after the process ended: %lu", Copy, GetLastError());
    }

    /* Load a renamed copy of a system DLL from the test volume */
    GetSystemDirectoryW(System, _countof(System));
    wcscat(System, L"\\shlwapi.dll");
    _snwprintf(DllCopy, _countof(DllCopy), L"%s\\mmtdll-%lu-%ld.dll", Dir, GetCurrentProcessId(), n);
    if (CopyFileW(System, DllCopy, FALSE))
    {
        Module = LoadLibraryW(DllCopy);
        if (!Module)
            MmtFail("mapping: LoadLibrary %S failed %lu", DllCopy, GetLastError());
        else if (!GetProcAddress(Module, "StrStrIW"))
            MmtFail("mapping: no StrStrIW in %S", DllCopy);
        if (Module)
            FreeLibrary(Module);
        if (!DeleteFileW(DllCopy))
        {
            Sleep(1000);
            if (!DeleteFileW(DllCopy))
                MmtFail("mapping: cannot delete %S after FreeLibrary: %lu", DllCopy, GetLastError());
        }
    }
}

static VOID
GrowAction(MMT_RNG *Rng)
{
    WCHAR Path[MAX_PATH];
    HANDLE File, Section, Section2;
    PUCHAR View, View2;
    ULONG Pages = MmtRandRange(Rng, 16, 512), More = MmtRandRange(Rng, 16, 1024), i;
    LARGE_INTEGER Size;

    _snwprintf(Path, _countof(Path), L"%s\\mmt-grow-%lu-%lu.dat", Dir, GetCurrentProcessId(), GetCurrentThreadId());
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        MmtFail("mapping: cannot create %S: %lu", Path, GetLastError());
        return;
    }
    Section = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, Pages * PAGE_SIZE, NULL);
    View = Section ? MapViewOfFile(Section, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (!View)
    {
        MmtLog("mapping: grow map failed %lu", GetLastError());
        goto Out;
    }
    for (i = 0; i < Pages; i++)
        WriteVersionedPage(View + i * PAGE_SIZE, i, 7);

    /* Grow the file under the mapped view, map the new part through a new section */
    Size.QuadPart = (LONGLONG)(Pages + More) * PAGE_SIZE;
    if (!SetFilePointerEx(File, Size, NULL, FILE_BEGIN) || !SetEndOfFile(File))
    {
        MmtFail("mapping: growing a mapped file failed %lu", GetLastError());
        goto Out;
    }
    Section2 = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, 0, NULL);
    View2 = Section2 ? MapViewOfFile(Section2, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (View2)
    {
        for (i = 0; i < Pages; i++)
        {
            if (!CheckVersionedPage(View2 + i * PAGE_SIZE, i, "grow-second-view"))
                break;
        }
        for (i = Pages; i < Pages + More; i++)
        {
            if (!MmtIsZeroPage(View2 + i * PAGE_SIZE))
            {
                MmtFail("mapping: extended part of %S not zero at page %lu", Path, i);
                break;
            }
            WriteVersionedPage(View2 + i * PAGE_SIZE, i, 8);
        }
        /* Truncating below a mapped view must fail */
        Size.QuadPart = PAGE_SIZE;
        if (SetFilePointerEx(File, Size, NULL, FILE_BEGIN) && SetEndOfFile(File))
            MmtFail("mapping: truncated a file below a mapped view");
        UnmapViewOfFile(View2);
    }
    else
    {
        MmtLog("mapping: second grow map failed %lu", GetLastError());
    }
    if (Section2)
        CloseHandle(Section2);
    /* Still mapped through View, with its section handle closed: truncation must still fail */
    CloseHandle(Section);
    Section = NULL;
    Size.QuadPart = PAGE_SIZE;
    if (SetFilePointerEx(File, Size, NULL, FILE_BEGIN) && SetEndOfFile(File))
        MmtFail("mapping: truncated a file below a view whose section handle is closed");
    UnmapViewOfFile(View);
    View = NULL;

    /* With every view gone the data must read back through the file */
    {
        PUCHAR Buffer = VirtualAlloc(NULL, PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
        DWORD Done;
        for (i = 0; Buffer && i < Pages + More; i += 13)
        {
            if (!ReadAt(File, (ULONGLONG)i * PAGE_SIZE, Buffer, PAGE_SIZE, &Done) || Done != PAGE_SIZE)
            {
                MmtFail("mapping: read back of grown file failed at page %lu: %lu", i, GetLastError());
                break;
            }
            if (!CheckVersionedPage(Buffer, i, "grow-readback"))
                break;
        }
        if (Buffer)
            VirtualFree(Buffer, 0, MEM_RELEASE);
    }
    Size.QuadPart = PAGE_SIZE;
    if (!SetFilePointerEx(File, Size, NULL, FILE_BEGIN) || !SetEndOfFile(File))
        MmtFail("mapping: truncate without views failed %lu", GetLastError());
Out:
    if (View)
        UnmapViewOfFile(View);
    if (Section)
        CloseHandle(Section);
    CloseHandle(File);
    if (!DeleteFileW(Path))
        MmtFail("mapping: cannot delete %S: %lu", Path, GetLastError());
}

static DWORD WINAPI
MappingThread(PVOID Context)
{
    ULONG Index = (ULONG)(ULONG_PTR)Context;
    HANDLE Section = NULL;
    PUCHAR Buffer, View;
    MMT_RNG Rng;
    ULONG n = 0;
    DWORD Done;

    MmtRngInit(&Rng, GetTickCount() ^ (Index << 24) ^ GetCurrentProcessId());
    Buffer = VirtualAlloc(NULL, SLICE_BYTES, MEM_COMMIT, PAGE_READWRITE);
    while (!MmtShouldStop())
    {
        ULONG Slice = MmtRandRange(&Rng, 0, Map.Slices - 1);
        ULONG Action = MmtRandRange(&Rng, 0, 99);
        ULONG i;

        if (!Section || (n++ % 200) == 0)
        {
            /* A new section object for the same file now and then */
            if (Section)
                CloseHandle(Section);
            Section = CreateFileMappingW(Map.File, NULL, PAGE_READWRITE, 0, 0, NULL);
            if (!Section)
            {
                MmtFail("mapping: CreateFileMapping failed %lu", GetLastError());
                Sleep(1000);
                continue;
            }
        }

        if (Action < 2)
        {
            ImageAction(&Rng);
            MmtProgress(Slot);
            continue;
        }
        if (Action < 4)
        {
            GrowAction(&Rng);
            MmtProgress(Slot);
            continue;
        }

        EnterCriticalSection(&Map.Locks[Slice]);
        if (Action < 30)
        {
            View = MapSlice(Section, Slice, FALSE);
            if (View)
            {
                CheckSliceBuffer(View, Slice, "map-read");
                UnmapViewOfFile(View);
            }
            else
            {
                MmtLog("mapping: map-read failed %lu", GetLastError());
            }
        }
        else if (Action < 50)
        {
            View = MapSlice(Section, Slice, TRUE);
            if (View)
            {
                for (i = 0; i < 16; i++)
                {
                    ULONG Page = MmtRandRange(&Rng, 0, SLICE_PAGES - 1);
                    ULONGLONG *P = (ULONGLONG *)(View + Page * PAGE_SIZE);
                    if (CheckVersionedPage((PUCHAR)P, (ULONGLONG)Slice * SLICE_PAGES + Page, "map-write-before"))
                        WriteVersionedPage((PUCHAR)P, (ULONGLONG)Slice * SLICE_PAGES + Page, P[1] + 1);
                }
                if (MmtRand(&Rng) % 4 == 0 && !FlushViewOfFile(View, 0))
                    MmtFail("mapping: FlushViewOfFile failed %lu", GetLastError());
                UnmapViewOfFile(View);
            }
            else
            {
                MmtLog("mapping: map-write failed %lu", GetLastError());
            }
        }
        else if (Action < 70 && Buffer)
        {
            if (ReadAt(Map.File, (ULONGLONG)Slice * SLICE_BYTES, Buffer, SLICE_BYTES, &Done) && Done == SLICE_BYTES)
            {
                CheckSliceBuffer(Buffer, Slice, "file-read");
            }
            else
            {
                MmtFail("mapping: ReadFile slice %lu failed %lu (got %lu)", Slice, GetLastError(), Done);
            }
        }
        else if (Action < 85 && Buffer)
        {
            /* Read the pages, bump their versions, write them back through the cache */
            ULONG Page = MmtRandRange(&Rng, 0, SLICE_PAGES - 8), Count = MmtRandRange(&Rng, 1, 8);
            ULONGLONG Offset = (ULONGLONG)Slice * SLICE_BYTES + (ULONGLONG)Page * PAGE_SIZE;
            if (ReadAt(Map.File, Offset, Buffer, Count * PAGE_SIZE, &Done) && Done == Count * PAGE_SIZE)
            {
                for (i = 0; i < Count; i++)
                {
                    ULONGLONG *P = (ULONGLONG *)(Buffer + i * PAGE_SIZE);
                    if (CheckVersionedPage((PUCHAR)P, (ULONGLONG)Slice * SLICE_PAGES + Page + i, "file-write-before"))
                        WriteVersionedPage((PUCHAR)P, (ULONGLONG)Slice * SLICE_PAGES + Page + i, P[1] + 1);
                }
                if (!WriteAt(Map.File, Offset, Buffer, Count * PAGE_SIZE, &Done) || Done != Count * PAGE_SIZE)
                    MmtFail("mapping: WriteFile failed %lu", GetLastError());
            }
            else
            {
                MmtFail("mapping: ReadFile for write failed %lu", GetLastError());
            }
        }
        else if (Slice < Map.LongSlices)
        {
            /* Through the long-lived view */
            PUCHAR Base = Map.LongView + (SIZE_T)Slice * SLICE_BYTES;
            ULONG Page = MmtRandRange(&Rng, 0, SLICE_PAGES - 1);
            ULONGLONG *P = (ULONGLONG *)(Base + Page * PAGE_SIZE);
            CheckSliceBuffer(Base, Slice, "long-view");
            if (CheckVersionedPage((PUCHAR)P, (ULONGLONG)Slice * SLICE_PAGES + Page, "long-view-write"))
                WriteVersionedPage((PUCHAR)P, (ULONGLONG)Slice * SLICE_PAGES + Page, P[1] + 1);
        }
        LeaveCriticalSection(&Map.Locks[Slice]);
        MmtProgress(Slot);

        if (MmtRandRange(&Rng, 0, 499) == 0 && !FlushFileBuffers(Map.File))
            MmtFail("mapping: FlushFileBuffers failed %lu", GetLastError());
    }
    if (Section)
        CloseHandle(Section);
    if (Buffer)
        VirtualFree(Buffer, 0, MEM_RELEASE);
    return 0;
}

static VOID
FinalCheck(VOID)
{
    PUCHAR Buffer = VirtualAlloc(NULL, SLICE_BYTES, MEM_COMMIT, PAGE_READWRITE);
    HANDLE File;
    ULONG Slice;
    DWORD Done;

    /* A fresh handle, after every view and section is gone */
    File = CreateFileW(Map.Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE || !Buffer)
    {
        MmtFail("mapping: final reopen failed %lu", GetLastError());
        return;
    }
    for (Slice = 0; Slice < Map.Slices; Slice++)
    {
        if (!ReadFile(File, Buffer, SLICE_BYTES, &Done, NULL) || Done != SLICE_BYTES)
        {
            MmtFail("mapping: final read failed at slice %lu: %lu", Slice, GetLastError());
            break;
        }
        if (!CheckSliceBuffer(Buffer, Slice, "final"))
            break;
    }
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    MmtLog("mapping: final check of %lu slices done", Slice);
}

/* mapping DIR FILE_MB THREADS [LONG_VIEW_MB] */
int
MmtMappingMain(int argc, char **argv)
{
    ULONG FileMb = MmtArgUlong(argc, argv, 3, 256);
    ULONG Threads = min(MmtArgUlong(argc, argv, 4, 4), 32);
    ULONG LongMb = MmtArgUlong(argc, argv, 5, 32);
    HANDLE Handles[32];
    ULONG i, Count = 0;

    MmtOpenShared(FALSE);
    Slot = MmtAllocSlot("mapping");
    MultiByteToWideChar(CP_ACP, 0, argc > 2 ? argv[2] : "C:\\mmt", -1, Dir, _countof(Dir));
    CreateDirectoryW(Dir, NULL);
    if (!CreateDataFile(FileMb))
        return 1;

    Map.LongSlices = min(Map.Slices, LongMb * 1024 * 1024 / SLICE_BYTES);
    if (Map.LongSlices)
    {
        Map.LongSection = CreateFileMappingW(Map.File, NULL, PAGE_READWRITE, 0, 0, NULL);
        Map.LongView = Map.LongSection ? MapViewOfFile(Map.LongSection, FILE_MAP_WRITE, 0, 0, (SIZE_T)Map.LongSlices * SLICE_BYTES) : NULL;
        if (!Map.LongView)
        {
            MmtLog("mapping: long view failed %lu", GetLastError());
            Map.LongSlices = 0;
        }
    }

    for (i = 0; i < Threads; i++)
    {
        Handles[Count] = CreateThread(NULL, 0, MappingThread, (PVOID)(ULONG_PTR)i, 0, NULL);
        if (Handles[Count])
            Count++;
    }
    WaitForMultipleObjects(Count, Handles, TRUE, INFINITE);

    if (Map.LongView)
        UnmapViewOfFile(Map.LongView);
    if (Map.LongSection)
        CloseHandle(Map.LongSection);
    CloseHandle(Map.File);
    FinalCheck();
    if (!DeleteFileW(Map.Path))
        MmtFail("mapping: cannot delete %S: %lu", Map.Path, GetLastError());
    MmtLog("mapping done");
    return 0;
}
