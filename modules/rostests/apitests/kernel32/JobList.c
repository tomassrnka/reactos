/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     PROC_THREAD_ATTRIBUTE_JOB_LIST: the child is a member of the job from creation;
 *              the Chromium sandbox's attribute list (handle list, job list, child process policy)
 *              and the primary token assigned to the suspended target (ProcessAccessToken)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Chromium's sandbox passes the target job with this attribute and frees the
 * target's resources when the job completion port reports the process gone, so a
 * child that never joins the job leaks them for as long as the browser runs.
 * The test also runs as a 32-bit process under WoW64 (kernel32_wow64_apitest),
 * where a refused attribute list kept Firefox 32-bit from starting any sandboxed
 * content process.
 */

#include "precomp.h"

#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif
#ifndef PROCESS_CREATION_CHILD_PROCESS_RESTRICTED
#define PROCESS_CREATION_CHILD_PROCESS_RESTRICTED 0x01
#endif
#ifndef JOB_OBJECT_MSG_NEW_PROCESS
#define JOB_OBJECT_MSG_NEW_PROCESS 6
#define JOB_OBJECT_MSG_EXIT_PROCESS 7
#endif

typedef BOOL (WINAPI *INITIALIZE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *UPDATE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR,
                                   PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *DELETE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST);
typedef BOOL (WINAPI *CREATE_AS_USER)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                      BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

#define KEY ((ULONG_PTR)0x4a4c)
#define CHILD_EXIT 37

static INITIALIZE_LIST pInitialize;
static UPDATE_LIST pUpdate;
static DELETE_LIST pDelete;
static CREATE_AS_USER pCreateAsUser;

/* Before the target runs, the broker finds its image through the PEB of
 * ProcessBasicInformation and reads the target's ntdll at the address of its
 * own ntdll, to install its system-call interceptions */
static VOID
CheckSuspendedTarget(HANDLE Process)
{
    static const char *Names[] = { "NtOpenFile", "NtCreateFile", "NtMapViewOfSection" };
    NTSTATUS (NTAPI *pQuery)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PROCESS_BASIC_INFORMATION Info;
    UCHAR Local[16], Remote[16];
    PVOID Image = NULL;
    SIZE_T Read = 0;
    char Magic[2] = {0};
    NTSTATUS Status;
    ULONG i;

    pQuery = (PVOID)GetProcAddress(Ntdll, "NtQueryInformationProcess");
    Status = pQuery(Process, ProcessBasicInformation, &Info, sizeof(Info), NULL);
    ok(Status == STATUS_SUCCESS, "sandbox: ProcessBasicInformation: 0x%lx\n", Status);
    if (Status != STATUS_SUCCESS) return;
    ok(ReadProcessMemory(Process, (PUCHAR)Info.PebBaseAddress + FIELD_OFFSET(PEB, ImageBaseAddress), &Image,
                         sizeof(Image), &Read) && Read == sizeof(Image),
       "sandbox: PEB read: %lu\n", GetLastError());
    ok(Image == (PVOID)GetModuleHandleW(NULL), "sandbox: image base %p, expected %p\n", Image, GetModuleHandleW(NULL));
    ok(ReadProcessMemory(Process, Image, Magic, sizeof(Magic), &Read) && Magic[0] == 'M' && Magic[1] == 'Z',
       "sandbox: no image at %p: %lu\n", Image, GetLastError());
    for (i = 0; i < _countof(Names); i++)
    {
        PUCHAR Code = (PUCHAR)GetProcAddress(Ntdll, Names[i]);
        memcpy(Local, Code, sizeof(Local));
        ok(ReadProcessMemory(Process, Code, Remote, sizeof(Remote), &Read) && Read == sizeof(Remote),
           "sandbox: %s is not mapped in the target: %lu\n", Names[i], GetLastError());
        ok(!memcmp(Local, Remote, sizeof(Local)), "sandbox: %s differs in the target\n", Names[i]);
    }
}

