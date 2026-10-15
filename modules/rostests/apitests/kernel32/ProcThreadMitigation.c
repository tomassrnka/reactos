/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY in an attribute list
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Expectations were measured on Windows Server 2008 R2 (6.1.7601) and Windows 10 22H2:
 * a list entry is a DWORD on Windows 7 and may also be a DWORD64 or two DWORD64 values
 * from Windows 8 on; any other size is ERROR_BAD_LENGTH. Update accepts any value, and
 * CreateProcess refuses a DWORD with a bit that is not a defined mitigation.
 */

#include "precomp.h"

#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif
#ifndef PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY
#define PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY 0x00020007
#endif

typedef BOOL (WINAPI *INITIALIZE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *UPDATE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR,
                                 PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *DELETE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST);

static INITIALIZE_LIST pInitialize;
static UPDATE_LIST pUpdate;
static DELETE_LIST pDelete;
static WCHAR Image[MAX_PATH];
static BOOL IsReactOS, IsWin8;

/* Returns the error of the failing step, or ERROR_SUCCESS when the child ran and exited with 0 */
static DWORD RunWithPolicy(const void *Value, SIZE_T Size, BOOL *UpdateFailed)
{
    SIZE_T ListSize = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST List;
    STARTUPINFOEXW Startup;
    PROCESS_INFORMATION Info;
    WCHAR CommandLine[MAX_PATH + 64];
    DWORD Error, ExitCode = 0xdead;
    BOOL Ret;

    pInitialize(NULL, 1, 0, &ListSize);
    List = HeapAlloc(GetProcessHeap(), 0, ListSize);
    if (!List) return ERROR_OUTOFMEMORY;
    Ret = pInitialize(List, 1, 0, &ListSize);
    ok(Ret, "InitializeProcThreadAttributeList: %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pUpdate(List, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, (PVOID)Value, Size, NULL, NULL);
    *UpdateFailed = !Ret;
    if (!Ret)
    {
        Error = GetLastError();
        pDelete(List);
        HeapFree(GetProcessHeap(), 0, List);
        return Error;
    }

    ZeroMemory(&Startup, sizeof(Startup));
    ZeroMemory(&Info, sizeof(Info));
    Startup.StartupInfo.cb = sizeof(Startup);
    Startup.lpAttributeList = List;
    StringCchPrintfW(CommandLine, _countof(CommandLine), L"\"%ls\" ProcThreadMitigation child", Image);
    SetLastError(0xdeadbeef);
    Ret = CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                         NULL, NULL, &Startup.StartupInfo, &Info);
    Error = Ret ? ERROR_SUCCESS : GetLastError();
    if (Ret)
    {
        ok(WaitForSingleObject(Info.hProcess, 60000) == WAIT_OBJECT_0, "the child did not exit\n");
        GetExitCodeProcess(Info.hProcess, &ExitCode);
        ok(ExitCode == 0, "child exit code %lu\n", ExitCode);
        CloseHandle(Info.hThread);
        CloseHandle(Info.hProcess);
    }
    pDelete(List);
    HeapFree(GetProcessHeap(), 0, List);
    return Error;
}

static VOID ExpectDword(DWORD Value, DWORD ExpectedError)
{
    BOOL UpdateFailed;
    DWORD Error = RunWithPolicy(&Value, sizeof(Value), &UpdateFailed);

    ok(!UpdateFailed, "0x%lx: Update failed with %lu\n", Value, Error);
    ok(Error == ExpectedError, "0x%lx: CreateProcess error %lu, expected %lu\n", Value, Error, ExpectedError);
}

static VOID ExpectSize(SIZE_T Size, DWORD ExpectedError)
{
    ULONGLONG Value[2] = {0, 0};
    BOOL UpdateFailed;
    DWORD Error = RunWithPolicy(Value, Size, &UpdateFailed);

    ok(Error == ExpectedError, "size %lu: error %lu, expected %lu\n", (ULONG)Size, Error, ExpectedError);
    ok(UpdateFailed == (ExpectedError != ERROR_SUCCESS),
       "size %lu: Update failed %d for expected error %lu\n", (ULONG)Size, UpdateFailed, ExpectedError);
}

START_TEST(ProcThreadMitigation)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    OSVERSIONINFOW Version = {sizeof(Version)};
    char **Argv;
    int Argc = winetest_get_mainargs(&Argv);
    DWORD EightByteError;

    if (Argc == 3 && !strcmp(Argv[2], "child")) ExitProcess(0);
    pInitialize = (INITIALIZE_LIST)GetProcAddress(Kernel32, "InitializeProcThreadAttributeList");
    pUpdate = (UPDATE_LIST)GetProcAddress(Kernel32, "UpdateProcThreadAttribute");
    pDelete = (DELETE_LIST)GetProcAddress(Kernel32, "DeleteProcThreadAttributeList");
    if (!pInitialize || !pUpdate || !pDelete)
    {
        skip("Process attributes unavailable\n");
        return;
    }
    GetVersionExW(&Version);
    IsReactOS = is_reactos();
    IsWin8 = Version.dwMajorVersion > 6 || (Version.dwMajorVersion == 6 && Version.dwMinorVersion >= 2);
    GetModuleFileNameW(NULL, Image, _countof(Image));

    /* Defined DWORD mitigations: DEP, DEP without ATL thunk emulation, SEHOP */
    ExpectDword(0, ERROR_SUCCESS);
    ExpectDword(1, ERROR_SUCCESS);
    ExpectDword(2, ERROR_SUCCESS);
    ExpectDword(3, ERROR_SUCCESS);
    ExpectDword(4, ERROR_SUCCESS);
    ExpectDword(7, ERROR_SUCCESS);
    /* Update takes any value; CreateProcess refuses bits that no mitigation uses */
    ExpectDword(0x8, ERROR_INVALID_PARAMETER);
    ExpectDword(0x10, ERROR_INVALID_PARAMETER);
    ExpectDword(0x40101067, ERROR_INVALID_PARAMETER);

    /* The sizes of the Windows 8 and 10 forms exist on ReactOS for programs that choose them */
    EightByteError = (IsReactOS || IsWin8) ? ERROR_SUCCESS : ERROR_BAD_LENGTH;
    ExpectSize(sizeof(DWORD), ERROR_SUCCESS);
    ExpectSize(sizeof(DWORD64), EightByteError);
    ExpectSize(2 * sizeof(DWORD64), EightByteError);
    ExpectSize(3, ERROR_BAD_LENGTH);
    ExpectSize(12, ERROR_BAD_LENGTH);
}
