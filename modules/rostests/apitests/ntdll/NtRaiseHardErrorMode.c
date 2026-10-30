/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NtRaiseHardError returns at once when the process or the thread asks for no critical-error UI
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The loader's "Entry Point Not Found" box is a hard error. A program that sets SEM_FAILCRITICALERRORS,
 * for the process (SetErrorMode) or for one thread (SetThreadErrorMode, which the .NET runtime uses around
 * its native library loads), gets no box and a failed load. A 32-bit thread on the 64-bit system keeps its
 * thread error mode in the 32-bit TEB, which the kernel does not read; the WoW64 layer has to carry it.
 * Only the suppressed cases can be tested: without the mode a box waits for a click.
 */

#include "precomp.h"

typedef struct
{
    BOOL ThreadMode;
    NTSTATUS Status;
    ULONG Response;
} RAISE;

static DWORD WINAPI RaiseThread(PVOID Context)
{
    RAISE *Raise = Context;
    ULONG Old;
    UNICODE_STRING EntryPoint, Dll;
    ULONG_PTR Parameters[2];

    if (Raise->ThreadMode)
        RtlSetThreadErrorMode(RTL_SEM_FAILCRITICALERRORS, &Old);
    /* the error the loader raises for an unresolved import: the entry point name and the DLL name */
    RtlInitUnicodeString(&EntryPoint, L"NoSuchEntryPoint");
    RtlInitUnicodeString(&Dll, L"NoSuchDll.dll");
    Parameters[0] = (ULONG_PTR)&EntryPoint;
    Parameters[1] = (ULONG_PTR)&Dll;
    Raise->Status = NtRaiseHardError(STATUS_ENTRYPOINT_NOT_FOUND, 2, 3, Parameters, OptionOk, &Raise->Response);
    return 0;
}

/* returns FALSE when the call did not return in time (a box is up) */
static BOOL Raise(const char *Name, BOOL ProcessMode, BOOL ThreadMode)
{
    RAISE Raise;
    HANDLE Thread;
    UINT OldMode = 0;
    DWORD Wait;

    Raise.ThreadMode = ThreadMode;
    Raise.Status = 0xdeadbeef;
    Raise.Response = 0xdeadbeef;
    /* the test framework sets SEM_FAILCRITICALERRORS for the process: start from none */
    OldMode = SetErrorMode(ProcessMode ? SEM_FAILCRITICALERRORS : 0);

    Thread = CreateThread(NULL, 0, RaiseThread, &Raise, 0, NULL);
    ok(Thread != NULL, "%s: CreateThread failed: %lu\n", Name, GetLastError());
    if (!Thread)
        return TRUE;
    Wait = WaitForSingleObject(Thread, 15000);
    SetErrorMode(OldMode);

    ok(Wait == WAIT_OBJECT_0, "%s: NtRaiseHardError did not return (the error box is waiting)\n", Name);
    if (Wait != WAIT_OBJECT_0)
        return FALSE;

    ok(Raise.Status == STATUS_SUCCESS, "%s: status 0x%08lx\n", Name, Raise.Status);
    ok(Raise.Response == ResponseReturnToCaller, "%s: response %lu\n", Name, Raise.Response);
    CloseHandle(Thread);
    return TRUE;
}

START_TEST(NtRaiseHardErrorMode)
{
    if (!Raise("process mode", TRUE, FALSE))
        return;
    if (!Raise("thread mode", FALSE, TRUE))
        return;
    Raise("process and thread mode", TRUE, TRUE);
}
