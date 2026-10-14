/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for process handles with PROCESS_QUERY_LIMITED_INFORMATION
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#ifndef PROCESS_NAME_NATIVE
#define PROCESS_NAME_NATIVE 1
#endif

#define PROCESS_SET_LIMITED 0x2000
#define VISTA_PROCESS_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF)

typedef BOOL (WINAPI *PFN_QueryFullProcessImageNameW)(HANDLE, DWORD, LPWSTR, PDWORD);
typedef DWORD (WINAPI *PFN_GetProcessId)(HANDLE);
typedef BOOL (WINAPI *PFN_GetProcessHandleCount)(HANDLE, PDWORD);

static PFN_QueryFullProcessImageNameW pQueryFullProcessImageNameW;
static PFN_GetProcessId pGetProcessId;
static PFN_GetProcessHandleCount pGetProcessHandleCount;

static
ACCESS_MASK
GetGrantedAccess(
    HANDLE Handle)
{
    OBJECT_BASIC_INFORMATION Info;
    NTSTATUS Status;

    Status = NtQueryObject(Handle, ObjectBasicInformation, &Info, sizeof(Info), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? Info.GrantedAccess : 0xdeadbeef;
}

static
void
Test_ProcessType(void)
{
    union
    {
        OBJECT_TYPE_INFORMATION Info;
        UCHAR Buffer[1024];
    } u;
    PGENERIC_MAPPING Mapping = &u.Info.GenericMapping;
    NTSTATUS Status;

    Status = NtQueryObject(NtCurrentProcess(), ObjectTypeInformation, &u, sizeof(u), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    ok_hex(Mapping->GenericRead,
           STANDARD_RIGHTS_READ | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ);
    ok_hex(Mapping->GenericWrite,
           STANDARD_RIGHTS_WRITE | PROCESS_CREATE_PROCESS | PROCESS_CREATE_THREAD |
           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_DUP_HANDLE |
           PROCESS_SET_QUOTA | PROCESS_SET_INFORMATION | PROCESS_SUSPEND_RESUME);
    ok_hex(Mapping->GenericExecute,
           STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE | PROCESS_TERMINATE |
           PROCESS_QUERY_LIMITED_INFORMATION);
    ok_hex(Mapping->GenericAll, VISTA_PROCESS_ALL_ACCESS);
    ok_hex(u.Info.ValidAccessMask, VISTA_PROCESS_ALL_ACCESS);
}

static
void
Test_GrantedAccess(
    DWORD ProcessId)
{
    /* Windows 8 and later also imply PROCESS_SET_LIMITED_INFORMATION (Win8Granted) */
    BOOL Win8OrLater = GetNTVersion() >= _WIN32_WINNT_WIN8;
    static const struct
    {
        ACCESS_MASK Desired;
        ACCESS_MASK Granted;
        ACCESS_MASK Win8Granted;
    } Tests[] =
    {
        { PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION, 0 },
        { PROCESS_QUERY_INFORMATION,
          PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, 0 },
        { PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
          PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, 0 },
        { PROCESS_SET_INFORMATION,
          PROCESS_SET_INFORMATION, PROCESS_SET_INFORMATION | PROCESS_SET_LIMITED },
        { GENERIC_READ,
          STANDARD_RIGHTS_READ | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
          PROCESS_QUERY_LIMITED_INFORMATION, 0 },
        { GENERIC_EXECUTE,
          STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE | PROCESS_TERMINATE |
          PROCESS_QUERY_LIMITED_INFORMATION, 0 },
        { MAXIMUM_ALLOWED, VISTA_PROCESS_ALL_ACCESS, 0 },
        { STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFF,
          STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x1FFF,
          STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x3FFF },
    };
    HANDLE Process;
    ACCESS_MASK Granted;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        Process = OpenProcess(Tests[i].Desired, FALSE, ProcessId);
        ok(Process != NULL, "[%lu] OpenProcess(0x%lx) failed, error %lu\n",
           i, Tests[i].Desired, GetLastError());
        if (!Process) continue;
        Granted = GetGrantedAccess(Process);
        ok(Granted == Tests[i].Granted ||
           (Win8OrLater && Tests[i].Win8Granted && Granted == Tests[i].Win8Granted),
           "[%lu] granted 0x%lx, expected 0x%lx\n", i, Granted, Tests[i].Granted);
        CloseHandle(Process);
    }

    if (ProcessId != GetCurrentProcessId()) return;

    /* The same rules hold for duplicated handles */
    ok(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &Process,
                       PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0),
       "DuplicateHandle failed, error %lu\n", GetLastError());
    ok_hex(GetGrantedAccess(Process), PROCESS_QUERY_LIMITED_INFORMATION);
    CloseHandle(Process);
    ok(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &Process,
                       PROCESS_QUERY_INFORMATION, FALSE, 0),
       "DuplicateHandle failed, error %lu\n", GetLastError());
    ok_hex(GetGrantedAccess(Process),
           PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION);
    CloseHandle(Process);
}

