/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Tests for the NtSetInformationProcess API
 * COPYRIGHT:       Copyright 2020 George Bișoc <george.bisoc@reactos.org>
 */

#include "precomp.h"
#include <internal/ps_i.h>

static
void
Test_ProcBasePriorityClass(void)
{
    NTSTATUS Status;

    /*
     * Assign a priority of HIGH_PRIORITY (see pstypes.h).
     * The function will fail with a status of STATUS_PRIVILEGE_NOT_HELD
     * as the process who executed the caller doesn't have the requested
     * privileges to perform the operation.
    */
    KPRIORITY BasePriority = HIGH_PRIORITY;

    /* Everything is NULL */
    Status = NtSetInformationProcess(NULL,
                                     ProcessBasePriority,
                                     NULL,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Set the base priority to an invalid process handle */
    Status = NtSetInformationProcess(NULL,
                                     ProcessBasePriority,
                                     &BasePriority,
                                     sizeof(KPRIORITY));
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Don't input anything to the caller but the length information is valid */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessBasePriority,
                                     NULL,
                                     sizeof(KPRIORITY));
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Give the base priority input to the caller but length is invalid */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessBasePriority,
                                     &BasePriority,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* The input buffer is misaligned and length information invalid */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessBasePriority,
                                     (PVOID)1,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* The input buffer is misaligned */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessBasePriority,
                                     (PVOID)1,
                                     sizeof(KPRIORITY));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* The input buffer is misaligned (try with an alignment of 2) */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessBasePriority,
                                     (PVOID)2,
                                     sizeof(KPRIORITY));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Set the base priority but we have lack privileges to do so */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessBasePriority,
                                     &BasePriority,
                                     sizeof(KPRIORITY));
    ok_hex(Status, STATUS_PRIVILEGE_NOT_HELD);

    /*
     * Assign a random priority value this time and
     * set the base priority to the current process.
    */
    BasePriority = 8;
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessBasePriority,
                                     &BasePriority,
                                     sizeof(KPRIORITY));
    ok_hex(Status, STATUS_SUCCESS);
}

static
void
Test_ProcRaisePriorityClass(void)
{
    NTSTATUS Status;

    /* Raise the priority as much as possible */
    ULONG RaisePriority = MAXIMUM_PRIORITY;

    /* Everything is NULL */
    Status = NtSetInformationProcess(NULL,
                                     ProcessRaisePriority,
                                     NULL,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* A invalid handle to process is given */
    Status = NtSetInformationProcess(NULL,
                                     ProcessRaisePriority,
                                     &RaisePriority,
                                     sizeof(ULONG));
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* The input buffer is misaligned and length information invalid */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessRaisePriority,
                                     (PVOID)1,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* A NULL buffer has been accessed */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessRaisePriority,
                                     NULL,
                                     sizeof(ULONG));
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* The input buffer is misaligned */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessRaisePriority,
                                     (PVOID)1,
                                     sizeof(ULONG));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* The input buffer is misaligned -- try with an alignment size of 2 */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessRaisePriority,
                                     (PVOID)2,
                                     sizeof(ULONG));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Raise the priority of the given current process */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessRaisePriority,
                                     &RaisePriority,
                                     sizeof(ULONG));
    ok_hex(Status, STATUS_SUCCESS);
}

static
void
Test_ProcForegroundBackgroundClass(void)
{
    NTSTATUS Status;
    PPROCESS_FOREGROUND_BACKGROUND ProcForeground;

    ProcForeground = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PROCESS_FOREGROUND_BACKGROUND));
    if (ProcForeground == NULL)
    {
        skip("Failed to allocate memory block from heap for PROCESS_FOREGROUND_BACKGROUND!\n");
        return;
    }

    /* As a test, set the foreground of the retrieved current process to FALSE */
    ProcForeground->Foreground = FALSE;

    /* Set everything NULL for the caller */
    Status = NtSetInformationProcess(NULL,
                                     ProcessForegroundInformation,
                                     NULL,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Give an invalid process handle (but the input buffer and length are correct) */
    Status = NtSetInformationProcess(NULL,
                                     ProcessForegroundInformation,
                                     ProcForeground,
                                     sizeof(PROCESS_FOREGROUND_BACKGROUND));
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Give a buffer data to the argument input as NULL */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessForegroundInformation,
                                     NULL,
                                     sizeof(PROCESS_FOREGROUND_BACKGROUND));
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Set the information process foreground with an incorrect buffer alignment and zero buffer length */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessForegroundInformation,
                                     (PVOID)1,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /*
     * Set the information process foreground with an incorrect buffer alignment but correct size length.
     * The function will return STATUS_ACCESS_VIOLATION as the alignment probe requirement is not performed
     * in this class.
     */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessForegroundInformation,
                                     (PVOID)1,
                                     sizeof(PROCESS_FOREGROUND_BACKGROUND));
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Set the foreground information to the current given process, we must expect the function should succeed */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessForegroundInformation,
                                     ProcForeground,
                                     sizeof(PROCESS_FOREGROUND_BACKGROUND));
    ok_hex(Status, STATUS_SUCCESS);

    /* Clear all the stuff */
    HeapFree(GetProcessHeap(), 0, ProcForeground);
}

