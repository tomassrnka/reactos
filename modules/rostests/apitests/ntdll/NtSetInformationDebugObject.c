/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NtSetInformationDebugObject user-buffer handling (T56)
 *
 * Defensive contract test: a user buffer that is bad, or that goes bad while
 * the call is in flight, must produce a clean failure status. The kernel must
 * not fault while holding the debug object mutex (that would bugcheck).
 */

#include "precomp.h"

typedef struct _T56_KILL_INFO { ULONG KillProcessOnExit; } T56_KILL_INFO;

static volatile LONG g_Stop;
static PVOID g_Page;

static DWORD WINAPI FlipThread(PVOID Arg)
{
    DWORD Old;
    while (!g_Stop)
    {
        VirtualProtect(g_Page, 4096, PAGE_NOACCESS, &Old);
        VirtualProtect(g_Page, 4096, PAGE_READWRITE, &Old);
    }
    return 0;
}

START_TEST(NtSetInformationDebugObject)
{
    NTSTATUS Status;
    HANDLE Debug;
    OBJECT_ATTRIBUTES Oa;
    T56_KILL_INFO Info = { 1 };
    PVOID Unmapped;
    SIZE_T Size = 4096;
    ULONG i, Ret = 0;
    HANDLE Thread;
    DWORD Old;

    InitializeObjectAttributes(&Oa, NULL, 0, NULL, NULL);
    Status = NtCreateDebugObject(&Debug, DEBUG_OBJECT_ALL_ACCESS, &Oa, 0);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Valid call works */
    Status = NtSetInformationDebugObject(Debug, DebugObjectKillProcessOnExitInformation,
                                         &Info, sizeof(Info), &Ret);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Info.KillProcessOnExit = 0;
    Status = NtSetInformationDebugObject(Debug, DebugObjectKillProcessOnExitInformation,
                                         &Info, sizeof(Info), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* Unmapped / kernel pointer rejected cleanly */
    Unmapped = NULL;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Unmapped, 0, &Size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    NtFreeVirtualMemory(NtCurrentProcess(), &Unmapped, &Size, MEM_RELEASE);
    Status = NtSetInformationDebugObject(Debug, DebugObjectKillProcessOnExitInformation,
                                         Unmapped, sizeof(Info), NULL);
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);
    Status = NtSetInformationDebugObject(Debug, DebugObjectKillProcessOnExitInformation,
                                         (PVOID)(ULONG_PTR)(sizeof(PVOID) == 8 ? 0xFFFFF78000000000ULL : 0x80000000UL), sizeof(Info), NULL);
    ok(Status == STATUS_ACCESS_VIOLATION || Status == STATUS_DATATYPE_MISALIGNMENT,
       "Status = 0x%lx\n", Status);

    /* Race: buffer page toggles NOACCESS while the syscall runs.
     * Pre-fix this could fault under the object mutex. */
    g_Page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok(g_Page != NULL, "VirtualAlloc failed\n");
    if (g_Page)
    {
        Thread = CreateThread(NULL, 0, FlipThread, NULL, 0, NULL);
        ok(Thread != NULL, "CreateThread failed\n");
        for (i = 0; i < 200000; i++)
        {
            Status = NtSetInformationDebugObject(Debug, DebugObjectKillProcessOnExitInformation,
                                                 g_Page, sizeof(Info), NULL);
            if (Status != STATUS_SUCCESS && Status != STATUS_ACCESS_VIOLATION)
            {
                ok(0, "unexpected status 0x%lx\n", Status);
                break;
            }
        }
        g_Stop = 1;
        if (Thread)
        {
            WaitForSingleObject(Thread, INFINITE);
            CloseHandle(Thread);
        }
        VirtualProtect(g_Page, 4096, PAGE_READWRITE, &Old);
        trace("race loop done, last status 0x%lx\n", Status);
    }

    /* Lock must still be free: another call and the close must not hang */
    Info.KillProcessOnExit = 1;
    Status = NtSetInformationDebugObject(Debug, DebugObjectKillProcessOnExitInformation,
                                         &Info, sizeof(Info), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtClose(Debug);
    ok_ntstatus(Status, STATUS_SUCCESS);
}