static DWORD
PrivilegeCount(HANDLE Process)
{
    TOKEN_STATISTICS Stats;
    HANDLE Token;
    DWORD Size = 0;

    if (!OpenProcessToken(Process, TOKEN_QUERY, &Token))
        return MAXDWORD;
    if (!GetTokenInformation(Token, TokenStatistics, &Stats, sizeof(Stats), &Size))
        Stats.PrivilegeCount = MAXDWORD;
    CloseHandle(Token);
    return Stats.PrivilegeCount;
}

static BOOL
Launch(HANDLE *Jobs, DWORD Count, DWORD Flags, PROCESS_INFORMATION *Pi, DWORD *Error)
{
    STARTUPINFOEXW Si;
    WCHAR Cmd[MAX_PATH + 32];
    SIZE_T Size = 0;
    BOOL Ret = FALSE;

    ZeroMemory(&Si, sizeof(Si));
    Si.StartupInfo.cb = sizeof(Si);
    GetModuleFileNameW(NULL, Cmd, MAX_PATH);
    StringCchCatW(Cmd, _countof(Cmd), L" JobList child");
    pInitialize(NULL, 1, 0, &Size);
    Si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Si.lpAttributeList) return FALSE;
    if (pInitialize(Si.lpAttributeList, 1, 0, &Size) &&
        pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, Jobs,
                Count * sizeof(HANDLE), NULL, NULL))
    {
        Ret = CreateProcessW(NULL, Cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | Flags,
                             NULL, NULL, &Si.StartupInfo, Pi);
        *Error = GetLastError();
        pDelete(Si.lpAttributeList);
    }
    HeapFree(GetProcessHeap(), 0, Si.lpAttributeList);
    return Ret;
}

static VOID
CheckMessages(HANDLE Port, DWORD Pid, PCSTR Name)
{
    DWORD Bytes;
    ULONG_PTR Key;
    LPOVERLAPPED Ovl;
    BOOL SawNew = FALSE, SawExit = FALSE, SawZero = FALSE;
    ULONG i;

    for (i = 0; i < 8 && !(SawNew && SawExit && SawZero); ++i)
    {
        if (!GetQueuedCompletionStatus(Port, &Bytes, &Key, &Ovl, 10000))
            break;
        ok(Key == KEY, "%s: key %p\n", Name, (PVOID)Key);
        if (Bytes == JOB_OBJECT_MSG_NEW_PROCESS)
        {
            ok((DWORD)(ULONG_PTR)Ovl == Pid, "%s: new process %lu, expected %lu\n", Name, (DWORD)(ULONG_PTR)Ovl, Pid);
            SawNew = TRUE;
        }
        else if (Bytes == JOB_OBJECT_MSG_EXIT_PROCESS)
        {
            ok((DWORD)(ULONG_PTR)Ovl == Pid, "%s: exit process %lu, expected %lu\n", Name, (DWORD)(ULONG_PTR)Ovl, Pid);
            SawExit = TRUE;
        }
        else if (Bytes == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO)
            SawZero = TRUE;
    }
    ok(SawNew, "%s: no JOB_OBJECT_MSG_NEW_PROCESS\n", Name);
    ok(SawExit, "%s: no JOB_OBJECT_MSG_EXIT_PROCESS\n", Name);
    ok(SawZero, "%s: no JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO\n", Name);
}

/* What sandbox::TargetProcess::Create does on Windows 10: CreateProcessAsUser
 * with a restricted token, suspended, with the handles the target inherits,
 * its job, and no further child processes */