static
void
Test_ProcessWx86InformationClass(void)
{
    NTSTATUS Status;
    ULONG VdmPower = 1;

    /* Everything is NULL */
    Status = NtSetInformationProcess(NULL,
                                     ProcessWx86Information,
                                     NULL,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Don't set anything to the process */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessWx86Information,
                                     NULL,
                                     sizeof(VdmPower));
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* The buffer is misaligned and information length is wrong */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessWx86Information,
                                     (PVOID)1,
                                     0);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* The buffer is misaligned */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessWx86Information,
                                     (PVOID)1,
                                     sizeof(VdmPower));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* The buffer is misaligned -- try with an alignment size of 2 */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessWx86Information,
                                     (PVOID)2,
                                     sizeof(VdmPower));
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* We do not have privileges to set the VDM power */
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessWx86Information,
                                     &VdmPower,
                                     sizeof(VdmPower));
    ok_hex(Status, STATUS_PRIVILEGE_NOT_HELD);
}

static
void
Test_ProcSetAlignmentProbe(void)
{
    ULONG InfoClass;

    /* Iterate over the process info classes and begin the tests */
    for (InfoClass = 0; InfoClass < _countof(PsProcessInfoClass); InfoClass++)
    {
        /* The buffer is misaligned */
        QuerySetProcessValidator(SET,
                                 InfoClass,
                                 (PVOID)(ULONG_PTR)1,
                                 PsProcessInfoClass[InfoClass].RequiredSizeSET,
                                 STATUS_DATATYPE_MISALIGNMENT);

        /* We set an invalid buffer address */
        QuerySetProcessValidator(SET,
                                 InfoClass,
                                 (PVOID)(ULONG_PTR)PsProcessInfoClass[InfoClass].AlignmentSET,
                                 PsProcessInfoClass[InfoClass].RequiredSizeSET,
                                 STATUS_ACCESS_VIOLATION);

        /* The information length is wrong */
        QuerySetProcessValidator(SET,
                                 InfoClass,
                                 (PVOID)(ULONG_PTR)PsProcessInfoClass[InfoClass].AlignmentSET,
                                 PsProcessInfoClass[InfoClass].RequiredSizeSET - 1,
                                 STATUS_INFO_LENGTH_MISMATCH);
    }
}

#define QUOTA_SELFBLOCK_CHILD_ENV   L"NTDLL_APITEST_QUOTA_SELFBLOCK_CHILD"
#define QUOTA_SELFBLOCK_HANDLES     3000
#define QUOTA_SELFBLOCK_REGIONS     32

static
NTSTATUS
QueryQuotaUsage(
    _Out_ PSIZE_T Paged,
    _Out_ PSIZE_T NonPaged)
{
    VM_COUNTERS VmCounters;
    NTSTATUS Status;

    RtlZeroMemory(&VmCounters, sizeof(VmCounters));
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessVmCounters,
                                       &VmCounters,
                                       sizeof(VmCounters),
                                       NULL);
    *Paged = VmCounters.QuotaPagedPoolUsage;
    *NonPaged = VmCounters.QuotaNonPagedPoolUsage;
    return Status;
}

/*
 * Runs in a child process: moves the process onto its own quota block while
 * it still holds quota charged against the shared default block, then
 * releases it and exits. Returns 0 on success, a step number otherwise.
 * An unfixed kernel bugchecks with QUOTA_UNDERFLOW instead of returning.
 */
