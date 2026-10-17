/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "precomp.h"

static VOID RemoteCreator(DWORD ProcessId)
{
    HANDLE Process, Thread;
    DWORD Exit = ~0;
    CONTEXT Context;
    LPTHREAD_START_ROUTINE Routine = (LPTHREAD_START_ROUTINE)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserThread");
    Process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ProcessId);
    ok(Process != NULL, "open target: %lu\n", GetLastError());
    if (!Process) return;
    /* The two identical PE32 processes use the same system ntdll mapping. */
    Thread = CreateRemoteThread(Process, NULL, 0, Routine, (PVOID)0x35a,
                                CREATE_SUSPENDED, NULL);
    ok(Thread != NULL, "cross-process thread creation: %lu\n", GetLastError());
    if (Thread)
    {
        ZeroMemory(&Context, sizeof(Context));
        Context.ContextFlags = CONTEXT_FULL;
        ok(GetThreadContext(Thread, &Context), "remote startup context: %lu\n", GetLastError());
#ifdef _M_IX86
        ok(Context.Eax == (DWORD)(ULONG_PTR)Routine && Context.Ebx == 0x35a,
           "remote start eax %lx ebx %lx\n", Context.Eax, Context.Ebx);
        Context.Ebx = 0x35b;
        ok(SetThreadContext(Thread, &Context), "remote context update: %lu\n", GetLastError());
#endif
        ok(ResumeThread(Thread) == 1, "remote resume failed\n");
        ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "remote wait failed\n");
        GetExitCodeThread(Thread, &Exit);
#ifdef _M_IX86
        ok(Exit == 0x35b, "remote exit %lx\n", Exit);
#else
        ok(Exit == 0x35a, "remote exit %lx\n", Exit);
#endif
        CloseHandle(Thread);
    }
    CloseHandle(Process);
}

static VOID WaitChild(PROCESS_INFORMATION *Child)
{
    DWORD Exit = ~0, Wait = WaitForSingleObject(Child->hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "child process wait %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(Child->hProcess, 0xdead);
        WaitForSingleObject(Child->hProcess, 5000);
    }
    GetExitCodeProcess(Child->hProcess, &Exit);
    ok(Exit == 0, "child process exit %lx\n", Exit);
    CloseHandle(Child->hThread);
    CloseHandle(Child->hProcess);
}

START_TEST(Wow64RemoteThread)
{
    char **Argv, Image[MAX_PATH], Command[2 * MAX_PATH], ReadyName[80], FinishName[80];
    int Argc = winetest_get_mainargs(&Argv);
    HANDLE Ready, Finish;
    STARTUPINFOA Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Target, Creator;
    BOOL Result, Guest = FALSE;

    if (Argc >= 5 && !strcmp(Argv[2], "target"))
    {
        Ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, Argv[3]);
        Finish = OpenEventA(SYNCHRONIZE, FALSE, Argv[4]);
        ok(Ready != NULL && Finish != NULL, "target event open failed\n");
        if (Ready && Finish)
        {
            SetEvent(Ready);
            ok(WaitForSingleObject(Finish, 60000) == WAIT_OBJECT_0, "target finish wait failed\n");
        }
        if (Ready) CloseHandle(Ready);
        if (Finish) CloseHandle(Finish);
        return;
    }
    if (Argc >= 4 && !strcmp(Argv[2], "creator"))
    {
        RemoteCreator(strtoul(Argv[3], NULL, 10));
        return;
    }
    IsWow64Process(GetCurrentProcess(), &Guest);
    if (Guest)
    {
        skip("Run the controller natively with a PE32 fixture path\n");
        return;
    }
    if (Argc >= 3) lstrcpynA(Image, Argv[2], _countof(Image));
    else GetModuleFileNameA(NULL, Image, _countof(Image));
    sprintf(ReadyName, "wow64-remote-ready-%lu", GetCurrentProcessId());
    sprintf(FinishName, "wow64-remote-finish-%lu", GetCurrentProcessId());
    Ready = CreateEventA(NULL, TRUE, FALSE, ReadyName);
    Finish = CreateEventA(NULL, TRUE, FALSE, FinishName);
    ok(Ready != NULL && Finish != NULL, "controller event creation failed\n");
    if (!Ready || !Finish) goto Done;
    _snprintf(Command, sizeof(Command), "\"%s\" Wow64RemoteThread target %s %s", Image, ReadyName, FinishName);
    Result = CreateProcessA(Image, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Target);
    ok(Result, "create PE32 target: %lu\n", GetLastError());
    if (!Result) goto Done;
    if (WaitForSingleObject(Ready, 30000) == WAIT_OBJECT_0)
    {
        _snprintf(Command, sizeof(Command), "\"%s\" Wow64RemoteThread creator %lu", Image, Target.dwProcessId);
        Result = CreateProcessA(Image, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Creator);
        ok(Result, "create PE32 creator: %lu\n", GetLastError());
        if (Result) WaitChild(&Creator);
    }
    else ok(FALSE, "target did not become ready\n");
    SetEvent(Finish);
    WaitChild(&Target);
Done:
    if (Ready) CloseHandle(Ready);
    if (Finish) CloseHandle(Finish);
}