static VOID
TestSandboxList(VOID)
{
    SECURITY_ATTRIBUTES Sa = {sizeof(Sa), NULL, TRUE};
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT Assoc;
    STARTUPINFOEXW Si;
    PROCESS_INFORMATION Pi;
    WCHAR Cmd[MAX_PATH + 64];
    HANDLE Job, Port, Event, Inherit[1], Token = NULL, Restricted = NULL;
    DWORD Policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED, Exit = 0, Error;
    SIZE_T Size = 0;
    BOOL InJob = FALSE, Ret;

    Job = CreateJobObjectW(NULL, NULL);
    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    Event = CreateEventW(&Sa, TRUE, FALSE, NULL);
    ok(Job && Port && Event, "sandbox: Create: %lu\n", GetLastError());
    if (!Job || !Port || !Event) return;
    /* A child token of the caller's: no privilege is needed to assign it */
    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &Token) &&
       CreateRestrictedToken(Token, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &Restricted),
       "sandbox: restricted token: %lu\n", GetLastError());
    Assoc.CompletionKey = (PVOID)KEY;
    Assoc.CompletionPort = Port;
    ok(SetInformationJobObject(Job, JobObjectAssociateCompletionPortInformation, &Assoc, sizeof(Assoc)),
       "sandbox: associate: %lu\n", GetLastError());

    ZeroMemory(&Si, sizeof(Si));
    Si.StartupInfo.cb = sizeof(Si);
    GetModuleFileNameW(NULL, Cmd, MAX_PATH);
    StringCchPrintfW(Cmd + wcslen(Cmd), _countof(Cmd) - wcslen(Cmd), L" JobList child %lu",
                     (DWORD)(ULONG_PTR)Event);
    pInitialize(NULL, 3, 0, &Size);
    Si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, Size);
    ok(Si.lpAttributeList && pInitialize(Si.lpAttributeList, 3, 0, &Size), "sandbox: initialize: %lu\n", GetLastError());
    Inherit[0] = Event;
    ok(pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Inherit, sizeof(Inherit), NULL, NULL),
       "sandbox: handle list: %lu\n", GetLastError());
    ok(pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &Job, sizeof(Job), NULL, NULL),
       "sandbox: job list: %lu\n", GetLastError());
    if (!pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &Policy, sizeof(Policy),
                 NULL, NULL))
    {
        /* Windows 10 version 1511 and later */
        skip("child process policy: %lu\n", GetLastError());
    }
    else
    {
        Ret = pCreateAsUser(Restricted, NULL, Cmd, NULL, NULL, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, NULL, NULL, &Si.StartupInfo, &Pi);
        Error = GetLastError();
        ok(Ret, "sandbox: CreateProcessAsUser failed %lu\n", Error);
        if (Ret)
        {
            ok(IsProcessInJob(Pi.hProcess, Job, &InJob) && InJob, "sandbox: child is not in the job\n");
            CheckSuspendedTarget(Pi.hProcess);
            ok(PrivilegeCount(Pi.hProcess) < PrivilegeCount(GetCurrentProcess()),
               "sandbox: child has %lu privileges, the caller %lu\n", PrivilegeCount(Pi.hProcess),
               PrivilegeCount(GetCurrentProcess()));
            ResumeThread(Pi.hThread);
            ok(WaitForSingleObject(Pi.hProcess, 20000) == WAIT_OBJECT_0, "sandbox: child did not exit\n");
            ok(GetExitCodeProcess(Pi.hProcess, &Exit) && Exit == CHILD_EXIT, "sandbox: exit code %lu\n", Exit);
            /* The child signals the event it inherited through the handle list */
            ok(WaitForSingleObject(Event, 0) == WAIT_OBJECT_0, "sandbox: inherited event not signalled\n");
            CheckMessages(Port, Pi.dwProcessId, "sandbox");
            CloseHandle(Pi.hThread);
            CloseHandle(Pi.hProcess);
        }
    }
    pDelete(Si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, Si.lpAttributeList);
    if (Restricted) CloseHandle(Restricted);
    if (Token) CloseHandle(Token);
    CloseHandle(Event);
    CloseHandle(Job);
    CloseHandle(Port);
}