static
void
Test_InfoClasses(
    HANDLE Process,
    const char *Name)
{
    /* Windows 8 and later also allow ProcessBreakOnTermination */
    BOOL Win8OrLater = GetNTVersion() >= _WIN32_WINNT_WIN8;
    static const struct
    {
        PROCESSINFOCLASS Class;
        ULONG Length;
        NTSTATUS Status;
    } Tests[] =
    {
        { ProcessBasicInformation, sizeof(PROCESS_BASIC_INFORMATION), STATUS_SUCCESS },
        { ProcessQuotaLimits, sizeof(QUOTA_LIMITS), STATUS_SUCCESS },
        { ProcessIoCounters, sizeof(IO_COUNTERS), STATUS_SUCCESS },
        { ProcessVmCounters, sizeof(VM_COUNTERS), STATUS_SUCCESS },
        { ProcessTimes, sizeof(KERNEL_USER_TIMES), STATUS_SUCCESS },
        { ProcessDefaultHardErrorMode, sizeof(ULONG), STATUS_SUCCESS },
        { ProcessPriorityClass, sizeof(PROCESS_PRIORITY_CLASS), STATUS_SUCCESS },
        { ProcessHandleCount, sizeof(ULONG), STATUS_SUCCESS },
        { ProcessPriorityBoost, sizeof(ULONG), STATUS_SUCCESS },
        { ProcessSessionInformation, sizeof(PROCESS_SESSION_INFORMATION), STATUS_SUCCESS },
        { ProcessWow64Information, sizeof(ULONG_PTR), STATUS_SUCCESS },
        { ProcessDebugPort, sizeof(HANDLE), STATUS_ACCESS_DENIED },
        { ProcessBreakOnTermination, sizeof(ULONG), STATUS_ACCESS_DENIED },
        { ProcessDebugObjectHandle, sizeof(HANDLE), STATUS_ACCESS_DENIED },
        { ProcessDebugFlags, sizeof(ULONG), STATUS_ACCESS_DENIED },
    };
    union
    {
        UNICODE_STRING Name;
        UCHAR Buffer[1024];
    } u;
    ULONG i, ReturnLength;
    NTSTATUS Status;
    int Chars;

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        RtlZeroMemory(&u, sizeof(u));
        Status = NtQueryInformationProcess(Process, Tests[i].Class, &u, Tests[i].Length,
                                           &ReturnLength);
        ok(Status == Tests[i].Status ||
           (Win8OrLater && Tests[i].Class == ProcessBreakOnTermination && Status == STATUS_SUCCESS),
           "%s: class %d returned 0x%lx, expected 0x%lx\n",
           Name, Tests[i].Class, Status, Tests[i].Status);
    }

    Status = NtQueryInformationProcess(Process, ProcessImageFileName, &u, sizeof(u),
                                       &ReturnLength);
    ok(Status == STATUS_SUCCESS, "%s: ProcessImageFileName returned 0x%lx\n", Name, Status);
    if (NT_SUCCESS(Status))
    {
        Chars = u.Name.Length / sizeof(WCHAR);
        ok(Chars > 0 && u.Name.Buffer && u.Name.Buffer[0] == L'\\', "%s: native name %.*ls\n",
           Name, Chars, u.Name.Buffer);
    }
    /* Builds for NT 5.2 do not have this class */
    Status = NtQueryInformationProcess(NtCurrentProcess(), ProcessImageFileNameWin32, &u,
                                       sizeof(u), &ReturnLength);
    if (Status == STATUS_INVALID_INFO_CLASS)
    {
        skip("ProcessImageFileNameWin32 is not supported\n");
        return;
    }
    Status = NtQueryInformationProcess(Process, ProcessImageFileNameWin32, &u, sizeof(u),
                                       &ReturnLength);
    ok(Status == STATUS_SUCCESS, "%s: ProcessImageFileNameWin32 returned 0x%lx\n", Name, Status);
    if (NT_SUCCESS(Status))
    {
        /* A drive path or a UNC path */
        Chars = u.Name.Length / sizeof(WCHAR);
        ok(Chars > 2 && u.Name.Buffer &&
           (u.Name.Buffer[1] == L':' || (u.Name.Buffer[0] == L'\\' && u.Name.Buffer[1] == L'\\')),
           "%s: Win32 name %.*ls\n", Name, Chars, u.Name.Buffer);
    }
}

