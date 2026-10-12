/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test which services with the same image share a process
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* Service process side */
static WCHAR ServiceNames[2][64];
static SERVICE_STATUS_HANDLE StatusHandles[2];
static HANDLE StopEvents[2];

static VOID
ReportState(INT Index, DWORD State)
{
    SERVICE_STATUS Status;

    RtlZeroMemory(&Status, sizeof(Status));
    Status.dwServiceType = SERVICE_WIN32;
    Status.dwCurrentState = State;
    Status.dwControlsAccepted = (State == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;
    Status.dwWaitHint = (State == SERVICE_RUNNING || State == SERVICE_STOPPED) ? 0 : 5000;
    SetServiceStatus(StatusHandles[Index], &Status);
}

static DWORD WINAPI
ServiceHandler(DWORD Control, DWORD EventType, LPVOID EventData, LPVOID Context)
{
    INT Index = (INT)(INT_PTR)Context;

    if (Control == SERVICE_CONTROL_STOP)
    {
        ReportState(Index, SERVICE_STOP_PENDING);
        SetEvent(StopEvents[Index]);
    }
    return NO_ERROR;
}

static VOID WINAPI
ServiceMain(DWORD argc, LPWSTR *argv)
{
    INT Index = (argc > 0 && !_wcsicmp(argv[0], ServiceNames[1])) ? 1 : 0;

    StatusHandles[Index] = RegisterServiceCtrlHandlerExW(argc > 0 ? argv[0] : ServiceNames[Index],
                                                         ServiceHandler,
                                                         (LPVOID)(INT_PTR)Index);
    if (!StatusHandles[Index])
        return;
    /* A restarted service must not see the previous stop */
    ResetEvent(StopEvents[Index]);
    ReportState(Index, SERVICE_RUNNING);
    WaitForSingleObject(StopEvents[Index], 60000);
    ReportState(Index, SERVICE_STOPPED);
}

static VOID
ServiceProcess(PCSTR NameA, PCSTR NameB)
{
    SERVICE_TABLE_ENTRYW Table[3];

    MultiByteToWideChar(CP_ACP, 0, NameA, -1, ServiceNames[0], _countof(ServiceNames[0]));
    MultiByteToWideChar(CP_ACP, 0, NameB, -1, ServiceNames[1], _countof(ServiceNames[1]));
    StopEvents[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
    StopEvents[1] = CreateEventW(NULL, TRUE, FALSE, NULL);
    Table[0].lpServiceName = ServiceNames[0];
    Table[0].lpServiceProc = ServiceMain;
    Table[1].lpServiceName = ServiceNames[1];
    Table[1].lpServiceProc = ServiceMain;
    Table[2].lpServiceName = NULL;
    Table[2].lpServiceProc = NULL;
    StartServiceCtrlDispatcherW(Table);
}

/* Tester side */
static BOOL
WaitForState(SC_HANDLE hService, DWORD State, LPSERVICE_STATUS_PROCESS Status)
{
    DWORD Needed, Waited;

    RtlZeroMemory(Status, sizeof(*Status));
    for (Waited = 0; Waited <= 15000; Waited += 100)
    {
        if (!QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)Status, sizeof(*Status), &Needed))
            return FALSE;
        if (Status->dwCurrentState == State)
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static VOID
Cleanup(SC_HANDLE hService)
{
    SERVICE_STATUS_PROCESS Status;
    SERVICE_STATUS StopStatus;
    HANDLE hProcess;
    DWORD Needed;

    if (!hService)
        return;
    if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&Status, sizeof(Status), &Needed) &&
        Status.dwCurrentState != SERVICE_STOPPED)
    {
        ControlService(hService, SERVICE_CONTROL_STOP, &StopStatus);
        if (!WaitForState(hService, SERVICE_STOPPED, &Status) && Status.dwProcessId)
        {
            hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, Status.dwProcessId);
            if (hProcess)
            {
                TerminateProcess(hProcess, 1);
                CloseHandle(hProcess);
            }
        }
    }
    DeleteService(hService);
    CloseServiceHandle(hService);
}