/* CreateProcessAsUser of the guest advapi32 (and of Windows before Vista) creates
 * the child suspended and then assigns its primary token with
 * NtSetInformationProcess(ProcessAccessToken). A 32-bit sandbox broker under WoW64
 * makes that call for every target, here with the job list and child process
 * policy of TestSandboxList */
static VOID
TestAccessToken(VOID)
{
    NTSTATUS (NTAPI *pSet)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG);
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT Assoc;
    PROCESS_ACCESS_TOKEN Access;
    STARTUPINFOEXW Si;
    PROCESS_INFORMATION Pi;
    WCHAR Cmd[MAX_PATH + 32];
    HANDLE Job, Port, Token = NULL, Restricted = NULL;
    DWORD Policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED, Exit = 0, Before, After;
    SIZE_T Size = 0;
    NTSTATUS Status;
    BOOL InJob = FALSE, Ret;

    pSet = (PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess");
    Job = CreateJobObjectW(NULL, NULL);
    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    ok(pSet && Job && Port, "token: Create: %lu\n", GetLastError());
    if (!pSet || !Job || !Port) return;
    /* A child token of the caller's: no privilege is needed to assign it */
    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &Token) &&
       CreateRestrictedToken(Token, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &Restricted),
       "token: restricted token: %lu\n", GetLastError());
    Assoc.CompletionKey = (PVOID)KEY;
    Assoc.CompletionPort = Port;
    ok(SetInformationJobObject(Job, JobObjectAssociateCompletionPortInformation, &Assoc, sizeof(Assoc)),
       "token: associate: %lu\n", GetLastError());

    ZeroMemory(&Si, sizeof(Si));
    Si.StartupInfo.cb = sizeof(Si);
    GetModuleFileNameW(NULL, Cmd, MAX_PATH);
    StringCchCatW(Cmd, _countof(Cmd), L" JobList child");
    pInitialize(NULL, 2, 0, &Size);
    Si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, Size);
    ok(Si.lpAttributeList && pInitialize(Si.lpAttributeList, 2, 0, &Size), "token: initialize: %lu\n", GetLastError());
    ok(pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &Job, sizeof(Job), NULL, NULL),
       "token: job list: %lu\n", GetLastError());
    if (!pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &Policy, sizeof(Policy),
                 NULL, NULL))
    {
        /* Windows 10 version 1511 and later */
        skip("token: child process policy: %lu\n", GetLastError());
    }
    else if (!Restricted)
    {
        skip("token: no restricted token\n");
    }
    else
    {
        Ret = CreateProcessW(NULL, Cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                             NULL, NULL, &Si.StartupInfo, &Pi);
        ok(Ret, "token: CreateProcess failed %lu\n", GetLastError());
        if (Ret)
        {
            ok(IsProcessInJob(Pi.hProcess, Job, &InJob) && InJob, "token: child is not in the job\n");
            Before = PrivilegeCount(Pi.hProcess);
            Access.Token = Restricted;
            Access.Thread = Pi.hThread;
            Status = pSet(Pi.hProcess, ProcessAccessToken, &Access, sizeof(Access) - 1);
            ok(Status == STATUS_INFO_LENGTH_MISMATCH, "token: short length: 0x%lx\n", Status);
            Status = pSet(Pi.hProcess, ProcessAccessToken, &Access, sizeof(Access));
            ok(Status == STATUS_SUCCESS, "token: ProcessAccessToken: 0x%lx\n", Status);
            After = PrivilegeCount(Pi.hProcess);
            ok(After < Before, "token: child has %lu privileges, %lu before\n", After, Before);
            ResumeThread(Pi.hThread);
            ok(WaitForSingleObject(Pi.hProcess, 20000) == WAIT_OBJECT_0, "token: child did not exit\n");
            ok(GetExitCodeProcess(Pi.hProcess, &Exit) && Exit == CHILD_EXIT, "token: exit code %lu\n", Exit);
            CheckMessages(Port, Pi.dwProcessId, "token");
            CloseHandle(Pi.hThread);
            CloseHandle(Pi.hProcess);
        }
    }
    if (Si.lpAttributeList)
    {
        pDelete(Si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, Si.lpAttributeList);
    }
    if (Restricted) CloseHandle(Restricted);
    if (Token) CloseHandle(Token);
    CloseHandle(Job);
    CloseHandle(Port);
}

