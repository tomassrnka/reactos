/* NtQueryVirtualMemory(MemorySectionName) from a 32-bit process: the name of the file mapped
 * at an address (GetMappedFileName). Expected values measured on Windows Server 2008 R2 and
 * Windows 10 (x64, WoW64). SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"

static const UCHAR Fill = 0x5a;

/* The name ends with Suffix, case-insensitively */
static BOOLEAN EndsWith(PCUNICODE_STRING Name, PCWSTR Suffix)
{
    SIZE_T Length = wcslen(Suffix) * sizeof(WCHAR);
    return Name->Length >= Length &&
           !_wcsnicmp((PWCHAR)((PUCHAR)Name->Buffer + Name->Length - Length), Suffix, Length / sizeof(WCHAR));
}

/* A successful query of Address into Buffer + Offset (Offset unaligns the buffer); returns the
 * required length, 0 on failure */
static ULONG QueryName(PVOID Address, PCWSTR Suffix, ULONG Offset)
{
    UCHAR Buffer[2048];
    PUNICODE_STRING Name = (PUNICODE_STRING)(Buffer + Offset);
    SIZE_T Needed = 0xdeadbeef;
    NTSTATUS Status;

    memset(Buffer, Fill, sizeof(Buffer));
    Status = NtQueryVirtualMemory(NtCurrentProcess(), Address, MemorySectionName, Name, sizeof(Buffer) - Offset, &Needed);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return 0;
    ok(Name->Buffer == (PWSTR)(Buffer + Offset + sizeof(UNICODE_STRING)), "Buffer %p, expected %p\n",
       Name->Buffer, Buffer + Offset + sizeof(UNICODE_STRING));
    ok(Name->Length > 0 && !(Name->Length & 1), "Length %u\n", Name->Length);
    ok(Name->MaximumLength == Name->Length + sizeof(WCHAR), "MaximumLength %u, Length %u\n",
       Name->MaximumLength, Name->Length);
    ok(Needed == sizeof(UNICODE_STRING) + Name->Length + sizeof(WCHAR), "Return length %lu, Length %u\n",
       (ULONG)Needed, Name->Length);
    if (Name->Buffer != (PWSTR)(Buffer + Offset + sizeof(UNICODE_STRING)) || Name->Length > 1024) return 0;
    ok(Name->Buffer[Name->Length / sizeof(WCHAR)] == UNICODE_NULL, "Not terminated\n");
    ok(Buffer[Offset + Needed] == Fill, "Wrote past the name\n");
    ok(Name->Length > 8 && !_wcsnicmp(Name->Buffer, L"\\Device\\", 8), "Name %wZ\n", Name);
    ok(EndsWith(Name, Suffix), "Name %wZ does not end with %S\n", Name, Suffix);
    return (ULONG)Needed;
}

