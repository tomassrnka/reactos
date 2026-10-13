/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for the structure layouts of the Toolhelp functions
 *
 * The structures below are the ones of the Windows SDK, spelled out so that they do not depend
 * on the tlhelp32.h that is under test: a 64-bit program built with the Windows headers passes
 * these sizes to kernel32.
 */

#include "precomp.h"

#include <tlhelp32.h>
#include <versionhelpers.h>

typedef struct
{
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[MAX_PATH];
} SDK_PROCESSENTRY32W;

typedef struct
{
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    CHAR szExeFile[MAX_PATH];
} SDK_PROCESSENTRY32A;

typedef struct
{
    SIZE_T dwSize;
    DWORD th32ProcessID;
    ULONG_PTR th32HeapID;
    DWORD dwFlags;
} SDK_HEAPLIST32;

typedef struct
{
    SIZE_T dwSize;
    HANDLE hHandle;
    ULONG_PTR dwAddress;
    SIZE_T dwBlockSize;
    DWORD dwFlags;
    DWORD dwLockCount;
    DWORD dwResvd;
    DWORD th32ProcessID;
    ULONG_PTR th32HeapID;
} SDK_HEAPENTRY32;

#ifdef _WIN64
#define SIZE_PROCESSENTRY32W 568
#define SIZE_PROCESSENTRY32A 304
#define SIZE_HEAPLIST32 32
#define SIZE_HEAPENTRY32 56
#else
#define SIZE_PROCESSENTRY32W 556
#define SIZE_PROCESSENTRY32A 296
#define SIZE_HEAPLIST32 16
#define SIZE_HEAPENTRY32 36
#endif

static const WCHAR *BaseName(const WCHAR *Path)
{
    const WCHAR *Slash = wcsrchr(Path, L'\\');
    return Slash ? Slash + 1 : Path;
}

static void test_layouts(void)
{
    ok_int(sizeof(SDK_PROCESSENTRY32W), SIZE_PROCESSENTRY32W);
    ok_int(sizeof(SDK_PROCESSENTRY32A), SIZE_PROCESSENTRY32A);
    ok_int(sizeof(SDK_HEAPLIST32), SIZE_HEAPLIST32);
    ok_int(sizeof(SDK_HEAPENTRY32), SIZE_HEAPENTRY32);

    ok_int(sizeof(PROCESSENTRY32W), SIZE_PROCESSENTRY32W);
    ok_int(sizeof(PROCESSENTRY32), SIZE_PROCESSENTRY32A);
    ok_int(sizeof(HEAPLIST32), SIZE_HEAPLIST32);
    ok_int(sizeof(HEAPENTRY32), SIZE_HEAPENTRY32);

    ok_int(FIELD_OFFSET(PROCESSENTRY32W, th32DefaultHeapID), FIELD_OFFSET(SDK_PROCESSENTRY32W, th32DefaultHeapID));
    ok_int(FIELD_OFFSET(PROCESSENTRY32W, szExeFile), FIELD_OFFSET(SDK_PROCESSENTRY32W, szExeFile));
    ok_int(FIELD_OFFSET(PROCESSENTRY32, szExeFile), FIELD_OFFSET(SDK_PROCESSENTRY32A, szExeFile));
    ok_int(FIELD_OFFSET(HEAPLIST32, th32HeapID), FIELD_OFFSET(SDK_HEAPLIST32, th32HeapID));
    ok_int(FIELD_OFFSET(HEAPENTRY32, dwBlockSize), FIELD_OFFSET(SDK_HEAPENTRY32, dwBlockSize));
    ok_int(FIELD_OFFSET(HEAPENTRY32, th32HeapID), FIELD_OFFSET(SDK_HEAPENTRY32, th32HeapID));
}

