/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtGetNextThread rejecting a thread of another process
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

START_TEST(NtGetNextThread)
{
    NTSTATUS Status;
    HANDLE NextThread;
    WCHAR Path[MAX_PATH];
    STARTUPINFOW StartupInfo = { 0 };
    PROCESS_INFORMATION ProcessInfo = { 0 };

    StartupInfo.cb = sizeof(StartupInfo);

    /* Baseline: enumerating our own process from the start must succeed. */
    NextThread = NULL;
    Status = NtGetNextThread(NtCurrentProcess(), NULL,
                             THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &NextThread);
    ok(Status == STATUS_SUCCESS,
       "NtGetNextThread(self, NULL) returned 0x%lx\n", Status);
    ok(NextThread != NULL, "NtGetNextThread(self, NULL) gave a NULL handle\n");
    if (NextThread != NULL)
        NtClose(NextThread);

    /* Create a suspended child so we hold a thread handle of a DIFFERENT process. */
    GetModuleFileNameW(NULL, Path, RTL_NUMBER_OF(Path));
    if (!CreateProcessW(Path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                        &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcessW failed, error %lu\n", GetLastError());
        return;
    }

    /*
     * Cross-process: ProcessInfo.hThread belongs to the child, not to our process.
     * NtGetNextThread must reject it with STATUS_INVALID_PARAMETER instead of
     * resuming the walk of our own thread list from a foreign list entry (which
     * makes the helper interpret the child's EPROCESS list head as an ETHREAD).
     */
    NextThread = (HANDLE)(ULONG_PTR)0xdeadbeef;
    Status = NtGetNextThread(NtCurrentProcess(), ProcessInfo.hThread,
                             THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &NextThread);
    ok(Status == STATUS_INVALID_PARAMETER,
       "NtGetNextThread(self, foreign thread) returned 0x%lx, expected STATUS_INVALID_PARAMETER\n",
       Status);
    ok(NextThread == NULL || NextThread == (HANDLE)(ULONG_PTR)0xdeadbeef,
       "NtGetNextThread returned a thread handle on failure: %p\n", NextThread);
    if (Status == STATUS_SUCCESS &&
        NextThread != NULL && NextThread != (HANDLE)(ULONG_PTR)0xdeadbeef)
    {
        NtClose(NextThread);
    }

    /* Positive control: the child's own thread, queried through the child
     * process handle, must still enumerate. */
    NextThread = NULL;
    Status = NtGetNextThread(ProcessInfo.hProcess, NULL,
                             THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &NextThread);
    ok(Status == STATUS_SUCCESS,
       "NtGetNextThread(child, NULL) returned 0x%lx\n", Status);
    if (NextThread != NULL)
        NtClose(NextThread);

    TerminateProcess(ProcessInfo.hProcess, 0);
    WaitForSingleObject(ProcessInfo.hProcess, 10000);
    NtClose(ProcessInfo.hThread);
    NtClose(ProcessInfo.hProcess);
}
