/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     TLS of a DLL that is loaded after the process started
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef ULONG (WINAPI *FN_TlsDynGetValue)(VOID);
typedef VOID (WINAPI *FN_TlsDynSetValue)(ULONG);
typedef PCSTR (WINAPI *FN_TlsDynGetEvents)(VOID);
typedef VOID (WINAPI *FN_TlsDynSetLog)(char *, LONG *);

#define TEMPLATE_VALUE 0x5EEDF00D
#define AFTER_UNLOAD_CODE 0x1234

static FN_TlsDynGetValue pTlsDynGetValue;
static FN_TlsDynSetValue pTlsDynSetValue;

/* A thread started before the first load that lives until the end, doing what it is told */
enum { CMD_GET, CMD_SET, CMD_EXIT };
static HANDLE CommandEvent, DoneEvent;
static LONG Command;
static ULONG CommandValue;

static DWORD WINAPI Worker(LPVOID Parameter)
{
    for (;;)
    {
        SetEvent(DoneEvent);
        WaitForSingleObject(CommandEvent, INFINITE);
        if (Command == CMD_EXIT)
            return 0;
        if (Command == CMD_GET)
            CommandValue = pTlsDynGetValue();
        else
            pTlsDynSetValue(CommandValue);
    }
}

static BOOL RunCommand(LONG Cmd, ULONG *Value)
{
    Command = Cmd;
    CommandValue = *Value;
    SetEvent(CommandEvent);
    if (WaitForSingleObject(DoneEvent, 10000) != WAIT_OBJECT_0)
        return FALSE;
    *Value = CommandValue;
    return TRUE;
}

static ULONG WorkerGet(VOID)
{
    ULONG Value = 0xDEAD;
    ok(RunCommand(CMD_GET, &Value), "The worker did not answer\n");
    return Value;
}

static VOID WorkerSet(ULONG Value)
{
    ok(RunCommand(CMD_SET, &Value), "The worker did not answer\n");
}

static DWORD WINAPI NewThread(LPVOID Parameter)
{
    return Parameter ? AFTER_UNLOAD_CODE : pTlsDynGetValue();
}

static DWORD RunThread(LPVOID Parameter)
{
    HANDLE hThread;
    DWORD Code = 0xDEAD;

    hThread = CreateThread(NULL, 0, NewThread, Parameter, 0, NULL);
    ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!hThread)
        return Code;
    ok(WaitForSingleObject(hThread, 10000) == WAIT_OBJECT_0, "The thread did not finish\n");
    ok(GetExitCodeThread(hThread, &Code), "GetExitCodeThread failed: %lu\n", GetLastError());
    CloseHandle(hThread);
    return Code;
}

static HMODULE LoadTlsDll(VOID)
{
    HMODULE hDll;

    hDll = LoadLibraryW(L"kernel32_tlsdyn.dll");
    ok(hDll != NULL, "kernel32_tlsdyn.dll did not load: %lu\n", GetLastError());
    if (!hDll)
        return NULL;
    pTlsDynGetValue = (FN_TlsDynGetValue)GetProcAddress(hDll, "TlsDynGetValue");
    pTlsDynSetValue = (FN_TlsDynSetValue)GetProcAddress(hDll, "TlsDynSetValue");
    ok(pTlsDynGetValue && pTlsDynSetValue, "Exports missing\n");
    if (!pTlsDynGetValue || !pTlsDynSetValue)
    {
        FreeLibrary(hDll);
        return NULL;
    }
    return hDll;
}