static void test_process_wide(void)
{
    SDK_PROCESSENTRY32W Entry;
    WCHAR ModulePath[MAX_PATH];
    HANDLE Snapshot;
    DWORD Pid = GetCurrentProcessId();
    BOOL Found = FALSE;
    BOOL Ret;

    GetModuleFileNameW(NULL, ModulePath, ARRAYSIZE(ModulePath));

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    if (Snapshot == INVALID_HANDLE_VALUE)
        return;

    memset(&Entry, 0x55, sizeof(Entry));
    Entry.dwSize = 0;
    SetLastError(0xdeadbeef);
    Ret = Process32FirstW(Snapshot, (PROCESSENTRY32W *)&Entry);
    ok(!Ret, "Process32FirstW succeeded with dwSize 0\n");

    memset(&Entry, 0x55, sizeof(Entry));
    Entry.dwSize = sizeof(Entry);
    SetLastError(0xdeadbeef);
    Ret = Process32FirstW(Snapshot, (PROCESSENTRY32W *)&Entry);
    ok(Ret, "Process32FirstW failed with %lu\n", GetLastError());
    while (Ret)
    {
        ok_int(Entry.dwSize, sizeof(Entry));
        if (Entry.th32ProcessID == Pid)
        {
            Found = TRUE;
            ok(!_wcsicmp(Entry.szExeFile, BaseName(ModulePath)), "got %ls, expected %ls\n", Entry.szExeFile, BaseName(ModulePath));
            ok(Entry.cntThreads >= 1, "got %lu threads\n", Entry.cntThreads);
        }
        Ret = Process32NextW(Snapshot, (PROCESSENTRY32W *)&Entry);
    }
    ok(Found, "The current process is not in the snapshot\n");
    ok_err(ERROR_NO_MORE_FILES);

    CloseHandle(Snapshot);
}

static void test_process_ansi(void)
{
    SDK_PROCESSENTRY32A Entry;
    WCHAR ModulePath[MAX_PATH];
    CHAR NameA[MAX_PATH];
    HANDLE Snapshot;
    DWORD Pid = GetCurrentProcessId();
    BOOL Found = FALSE;
    BOOL Ret;

    GetModuleFileNameW(NULL, ModulePath, ARRAYSIZE(ModulePath));
    WideCharToMultiByte(CP_ACP, 0, BaseName(ModulePath), -1, NameA, sizeof(NameA), NULL, NULL);

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    if (Snapshot == INVALID_HANDLE_VALUE)
        return;

    memset(&Entry, 0x55, sizeof(Entry));
    Entry.dwSize = sizeof(Entry);
    SetLastError(0xdeadbeef);
    Ret = Process32First(Snapshot, (PROCESSENTRY32 *)&Entry);
    ok(Ret, "Process32First failed with %lu\n", GetLastError());
    while (Ret)
    {
        if (Entry.th32ProcessID == Pid)
        {
            Found = TRUE;
            ok(!_stricmp(Entry.szExeFile, NameA), "got %s, expected %s\n", Entry.szExeFile, NameA);
        }
        Ret = Process32Next(Snapshot, (PROCESSENTRY32 *)&Entry);
    }
    ok(Found, "The current process is not in the snapshot\n");

    CloseHandle(Snapshot);
}

#define MAX_HEAPS 128

/* How the child process hands the list of its heaps to the parent */
typedef struct
{
    DWORD Count;
    ULONG_PTR Heaps[MAX_HEAPS];
} HEAP_REPORT;

static DWORD HeapProcessHeaps(HANDLE *Heaps)
{
    DWORD Count = GetProcessHeaps(MAX_HEAPS, Heaps);

    ok(Count != 0 && Count <= MAX_HEAPS, "GetProcessHeaps returned %lu\n", Count);
    return min(Count, MAX_HEAPS);
}

/* Reads the heap list of a snapshot into Ids and checks the fields of every entry */
static DWORD ReadHeapList(HANDLE Snapshot, DWORD Pid, ULONG_PTR *Ids, DWORD *Flags)
{
    SDK_HEAPLIST32 List;
    DWORD Count = 0;
    BOOL Ret;

    memset(&List, 0x55, sizeof(List));
    List.dwSize = sizeof(List);
    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
    ok(Ret, "Heap32ListFirst failed with %lu\n", GetLastError());
    while (Ret && Count < MAX_HEAPS)
    {
        ok_int(List.dwSize, sizeof(List));
        ok_int(List.th32ProcessID, Pid);
        Ids[Count] = List.th32HeapID;
        Flags[Count] = List.dwFlags;
        Count++;
        List.dwSize = sizeof(List);
        SetLastError(0xdeadbeef);
        Ret = Heap32ListNext(Snapshot, (HEAPLIST32 *)&List);
    }
    ok(!Ret, "The heap list has more than %u entries\n", MAX_HEAPS);
    ok_err(ERROR_NO_MORE_FILES);
    return Count;
}

