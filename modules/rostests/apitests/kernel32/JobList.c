/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     PROC_THREAD_ATTRIBUTE_JOB_LIST: the child is a member of the job from creation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Chromium's sandbox passes the target job with this attribute and frees the
 * target's resources when the job completion port reports the process gone, so a
 * child that never joins the job leaks them for as long as the browser runs.
 */

#include "precomp.h"

#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif
#ifndef JOB_OBJECT_MSG_NEW_PROCESS
#define JOB_OBJECT_MSG_NEW_PROCESS 6
#define JOB_OBJECT_MSG_EXIT_PROCESS 7
#endif

typedef BOOL (WINAPI *INITIALIZE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *UPDATE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR,
                                   PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *DELETE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST);

#define KEY ((ULONG_PTR)0x4a4c)
#define CHILD_EXIT 37

static INITIALIZE_LIST pInitialize;
static UPDATE_LIST pUpdate;
static DELETE_LIST pDelete;

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
        Sleep(300);
        ExitProcess(CHILD_EXIT);
    }
    pInitialize = (INITIALIZE_LIST)GetProcAddress(Kernel32, "InitializeProcThreadAttributeList");
    pUpdate = (UPDATE_LIST)GetProcAddress(Kernel32, "UpdateProcThreadAttribute");
    pDelete = (DELETE_LIST)GetProcAddress(Kernel32, "DeleteProcThreadAttributeList");
    if (!pInitialize || !pUpdate || !pDelete)
    {
        skip("Proc-thread attribute lists are not available\n");
        return;
    }
    TestJob(FALSE);
    TestJob(TRUE);
}
