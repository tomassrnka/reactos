/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for RTL_DEBUG_INFORMATION
 * COPYRIGHT:   Copyright 2020 Mark Jansen (mark.jansen@reactos.org)
 */

#include "precomp.h"

#ifndef _WIN64
C_ASSERT(sizeof(RTL_DEBUG_INFORMATION) == 0x68);
#endif

static void Test_Buffersizes()
{
    PRTL_DEBUG_INFORMATION Buffer;
    NTSTATUS Status;

    Buffer = RtlCreateQueryDebugBuffer(0, FALSE);
    ok(Buffer != NULL, "Unable to create default buffer\n");
    if (!Buffer)
        return;

    ok_ptr(Buffer->ViewBaseClient, Buffer);
    ok_hex(Buffer->Flags, 0);
    ok(Buffer->OffsetFree == sizeof(*Buffer) || Buffer->OffsetFree == 0x60, "Expected %u or %u, got %lu\n", sizeof(*Buffer), 0x60, Buffer->OffsetFree);
    ok_hex(Buffer->CommitSize, 0x1000);
    ok_hex(Buffer->ViewSize, 0x400000);

    Status = RtlDestroyQueryDebugBuffer(Buffer);
    ok_hex(Status, STATUS_SUCCESS);

    Buffer = RtlCreateQueryDebugBuffer(1, FALSE);
    ok(Buffer != NULL, "Unable to create default buffer\n");
    if (!Buffer)
        return;

    ok_ptr(Buffer->ViewBaseClient, Buffer);
    ok_hex(Buffer->Flags, 0);
    ok(Buffer->OffsetFree == sizeof(*Buffer) || Buffer->OffsetFree == 0x60, "Expected %u or %u, got %lu\n", sizeof(*Buffer), 0x60, Buffer->OffsetFree);
    ok_hex(Buffer->CommitSize, 0x1000);
    ok_hex(Buffer->ViewSize, 0x1000);

    Status = RtlDestroyQueryDebugBuffer(Buffer);
    ok_hex(Status, STATUS_SUCCESS);

    Buffer = RtlCreateQueryDebugBuffer(0x1000, FALSE);
    ok(Buffer != NULL, "Unable to create default buffer\n");
    if (!Buffer)
        return;

    ok_ptr(Buffer->ViewBaseClient, Buffer);
    ok_hex(Buffer->Flags, 0);
    ok(Buffer->OffsetFree == sizeof(*Buffer) || Buffer->OffsetFree == 0x60, "Expected %u or %u, got %lu\n", sizeof(*Buffer), 0x60, Buffer->OffsetFree);
    ok_hex(Buffer->CommitSize, 0x1000);
    ok_hex(Buffer->ViewSize, 0x1000);

    Status = RtlDestroyQueryDebugBuffer(Buffer);
    ok_hex(Status, STATUS_SUCCESS);

    Buffer = RtlCreateQueryDebugBuffer(0x1001, FALSE);
    ok(Buffer != NULL, "Unable to create default buffer\n");
    if (!Buffer)
        return;

    ok_ptr(Buffer->ViewBaseClient, Buffer);
    ok_hex(Buffer->Flags, 0);
    ok(Buffer->OffsetFree == sizeof(*Buffer) || Buffer->OffsetFree == 0x60, "Expected %u or %u, got %lu\n", sizeof(*Buffer), 0x60, Buffer->OffsetFree);
    ok_hex(Buffer->CommitSize, 0x1000);
    ok_hex(Buffer->ViewSize, 0x2000);

    Status = RtlDestroyQueryDebugBuffer(Buffer);
    ok_hex(Status, STATUS_SUCCESS);

#ifndef _WIN64
    /* A 64-bit process has the address space for 2 GB, on Windows as well */
    Buffer = RtlCreateQueryDebugBuffer(0x7fffffff, FALSE);
    ok(Buffer == NULL, "Got a valid thing?\n");
    if (Buffer)
    {
        Status = RtlDestroyQueryDebugBuffer(Buffer);
        ok_hex(Status, STATUS_SUCCESS);
    }
#endif
}