static void CheckHeapListAgainst(const ULONG_PTR *Expected, DWORD ExpectedCount, HANDLE ProcessHeap, DWORD Pid, HANDLE Snapshot)
{
    ULONG_PTR Ids[MAX_HEAPS];
    DWORD Flags[MAX_HEAPS];
    DWORD Count, i;

    Count = ReadHeapList(Snapshot, Pid, Ids, Flags);
    ok_int(Count, ExpectedCount);
    for (i = 0; i < min(Count, ExpectedCount); i++)
    {
        ok(Ids[i] == Expected[i], "heap %lu: got %p, expected %p\n", i, (PVOID)Ids[i], (PVOID)Expected[i]);
        ok(Flags[i] == (i == 0 ? HF32_DEFAULT : 0), "heap %lu (%p): flags %lx\n", i, (PVOID)Ids[i], Flags[i]);
    }
    if (Count != 0)
        ok(Ids[0] == (ULONG_PTR)ProcessHeap, "The first heap is %p, the process heap is %p\n", (PVOID)Ids[0], ProcessHeap);
}

/* Walks the blocks of one heap through Heap32First and Heap32Next. Returns FALSE when the system lists no blocks. */
static BOOL CheckHeapBlocks(DWORD Pid, ULONG_PTR HeapId, BOOL IsProcessHeap)
{
    SDK_HEAPENTRY32 Entry;
    DWORD Count = 0, Fixed = 0;
    BOOL Ret;

    /* Windows leaves dwFlags alone for a block that is neither allocated nor an uncommitted range */
    memset(&Entry, 0x55, sizeof(Entry));
    Entry.dwSize = sizeof(Entry);
    Entry.dwFlags = 0;
    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Entry, Pid, HeapId);
    if (!Ret && GetLastError() == ERROR_NO_MORE_FILES && IsReactOS())
        return FALSE;

    ok(Ret, "Heap32First(%p) failed with %lu\n", (PVOID)HeapId, GetLastError());
    if (Ret)
        ok(Entry.hHandle == (HANDLE)HeapId, "hHandle is %p, expected %p\n", Entry.hHandle, (PVOID)HeapId);
    while (Ret && Count < 100000)
    {
        ok_int(Entry.dwSize, sizeof(Entry));
        ok_int(Entry.th32ProcessID, Pid);
        ok(Entry.th32HeapID == HeapId, "th32HeapID is %p, expected %p\n", (PVOID)Entry.th32HeapID, (PVOID)HeapId);
        ok(Entry.dwAddress != 0, "Block %lu has no address\n", Count);
        ok(Entry.dwFlags == 0 || Entry.dwFlags == LF32_FIXED || Entry.dwFlags == LF32_FREE,
           "Block %lu has flags %lx\n", Count, Entry.dwFlags);
        if (Entry.dwFlags == LF32_FIXED)
            Fixed++;
        Count++;
        SetLastError(0xdeadbeef);
        Ret = Heap32Next((HEAPENTRY32 *)&Entry);
    }
    ok(Count != 0, "Heap %p has no blocks\n", (PVOID)HeapId);
    ok(!Ret, "Heap %p has more than %lu blocks\n", (PVOID)HeapId, Count);
    ok_err(ERROR_NO_MORE_FILES);
    if (IsProcessHeap)
        ok(Fixed != 0, "The process heap has no allocated block\n");
    return TRUE;
}

