/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test the granted access of the handles that process and thread creators return
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define NT4_PROCESS_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFF)
#define NT6_PROCESS_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF)
#define NT4_THREAD_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x3FF)
#define NT6_THREAD_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0xFFFF)

/* The NT4 mask plus the implied limited rights */
#define NT4_PROCESS_OPENED (NT4_PROCESS_ALL_ACCESS | PROCESS_QUERY_LIMITED_INFORMATION)
#define NT4_PROCESS_OPENED_WIN8 (NT4_PROCESS_OPENED | 0x2000)
#define NT4_THREAD_OPENED (NT4_THREAD_ALL_ACCESS | 0xC00)
/* Windows 8 and later also imply THREAD_RESUME */
#define NT4_THREAD_OPENED_WIN8 (NT4_THREAD_OPENED | 0x1000)

#define TEST_PS_ATTRIBUTE_IMAGE_NAME 0x00020005
#define TEST_THREAD_CREATE_SUSPENDED 0x00000001
#define TEST_RTL_USER_PROC_PARAMS_NORMALIZED 0x00000001
#define TEST_PS_CREATE_SUCCESS 6

/* The NtCreateUserProcess create-info block, in its initial-state form */
typedef struct _TEST_PS_CREATE_INFO
{
    SIZE_T Size;
    ULONG State;
    union
    {
        struct
        {
            ULONG InitFlags;
            ACCESS_MASK AdditionalFileAccess;
        } InitState;
        struct
        {
            ULONG OutputFlags;
            HANDLE FileHandle;
            HANDLE SectionHandle;
            ULONGLONG UserProcessParametersNative;
            ULONG UserProcessParametersWow64;
            ULONG CurrentParameterFlags;
            ULONGLONG PebAddressNative;
            ULONG PebAddressWow64;
            ULONGLONG ManifestAddress;
            ULONG ManifestSize;
        } SuccessState;
    };
} TEST_PS_CREATE_INFO;
#ifdef _WIN64
C_ASSERT(sizeof(TEST_PS_CREATE_INFO) == 88);
#else
C_ASSERT(sizeof(TEST_PS_CREATE_INFO) == 72);
#endif

typedef NTSTATUS (NTAPI *PFN_NtCreateUserProcess)(PHANDLE, PHANDLE, ACCESS_MASK, ACCESS_MASK,
                                                  POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES,
                                                  ULONG, ULONG, PRTL_USER_PROCESS_PARAMETERS,
                                                  TEST_PS_CREATE_INFO *, PROS_PS_ATTRIBUTE_LIST);
typedef NTSTATUS (NTAPI *PFN_NtCreateThreadEx)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE,
                                               PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T,
                                               PVOID);
typedef NTSTATUS (NTAPI *PFN_RtlCreateProcessParametersEx)(PRTL_USER_PROCESS_PARAMETERS *,
                                                           PUNICODE_STRING, PUNICODE_STRING,
                                                           PUNICODE_STRING, PUNICODE_STRING,
                                                           PVOID, PUNICODE_STRING,
                                                           PUNICODE_STRING, PUNICODE_STRING,
                                                           PUNICODE_STRING, ULONG);
/* The classic signature, which every NT version up to Windows 10 exports */
typedef NTSTATUS (NTAPI *PFN_RtlCreateUserThread)(HANDLE, PSECURITY_DESCRIPTOR, BOOLEAN, ULONG,
                                                  SIZE_T, SIZE_T, PTHREAD_START_ROUTINE, PVOID,
                                                  PHANDLE, PCLIENT_ID);

static BOOL Win8OrLater;

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

/* Win8 is the Windows 8 and later value where it differs, else 0 */
static
void
CheckAccess(
    HANDLE Handle,
    ACCESS_MASK Expected,
    ACCESS_MASK Win8,
    const char *Name)
{
    ACCESS_MASK Granted = GetGrantedAccess(Handle);

    trace("%s: granted 0x%lx\n", Name, Granted);
    ok(Granted == Expected || (Win8OrLater && Win8 && Granted == Win8),
       "%s: granted 0x%lx, expected 0x%lx\n", Name, Granted, Expected);
}