static void TestLengths(PVOID Address, ULONG Needed)
{
    UCHAR Buffer[2048];
    SIZE_T Bytes;
    NTSTATUS Status;
    ULONG Lengths[] = {0, 4, sizeof(UNICODE_STRING) - 1, sizeof(UNICODE_STRING), sizeof(UNICODE_STRING) + 2, Needed - 2, Needed - 1};
    ULONG i, j;

    for (i = 0; i < ARRAYSIZE(Lengths); ++i)
    {
        memset(Buffer, Fill, sizeof(Buffer));
        Bytes = 0xdeadbeef;
        Status = NtQueryVirtualMemory(NtCurrentProcess(), Address, MemorySectionName, Buffer, Lengths[i], &Bytes);
        ok(Status == (Lengths[i] < sizeof(UNICODE_STRING) ? STATUS_INFO_LENGTH_MISMATCH : STATUS_BUFFER_OVERFLOW),
           "Length %lu: status 0x%lx\n", Lengths[i], Status);
        ok(Bytes == Needed, "Length %lu: return length %lu, expected %lu\n", Lengths[i], (ULONG)Bytes, Needed);
        for (j = 0; j < sizeof(Buffer) && Buffer[j] == Fill; ++j);
        ok(j == sizeof(Buffer), "Length %lu: buffer written at %lu\n", Lengths[i], j);
    }
    /* Exactly the required length; no return length */
    Status = NtQueryVirtualMemory(NtCurrentProcess(), Address, MemorySectionName, Buffer, Needed, NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    /* The return length is stored after the name, also when it lies inside the buffer */
    Status = NtQueryVirtualMemory(NtCurrentProcess(), Address, MemorySectionName, Buffer, sizeof(Buffer),
                                  (PSIZE_T)(Buffer + sizeof(UNICODE_STRING)));
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(*(PSIZE_T)(Buffer + sizeof(UNICODE_STRING)) == Needed, "Aliased return length %lu, expected %lu\n",
       (ULONG)*(PSIZE_T)(Buffer + sizeof(UNICODE_STRING)), Needed);
}

/* Another process: the name must be that process's mapping or an error, never the caller's own
 * mapping at that address (DataView exists only in this process) */
static void TestOtherProcess(PVOID Image, PVOID DataView)
{
    WCHAR Path[MAX_PATH], *Exe;
    UCHAR Buffer[1024];
    PUNICODE_STRING Name = (PUNICODE_STRING)Buffer;
    char Application[MAX_PATH], Command[] = "child-never-resumed";
    STARTUPINFOA Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Child;
    HANDLE Self;
    SIZE_T Bytes;
    NTSTATUS Status;

    /* A real handle to this process works like the pseudo handle */
    Self = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
    ok(Self != NULL, "OpenProcess failed: %lu\n", GetLastError());
    if (Self)
    {
        Status = NtQueryVirtualMemory(Self, DataView, MemorySectionName, Buffer, sizeof(Buffer), &Bytes);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ok(EndsWith(Name, L"\\w64gqvm.tmp"), "Own process by handle: %wZ\n", Name);
        CloseHandle(Self);
    }

    GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    Exe = wcsrchr(Path, L'\\');
    if (!Exe) Exe = Path;
    GetModuleFileNameA(NULL, Application, ARRAYSIZE(Application));
    if (!CreateProcessA(Application, Command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &Startup, &Child))
    {
        ok(0, "CreateProcessA failed: %lu\n", GetLastError());
        return;
    }
    memset(Buffer, 0x5a, sizeof(Buffer));
    Status = NtQueryVirtualMemory(Child.hProcess, DataView, MemorySectionName, Buffer, sizeof(Buffer), &Bytes);
    ok(!NT_SUCCESS(Status), "Other process at this process's data view: 0x%lx\n", Status);
    /* ReactOS does not translate requests on another process (its kernel would answer from
     * the caller's address space) */
    if (is_reactos()) ok_ntstatus(Status, STATUS_NOT_SUPPORTED);
    if (NT_SUCCESS(Status))
        ok(!EndsWith(Name, L"\\w64gqvm.tmp"), "Got this process's mapping %wZ\n", Name);
    /* Its executable image: the right name (Windows) or an error (ReactOS) */
    memset(Buffer, 0x5a, sizeof(Buffer));
    Status = NtQueryVirtualMemory(Child.hProcess, Image, MemorySectionName, Buffer, sizeof(Buffer), &Bytes);
    if (is_reactos())
        ok_ntstatus(Status, STATUS_NOT_SUPPORTED);
    else
    {
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status)) ok(EndsWith(Name, Exe), "Other process image %wZ\n", Name);
    }
    TerminateProcess(Child.hProcess, 0);
    CloseHandle(Child.hThread);
    CloseHandle(Child.hProcess);
}