static void test_heap_list_own(void)
{
    HANDLE Heaps[MAX_HEAPS];
    ULONG_PTR Expected[MAX_HEAPS];
    SDK_HEAPLIST32 List;
    HANDLE Snapshot, Snapshot0, Private, Late;
    HANDLE ProcessHeap = GetProcessHeap();
    DWORD Pid = GetCurrentProcessId();
    DWORD Count, i;
    BOOL Ret;

    Private = HeapCreate(0, 0, 0);
    ok(Private != NULL, "HeapCreate failed with %lu\n", GetLastError());
    ok(HeapAlloc(Private, 0, 100) != NULL, "HeapAlloc failed\n");

    Count = HeapProcessHeaps(Heaps);
    for (i = 0; i < Count; i++)
        Expected[i] = (ULONG_PTR)Heaps[i];
    ok(Count >= 2, "Expected the process heap and a private heap, got %lu heaps\n", Count);
    ok(Heaps[0] == ProcessHeap, "The first heap is %p, the process heap is %p\n", Heaps[0], ProcessHeap);

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, Pid);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    Snapshot0 = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, 0);
    ok(Snapshot0 != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot(0) failed with %lu\n", GetLastError());

    /* The snapshot keeps the heaps of its time */
    Late = HeapCreate(0, 0, 0);

    if (Snapshot != INVALID_HANDLE_VALUE)
    {
        /* The list starts at its first entry even if nobody asked for it */
        memset(&List, 0x55, sizeof(List));
        List.dwSize = sizeof(List);
        Ret = Heap32ListNext(Snapshot, (HEAPLIST32 *)&List);
        ok(Ret, "Heap32ListNext failed with %lu\n", GetLastError());
        ok(List.th32HeapID == (ULONG_PTR)ProcessHeap, "got %p, expected %p\n", (PVOID)List.th32HeapID, ProcessHeap);

        CheckHeapListAgainst(Expected, Count, ProcessHeap, Pid, Snapshot);

        /* Heap32ListFirst starts over */
        memset(&List, 0x55, sizeof(List));
        List.dwSize = sizeof(List);
        Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
        ok(Ret, "Heap32ListFirst failed with %lu\n", GetLastError());
        ok(List.th32HeapID == (ULONG_PTR)ProcessHeap, "got %p, expected %p\n", (PVOID)List.th32HeapID, ProcessHeap);
        CloseHandle(Snapshot);
    }

    if (Snapshot0 != INVALID_HANDLE_VALUE)
    {
        /* Process 0 is the calling process */
        CheckHeapListAgainst(Expected, Count, ProcessHeap, Pid, Snapshot0);
        CloseHandle(Snapshot0);
    }

    /* Combined with other lists, the heap list is still the first */
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST | TH32CS_SNAPMODULE | TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, Pid);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    if (Snapshot != INVALID_HANDLE_VALUE)
    {
        ULONG_PTR Ids[MAX_HEAPS];
        DWORD Flags[MAX_HEAPS];

        ok(ReadHeapList(Snapshot, Pid, Ids, Flags) >= Count, "The combined snapshot lists fewer heaps\n");
        CloseHandle(Snapshot);
    }

    /* A snapshot of other lists has no heaps */
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    if (Snapshot != INVALID_HANDLE_VALUE)
    {
        memset(&List, 0x55, sizeof(List));
        List.dwSize = sizeof(List);
        SetLastError(0xdeadbeef);
        Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
        ok(!Ret, "Heap32ListFirst succeeded on a process snapshot\n");
        ok_err(ERROR_NO_MORE_FILES);
        CloseHandle(Snapshot);
    }

    HeapDestroy(Late);
    HeapDestroy(Private);
}