static
void
CheckDuplicate(
    HANDLE Source,
    ACCESS_MASK Desired,
    DWORD Options,
    ACCESS_MASK Expected,
    ACCESS_MASK Win8,
    const char *Name)
{
    HANDLE Handle;

    if (!DuplicateHandle(GetCurrentProcess(), Source, GetCurrentProcess(), &Handle, Desired,
                         FALSE, Options))
    {
        ok(0, "%s: DuplicateHandle failed, error %lu\n", Name, GetLastError());
        return;
    }
    CheckAccess(Handle, Expected, Win8, Name);
    CloseHandle(Handle);
}

static
DWORD
WINAPI
ThreadProc(
    PVOID Parameter)
{
    return 0;
}

static
void
EndThread(
    HANDLE Thread)
{
    TerminateThread(Thread, 0);
    WaitForSingleObject(Thread, 10000);
    CloseHandle(Thread);
}

static
void
Test_CreateProcess(
    PCWSTR Path)
{
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION Info;
    HANDLE Handle, Thread;

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(Path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                        &StartupInfo, &Info))
    {
        ok(0, "CreateProcessW failed, error %lu\n", GetLastError());
        return;
    }

    /* The creator's handles */
    CheckAccess(Info.hProcess, NT6_PROCESS_ALL_ACCESS, 0, "CreateProcess process");
    CheckAccess(Info.hThread, NT6_THREAD_ALL_ACCESS, 0, "CreateProcess thread");
    CheckDuplicate(Info.hProcess, 0, DUPLICATE_SAME_ACCESS, NT6_PROCESS_ALL_ACCESS, 0,
                   "CreateProcess process, same access");
    CheckDuplicate(Info.hThread, 0, DUPLICATE_SAME_ACCESS, NT6_THREAD_ALL_ACCESS, 0,
                   "CreateProcess thread, same access");
    CheckDuplicate(Info.hProcess, NT4_PROCESS_ALL_ACCESS, 0, NT4_PROCESS_OPENED,
                   NT4_PROCESS_OPENED_WIN8, "CreateProcess process, NT4 access");
    CheckDuplicate(Info.hProcess, NT6_PROCESS_ALL_ACCESS, 0, NT6_PROCESS_ALL_ACCESS, 0,
                   "CreateProcess process, NT6 access");
    CheckDuplicate(Info.hThread, NT4_THREAD_ALL_ACCESS, 0, NT4_THREAD_OPENED,
                   NT4_THREAD_OPENED_WIN8, "CreateProcess thread, NT4 access");
    CheckDuplicate(Info.hThread, NT6_THREAD_ALL_ACCESS, 0, NT6_THREAD_ALL_ACCESS, 0,
                   "CreateProcess thread, NT6 access");

    /* Opening the same objects with either all-access value */
    Handle = OpenProcess(NT4_PROCESS_ALL_ACCESS, FALSE, Info.dwProcessId);
    ok(Handle != NULL, "OpenProcess(NT4) failed, error %lu\n", GetLastError());
    if (Handle)
    {
        CheckAccess(Handle, NT4_PROCESS_OPENED, NT4_PROCESS_OPENED_WIN8, "OpenProcess NT4");
        CheckDuplicate(Handle, 0, DUPLICATE_SAME_ACCESS, NT4_PROCESS_OPENED,
                       NT4_PROCESS_OPENED_WIN8, "OpenProcess NT4, same access");
        CloseHandle(Handle);
    }
    Handle = OpenProcess(NT6_PROCESS_ALL_ACCESS, FALSE, Info.dwProcessId);
    ok(Handle != NULL, "OpenProcess(NT6) failed, error %lu\n", GetLastError());
    if (Handle)
    {
        CheckAccess(Handle, NT6_PROCESS_ALL_ACCESS, 0, "OpenProcess NT6");
        CloseHandle(Handle);
    }
    Thread = OpenThread(NT4_THREAD_ALL_ACCESS, FALSE, Info.dwThreadId);
    ok(Thread != NULL, "OpenThread(NT4) failed, error %lu\n", GetLastError());
    if (Thread)
    {
        CheckAccess(Thread, NT4_THREAD_OPENED, NT4_THREAD_OPENED_WIN8, "OpenThread NT4");
        CheckDuplicate(Thread, 0, DUPLICATE_SAME_ACCESS, NT4_THREAD_OPENED,
                       NT4_THREAD_OPENED_WIN8, "OpenThread NT4, same access");
        CloseHandle(Thread);
    }
    Thread = OpenThread(NT6_THREAD_ALL_ACCESS, FALSE, Info.dwThreadId);
    ok(Thread != NULL, "OpenThread(NT6) failed, error %lu\n", GetLastError());
    if (Thread)
    {
        CheckAccess(Thread, NT6_THREAD_ALL_ACCESS, 0, "OpenThread NT6");
        CloseHandle(Thread);
    }

    /* A thread created in the suspended child */
    Thread = CreateRemoteThread(Info.hProcess, NULL, 0, ThreadProc, NULL, CREATE_SUSPENDED, NULL);
    ok(Thread != NULL, "CreateRemoteThread(child) failed, error %lu\n", GetLastError());
    if (Thread)
    {
        CheckAccess(Thread, NT6_THREAD_ALL_ACCESS, 0, "CreateRemoteThread child");
        CloseHandle(Thread);
    }

    TerminateProcess(Info.hProcess, 0);
    WaitForSingleObject(Info.hProcess, 10000);
    CloseHandle(Info.hThread);
    CloseHandle(Info.hProcess);
}