START_TEST(DynamicTls)
{
    FN_TlsDynGetEvents pTlsDynGetEvents;
    FN_TlsDynSetLog pTlsDynSetLog;
    static char HostLog[64];
    static LONG HostCount;
    HANDLE hWorker;
    HMODULE hDll;
    ULONG Value;
    PCSTR Events;
    size_t Length;

    CommandEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    DoneEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(CommandEvent && DoneEvent, "CreateEventW failed: %lu\n", GetLastError());
    if (!CommandEvent || !DoneEvent)
        return;

    /* A thread that has initialized (its TLS too) before the load */
    hWorker = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    ok(hWorker != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!hWorker)
        return;
    ok(WaitForSingleObject(DoneEvent, 10000) == WAIT_OBJECT_0, "The worker did not start\n");

    hDll = LoadTlsDll();
    if (!hDll)
        goto Cleanup;

    /* TLS callbacks run for the process attach, before DllMain */
    pTlsDynGetEvents = (FN_TlsDynGetEvents)GetProcAddress(hDll, "TlsDynGetEvents");
    pTlsDynSetLog = (FN_TlsDynSetLog)GetProcAddress(hDll, "TlsDynSetLog");
    ok(pTlsDynGetEvents && pTlsDynSetLog, "Exports missing\n");
    Events = pTlsDynGetEvents ? pTlsDynGetEvents() : "";
    ok(!strncmp(Events, "T1M1", 4), "Events at load: '%s'\n", Events);

    /* The loading thread sees the template */
    Value = pTlsDynGetValue();
    ok(Value == TEMPLATE_VALUE, "Loading thread: 0x%lx\n", Value);
    pTlsDynSetValue(0xAAAA);
    ok(pTlsDynGetValue() == 0xAAAA, "Loading thread after set: 0x%lx\n", pTlsDynGetValue());

    /* The thread that existed before the load has its own copy of the template */
    ok(WorkerGet() == TEMPLATE_VALUE, "Thread started before the load\n");
    WorkerSet(0x0D0D);
    ok(WorkerGet() == 0x0D0D, "Thread started before the load, after its set\n");
    ok(pTlsDynGetValue() == 0xAAAA, "The other thread's set changed this thread's copy: 0x%lx\n", pTlsDynGetValue());

    /* A thread started after the load gets the template too */
    Value = RunThread(NULL);
    ok(Value == TEMPLATE_VALUE, "Thread started after the load: 0x%lx\n", Value);

    /* The unload runs the TLS callbacks, then DllMain, for the process detach */
    if (pTlsDynSetLog)
        pTlsDynSetLog(HostLog, &HostCount);
    ok(FreeLibrary(hDll), "FreeLibrary failed: %lu\n", GetLastError());
    ok(GetModuleHandleW(L"kernel32_tlsdyn.dll") == NULL, "The DLL is still loaded\n");
    Length = strlen(HostLog);
    ok(Length >= 4 && !strcmp(HostLog + Length - 4, "T0M0"), "Events at unload: '%s'\n", HostLog);

    /* A thread started after the unload does not get the gone DLL's TLS */
    Value = RunThread((LPVOID)1);
    ok(Value == AFTER_UNLOAD_CODE, "Thread started after the unload: 0x%lx\n", Value);

    /* A second load starts from the template again, in the old thread and the loading one */
    hDll = LoadTlsDll();
    if (!hDll)
        goto Cleanup;
    ok(pTlsDynGetValue() == TEMPLATE_VALUE, "Loading thread after a reload: 0x%lx\n", pTlsDynGetValue());
    ok(WorkerGet() == TEMPLATE_VALUE, "Thread started before the first load, after a reload\n");
    WorkerSet(0x0E0E);
    ok(WorkerGet() == 0x0E0E, "Thread started before the first load, after a reload and a set\n");
    ok(pTlsDynGetValue() == TEMPLATE_VALUE, "The other thread's set changed this thread's copy: 0x%lx\n", pTlsDynGetValue());
    ok(FreeLibrary(hDll), "FreeLibrary failed: %lu\n", GetLastError());

Cleanup:
    Command = CMD_EXIT;
    SetEvent(CommandEvent);
    ok(WaitForSingleObject(hWorker, 10000) == WAIT_OBJECT_0, "The worker did not exit\n");
    CloseHandle(hWorker);
    CloseHandle(CommandEvent);
    CloseHandle(DoneEvent);
}