static void test_heap_errors(void)
{
    SDK_HEAPLIST32 List;
    SDK_HEAPENTRY32 Entry;
    HANDLE Snapshot;
    HANDLE ProcessHeap = GetProcessHeap();
    DWORD Pid = GetCurrentProcessId();
    BOOL Ret;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, Pid);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());

    /* The size of the structure must be exact */
    memset(&List, 0x55, sizeof(List));
    List.dwSize = 0;
    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
    ok(!Ret, "Heap32ListFirst succeeded with dwSize 0\n");
    ok_err(ERROR_BAD_LENGTH);

    List.dwSize = sizeof(List) - 1;
    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
    ok(!Ret, "Heap32ListFirst succeeded with a short dwSize\n");
    ok_err(ERROR_BAD_LENGTH);

    List.dwSize = sizeof(List) + 8;
    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
    ok(!Ret, "Heap32ListFirst succeeded with a long dwSize\n");
    ok_err(ERROR_BAD_LENGTH);

    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(Snapshot, NULL);
    ok(!Ret, "Heap32ListFirst succeeded with NULL\n");
    ok_err(ERROR_BAD_LENGTH);

    List.dwSize = sizeof(List) - 1;
    SetLastError(0xdeadbeef);
    Ret = Heap32ListNext(Snapshot, (HEAPLIST32 *)&List);
    ok(!Ret, "Heap32ListNext succeeded with a short dwSize\n");
    ok_err(ERROR_BAD_LENGTH);

    List.dwSize = sizeof(List);
    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst((HANDLE)(ULONG_PTR)0x12345678, (HEAPLIST32 *)&List);
    ok(!Ret, "Heap32ListFirst succeeded with a bad handle\n");
    ok_err(ERROR_INVALID_HANDLE);

    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(INVALID_HANDLE_VALUE, (HEAPLIST32 *)&List);
    ok(!Ret, "Heap32ListFirst succeeded with INVALID_HANDLE_VALUE\n");
    ok_err(ERROR_INVALID_HANDLE);

    memset(&Entry, 0x55, sizeof(Entry));
    Entry.dwSize = 0;
    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Entry, Pid, (ULONG_PTR)ProcessHeap);
    ok(!Ret, "Heap32First succeeded with dwSize 0\n");
    ok_err(ERROR_BAD_LENGTH);

    Entry.dwSize = sizeof(Entry) + 8;
    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Entry, Pid, (ULONG_PTR)ProcessHeap);
    ok(!Ret, "Heap32First succeeded with a long dwSize\n");
    ok_err(ERROR_BAD_LENGTH);

    SetLastError(0xdeadbeef);
    Ret = Heap32First(NULL, Pid, (ULONG_PTR)ProcessHeap);
    ok(!Ret, "Heap32First succeeded with NULL\n");
    ok_err(ERROR_BAD_LENGTH);

    Entry.dwSize = sizeof(Entry) - 1;
    SetLastError(0xdeadbeef);
    Ret = Heap32Next((HEAPENTRY32 *)&Entry);
    ok(!Ret, "Heap32Next succeeded with a short dwSize\n");
    ok_err(ERROR_BAD_LENGTH);

    /* A heap that does not exist fails and leaves the error alone */
    Entry.dwSize = sizeof(Entry);
    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Entry, Pid, 0);
    ok(!Ret, "Heap32First succeeded with heap 0\n");
    ok_err(0xdeadbeef);

    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Entry, Pid, (ULONG_PTR)ProcessHeap + 16);
    ok(!Ret, "Heap32First succeeded with an address inside the process heap\n");
    ok_err(0xdeadbeef);

    {
        HANDLE Gone = HeapCreate(0, 0, 0);

        HeapDestroy(Gone);
        SetLastError(0xdeadbeef);
        Ret = Heap32First((HEAPENTRY32 *)&Entry, Pid, (ULONG_PTR)Gone);
        ok(!Ret, "Heap32First succeeded with a destroyed heap\n");
        ok_err(0xdeadbeef);
    }

    /* Process 0 is not the calling process here. A thread identifier is never a process identifier. */
    Entry.dwSize = sizeof(Entry);
    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Entry, 0, (ULONG_PTR)ProcessHeap);
    ok(!Ret, "Heap32First succeeded with process 0\n");
    ok_err(ERROR_INVALID_HANDLE);

    memset(&Entry, 0, sizeof(Entry));
    Entry.dwSize = sizeof(Entry);
    SetLastError(0xdeadbeef);
    Ret = Heap32Next((HEAPENTRY32 *)&Entry);
    ok(!Ret, "Heap32Next succeeded with an empty entry\n");
    ok_err(ERROR_INVALID_HANDLE);

    Entry.dwSize = sizeof(Entry);
    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Entry, GetCurrentThreadId(), (ULONG_PTR)ProcessHeap);
    ok(!Ret, "Heap32First succeeded with a process that does not exist\n");
    ok_err(ERROR_INVALID_PARAMETER);

    if (Snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(Snapshot);

    SetLastError(0xdeadbeef);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, GetCurrentThreadId());
    ok(Snapshot == INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot returned %p for a process that does not exist\n", Snapshot);
    ok_err(ERROR_INVALID_PARAMETER);

    /* The system process is not open to the caller */
    SetLastError(0xdeadbeef);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, 4);
    ok(Snapshot == INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot returned %p for the system process\n", Snapshot);
    ok_err(ERROR_ACCESS_DENIED);
}