static void Test_ProcessModules(void)
{
    PRTL_DEBUG_INFORMATION Buffer;
    NTSTATUS Status;
    ULONG RequiredSize = 0;
    PRTL_PROCESS_MODULES ExpectedModules;

    Buffer = RtlCreateQueryDebugBuffer(0, FALSE);
    ok(Buffer != NULL, "Unable to create default buffer\n");
    if (!Buffer)
        return;

    Status = LdrQueryProcessModuleInformation(NULL, 0, &RequiredSize);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    RequiredSize;
    ExpectedModules = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, RequiredSize);

    Status = LdrQueryProcessModuleInformation(ExpectedModules, RequiredSize, &RequiredSize);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {

        Status = RtlQueryProcessDebugInformation(GetCurrentProcessId(), RTL_DEBUG_QUERY_MODULES, Buffer);
        ok_hex(Status, STATUS_SUCCESS);
        if (SUCCEEDED(Status))
        {
            ok(!memcmp(ExpectedModules, Buffer->Modules, RequiredSize), "Unexpected difference!\n");
        }
    }
    if (ExpectedModules)
        HeapFree(GetProcessHeap(), 0, ExpectedModules);

    Status = RtlDestroyQueryDebugBuffer(Buffer);
    ok_hex(Status, STATUS_SUCCESS);
}


/* System DLLs the child loads, so that its module list needs more than one page */
static const char *ChildDlls[] =
{
    "advapi32.dll", "user32.dll", "gdi32.dll", "shell32.dll", "shlwapi.dll",
    "ole32.dll", "oleaut32.dll", "comctl32.dll", "comdlg32.dll", "ws2_32.dll",
    "crypt32.dll", "wininet.dll", "setupapi.dll", "version.dll", "winmm.dll",
    "rpcrt4.dll", "secur32.dll", "imm32.dll", "msvcrt.dll", "urlmon.dll",
    "dbghelp.dll", "netapi32.dll", "iphlpapi.dll", "userenv.dll", "uxtheme.dll",
    "wintrust.dll", "mpr.dll", "psapi.dll", "winspool.drv", "msacm32.dll",
};

static void RunModulesChild(const char *EventName)
{
    HANDLE Event;
    ULONG i, Loaded = 0;

    for (i = 0; i < ARRAYSIZE(ChildDlls); i++)
    {
        if (LoadLibraryA(ChildDlls[i]))
            Loaded++;
    }

    /* Report readiness only with the DLLs loaded; the exit code says how many */
    if (Loaded == ARRAYSIZE(ChildDlls))
    {
        Event = OpenEventA(EVENT_MODIFY_STATE, FALSE, EventName);
        if (Event)
        {
            SetEvent(Event);
            CloseHandle(Event);
            /* Wait for the parent to terminate this process */
            Sleep(INFINITE);
        }
    }

    /* Leave without the test summary, it would mix into the parent's output */
    ExitProcess(Loaded + 1);
}