static VOID
TestPair(SC_HANDLE hScm, DWORD ServiceType)
{
    CHAR FileName[MAX_PATH], Command[MAX_PATH + 128], Names[2][64];
    SC_HANDLE hServices[2] = { NULL, NULL };
    SERVICE_STATUS_PROCESS Status[2] = { { 0 } };
    SERVICE_STATUS StopStatus;
    BOOL Ret;
    DWORD Error;
    BOOL Shared = (ServiceType == SERVICE_WIN32_SHARE_PROCESS);
    PCSTR Kind = Shared ? "share" : "own";
    INT i;

    GetModuleFileNameA(NULL, FileName, _countof(FileName));
    StringCbPrintfA(Names[0], sizeof(Names[0]), "RosSvcProcA%lu", GetTickCount());
    StringCbPrintfA(Names[1], sizeof(Names[1]), "RosSvcProcB%lu", GetTickCount());
    /* Both services have exactly the same image path */
    StringCbPrintfA(Command, sizeof(Command), "\"%s\" ServiceProcess service %s %s", FileName, Names[0], Names[1]);

    for (i = 0; i < 2; i++)
    {
        hServices[i] = CreateServiceA(hScm, Names[i], Names[i], SERVICE_ALL_ACCESS, ServiceType,
                                      SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, Command,
                                      NULL, NULL, NULL, NULL, NULL);
        Error = GetLastError();
        ok(hServices[i] != NULL, "%s: CreateService(%s) failed with %lu\n", Kind, Names[i], Error);
        if (!hServices[i])
            goto done;
    }

    for (i = 0; i < 2; i++)
    {
        Ret = StartServiceA(hServices[i], 0, NULL);
        Error = GetLastError();
        ok(Ret, "%s: StartService(%d) failed with %lu\n", Kind, i, Error);
        Ret = WaitForState(hServices[i], SERVICE_RUNNING, &Status[i]);
        ok(Ret, "%s: service %d state %lu, expected running\n", Kind, i, Status[i].dwCurrentState);
    }

    ok(Status[0].dwProcessId != 0 && Status[1].dwProcessId != 0,
       "%s: process ids %lu, %lu\n", Kind, Status[0].dwProcessId, Status[1].dwProcessId);
    if (Shared)
        ok(Status[0].dwProcessId == Status[1].dwProcessId,
           "share: process ids %lu and %lu differ\n", Status[0].dwProcessId, Status[1].dwProcessId);
    else
        ok(Status[0].dwProcessId != Status[1].dwProcessId,
           "own: both services run in process %lu\n", Status[0].dwProcessId);

    /* Stopping one leaves the other running */
    Ret = ControlService(hServices[1], SERVICE_CONTROL_STOP, &StopStatus);
    Error = GetLastError();
    ok(Ret, "%s: ControlService(1, STOP) failed with %lu\n", Kind, Error);
    Ret = WaitForState(hServices[1], SERVICE_STOPPED, &Status[1]);
    ok(Ret, "%s: service 1 state %lu, expected stopped\n", Kind, Status[1].dwCurrentState);
    Ret = WaitForState(hServices[0], SERVICE_RUNNING, &Status[0]);
    ok(Ret, "%s: service 0 state %lu, expected running\n", Kind, Status[0].dwCurrentState);

    /* Restart the stopped one while the other still runs */
    Ret = StartServiceA(hServices[1], 0, NULL);
    Error = GetLastError();
    ok(Ret, "%s: restarting service 1 failed with %lu\n", Kind, Error);
    Ret = WaitForState(hServices[1], SERVICE_RUNNING, &Status[1]);
    ok(Ret, "%s: restarted service 1 state %lu, expected running\n", Kind, Status[1].dwCurrentState);
    if (Shared)
        ok(Status[1].dwProcessId == Status[0].dwProcessId,
           "share: restarted service in process %lu, expected %lu\n", Status[1].dwProcessId, Status[0].dwProcessId);
    else
        ok(Status[1].dwProcessId != 0 && Status[1].dwProcessId != Status[0].dwProcessId,
           "own: restarted service in process %lu, other in %lu\n", Status[1].dwProcessId, Status[0].dwProcessId);
    Ret = ControlService(hServices[1], SERVICE_CONTROL_STOP, &StopStatus);
    Error = GetLastError();
    ok(Ret, "%s: ControlService(1, STOP) after restart failed with %lu\n", Kind, Error);
    Ret = WaitForState(hServices[1], SERVICE_STOPPED, &Status[1]);
    ok(Ret, "%s: restarted service 1 state %lu, expected stopped\n", Kind, Status[1].dwCurrentState);

    Ret = ControlService(hServices[0], SERVICE_CONTROL_STOP, &StopStatus);
    Error = GetLastError();
    ok(Ret, "%s: ControlService(0, STOP) failed with %lu\n", Kind, Error);
    Ret = WaitForState(hServices[0], SERVICE_STOPPED, &Status[0]);
    ok(Ret, "%s: service 0 state %lu, expected stopped\n", Kind, Status[0].dwCurrentState);

done:
    Cleanup(hServices[0]);
    Cleanup(hServices[1]);
}

START_TEST(ServiceProcess)
{
    SC_HANDLE hScm;
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 5 && !strcmp(argv[2], "service"))
    {
        ServiceProcess(argv[3], argv[4]);
        return;
    }

    hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hScm)
    {
        skip("OpenSCManager failed with %lu\n", GetLastError());
        return;
    }
    TestPair(hScm, SERVICE_WIN32_OWN_PROCESS);
    TestPair(hScm, SERVICE_WIN32_SHARE_PROCESS);
    CloseServiceHandle(hScm);
}
