/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the actions a service controller takes when a service fails
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <sddl.h>

#define CTRL_CRASH          128
#define CTRL_EXIT_ERROR     129

#define POLL_MS             50

/*** Service part of the test ***/

static SERVICE_STATUS_HANDLE g_StatusHandle;
static HANDLE g_Done;
static DWORD g_ExitCode = NO_ERROR;
static DWORD g_SpecificCode;

static void ReportState(DWORD State, DWORD Win32Exit, DWORD Specific)
{
    SERVICE_STATUS Status = { SERVICE_WIN32_OWN_PROCESS, State };

    if (State == SERVICE_RUNNING)
        Status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    Status.dwWin32ExitCode = Win32Exit;
    Status.dwServiceSpecificExitCode = Specific;
    SetServiceStatus(g_StatusHandle, &Status);
}

static DWORD WINAPI CrashThread(PVOID Param)
{
    Sleep(100);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

static VOID WINAPI ServiceHandler(DWORD Control)
{
    switch (Control)
    {
        case SERVICE_CONTROL_STOP:
            ReportState(SERVICE_STOP_PENDING, NO_ERROR, 0);
            SetEvent(g_Done);
            return;

        case CTRL_CRASH:
            CloseHandle(CreateThread(NULL, 0, CrashThread, NULL, 0, NULL));
            break;

        case CTRL_EXIT_ERROR:
            g_ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
            g_SpecificCode = 7;
            SetEvent(g_Done);
            return;
    }
    ReportState(SERVICE_RUNNING, NO_ERROR, 0);
}

static void WhoAmIMode(PCSTR File);

static VOID WINAPI ServiceMain(DWORD argc, PWSTR *argv)
{
    CHAR File[MAX_PATH];

    g_StatusHandle = RegisterServiceCtrlHandlerW(argv[0], ServiceHandler);
    if (!g_StatusHandle)
        return;

    /* A start argument names a file for the account of this service */
    if (argc >= 2 && WideCharToMultiByte(CP_ACP, 0, argv[1], -1, File, sizeof(File), NULL, NULL))
        WhoAmIMode(File);
    g_Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ReportState(SERVICE_RUNNING, NO_ERROR, 0);
    WaitForSingleObject(g_Done, INFINITE);
    ReportState(SERVICE_STOPPED, g_ExitCode, g_SpecificCode);
}

/* Mode "runcmd FILE COUNT": the recovery command writes its count into a file */
static void RunCommandMode(PCSTR File, PCSTR Count)
{
    HANDLE hFile = CreateFileA(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD Written;

    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile(hFile, Count, lstrlenA(Count), &Written, NULL);
        CloseHandle(hFile);
    }
}

/* Mode "whoami FILE": the recovery command writes the name of its account into a file */
static void WhoAmIMode(PCSTR File)
{
    WCHAR User[128];
    CHAR Text[256] = "";
    DWORD Size = _countof(User), Written;
    HANDLE hFile;

    if (GetUserNameW(User, &Size))
        WideCharToMultiByte(CP_ACP, 0, User, -1, Text, sizeof(Text) - 1, NULL, NULL);
    hFile = CreateFileA(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile(hFile, Text, lstrlenA(Text), &Written, NULL);
        CloseHandle(hFile);
    }
}

/*** Tester part of the test ***/

typedef struct _SVC
{
    SC_HANDLE hSC;
    SC_HANDLE hService;
    WCHAR Name[64];
} SVC;

static DWORD g_Seq;

static BOOL CreateTestServiceAs(SVC *Svc, PCWSTR Account)
{
    WCHAR Path[MAX_PATH + 64];

    Svc->hSC = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!Svc->hSC)
        return FALSE;

    StringCchPrintfW(Svc->Name, _countof(Svc->Name), L"SvcFailTest%lu_%lu", GetCurrentProcessId(), ++g_Seq);
    Path[0] = L'"';
    if (!GetModuleFileNameW(NULL, Path + 1, MAX_PATH))
        return FALSE;
    StringCchCatW(Path, _countof(Path), L"\" ServiceRecovery ");
    StringCchCatW(Path, _countof(Path), Svc->Name);

    Svc->hService = CreateServiceW(Svc->hSC, Svc->Name, Svc->Name, SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
                                   SERVICE_ERROR_IGNORE, Path, NULL, NULL, NULL, Account, NULL);
    if (!Svc->hService)
    {
        CloseServiceHandle(Svc->hSC);
        return FALSE;
    }
    return TRUE;
}