static void test_heap_blocks_own(void)
{
    HANDLE Heaps[MAX_HEAPS];
    HANDLE Snapshot, Private;
    HANDLE ProcessHeap = GetProcessHeap();
    DWORD Pid = GetCurrentProcessId();
    DWORD Count, i;
    BOOL Walked;
    void *Block;

    Private = HeapCreate(0, 0, 0);
    ok(Private != NULL, "HeapCreate failed with %lu\n", GetLastError());
    Block = HeapAlloc(Private, 0, 64);
    ok(Block != NULL, "HeapAlloc failed\n");

    Count = HeapProcessHeaps(Heaps);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, Pid);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());

    Walked = CheckHeapBlocks(Pid, (ULONG_PTR)ProcessHeap, TRUE);
    if (!Walked)
    {
        skip("The blocks of a heap are not listed\n");
    }
    else
    {
        for (i = 1; i < Count; i++)
            CheckHeapBlocks(Pid, (ULONG_PTR)Heaps[i], FALSE);
    }

    if (Snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(Snapshot);
    HeapFree(Private, 0, Block);
    HeapDestroy(Private);
}

/* The objects of a parent and its child carry the identifier of the parent, so that runs at the same time do not meet */
static void HeapChildObjectName(WCHAR *Name, const WCHAR *Kind, DWORD ParentPid)
{
    StringCchPrintfW(Name, 64, L"kernel32_apitest_Toolhelp32_%ls_%lu", Kind, ParentPid);
}

static void StartHeapChild(HANDLE *Process, DWORD *Pid, HANDLE *Ready, HANDLE *Done)
{
    WCHAR Command[MAX_PATH + 50];
    WCHAR Name[64];
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Info;
    BOOL Ret;

    HeapChildObjectName(Name, L"ready", GetCurrentProcessId());
    *Ready = CreateEventW(NULL, TRUE, FALSE, Name);
    HeapChildObjectName(Name, L"done", GetCurrentProcessId());
    *Done = CreateEventW(NULL, TRUE, FALSE, Name);
    *Process = NULL;
    if (!*Ready || !*Done)
        return;

    Command[0] = L'"';
    GetModuleFileNameW(NULL, Command + 1, MAX_PATH);
    StringCchPrintfW(Command + wcslen(Command), ARRAYSIZE(Command) - wcslen(Command), L"\" Toolhelp32 child %lu", GetCurrentProcessId());
    Ret = CreateProcessW(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Info);
    ok(Ret, "CreateProcess failed with %lu\n", GetLastError());
    if (Ret)
    {
        *Process = Info.hProcess;
        *Pid = Info.dwProcessId;
        CloseHandle(Info.hThread);
    }
}

