/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Tests for the NtSetInformationThread API
 * COPYRIGHT:       Copyright 2020 George Bișoc <george.bisoc@reactos.org>
 */

#include "precomp.h"
#include <internal/ps_i.h>

static
void
Test_ThreadPriorityClass(void)
{
    NTSTATUS Status;
    KPRIORITY *Priority;

    Priority = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(KPRIORITY));
    if (Priority == NULL)
    {
        skip("Failed to allocate memory for thread priority variable!\n");
        return;
    }

    /* Assign a priority */
    *Priority = 11;

    /* Everything is NULL */
    Status = NtSetInformationThread(NULL,
                                    ThreadPriority,
                                    NULL,
                                    0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Give an invalid thread handle */
    Status = NtSetInformationThread(NULL,
                                    ThreadPriority,
                                    Priority,
                                    sizeof(KPRIORITY));
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Don't set any priority to the thread */
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadPriority,
                                    NULL,
                                    sizeof(KPRIORITY));
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* The information length is incorrect */
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadPriority,
                                    Priority,
                                    0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* The buffer is misaligned and information length is incorrect */
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadPriority,
                                    (PVOID)1,
                                    0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* The buffer is misaligned */
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadPriority,
                                    (PVOID)1,
                                    sizeof(KPRIORITY));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* The buffer is misaligned -- try with an alignment size of 2 */
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadPriority,
                                    (PVOID)2,
                                    sizeof(KPRIORITY));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Set the priority to the current thread */
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadPriority,
                                    Priority,
                                    sizeof(KPRIORITY));
    ok_hex(Status, STATUS_SUCCESS);

    /*
     * Set the priority as LOW_REALTIME_PRIORITY,
     * we don't have privileges to do so.
    */
    *Priority = LOW_REALTIME_PRIORITY;
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadPriority,
                                    Priority,
                                    sizeof(KPRIORITY));
    ok_hex(Status, STATUS_PRIVILEGE_NOT_HELD);

    HeapFree(GetProcessHeap(), 0, Priority);
}

static
void
Test_ThreadSetAlignmentProbe(void)
{
    ULONG InfoClass;

    /* Iterate over the process info classes and begin the tests */
    for (InfoClass = 0; InfoClass < _countof(PsThreadInfoClass); InfoClass++)
    {
        /* The buffer is misaligned */
        QuerySetThreadValidator(SET,
                                InfoClass,
                                (PVOID)(ULONG_PTR)1,
                                PsThreadInfoClass[InfoClass].RequiredSizeSET,
                                STATUS_DATATYPE_MISALIGNMENT);

        /* We set an invalid buffer address */
        QuerySetThreadValidator(SET,
                                InfoClass,
                                (PVOID)(ULONG_PTR)PsThreadInfoClass[InfoClass].AlignmentSET,
                                PsThreadInfoClass[InfoClass].RequiredSizeSET,
                                STATUS_ACCESS_VIOLATION);

        /* The information length is wrong */
        QuerySetThreadValidator(SET,
                                InfoClass,
                                (PVOID)(ULONG_PTR)PsThreadInfoClass[InfoClass].AlignmentSET,
                                PsThreadInfoClass[InfoClass].RequiredSizeSET - 1,
                                STATUS_INFO_LENGTH_MISMATCH);
    }
}

static
void
Test_ThreadZeroTlsCell(void)
{
    NTSTATUS Status;
    ULONG TlsIndex;
    PVOID *SavedSlots;
    PVOID GuardPage;
    PTEB Teb = NtCurrentTeb();

    /*
     * Install a committed-but-PAGE_NOACCESS user pointer as the current
     * thread's TlsExpansionSlots. Index TLS_MINIMUM_AVAILABLE selects the
     * first expansion slot. Without the kernel-side probe/SEH,
     * ThreadZeroTlsCell dereferenced this user-controlled pointer and the
     * resulting write faulted in the kernel (bugcheck / pointer-sized kernel
     * zero-write). With the fix the slot is probed under SEH, so the call
     * returns a status instead of taking the machine down.
     */
    GuardPage = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (GuardPage == NULL)
    {
        skip("Failed to reserve guard page for ThreadZeroTlsCell test\n");
        return;
    }

    SavedSlots = Teb->TlsExpansionSlots;
    Teb->TlsExpansionSlots = (PVOID *)GuardPage;

    TlsIndex = TLS_MINIMUM_AVAILABLE; /* first expansion slot */
    Status = NtSetInformationThread(GetCurrentThread(),
                                    ThreadZeroTlsCell,
                                    &TlsIndex,
                                    sizeof(ULONG));

    /* Restore the real vector before asserting (reaching here means the
       kernel did not fault, i.e. the probe/SEH fix is present). */
    Teb->TlsExpansionSlots = SavedSlots;
    VirtualFree(GuardPage, 0, MEM_RELEASE);

    ok(Status == STATUS_ACCESS_VIOLATION || Status == STATUS_SUCCESS,
       "ThreadZeroTlsCell with a bogus expansion vector returned 0x%lx\n",
       (unsigned long)Status);
}

START_TEST(NtSetInformationThread)
{
    Test_ThreadPriorityClass();
    Test_ThreadSetAlignmentProbe();
    Test_ThreadZeroTlsCell();
}