static BOOL CreateTestService(SVC *Svc)
{
    return CreateTestServiceAs(Svc, NULL);
}

static void DestroyTestService(SVC *Svc)
{
    SERVICE_STATUS Status;
    SC_ACTION NoAction = { SC_ACTION_NONE, 0 };
    SERVICE_FAILURE_ACTIONSW None = { 0, NULL, NULL, 0, &NoAction };
    DWORD Start;

    /* A restart that is already waiting must not start the service again: disable it, clear the recovery, then stop it */
    ChangeServiceConfigW(Svc->hService, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                         NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    ChangeServiceConfig2W(Svc->hService, SERVICE_CONFIG_FAILURE_ACTIONS, &None);
    for (Start = GetTickCount(); GetTickCount() - Start < 5000; Sleep(POLL_MS))
    {
        /* A start that was already under way when the service was disabled settles, and is stopped then */
        if (QueryServiceStatus(Svc->hService, &Status) && Status.dwCurrentState == SERVICE_STOPPED)
            break;
        if (Status.dwCurrentState == SERVICE_RUNNING)
            ControlService(Svc->hService, SERVICE_CONTROL_STOP, &Status);
    }
    DeleteService(Svc->hService);
    CloseServiceHandle(Svc->hService);
    CloseServiceHandle(Svc->hSC);
}

static BOOL Query(SVC *Svc, SERVICE_STATUS_PROCESS *Status)
{
    DWORD Needed;

    return QueryServiceStatusEx(Svc->hService, SC_STATUS_PROCESS_INFO, (PBYTE)Status, sizeof(*Status), &Needed);
}

/* Wait for a running service with a process id other than OldPid (0: any) */
static BOOL WaitRunning(SVC *Svc, DWORD OldPid, DWORD TimeoutMs, SERVICE_STATUS_PROCESS *Status, DWORD *ElapsedMs)
{
    DWORD Start = GetTickCount();

    while (GetTickCount() - Start < TimeoutMs)
    {
        if (Query(Svc, Status) && Status->dwCurrentState == SERVICE_RUNNING &&
            Status->dwProcessId != 0 && Status->dwProcessId != OldPid)
        {
            if (ElapsedMs)
                *ElapsedMs = GetTickCount() - Start;
            return TRUE;
        }
        Sleep(POLL_MS);
    }

    Query(Svc, Status);
    trace("not running again: state %lu, pid %lu (old %lu), exit code %lu\n",
          Status->dwCurrentState, Status->dwProcessId, OldPid, Status->dwWin32ExitCode);
    if (OldPid != 0)
    {
        HANDLE hOld = OpenProcess(SYNCHRONIZE, FALSE, OldPid);
        trace("old process %s\n", !hOld ? "is gone" : (WaitForSingleObject(hOld, 0) == WAIT_OBJECT_0 ? "has ended" : "still runs"));
        if (hOld)
            CloseHandle(hOld);
    }
    return FALSE;
}

static BOOL WaitStopped(SVC *Svc, DWORD TimeoutMs, SERVICE_STATUS_PROCESS *Status, DWORD *ElapsedMs)
{
    DWORD Start = GetTickCount();

    while (GetTickCount() - Start < TimeoutMs)
    {
        if (Query(Svc, Status) && Status->dwCurrentState == SERVICE_STOPPED)
        {
            if (ElapsedMs)
                *ElapsedMs = GetTickCount() - Start;
            return TRUE;
        }
        Sleep(POLL_MS);
    }
    return FALSE;
}

/* TRUE if the service stays stopped for the whole time */
static BOOL StaysStopped(SVC *Svc, DWORD DurationMs)
{
    SERVICE_STATUS_PROCESS Status;
    DWORD Start = GetTickCount();

    while (GetTickCount() - Start < DurationMs)
    {
        if (!Query(Svc, &Status) || Status.dwCurrentState != SERVICE_STOPPED)
            return FALSE;
        Sleep(POLL_MS);
    }
    return TRUE;
}

static void SetActions(SVC *Svc, DWORD ResetSeconds, PCWSTR Command, DWORD cActions, SC_ACTION *Actions)
{
    SERVICE_FAILURE_ACTIONSW Fa = { ResetSeconds, NULL, (PWSTR)Command, cActions, Actions };
    BOOL Ret = ChangeServiceConfig2W(Svc->hService, SERVICE_CONFIG_FAILURE_ACTIONS, &Fa);

    ok(Ret, "ChangeServiceConfig2W failed: %lu\n", GetLastError());
}

static BOOL StartAndWait(SVC *Svc, SERVICE_STATUS_PROCESS *Status)
{
    if (!StartServiceW(Svc->hService, 0, NULL))
    {
        ok(0, "StartService failed: %lu\n", GetLastError());
        return FALSE;
    }
    if (!WaitRunning(Svc, 0, 15000, Status, NULL))
    {
        ok(0, "The service did not reach the running state\n");
        return FALSE;
    }
    return TRUE;
}

static BOOL Crash(SVC *Svc)
{
    SERVICE_STATUS Status;
    SERVICE_STATUS_PROCESS Process;
    HANDLE hProcess = NULL;
    DWORD Start;
    BOOL Ret;

    if (Query(Svc, &Process) && Process.dwProcessId)
        hProcess = OpenProcess(SYNCHRONIZE, FALSE, Process.dwProcessId);

    Ret = ControlService(Svc->hService, CTRL_CRASH, &Status);
    ok(Ret, "ControlService(crash) failed: %lu\n", GetLastError());

    if (hProcess)
    {
        Start = GetTickCount();
        if (WaitForSingleObject(hProcess, 8000) != WAIT_OBJECT_0)
            trace("DIAG: the service process %lu is still alive %lu ms after the crash request\n", Process.dwProcessId, GetTickCount() - Start);
        CloseHandle(hProcess);
    }
    return Ret;
}

static void TestCrashNoActions(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    DWORD Ms = 0;

    if (!CreateTestService(&Svc))
    {
        skip("Cannot create the service: %lu\n", GetLastError());
        return;
    }
    if (StartAndWait(&Svc, &Status) && Crash(&Svc))
    {
        ok(WaitStopped(&Svc, 10000, &Status, &Ms), "A crashed service did not become stopped\n");
        trace("crash without actions: stopped after %lu ms, exit code %lu, specific %lu, controls %lx\n",
              Ms, Status.dwWin32ExitCode, Status.dwServiceSpecificExitCode, Status.dwControlsAccepted);
        ok(Status.dwWin32ExitCode == ERROR_PROCESS_ABORTED, "Exit code %lu\n", Status.dwWin32ExitCode);
        ok(Status.dwControlsAccepted == 0, "Controls accepted %lx\n", Status.dwControlsAccepted);
        ok(StaysStopped(&Svc, 2500), "A service without failure actions came back\n");
    }
    DestroyTestService(&Svc);
}

static void TestRestartSequence(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    SC_ACTION Actions[3] = { { SC_ACTION_RESTART, 1000 }, { SC_ACTION_RESTART, 500 }, { SC_ACTION_NONE, 0 } };
    DWORD Pid, Ms = 0;

    if (!CreateTestService(&Svc))
    {
        skip("Cannot create the service: %lu\n", GetLastError());
        return;
    }
    SetActions(&Svc, 3600, NULL, 3, Actions);
    if (!StartAndWait(&Svc, &Status))
        goto Done;

    Pid = Status.dwProcessId;
    Crash(&Svc);
    ok(WaitRunning(&Svc, Pid, 15000, &Status, &Ms), "No restart after the first failure\n");
    trace("first restart: %lu ms after the crash was asked for\n", Ms);
    ok(Ms >= 1000, "First restart after %lu ms, the delay is 1000\n", Ms);

    Pid = Status.dwProcessId;
    Crash(&Svc);
    ok(WaitRunning(&Svc, Pid, 15000, &Status, &Ms), "No restart after the second failure\n");
    trace("second restart: %lu ms after the crash was asked for\n", Ms);
    ok(Ms >= 500, "Second restart after %lu ms, the delay is 500\n", Ms);

    Crash(&Svc);
    ok(WaitStopped(&Svc, 10000, &Status, NULL), "The third crash left the service running\n");
    ok(StaysStopped(&Svc, 3000), "A service came back although its third action is none\n");

Done:
    DestroyTestService(&Svc);
}

static void TestNormalStop(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    SERVICE_STATUS Plain;
    SC_ACTION Actions[1] = { { SC_ACTION_RESTART, 300 } };

    if (!CreateTestService(&Svc))
    {
        skip("Cannot create the service: %lu\n", GetLastError());
        return;
    }
    SetActions(&Svc, 3600, NULL, 1, Actions);
    if (StartAndWait(&Svc, &Status))
    {
        ok(ControlService(Svc.hService, SERVICE_CONTROL_STOP, &Plain), "Stop failed: %lu\n", GetLastError());
        ok(WaitStopped(&Svc, 10000, &Status, NULL), "The service did not stop\n");
        ok(Status.dwWin32ExitCode == NO_ERROR, "Exit code %lu\n", Status.dwWin32ExitCode);
        ok(StaysStopped(&Svc, 2500), "A service that was stopped on request came back\n");
    }
    DestroyTestService(&Svc);
}

/* SERVICE_FAILURE_ACTIONS_FLAG, which the SDK header here lacks */
typedef struct _FAILURE_ACTIONS_FLAG
{
    BOOL fFailureActionsOnNonCrashFailures;
} FAILURE_ACTIONS_FLAG;

/* With the flag for non-crash failures, a service that reports an error and stops gets its failure actions */
static void TestNonCrashFlag(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    SERVICE_STATUS Plain;
    SC_ACTION Actions[1] = { { SC_ACTION_RESTART, 500 } };
    FAILURE_ACTIONS_FLAG Flag = { TRUE }, Out = { FALSE };
    DWORD Needed, Pid;
    BOOL Ret;

    if (!CreateTestService(&Svc))
    {
        skip("Cannot create the service: %lu\n", GetLastError());
        return;
    }
    SetActions(&Svc, 3600, NULL, 1, Actions);

    Ret = QueryServiceConfig2W(Svc.hService, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, (PBYTE)&Out, sizeof(Out), &Needed);
    ok(Ret, "QueryServiceConfig2W(flag) failed: %lu\n", GetLastError());
    ok(Out.fFailureActionsOnNonCrashFailures == FALSE, "The flag is set by default\n");

    Ret = ChangeServiceConfig2W(Svc.hService, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &Flag);
    ok(Ret, "ChangeServiceConfig2W(flag) failed: %lu\n", GetLastError());
    Out.fFailureActionsOnNonCrashFailures = FALSE;
    Ret = QueryServiceConfig2W(Svc.hService, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, (PBYTE)&Out, sizeof(Out), &Needed);
    ok(Ret, "QueryServiceConfig2W(flag) failed: %lu\n", GetLastError());
    ok(Out.fFailureActionsOnNonCrashFailures == TRUE, "The flag did not stick\n");

    if (StartAndWait(&Svc, &Status))
    {
        Pid = Status.dwProcessId;
        ControlService(Svc.hService, CTRL_EXIT_ERROR, &Plain);
        ok(WaitRunning(&Svc, Pid, 15000, &Status, NULL), "No restart after a non-crash failure with the flag set\n");

        /* Without the flag the same exit leaves the service stopped */
        Flag.fFailureActionsOnNonCrashFailures = FALSE;
        ok(ChangeServiceConfig2W(Svc.hService, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &Flag),
           "ChangeServiceConfig2W(flag) failed: %lu\n", GetLastError());
        ControlService(Svc.hService, CTRL_EXIT_ERROR, &Plain);
        ok(WaitStopped(&Svc, 10000, &Status, NULL), "The service did not stop\n");
        ok(StaysStopped(&Svc, 2500), "A non-crash failure restarted the service with the flag cleared\n");
    }
    DestroyTestService(&Svc);
}

/* A service that ends itself with an error and reports it is not a crash */
static void TestSpecificExit(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    SERVICE_STATUS Plain;
    SC_ACTION Actions[1] = { { SC_ACTION_RESTART, 500 } };

    if (!CreateTestService(&Svc))
    {
        skip("Cannot create the service: %lu\n", GetLastError());
        return;
    }
    SetActions(&Svc, 3600, NULL, 1, Actions);
    if (StartAndWait(&Svc, &Status))
    {
        ControlService(Svc.hService, CTRL_EXIT_ERROR, &Plain);
        ok(WaitStopped(&Svc, 10000, &Status, NULL), "The service did not stop\n");
        ok(Status.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR, "Exit code %lu\n", Status.dwWin32ExitCode);
        ok(Status.dwServiceSpecificExitCode == 7, "Specific exit code %lu\n", Status.dwServiceSpecificExitCode);
        ok(StaysStopped(&Svc, 2500), "A non-zero exit restarted the service\n");
    }
    DestroyTestService(&Svc);
}

static void TestResetPeriod(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    SC_ACTION Actions[2] = { { SC_ACTION_RESTART, 300 }, { SC_ACTION_NONE, 0 } };
    DWORD Pid;

    if (!CreateTestService(&Svc))
    {
        skip("Cannot create the service: %lu\n", GetLastError());
        return;
    }
    SetActions(&Svc, 2, NULL, 2, Actions);
    if (!StartAndWait(&Svc, &Status))
        goto Done;

    Pid = Status.dwProcessId;
    Crash(&Svc);
    ok(WaitRunning(&Svc, Pid, 15000, &Status, NULL), "No restart after the first failure\n");

    /* After the reset period the failure count starts over, so the first action applies again */
    Sleep(3500);
    Pid = Status.dwProcessId;
    Crash(&Svc);
    ok(WaitRunning(&Svc, Pid, 15000, &Status, NULL), "No restart after the failure count was reset\n");

    /* Within the period the second action, none, applies */
    Pid = Status.dwProcessId;
    Crash(&Svc);
    ok(WaitStopped(&Svc, 10000, &Status, NULL), "The crash left the service running\n");
    ok(StaysStopped(&Svc, 2500), "A service came back within the reset period with the action none\n");

Done:
    DestroyTestService(&Svc);
}

static void TestRunCommand(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    SC_ACTION Actions[1] = { { SC_ACTION_RUN_COMMAND, 200 } };
    WCHAR Exe[MAX_PATH], Dir[MAX_PATH], File[MAX_PATH], Command[3 * MAX_PATH];
    DWORD Start;
    HANDLE hFile;
    CHAR Text[16] = "";
    DWORD Read;

    if (!GetTempPathW(_countof(Dir), Dir) || !GetModuleFileNameW(NULL, Exe, _countof(Exe)))
    {
        skip("No temporary path\n");
        return;
    }
    StringCchPrintfW(File, _countof(File), L"%lssvcfail%lu.txt", Dir, GetCurrentProcessId());
    DeleteFileW(File);
    StringCchPrintfW(Command, _countof(Command), L"\"%ls\" ServiceRecovery runcmd \"%ls\" %%1%%", Exe, File);

    if (!CreateTestService(&Svc))
    {
        skip("Cannot create the service: %lu\n", GetLastError());
        return;
    }
    SetActions(&Svc, 3600, Command, 1, Actions);
    if (StartAndWait(&Svc, &Status))
    {
        Crash(&Svc);
        for (Start = GetTickCount(); GetTickCount() - Start < 10000; Sleep(100))
        {
            if (GetFileAttributesW(File) != INVALID_FILE_ATTRIBUTES)
                break;
        }
        Sleep(500);
        hFile = CreateFileW(File, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        ok(hFile != INVALID_HANDLE_VALUE, "The recovery command did not run\n");
        if (hFile != INVALID_HANDLE_VALUE)
        {
            ReadFile(hFile, Text, sizeof(Text) - 1, &Read, NULL);
            CloseHandle(hFile);
            trace("recovery command count text: '%s'\n", Text);
            ok(!strcmp(Text, "1"), "The command saw the failure count '%s', expected 1\n", Text);
        }
        ok(StaysStopped(&Svc, 1500), "A run-command action restarted the service\n");
    }
    DestroyTestService(&Svc);
    DeleteFileW(File);
}

static BOOL ReadText(PCWSTR File, PSTR Text, DWORD Size)
{
    HANDLE hFile = CreateFileW(File, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD Read = 0;

    Text[0] = 0;
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(hFile, Text, Size - 1, &Read, NULL);
    Text[Read] = 0;
    CloseHandle(hFile);
    return TRUE;
}

/* The recovery command of a service runs in the account of the service */
static void TestRunCommandAccount(void)
{
    SVC Svc;
    SERVICE_STATUS_PROCESS Status;
    SC_ACTION Actions[1] = { { SC_ACTION_RUN_COMMAND, 200 } };
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, FALSE };
    WCHAR Exe[MAX_PATH], Dir[MAX_PATH], File[MAX_PATH], SvcFile[MAX_PATH], Command[3 * MAX_PATH];
    CHAR Text[256] = "", Self[256] = "";
    PCWSTR StartArg;
    DWORD Start;

    if (!GetTempPathW(_countof(Dir), Dir) || !GetModuleFileNameW(NULL, Exe, _countof(Exe)) ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OICI;GA;;;WD)", SDDL_REVISION_1,
                                                              &Sa.lpSecurityDescriptor, NULL))
    {
        skip("No temporary path or security descriptor\n");
        return;
    }
    StringCchPrintfW(Dir + wcslen(Dir), _countof(Dir) - wcslen(Dir), L"svcacct%lu", GetCurrentProcessId());
    if (!CreateDirectoryW(Dir, &Sa))
    {
        skip("Cannot create %ls: %lu\n", Dir, GetLastError());
        LocalFree(Sa.lpSecurityDescriptor);
        return;
    }
    StringCchPrintfW(File, _countof(File), L"%ls\\who.txt", Dir);
    StringCchPrintfW(SvcFile, _countof(SvcFile), L"%ls\\svc.txt", Dir);
    StringCchPrintfW(Command, _countof(Command), L"\"%ls\" ServiceRecovery whoami \"%ls\"", Exe, File);

    if (CreateTestServiceAs(&Svc, L"NT AUTHORITY\\LocalService"))
    {
        SetActions(&Svc, 3600, Command, 1, Actions);
        StartArg = SvcFile;
        if (StartServiceW(Svc.hService, 1, &StartArg) && WaitRunning(&Svc, 0, 15000, &Status, NULL))
        {
            Crash(&Svc);
            for (Start = GetTickCount(); GetTickCount() - Start < 10000; Sleep(100))
            {
                if (GetFileAttributesW(File) != INVALID_FILE_ATTRIBUTES)
                    break;
            }
            Sleep(500);
            ReadText(SvcFile, Self, sizeof(Self));
            ok(ReadText(File, Text, sizeof(Text)), "The recovery command did not run\n");
            trace("recovery command account '%s', service account '%s'\n", Text, Self);
            ok(Self[0] != 0, "The service did not tell its account\n");
            ok(!strcmp(Text, Self), "The recovery command ran as '%s', the service as '%s'\n", Text, Self);
        }
        else
        {
            ok(0, "The LocalService service did not start\n");
        }
        DestroyTestService(&Svc);
    }
    else
    {
        skip("Cannot create a service for LocalService: %lu\n", GetLastError());
    }

    DeleteFileW(File);
    DeleteFileW(SvcFile);
    RemoveDirectoryW(Dir);
    LocalFree(Sa.lpSecurityDescriptor);
}

START_TEST(ServiceRecovery)
{
    int argc;
    char **argv;
    SC_HANDLE hSC;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 5 && !strcmp(argv[2], "runcmd"))
    {
        RunCommandMode(argv[3], argv[4]);
        return;
    }
    if (argc >= 4 && !strcmp(argv[2], "whoami"))
    {
        WhoAmIMode(argv[3]);
        return;
    }
    if (argc >= 3)
    {
        WCHAR Name[64];
        SERVICE_TABLE_ENTRYW Table[] = { { Name, ServiceMain }, { NULL, NULL } };

        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, Name, _countof(Name));
        StartServiceCtrlDispatcherW(Table);
        return;
    }

    hSC = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSC)
    {
        skip("OpenSCManagerW failed with error %lu\n", GetLastError());
        return;
    }
    CloseServiceHandle(hSC);

    TestCrashNoActions();
    TestRestartSequence();
    TestNormalStop();
    TestSpecificExit();
    TestNonCrashFlag();
    TestResetPeriod();
    TestRunCommand();
    TestRunCommandAccount();
}