static void test_heap_child(void)
{
    HEAP_REPORT *Report;
    HANDLE Mapping, Ready, Done, Process = NULL;
    HANDLE Snapshot;
    ULONG_PTR Heaps[MAX_HEAPS];
    WCHAR Name[64];
    DWORD Pid = 0, Wait, ExitCode, i;

    StartHeapChild(&Process, &Pid, &Ready, &Done);
    if (!Process)
    {
        skip("Could not start the child process\n");
        goto Cleanup;
    }

    Wait = WaitForSingleObject(Ready, 20000);
    ok_int(Wait, WAIT_OBJECT_0);
    if (Wait != WAIT_OBJECT_0)
        goto Terminate;

    HeapChildObjectName(Name, L"heaps", GetCurrentProcessId());
    Mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, Name);
    ok(Mapping != NULL, "OpenFileMapping failed with %lu\n", GetLastError());
    if (!Mapping)
        goto Terminate;
    Report = MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, sizeof(*Report));
    ok(Report != NULL, "MapViewOfFile failed with %lu\n", GetLastError());
    if (Report)
    {
        ok(Report->Count >= 2 && Report->Count <= MAX_HEAPS, "The child reports %lu heaps\n", Report->Count);
        for (i = 0; i < min(Report->Count, MAX_HEAPS); i++)
            Heaps[i] = Report->Heaps[i];

        Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, Pid);
        ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
        if (Snapshot != INVALID_HANDLE_VALUE)
        {
            /* The child reports its process heap first, whose address differs from ours */
            CheckHeapListAgainst(Heaps, min(Report->Count, MAX_HEAPS), (HANDLE)Heaps[0], Pid, Snapshot);
            CloseHandle(Snapshot);

            if (CheckHeapBlocks(Pid, Heaps[0], TRUE))
            {
                for (i = 1; i < min(Report->Count, MAX_HEAPS); i++)
                    CheckHeapBlocks(Pid, Heaps[i], FALSE);
            }
            else
            {
                skip("The blocks of a heap are not listed\n");
            }
        }
        UnmapViewOfFile(Report);
    }
    CloseHandle(Mapping);

Terminate:
    SetEvent(Done);
    Wait = WaitForSingleObject(Process, 20000);
    ok_int(Wait, WAIT_OBJECT_0);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(Process, 0);
    ExitCode = 0xdeadbeef;
    ok(GetExitCodeProcess(Process, &ExitCode), "GetExitCodeProcess failed with %lu\n", GetLastError());
    ok_int(ExitCode, 0);

    /* The process is gone. Its handle stays open so that nobody else gets its identifier meanwhile. */
    SetLastError(0xdeadbeef);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, Pid);
    ok(Snapshot == INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot returned %p for a process that has exited\n", Snapshot);
    if (Snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(Snapshot);
    CloseHandle(Process);

Cleanup:
    if (Ready)
        CloseHandle(Ready);
    if (Done)
        CloseHandle(Done);
}

static void heap_child_main(DWORD ParentPid)
{
    HEAP_REPORT *Report;
    HANDLE Heaps[MAX_HEAPS];
    HANDLE Mapping, Ready, Done, Private;
    WCHAR Name[64];
    DWORD Count, i;
    int n;

    HeapChildObjectName(Name, L"ready", ParentPid);
    Ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, Name);
    HeapChildObjectName(Name, L"done", ParentPid);
    Done = OpenEventW(SYNCHRONIZE, FALSE, Name);
    HeapChildObjectName(Name, L"heaps", ParentPid);
    Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*Report), Name);
    ok(Ready && Done && Mapping, "The child could not open its objects\n");
    if (!Ready || !Done || !Mapping)
        return;

    for (n = 0; n < 3; n++)
    {
        Private = HeapCreate(0, 0, 0);
        ok(Private != NULL, "HeapCreate failed with %lu\n", GetLastError());
        ok(HeapAlloc(Private, 0, 100 + n) != NULL, "HeapAlloc failed\n");
    }

    Report = MapViewOfFile(Mapping, FILE_MAP_WRITE, 0, 0, sizeof(*Report));
    ok(Report != NULL, "MapViewOfFile failed with %lu\n", GetLastError());
    if (Report)
    {
        Count = HeapProcessHeaps(Heaps);
        Report->Count = Count;
        for (i = 0; i < Count; i++)
            Report->Heaps[i] = (ULONG_PTR)Heaps[i];
        UnmapViewOfFile(Report);
    }

    SetEvent(Ready);
    WaitForSingleObject(Done, 60000);
    CloseHandle(Mapping);
}

static void test_heaps(void)
{
    test_heap_list_own();
    test_heap_errors();
    test_heap_blocks_own();
    test_heap_child();
}

START_TEST(Toolhelp32)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    if (argc >= 4 && !strcmp(argv[2], "child"))
    {
        heap_child_main(strtoul(argv[3], NULL, 10));
        return;
    }

    test_layouts();
    test_process_wide();
    test_process_ansi();
    test_heaps();
}