static
int
QuotaSelfBlockChild(void)
{
    static HANDLE Handles[QUOTA_SELFBLOCK_HANDLES];
    PVOID Regions[QUOTA_SELFBLOCK_REGIONS];
    QUOTA_LIMITS Limits;
    BOOLEAN Old;
    SIZE_T PagedBefore, NonPagedBefore, PagedSwitch, NonPagedSwitch, PagedEnd, NonPagedEnd;
    NTSTATUS Status;
    ULONG i;

    Status = RtlAdjustPrivilege(SE_INCREASE_QUOTA_PRIVILEGE, TRUE, FALSE, &Old);
    if (!NT_SUCCESS(Status))
        return 1;

    /* Charges made before the switch: handle table growth, VADs and the like */
    for (i = 0; i < QUOTA_SELFBLOCK_HANDLES; i++)
    {
        Handles[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (Handles[i] == NULL)
            return 2;
    }
    for (i = 0; i < QUOTA_SELFBLOCK_REGIONS; i++)
    {
        Regions[i] = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (Regions[i] == NULL)
            return 3;
    }

    if (!NT_SUCCESS(QueryQuotaUsage(&PagedBefore, &NonPagedBefore)))
        return 4;

    /* Zero limits: a private block that otherwise matches the default one */
    RtlZeroMemory(&Limits, sizeof(Limits));
    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessQuotaLimits,
                                     &Limits,
                                     sizeof(Limits));
    if (Status != STATUS_SUCCESS)
        return 5;

    /* The process' own usage must be unaffected by the switch */
    if (!NT_SUCCESS(QueryQuotaUsage(&PagedSwitch, &NonPagedSwitch)))
        return 6;
    if (PagedSwitch != PagedBefore || NonPagedSwitch != NonPagedBefore)
        return 7;

    /* Return the pre-switch charges: this is what underflowed the new block */
    for (i = 0; i < QUOTA_SELFBLOCK_REGIONS; i++)
    {
        if (!VirtualFree(Regions[i], 0, MEM_RELEASE))
            return 8;
    }
    for (i = 0; i < QUOTA_SELFBLOCK_HANDLES; i++)
    {
        if (!CloseHandle(Handles[i]))
            return 9;
    }

    if (!NT_SUCCESS(QueryQuotaUsage(&PagedEnd, &NonPagedEnd)))
        return 10;
    if (NonPagedEnd >= NonPagedSwitch)
        return 11;

    /* Exiting returns the remaining charges (handle table) to the block too */
    return 0;
}

static
void
Test_ProcQuotaSelfBlock(void)
{
    WCHAR Path[MAX_PATH];
    WCHAR CmdLine[MAX_PATH + 64];
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    DWORD ExitCode = 0xdeadbeef;
    BOOL Ret;

    if (!GetModuleFileNameW(NULL, Path, _countof(Path)))
    {
        skip(FALSE, "GetModuleFileNameW failed\n");
        return;
    }
    wcscpy(CmdLine, L"\"");
    wcscat(CmdLine, Path);
    wcscat(CmdLine, L"\" NtSetInformationProcess");
    SetEnvironmentVariableW(QUOTA_SELFBLOCK_CHILD_ENV, L"1");

    RtlZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    Ret = CreateProcessW(Path, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi);
    SetEnvironmentVariableW(QUOTA_SELFBLOCK_CHILD_ENV, NULL);
    ok(Ret, "CreateProcess failed with %lu\n", GetLastError());
    if (!Ret)
        return;

    WaitForSingleObject(Pi.hProcess, INFINITE);
    GetExitCodeProcess(Pi.hProcess, &ExitCode);
    ok_dec(ExitCode, 0);
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);
}

START_TEST(NtSetInformationProcess)
{
    if (GetEnvironmentVariableW(QUOTA_SELFBLOCK_CHILD_ENV, NULL, 0) != 0)
    {
        ExitProcess(QuotaSelfBlockChild());
    }

    Test_ProcForegroundBackgroundClass();
    Test_ProcBasePriorityClass();
    Test_ProcRaisePriorityClass();
    Test_ProcessWx86InformationClass();
    Test_ProcSetAlignmentProbe();
    Test_ProcQuotaSelfBlock();
}