static VOID
TestJob(BOOL Limits)
{
    HANDLE Job, Port;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT Assoc;
    PROCESS_INFORMATION Pi;
    DWORD Error = 0, Exit = 0;
    BOOL InJob = FALSE, Ret;
    PCSTR Name = Limits ? "limited" : "plain";

    Job = CreateJobObjectW(NULL, NULL);
    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    ok(Job && Port, "%s: Create: %lu\n", Name, GetLastError());
    if (!Job || !Port) return;
    Assoc.CompletionKey = (PVOID)KEY;
    Assoc.CompletionPort = Port;
    ok(SetInformationJobObject(Job, JobObjectAssociateCompletionPortInformation, &Assoc, sizeof(Assoc)),
       "%s: associate: %lu\n", Name, GetLastError());
    if (Limits)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Li;
        JOBOBJECT_BASIC_UI_RESTRICTIONS Ui = {JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                                              JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD};
        ZeroMemory(&Li, sizeof(Li));
        Li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                              JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Li.BasicLimitInformation.ActiveProcessLimit = 1;
        ok(SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Li, sizeof(Li)),
           "limits: %lu\n", GetLastError());
        ok(SetInformationJobObject(Job, JobObjectBasicUIRestrictions, &Ui, sizeof(Ui)),
           "ui: %lu\n", GetLastError());
    }

    Ret = Launch(&Job, 1, CREATE_SUSPENDED, &Pi, &Error);
    ok(Ret, "%s: CreateProcess failed %lu\n", Name, Error);
    if (Ret)
    {
        /* Windows places the child in the job before its first thread runs */
        ok(IsProcessInJob(Pi.hProcess, Job, &InJob) && InJob, "%s: child is not in the job\n", Name);
        ResumeThread(Pi.hThread);
        ok(WaitForSingleObject(Pi.hProcess, 20000) == WAIT_OBJECT_0, "%s: child did not exit\n", Name);
        ok(GetExitCodeProcess(Pi.hProcess, &Exit) && Exit == CHILD_EXIT, "%s: exit code %lu\n", Name, Exit);
        CheckMessages(Port, Pi.dwProcessId, Name);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
    }
    CloseHandle(Job);
    CloseHandle(Port);
}

START_TEST(JobList)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc > 2 && !strcmp(argv[2], "child"))
    {
        if (argc > 3) SetEvent((HANDLE)(ULONG_PTR)strtoul(argv[3], NULL, 10));
        Sleep(300);
        ExitProcess(CHILD_EXIT);
    }
    pInitialize = (INITIALIZE_LIST)GetProcAddress(Kernel32, "InitializeProcThreadAttributeList");
    pUpdate = (UPDATE_LIST)GetProcAddress(Kernel32, "UpdateProcThreadAttribute");
    pDelete = (DELETE_LIST)GetProcAddress(Kernel32, "DeleteProcThreadAttributeList");
    /* kernel32 exports it from Windows 7 on */
    pCreateAsUser = (CREATE_AS_USER)GetProcAddress(Kernel32, "CreateProcessAsUserW");
    if (!pCreateAsUser)
        pCreateAsUser = (CREATE_AS_USER)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "CreateProcessAsUserW");
    if (!pInitialize || !pUpdate || !pDelete)
    {
        skip("Proc-thread attribute lists are not available\n");
        return;
    }
    TestJob(FALSE);
    TestJob(TRUE);
    TestSandboxList();
    TestAccessToken();
}