static
void
Test_Win32(
    HANDLE Process,
    DWORD ProcessId,
    DWORD ExpectedExitCode,
    const char *Name)
{
    /* Native names are longer than the drive letter form */
    WCHAR Path[1024], Expected[MAX_PATH];
    FILETIME Times[4];
    DWORD Size, Code, Count;
    BOOL Wow64, Ret;

    if (pQueryFullProcessImageNameW && ExpectedExitCode == STILL_ACTIVE)
    {
        Size = RTL_NUMBER_OF(Path);
        Ret = pQueryFullProcessImageNameW(Process, 0, Path, &Size);
        ok(Ret, "%s: QueryFullProcessImageNameW failed, error %lu\n", Name, GetLastError());
        GetModuleFileNameW(NULL, Expected, RTL_NUMBER_OF(Expected));
        if (Ret) ok(!_wcsicmp(Path, Expected), "%s: got %ls, expected %ls\n", Name, Path, Expected);
        Size = RTL_NUMBER_OF(Path);
        Ret = pQueryFullProcessImageNameW(Process, PROCESS_NAME_NATIVE, Path, &Size);
        ok(Ret, "%s: QueryFullProcessImageNameW(PROCESS_NAME_NATIVE) failed, error %lu\n",
           Name, GetLastError());
        if (Ret) ok(Path[0] == L'\\', "%s: native name %ls\n", Name, Path);
    }
    ok(GetProcessTimes(Process, &Times[0], &Times[1], &Times[2], &Times[3]),
       "%s: GetProcessTimes failed, error %lu\n", Name, GetLastError());
    Code = 0xdeadbeef;
    ok(GetExitCodeProcess(Process, &Code), "%s: GetExitCodeProcess failed, error %lu\n",
       Name, GetLastError());
    ok(Code == ExpectedExitCode, "%s: exit code %lu, expected %lu\n", Name, Code, ExpectedExitCode);
    if (pGetProcessId)
    {
        Code = pGetProcessId(Process);
        ok(Code == ProcessId, "%s: GetProcessId returned %lu, error %lu\n",
           Name, Code, GetLastError());
    }
    ok(GetPriorityClass(Process) != 0, "%s: GetPriorityClass failed, error %lu\n",
       Name, GetLastError());
    ok(IsWow64Process(Process, &Wow64), "%s: IsWow64Process failed, error %lu\n",
       Name, GetLastError());
    if (pGetProcessHandleCount)
    {
        ok(pGetProcessHandleCount(Process, &Count), "%s: GetProcessHandleCount failed, error %lu\n",
           Name, GetLastError());
    }

    /* The limited right is not enough for a wait */
    SetLastError(0xdeadbeef);
    ok(WaitForSingleObject(Process, 0) == WAIT_FAILED, "%s: wait succeeded\n", Name);
    ok_err(ERROR_ACCESS_DENIED);
}

START_TEST(NtOpenProcess)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    WCHAR Command[MAX_PATH];
    HANDLE Process, Limited;
    PROCESS_BASIC_INFORMATION BasicInfo;
    NTSTATUS Status;

    pQueryFullProcessImageNameW = (PFN_QueryFullProcessImageNameW)
        GetProcAddress(Kernel32, "QueryFullProcessImageNameW");
    pGetProcessId = (PFN_GetProcessId)GetProcAddress(Kernel32, "GetProcessId");
    pGetProcessHandleCount = (PFN_GetProcessHandleCount)
        GetProcAddress(Kernel32, "GetProcessHandleCount");

    Test_ProcessType();
    Test_GrantedAccess(GetCurrentProcessId());

    /* The current process pseudo handle keeps every class */
    Status = NtQueryInformationProcess(NtCurrentProcess(), ProcessBasicInformation,
                                       &BasicInfo, sizeof(BasicInfo), NULL);
    ok_hex(Status, STATUS_SUCCESS);

    Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Process != NULL, "OpenProcess failed, error %lu\n", GetLastError());
    if (Process)
    {
        Test_InfoClasses(Process, "self");
        Test_Win32(Process, GetCurrentProcessId(), STILL_ACTIVE, "self");
        CloseHandle(Process);
    }

    /* Another process of the same user: a suspended copy of this program */
    GetModuleFileNameW(NULL, Command, RTL_NUMBER_OF(Command));
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(Command, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                        &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcessW failed, error %lu\n", GetLastError());
        return;
    }
    Test_GrantedAccess(ProcessInfo.dwProcessId);
    Limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessInfo.dwProcessId);
    ok(Limited != NULL, "OpenProcess failed, error %lu\n", GetLastError());
    if (Limited)
    {
        ok_hex(GetGrantedAccess(Limited), PROCESS_QUERY_LIMITED_INFORMATION);
        Test_InfoClasses(Limited, "child");
        Test_Win32(Limited, ProcessInfo.dwProcessId, STILL_ACTIVE, "child");
    }

    /* The exit code of a finished process through the limited handle */
    ok(TerminateProcess(ProcessInfo.hProcess, 1234), "TerminateProcess failed, error %lu\n",
       GetLastError());
    ok(WaitForSingleObject(ProcessInfo.hProcess, 10000) == WAIT_OBJECT_0, "child did not exit\n");
    if (Limited)
    {
        Test_Win32(Limited, ProcessInfo.dwProcessId, 1234, "exited child");
        CloseHandle(Limited);
    }
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}