static void Test_RemoteProcessModules(void)
{
    char ExePath[MAX_PATH], CmdLine[MAX_PATH + 100], EventName[64];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    PRTL_DEBUG_INFORMATION Buffer;
    HANDLE Event, Handles[2];
    NTSTATUS Status;
    DWORD Wait, ExitCode = 0;

    GetModuleFileNameA(NULL, ExePath, ARRAYSIZE(ExePath));
    StringCbPrintfA(EventName, sizeof(EventName), "RtlDebugInformation_%lu", GetCurrentProcessId());
    StringCbPrintfA(CmdLine, sizeof(CmdLine), "\"%s\" RtlDebugInformation modules %s", ExePath, EventName);

    Event = CreateEventA(NULL, TRUE, FALSE, EventName);
    ok(Event != NULL, "CreateEventA failed with %lu\n", GetLastError());
    if (!Event)
        return;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        ok(0, "Event %s already exists\n", EventName);
        CloseHandle(Event);
        return;
    }

    if (!CreateProcessA(ExePath, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        ok(0, "CreateProcessA failed with %lu\n", GetLastError());
        CloseHandle(Event);
        return;
    }

    Handles[0] = Event;
    Handles[1] = ProcessInfo.hProcess;
    Wait = WaitForMultipleObjects(2, Handles, FALSE, 60000);
    if (Wait == WAIT_OBJECT_0 + 1)
    {
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        ok(0, "The child loaded only %lu of %u DLLs\n", ExitCode - 1, (unsigned)ARRAYSIZE(ChildDlls));
    }
    else
    {
        ok(Wait == WAIT_OBJECT_0, "The child did not get ready: wait returned %lu\n", Wait);
    }
    if (Wait == WAIT_OBJECT_0)
    {
        ULONG Count = 0;

        /* The default buffer takes the whole list; it is more than a page holds */
        Buffer = RtlCreateQueryDebugBuffer(0, FALSE);
        ok(Buffer != NULL, "Unable to create default buffer\n");
        if (Buffer)
        {
            Status = RtlQueryProcessDebugInformation(ProcessInfo.dwProcessId, RTL_DEBUG_QUERY_MODULES, Buffer);
            ok_hex(Status, STATUS_SUCCESS);
            ok(Buffer->Modules != NULL, "Default buffer: no module list\n");
            if (NT_SUCCESS(Status) && Buffer->Modules != NULL)
                Count = Buffer->Modules->NumberOfModules;
            /* The exe, ntdll, kernel32 and the DLLs above */
            ok(Count >= ARRAYSIZE(ChildDlls), "Default buffer: only %lu modules\n", Count);
            Status = RtlDestroyQueryDebugBuffer(Buffer);
            ok_hex(Status, STATUS_SUCCESS);
        }

        /* A buffer of one page cannot take the list: the call must fail, not
           report success without a module list */
        Buffer = RtlCreateQueryDebugBuffer(PAGE_SIZE, FALSE);
        ok(Buffer != NULL, "Unable to create a one-page buffer\n");
        if (Buffer)
        {
            Status = RtlQueryProcessDebugInformation(ProcessInfo.dwProcessId, RTL_DEBUG_QUERY_MODULES, Buffer);
            ok(!NT_SUCCESS(Status) || Buffer->Modules != NULL,
               "One-page buffer: status 0x%lx with no module list\n", Status);
            /* The list does not fit: a size error, not some other failure */
            ok(NT_SUCCESS(Status) || Status == STATUS_NO_MEMORY ||
               Status == STATUS_INFO_LENGTH_MISMATCH || Status == STATUS_BUFFER_TOO_SMALL,
               "One-page buffer: unexpected status 0x%lx\n", Status);
            if (NT_SUCCESS(Status) && Buffer->Modules != NULL)
            {
                ok(Buffer->Modules->NumberOfModules <=
                   (PAGE_SIZE - FIELD_OFFSET(RTL_PROCESS_MODULES, Modules)) / sizeof(RTL_PROCESS_MODULE_INFORMATION),
                   "One-page buffer: %lu modules cannot fit\n", Buffer->Modules->NumberOfModules);
            }
            Status = RtlDestroyQueryDebugBuffer(Buffer);
            ok_hex(Status, STATUS_SUCCESS);
        }
    }

    if (WaitForSingleObject(ProcessInfo.hProcess, 0) != WAIT_OBJECT_0)
    {
        ok(TerminateProcess(ProcessInfo.hProcess, 0), "TerminateProcess failed with %lu\n", GetLastError());
        ok(WaitForSingleObject(ProcessInfo.hProcess, 10000) == WAIT_OBJECT_0, "The child did not exit\n");
    }
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(Event);
}


static void Test_ProcessHeaps(void)
{
    PRTL_DEBUG_INFORMATION Buffer;
    HANDLE Heaps[64];
    ULONG Count, i;
    NTSTATUS Status;

    Buffer = RtlCreateQueryDebugBuffer(0, FALSE);
    ok(Buffer != NULL, "Unable to create default buffer\n");
    if (!Buffer)
        return;

    /* A private heap, so that the list is more than the process heap */
    HeapCreate(0, 0, 0);

    Count = RtlGetProcessHeaps(ARRAYSIZE(Heaps), Heaps);
    ok(Count >= 2 && Count <= ARRAYSIZE(Heaps), "RtlGetProcessHeaps returned %lu\n", Count);

    Status = RtlQueryProcessDebugInformation(GetCurrentProcessId(), RTL_DEBUG_QUERY_HEAPS, Buffer);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status) && Buffer->Heaps != NULL && Count <= ARRAYSIZE(Heaps))
    {
        ok_hex(Buffer->Flags, RTL_DEBUG_QUERY_HEAPS);
        ok_int(Buffer->Heaps->NumberOfHeaps, Count);
        for (i = 0; i < min(Count, Buffer->Heaps->NumberOfHeaps); i++)
            ok(Buffer->Heaps->Heaps[i].BaseAddress == Heaps[i], "heap %lu: got %p, expected %p\n", i, Buffer->Heaps->Heaps[i].BaseAddress, Heaps[i]);
    }
    else
    {
        ok(Buffer->Heaps != NULL, "No heaps\n");
    }

    /* Process 0 is no process */
    Status = RtlQueryProcessDebugInformation(0, RTL_DEBUG_QUERY_HEAPS, Buffer);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    Status = RtlDestroyQueryDebugBuffer(Buffer);
    ok_hex(Status, STATUS_SUCCESS);
}


START_TEST(RtlDebugInformation)
{
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "modules"))
    {
        RunModulesChild(argv[3]);
        return;
    }

    Test_Buffersizes();
    Test_ProcessModules();
    Test_RemoteProcessModules();
    Test_ProcessHeaps();
}