static
void
Test_CreateThread(void)
{
    HANDLE Thread;

    Thread = CreateThread(NULL, 0, ThreadProc, NULL, CREATE_SUSPENDED, NULL);
    ok(Thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (Thread)
    {
        CheckAccess(Thread, NT6_THREAD_ALL_ACCESS, 0, "CreateThread");
        CheckDuplicate(Thread, 0, DUPLICATE_SAME_ACCESS, NT6_THREAD_ALL_ACCESS, 0,
                       "CreateThread, same access");
        EndThread(Thread);
    }

    Thread = CreateRemoteThread(GetCurrentProcess(), NULL, 0, ThreadProc, NULL,
                                CREATE_SUSPENDED, NULL);
    ok(Thread != NULL, "CreateRemoteThread failed, error %lu\n", GetLastError());
    if (Thread)
    {
        CheckAccess(Thread, NT6_THREAD_ALL_ACCESS, 0, "CreateRemoteThread self");
        EndThread(Thread);
    }

    /* The pseudo handles stand for full access */
    CheckDuplicate(GetCurrentProcess(), 0, DUPLICATE_SAME_ACCESS, NT6_PROCESS_ALL_ACCESS, 0,
                   "current process, same access");
    CheckDuplicate(GetCurrentThread(), 0, DUPLICATE_SAME_ACCESS, NT6_THREAD_ALL_ACCESS, 0,
                   "current thread, same access");
}

static
void
Test_NtCreateProcessEx(
    PCWSTR Path)
{
    static const struct
    {
        ACCESS_MASK Desired;
        ACCESS_MASK Granted;
        ACCESS_MASK Win8Granted;
        const char *Name;
    } Tests[] =
    {
        { NT4_PROCESS_ALL_ACCESS, NT4_PROCESS_OPENED, NT4_PROCESS_OPENED_WIN8,
          "NtCreateProcessEx NT4" },
        { NT6_PROCESS_ALL_ACCESS, NT6_PROCESS_ALL_ACCESS, 0, "NtCreateProcessEx NT6" },
        { MAXIMUM_ALLOWED, NT6_PROCESS_ALL_ACCESS, 0, "NtCreateProcessEx MAXIMUM_ALLOWED" },
    };
    HANDLE File, Section, Process;
    NTSTATUS Status;
    ULONG i;

    File = CreateFileW(Path, GENERIC_READ | GENERIC_EXECUTE,
                       FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;
    Status = NtCreateSection(&Section, SECTION_ALL_ACCESS, NULL, NULL, PAGE_EXECUTE,
                             SEC_IMAGE, File);
    ok_hex(Status, STATUS_SUCCESS);
    CloseHandle(File);
    if (!NT_SUCCESS(Status)) return;

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        Status = NtCreateProcessEx(&Process, Tests[i].Desired, NULL, NtCurrentProcess(), 0,
                                   Section, NULL, NULL, FALSE);
        /* Windows 10 reports this success code to WOW64 callers */
        ok(Status == STATUS_SUCCESS || Status == STATUS_IMAGE_MACHINE_TYPE_MISMATCH,
           "%s: status 0x%lx\n", Tests[i].Name, Status);
        if (!NT_SUCCESS(Status)) continue;
        CheckAccess(Process, Tests[i].Granted, Tests[i].Win8Granted, Tests[i].Name);
        NtTerminateProcess(Process, 0);
        NtClose(Process);
    }
    NtClose(Section);
}

static
void
Test_RtlCreators(
    PCWSTR Path)
{
    PFN_RtlCreateUserThread pRtlCreateUserThread;
    RTL_USER_PROCESS_INFORMATION Info;
    PRTL_USER_PROCESS_PARAMETERS Params;
    UNICODE_STRING NtPath;
    HANDLE Thread;
    NTSTATUS Status;

    pRtlCreateUserThread = (PFN_RtlCreateUserThread)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlCreateUserThread");
    if (pRtlCreateUserThread)
    {
        Status = pRtlCreateUserThread(NtCurrentProcess(), NULL, TRUE, 0, 0, 0,
                                      (PTHREAD_START_ROUTINE)ThreadProc, NULL, &Thread, NULL);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            CheckAccess(Thread, NT6_THREAD_ALL_ACCESS, 0, "RtlCreateUserThread");
            EndThread(Thread);
        }
    }

    if (!RtlDosPathNameToNtPathName_U(Path, &NtPath, NULL, NULL))
    {
        ok(0, "RtlDosPathNameToNtPathName_U failed\n");
        return;
    }
    Status = RtlCreateProcessParameters(&Params, &NtPath, NULL, NULL, NULL, NULL, NULL, NULL,
                                        NULL, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(&Info, sizeof(Info));
        Status = RtlCreateUserProcess(&NtPath, OBJ_CASE_INSENSITIVE, Params, NULL, NULL, NULL,
                                      FALSE, NULL, NULL, &Info);
        ok(NT_SUCCESS(Status), "RtlCreateUserProcess returned 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
        {
            CheckAccess(Info.ProcessHandle, NT6_PROCESS_ALL_ACCESS, 0,
                        "RtlCreateUserProcess process");
            CheckAccess(Info.ThreadHandle, NT6_THREAD_ALL_ACCESS, 0,
                        "RtlCreateUserProcess thread");
            NtTerminateProcess(Info.ProcessHandle, 0);
            NtClose(Info.ThreadHandle);
            NtClose(Info.ProcessHandle);
        }
        RtlDestroyProcessParameters(Params);
    }
    RtlFreeUnicodeString(&NtPath);
}

/* Windows only: the ReactOS exports are stubs */
static
void
Test_NtCreateUserProcess(
    PCWSTR Path)
{
    static const struct
    {
        ACCESS_MASK ProcessDesired;
        ACCESS_MASK ThreadDesired;
        ACCESS_MASK ProcessGranted;
        ACCESS_MASK ProcessWin8Granted;
        ACCESS_MASK ThreadGranted;
        ACCESS_MASK ThreadWin8Granted;
        const char *Name;
    } Tests[] =
    {
        { NT4_PROCESS_ALL_ACCESS, NT4_THREAD_ALL_ACCESS,
          NT4_PROCESS_OPENED, NT4_PROCESS_OPENED_WIN8, NT4_THREAD_OPENED,
          NT4_THREAD_OPENED_WIN8, "NtCreateUserProcess NT4" },
        { NT6_PROCESS_ALL_ACCESS, NT6_THREAD_ALL_ACCESS,
          NT6_PROCESS_ALL_ACCESS, 0, NT6_THREAD_ALL_ACCESS, 0, "NtCreateUserProcess NT6" },
        { MAXIMUM_ALLOWED, MAXIMUM_ALLOWED,
          NT6_PROCESS_ALL_ACCESS, 0, NT6_THREAD_ALL_ACCESS, 0,
          "NtCreateUserProcess MAXIMUM_ALLOWED" },
    };
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NtCreateUserProcess pNtCreateUserProcess;
    PFN_NtCreateThreadEx pNtCreateThreadEx;
    PFN_RtlCreateProcessParametersEx pRtlCreateProcessParametersEx;
    PRTL_USER_PROCESS_PARAMETERS Params;
    TEST_PS_CREATE_INFO CreateInfo;
    ROS_PS_ATTRIBUTE_LIST Attributes;
    UNICODE_STRING NtPath;
    HANDLE Process, Thread;
    char Name[96];
    NTSTATUS Status;
    ULONG i;

    pNtCreateThreadEx = (PFN_NtCreateThreadEx)GetProcAddress(Ntdll, "NtCreateThreadEx");
    pNtCreateUserProcess = (PFN_NtCreateUserProcess)GetProcAddress(Ntdll, "NtCreateUserProcess");
    pRtlCreateProcessParametersEx = (PFN_RtlCreateProcessParametersEx)
        GetProcAddress(Ntdll, "RtlCreateProcessParametersEx");
    if (is_reactos() || !pNtCreateThreadEx || !pNtCreateUserProcess ||
        !pRtlCreateProcessParametersEx)
    {
        skip("No NtCreateUserProcess and NtCreateThreadEx\n");
        return;
    }

    Status = pNtCreateThreadEx(&Thread, NT4_THREAD_ALL_ACCESS, NULL, NtCurrentProcess(),
                               ThreadProc, NULL, TEST_THREAD_CREATE_SUSPENDED, 0, 0, 0, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        CheckAccess(Thread, NT4_THREAD_OPENED, NT4_THREAD_OPENED_WIN8, "NtCreateThreadEx NT4");
        EndThread(Thread);
    }
    Status = pNtCreateThreadEx(&Thread, NT6_THREAD_ALL_ACCESS, NULL, NtCurrentProcess(),
                               ThreadProc, NULL, TEST_THREAD_CREATE_SUSPENDED, 0, 0, 0, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        CheckAccess(Thread, NT6_THREAD_ALL_ACCESS, 0, "NtCreateThreadEx NT6");
        EndThread(Thread);
    }

    if (!RtlDosPathNameToNtPathName_U(Path, &NtPath, NULL, NULL))
    {
        ok(0, "RtlDosPathNameToNtPathName_U failed\n");
        return;
    }
    Status = pRtlCreateProcessParametersEx(&Params, &NtPath, NULL, NULL, NULL, NULL, NULL, NULL,
                                           NULL, NULL, TEST_RTL_USER_PROC_PARAMS_NORMALIZED);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeUnicodeString(&NtPath);
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        RtlZeroMemory(&CreateInfo, sizeof(CreateInfo));
        CreateInfo.Size = sizeof(CreateInfo);
        RtlZeroMemory(&Attributes, sizeof(Attributes));
        Attributes.TotalLength = sizeof(Attributes);
        Attributes.Attributes[0].Attribute = TEST_PS_ATTRIBUTE_IMAGE_NAME;
        Attributes.Attributes[0].Size = NtPath.Length;
        Attributes.Attributes[0].ValuePtr = NtPath.Buffer;
        Status = pNtCreateUserProcess(&Process, &Thread, Tests[i].ProcessDesired,
                                      Tests[i].ThreadDesired, NULL, NULL, 0,
                                      TEST_THREAD_CREATE_SUSPENDED, Params, &CreateInfo,
                                      &Attributes);
        ok(NT_SUCCESS(Status), "%s: status 0x%lx\n", Tests[i].Name, Status);
        if (!NT_SUCCESS(Status)) continue;
        StringCbPrintfA(Name, sizeof(Name), "%s process", Tests[i].Name);
        CheckAccess(Process, Tests[i].ProcessGranted, Tests[i].ProcessWin8Granted, Name);
        StringCbPrintfA(Name, sizeof(Name), "%s thread", Tests[i].Name);
        CheckAccess(Thread, Tests[i].ThreadGranted, Tests[i].ThreadWin8Granted, Name);
        NtTerminateProcess(Process, 0);
        if (CreateInfo.State == TEST_PS_CREATE_SUCCESS)
        {
            if (CreateInfo.SuccessState.FileHandle) NtClose(CreateInfo.SuccessState.FileHandle);
            if (CreateInfo.SuccessState.SectionHandle)
                NtClose(CreateInfo.SuccessState.SectionHandle);
        }
        NtClose(Thread);
        NtClose(Process);
    }
    RtlDestroyProcessParameters(Params);
    RtlFreeUnicodeString(&NtPath);
}

START_TEST(CreatorAccess)
{
    WCHAR Path[MAX_PATH];

    Win8OrLater = GetNTVersion() >= _WIN32_WINNT_WIN8;
    GetModuleFileNameW(NULL, Path, RTL_NUMBER_OF(Path));

    Test_CreateProcess(Path);
    Test_CreateThread();
    Test_NtCreateProcessEx(Path);
    Test_RtlCreators(Path);
    Test_NtCreateUserProcess(Path);
}