START_TEST(Wow64QueryVirtualMemory)
{
    SYSTEM_BASIC_INFORMATION Basic;
    WCHAR Path[MAX_PATH], *Exe;
    UCHAR Buffer[1024];
    HANDLE File, Map;
    PVOID View, Heap;
    PUCHAR Image = (PUCHAR)GetModuleHandleW(NULL);
    SIZE_T Bytes;
    ULONG Needed;
    NTSTATUS Status;
    DWORD Written;
    BOOL Wow64 = FALSE;

    /* The length semantics are those of the WoW64 translation: the ReactOS kernel
     * truncates the name for a native caller instead */
    IsWow64Process(GetCurrentProcess(), &Wow64);
    if (!Wow64)
    {
        skip("Not a 32-bit process under WoW64\n");
        return;
    }
    Status = NtQuerySystemInformation(SystemBasicInformation, &Basic, sizeof(Basic), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* The executable image, at its base and inside it, also into an unaligned buffer */
    GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    Exe = wcsrchr(Path, L'\\');
    ok(Exe != NULL, "No path in %S\n", Path);
    if (!Exe) return;
    Needed = QueryName(Image, Exe, 0);
    ok(QueryName(Image + 0x1000, Exe, 0) == Needed, "Inside the image\n");
    ok(QueryName(Image, Exe, 1) == Needed, "Unaligned buffer\n");
    ok(QueryName(Image, Exe, 2) == Needed, "Unaligned buffer\n");
    if (Needed) TestLengths(Image, Needed);

    /* A system DLL of the process's own architecture */
    QueryName(GetModuleHandleW(L"kernel32.dll"), L"\\SysWOW64\\kernel32.dll", 0);
    QueryName(GetModuleHandleW(L"ntdll.dll"), L"\\SysWOW64\\ntdll.dll", 0);

    /* A data file view */
    GetTempPathW(ARRAYSIZE(Path), Path);
    StringCbCatW(Path, sizeof(Path), L"w64gqvm.tmp");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        memset(Buffer, 1, sizeof(Buffer));
        WriteFile(File, Buffer, sizeof(Buffer), &Written, NULL);
        Map = CreateFileMappingW(File, NULL, PAGE_READONLY, 0, 0, NULL);
        ok(Map != NULL, "CreateFileMappingW failed: %lu\n", GetLastError());
        View = Map ? MapViewOfFile(Map, FILE_MAP_READ, 0, 0, 0) : NULL;
        ok(View != NULL, "MapViewOfFile failed: %lu\n", GetLastError());
        if (View)
        {
            Needed = QueryName(View, L"\\w64gqvm.tmp", 0);
            if (Needed) TestLengths(View, Needed);
            TestOtherProcess(Image, View);
            UnmapViewOfFile(View);
        }
        if (Map) CloseHandle(Map);
        CloseHandle(File);
    }

    /* No mapped file: free memory, private memory, a pagefile-backed view. Windows returns
     * STATUS_INVALID_ADDRESS for private memory and STATUS_FILE_INVALID for the pagefile view;
     * the ReactOS kernel returns STATUS_SECTION_NOT_IMAGE for both, for 64-bit callers too. */
    Bytes = 0xdeadbeef;
    Status = NtQueryVirtualMemory(NtCurrentProcess(), NULL, MemorySectionName, Buffer, sizeof(Buffer), &Bytes);
    ok_ntstatus(Status, STATUS_INVALID_ADDRESS);
    Heap = HeapAlloc(GetProcessHeap(), 0, 64);
    Status = NtQueryVirtualMemory(NtCurrentProcess(), Heap, MemorySectionName, Buffer, sizeof(Buffer), &Bytes);
    ok(Status == STATUS_INVALID_ADDRESS || Status == STATUS_SECTION_NOT_IMAGE, "Private memory: 0x%lx\n", Status);
    HeapFree(GetProcessHeap(), 0, Heap);
    Map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL);
    View = Map ? MapViewOfFile(Map, FILE_MAP_READ, 0, 0, 0) : NULL;
    ok(View != NULL, "Pagefile view failed: %lu\n", GetLastError());
    Status = NtQueryVirtualMemory(NtCurrentProcess(), View, MemorySectionName, Buffer, sizeof(Buffer), &Bytes);
    ok(Status == STATUS_FILE_INVALID || Status == STATUS_SECTION_NOT_IMAGE, "Pagefile view: 0x%lx\n", Status);
    if (View) UnmapViewOfFile(View);
    if (Map) CloseHandle(Map);

    /* Beyond the process's 2 GiB user range */
    if (Basic.MaximumUserModeAddress < 0x80000000)
    {
        Status = NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)0x80000000, MemorySectionName,
                                      Buffer, sizeof(Buffer), &Bytes);
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    }
}
