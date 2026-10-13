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

static void test_heaps(void)
{
    SDK_HEAPLIST32 List;
    SDK_HEAPENTRY32 Heap;
    HANDLE Snapshot;
    HANDLE ProcessHeap = GetProcessHeap();
    DWORD Pid = GetCurrentProcessId();
    BOOL Found = FALSE;
    BOOL HaveHeaps;
    BOOL Ret;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, Pid);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    if (Snapshot == INVALID_HANDLE_VALUE)
        return;

    memset(&List, 0x55, sizeof(List));
    List.dwSize = 0;
    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
    ok(!Ret, "Heap32ListFirst succeeded with dwSize 0\n");

    /* ReactOS has no heap enumeration, the list is then empty and ends with ERROR_NO_MORE_FILES */
    memset(&List, 0x55, sizeof(List));
    List.dwSize = sizeof(List);
    SetLastError(0xdeadbeef);
    Ret = Heap32ListFirst(Snapshot, (HEAPLIST32 *)&List);
    ok(Ret || GetLastError() == ERROR_NO_MORE_FILES, "Heap32ListFirst failed with %lu\n", GetLastError());
    HaveHeaps = Ret;
    while (Ret)
    {
        ok_int(List.th32ProcessID, Pid);
        if (List.th32HeapID == (ULONG_PTR)ProcessHeap)
            Found = TRUE;
        Ret = Heap32ListNext(Snapshot, (HEAPLIST32 *)&List);
    }
    if (HaveHeaps)
        ok(Found, "The process heap %p is not in the heap list\n", ProcessHeap);

    memset(&Heap, 0x55, sizeof(Heap));
    Heap.dwSize = sizeof(Heap);
    SetLastError(0xdeadbeef);
    Ret = Heap32First((HEAPENTRY32 *)&Heap, Pid, (ULONG_PTR)ProcessHeap);
    ok(Ret || GetLastError() == ERROR_NO_MORE_FILES, "Heap32First failed with %lu\n", GetLastError());
    if (Ret)
    {
        ok_int(Heap.th32ProcessID, Pid);
        ok(Heap.th32HeapID == (ULONG_PTR)ProcessHeap, "got %p, expected %p\n", (PVOID)Heap.th32HeapID, ProcessHeap);
        ok(Heap.dwBlockSize != 0, "The first block has size 0\n");
        ok(Heap.dwAddress != 0, "The first block has no address\n");
    }

    CloseHandle(Snapshot);
}

START_TEST(Toolhelp32)
{
    test_layouts();
    test_process_wide();
    test_process_ansi();
    test_heaps();
}
