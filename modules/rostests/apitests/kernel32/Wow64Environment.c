/* ReactOS WoW64 process queries and filesystem views. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"
#include <ndk/iofuncs.h>

static VOID CheckRelative(HANDLE Directory, CHAR Expected)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"wow64-view-test.dat");
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;
    HANDLE File;
    NTSTATUS Status;
    CHAR Value = 0;
    DWORD Bytes;
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, Directory, NULL);
    Status = NtOpenFile(&File, FILE_GENERIC_READ, &Attributes, &Io, FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    ok(Status == STATUS_SUCCESS, "root-relative open: %08lx\n", Status);
    if (!NT_SUCCESS(Status)) return;
    ok(ReadFile(File, &Value, 1, &Bytes, NULL), "relative read failed\n");
    ok(Bytes == 1 && Value == Expected, "relative got %c, expected %c\n", Value, Expected);
    CloseHandle(File);
}

static VOID CheckFile(PCWSTR Path, CHAR Expected)
{
    HANDLE File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHAR Value = 0;
    DWORD Bytes = 0;
    ok(File != INVALID_HANDLE_VALUE, "open %S: %lu\n", Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;
    ok(ReadFile(File, &Value, 1, &Bytes, NULL), "read %S: %lu\n", Path, GetLastError());
    ok(Bytes == 1 && Value == Expected, "%S: got %u/%c, expected %c\n", Path, Bytes, Value, Expected);
    CloseHandle(File);
}

static BOOL PutFile(PCWSTR Path, CHAR Value)
{
    HANDLE File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD Bytes = 0;
    BOOL Result;
    ok(File != INVALID_HANDLE_VALUE, "create %S: %lu\n", Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE) return FALSE;
    Result = WriteFile(File, &Value, 1, &Bytes, NULL);
    ok(Result && Bytes == 1, "write %S: %lu\n", Path, GetLastError());
    CloseHandle(File);
    return Result && Bytes == 1;
}

START_TEST(Wow64Environment)
{
    SYSTEM_INFO Info, Native;
    BOOL Wow64 = FALSE, Queried = FALSE;
    HANDLE Process, Directory;
    PVOID Old = (PVOID)0xdeadbeef, Nested;
    WCHAR System[MAX_PATH], Guest[MAX_PATH], Alias[MAX_PATH];

    ok(IsWow64Process(GetCurrentProcess(), &Wow64), "self query failed\n");
    Process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Process != NULL, "OpenProcess: %lu\n", GetLastError());
    if (Process)
    {
        ok(IsWow64Process(Process, &Queried), "real handle query: %lu\n", GetLastError());
        ok(Queried == Wow64, "pseudo/real handle disagreement\n");
        CloseHandle(Process);
    }
    Queried = 0x55;
    ok(!IsWow64Process(NULL, &Queried), "invalid handle accepted\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE && Queried == 0x55, "failed query changed result\n");
    GetSystemInfo(&Info);
    GetNativeSystemInfo(&Native);
    ok(Info.dwPageSize == Native.dwPageSize && Info.dwNumberOfProcessors == Native.dwNumberOfProcessors,
       "basic system information differs\n");
    if (!Wow64)
    {
        ok(!memcmp(&Info, &Native, sizeof(Info)), "native process system information differs\n");
        return;
    }
    ok(Info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL, "guest architecture %u\n", Info.wProcessorArchitecture);
    ok(Native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "native architecture %u\n", Native.wProcessorArchitecture);
    ok(Native.dwProcessorType == PROCESSOR_AMD_X8664, "native processor type %lu\n", Native.dwProcessorType);

    if (!Wow64DisableWow64FsRedirection(&Old))
    {
        ok(FALSE, "disable redirection: %lu\n", GetLastError());
        return;
    }
    ok(Old == NULL, "initial redirection token %p\n", Old);
    ok(Wow64DisableWow64FsRedirection(&Nested), "nested disable failed\n");
    ok(Nested != NULL, "nested token lost disabled state\n");
    ok(Wow64RevertWow64FsRedirection(Nested), "nested revert failed\n");
    GetWindowsDirectoryW(System, _countof(System));
    wcscpy(Guest, System);
    wcscpy(Alias, System);
    wcscat(System, L"\\System32");
    Directory = CreateFileW(System, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(Directory != INVALID_HANDLE_VALUE, "open native directory: %lu\n", GetLastError());
    wcscat(System, L"\\wow64-view-test.dat");
    wcscat(Guest, L"\\SysWOW64\\wow64-view-test.dat");
    wcscat(Alias, L"\\Sysnative\\wow64-view-test.dat");
    if (PutFile(System, '6') && PutFile(Guest, '3'))
    {
        CheckFile(System, '6');
        if (Directory != INVALID_HANDLE_VALUE) CheckRelative(Directory, '6');
        ok(Wow64RevertWow64FsRedirection(Old), "revert failed\n");
        CheckFile(System, '3');
        if (Directory != INVALID_HANDLE_VALUE) CheckRelative(Directory, '3');
        CheckFile(Guest, '3');
        CheckFile(Alias, '6');
        ok(!Wow64DisableWow64FsRedirection(NULL), "NULL token pointer accepted\n");
        CheckFile(System, '3'); /* A failed disable must preserve state. */
        ok(Wow64DisableWow64FsRedirection(&Old), "second disable failed\n");
        CheckFile(System, '6');
        ok(DeleteFileW(System), "native delete: %lu\n", GetLastError());
        ok(DeleteFileW(Guest), "guest delete: %lu\n", GetLastError());
    }
    ok(Wow64RevertWow64FsRedirection(Old), "final revert failed\n");
    if (Directory != INVALID_HANDLE_VALUE) CloseHandle(Directory);
}
