/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Tests for the NtQueryInformationProcess API
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 *                  George Bișoc <george.bisoc@reactos.org>
 */

#include "precomp.h"
#include <internal/ps_i.h>

static LARGE_INTEGER TestStartTime;

static
void
Test_ProcessTimes(void)
{
#define SPIN_TIME 1000000
    NTSTATUS Status;
    KERNEL_USER_TIMES Times1;
    KERNEL_USER_TIMES Times2;
    ULONG Length;
    LARGE_INTEGER Time1, Time2;

    /* Everything is NULL */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessTimes,
                                       NULL,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Right size, invalid process */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessTimes,
                                       NULL,
                                       sizeof(KERNEL_USER_TIMES),
                                       NULL);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Valid process, no buffer */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       NULL,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer, wrong size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       (PVOID)2,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer, correct size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       (PVOID)2,
                                       sizeof(KERNEL_USER_TIMES),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Buffer too small */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       NULL,
                                       sizeof(KERNEL_USER_TIMES) - 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Right buffer size but NULL pointer */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       NULL,
                                       sizeof(KERNEL_USER_TIMES),
                                       NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Buffer too large */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       NULL,
                                       sizeof(KERNEL_USER_TIMES) + 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Buffer too small, ask for length */
    Length = 0x55555555;
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       NULL,
                                       sizeof(KERNEL_USER_TIMES) - 1,
                                       &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok_dec(Length, 0x55555555);

    Status = NtQuerySystemTime(&Time1);
    ok_hex(Status, STATUS_SUCCESS);

    /* Do some busy waiting to increase UserTime */
    do
    {
        Status = NtQuerySystemTime(&Time2);
        if (!NT_SUCCESS(Status))
        {
            ok(0, "NtQuerySystemTime failed with %lx\n", Status);
            break;
        }
    } while (Time2.QuadPart - Time1.QuadPart < SPIN_TIME);

    /* Valid parameters, no return length */
    Status = NtQuerySystemTime(&Time1);
    ok_hex(Status, STATUS_SUCCESS);

    RtlFillMemory(&Times1, sizeof(Times1), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       &Times1,
                                       sizeof(KERNEL_USER_TIMES),
                                       NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Times1.CreateTime.QuadPart < TestStartTime.QuadPart,
       "CreateTime is %I64u, expected < %I64u\n", Times1.CreateTime.QuadPart, TestStartTime.QuadPart);
    ok(Times1.CreateTime.QuadPart > TestStartTime.QuadPart - 100000000LL,
       "CreateTime is %I64u, expected > %I64u\n", Times1.CreateTime.QuadPart, TestStartTime.QuadPart - 100000000LL);
    ok(Times1.ExitTime.QuadPart == 0,
       "ExitTime is %I64u, expected 0\n", Times1.ExitTime.QuadPart);
    ok(Times1.KernelTime.QuadPart != 0, "KernelTime is 0\n");
    ros_skip_flaky
    ok(Times1.UserTime.QuadPart != 0, "UserTime is 0\n");

    /* Do some busy waiting to increase UserTime */
    do
    {
        Status = NtQuerySystemTime(&Time2);
        if (!NT_SUCCESS(Status))
        {
            ok(0, "NtQuerySystemTime failed with %lx\n", Status);
            break;
        }
    } while (Time2.QuadPart - Time1.QuadPart < SPIN_TIME);

    /* Again, this time with a return length */
    Length = 0x55555555;
    RtlFillMemory(&Times2, sizeof(Times2), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessTimes,
                                       &Times2,
                                       sizeof(KERNEL_USER_TIMES),
                                       &Length);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(Length, sizeof(KERNEL_USER_TIMES));
    ok(Times1.CreateTime.QuadPart == Times2.CreateTime.QuadPart,
       "CreateTimes not equal: %I64u != %I64u\n", Times1.CreateTime.QuadPart, Times2.CreateTime.QuadPart);
    ok(Times2.ExitTime.QuadPart == 0,
       "ExitTime is %I64u, expected 0\n", Times2.ExitTime.QuadPart);
    ok(Times2.KernelTime.QuadPart != 0, "KernelTime is 0\n");
    ok(Times2.UserTime.QuadPart != 0, "UserTime is 0\n");

    /* Compare the two sets of KernelTime/UserTime values */
    Status = NtQuerySystemTime(&Time2);
    ok_hex(Status, STATUS_SUCCESS);
    /* Time values must have increased */
    ok(Times2.KernelTime.QuadPart > Times1.KernelTime.QuadPart,
       "KernelTime values inconsistent. Expected %I64u > %I64u\n", Times2.KernelTime.QuadPart, Times1.KernelTime.QuadPart);
    ros_skip_flaky
    ok(Times2.UserTime.QuadPart > Times1.UserTime.QuadPart,
       "UserTime values inconsistent. Expected %I64u > %I64u\n", Times2.UserTime.QuadPart, Times1.UserTime.QuadPart);
    /* They can't have increased by more than wall clock time difference (we only have one thread) */
    ros_skip_flaky
    ok(Times2.KernelTime.QuadPart - Times1.KernelTime.QuadPart < Time2.QuadPart - Time1.QuadPart,
       "KernelTime values inconsistent. Expected %I64u - %I64u < %I64u\n",
       Times2.KernelTime.QuadPart, Times1.KernelTime.QuadPart, Time2.QuadPart - Time1.QuadPart);
    ok(Times2.UserTime.QuadPart - Times1.UserTime.QuadPart < Time2.QuadPart - Time1.QuadPart,
       "UserTime values inconsistent. Expected %I64u - %I64u < %I64u\n",
       Times2.UserTime.QuadPart, Times1.UserTime.QuadPart, Time2.QuadPart - Time1.QuadPart);

    trace("KernelTime1 = %I64u\n", Times1.KernelTime.QuadPart);
    trace("KernelTime2 = %I64u\n", Times2.KernelTime.QuadPart);
    trace("UserTime1 = %I64u\n", Times1.UserTime.QuadPart);
    trace("UserTime2 = %I64u\n", Times2.UserTime.QuadPart);

    /* TODO: Test ExitTime on a terminated process */
#undef SPIN_TIME
}

static
void
Test_ProcessBasicInformation(void)
{
    NTSTATUS Status;
    ULONG Length;
    PROCESS_BASIC_INFORMATION BasicInfo;

    /* Everything is NULL */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessBasicInformation,
                                       NULL,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Right size, invalid process handle */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessBasicInformation,
                                       NULL,
                                       sizeof(BasicInfo),
                                       NULL);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Valid process handle, no buffer */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       NULL,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer, wrong size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       (PVOID)2,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer, correct size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       (PVOID)2,
                                       sizeof(BasicInfo),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Buffer too small */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       NULL,
                                       sizeof(BasicInfo) - 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Right buffer size but NULL pointer */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       NULL,
                                       sizeof(BasicInfo),
                                       NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Buffer too large */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       NULL,
                                       sizeof(BasicInfo) + 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Buffer too small, ask for length */
    Length = 0x55555555;
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       NULL,
                                       sizeof(BasicInfo) - 1,
                                       &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok_dec(Length, 0x55555555);

    /* Valid parameters, no return length */
    RtlFillMemory(&BasicInfo, sizeof(BasicInfo), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       &BasicInfo,
                                       sizeof(BasicInfo),
                                       NULL);
    ok_hex(Status, STATUS_SUCCESS);

    /* Trace the returned data (1) */
    trace("[1] BasicInfo.ExitStatus = %lx\n", BasicInfo.ExitStatus);
    trace("[1] BasicInfo.PebBaseAddress = %p\n", BasicInfo.PebBaseAddress);
    trace("[1] BasicInfo.AffinityMask = %Ix\n", BasicInfo.AffinityMask);
    trace("[1] BasicInfo.BasePriority = %ld\n", BasicInfo.BasePriority);
    trace("[1] BasicInfo.UniqueProcessId = %Iu\n", BasicInfo.UniqueProcessId);
    trace("[1] BasicInfo.InheritedFromUniqueProcessId = %Iu\n", BasicInfo.InheritedFromUniqueProcessId);

    /* Again, this time with a return length */
    Length = 0x55555555;
    RtlFillMemory(&BasicInfo, sizeof(BasicInfo), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessBasicInformation,
                                       &BasicInfo,
                                       sizeof(BasicInfo),
                                       &Length);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(Length, sizeof(BasicInfo));

    /* Trace the returned data (2) */
    trace("[2] BasicInfo.ExitStatus = %lx\n", BasicInfo.ExitStatus);
    trace("[2] BasicInfo.PebBaseAddress = %p\n", BasicInfo.PebBaseAddress);
    trace("[2] BasicInfo.AffinityMask = %Ix\n", BasicInfo.AffinityMask);
    trace("[2] BasicInfo.BasePriority = %ld\n", BasicInfo.BasePriority);
    trace("[2] BasicInfo.UniqueProcessId = %Iu\n", BasicInfo.UniqueProcessId);
    trace("[2] BasicInfo.InheritedFromUniqueProcessId = %Iu\n", BasicInfo.InheritedFromUniqueProcessId);
}

static
void
Test_ProcessQuotaLimits(void)
{
    NTSTATUS Status;
    ULONG Length;
    QUOTA_LIMITS QuotaLimits;

    /* Everything is NULL */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessQuotaLimits,
                                       NULL,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Right size, invalid process handle */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimits),
                                       NULL);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Valid process handle, no buffer */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer, wrong size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       (PVOID)2,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer, correct size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       (PVOID)2,
                                       sizeof(QuotaLimits),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Buffer too small */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimits) - 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Right buffer size but NULL pointer */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimits),
                                       NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Buffer too large */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimits) + 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Buffer too small, ask for length */
    Length = 0x55555555;
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimits) - 1,
                                       &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok_dec(Length, 0x55555555);

    /* Valid parameters, no return length */
    RtlFillMemory(&QuotaLimits, sizeof(QuotaLimits), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       &QuotaLimits,
                                       sizeof(QuotaLimits),
                                       NULL);
    ok_hex(Status, STATUS_SUCCESS);

    /* Trace the returned data (1) */
    trace("[1] QuotaLimits.PagedPoolLimit = %Iu\n", QuotaLimits.PagedPoolLimit);
    trace("[1] QuotaLimits.NonPagedPoolLimit = %Iu\n", QuotaLimits.NonPagedPoolLimit);
    trace("[1] QuotaLimits.MinimumWorkingSetSize = %Iu\n", QuotaLimits.MinimumWorkingSetSize);
    trace("[1] QuotaLimits.MaximumWorkingSetSize = %Iu\n", QuotaLimits.MaximumWorkingSetSize);
    trace("[1] QuotaLimits.PagefileLimit = %Iu\n", QuotaLimits.PagefileLimit);
    trace("[1] QuotaLimits.TimeLimit = %I64d\n", QuotaLimits.TimeLimit.QuadPart);

    /* Again, this time with a return length */
    Length = 0x55555555;
    RtlFillMemory(&QuotaLimits, sizeof(QuotaLimits), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       &QuotaLimits,
                                       sizeof(QuotaLimits),
                                       &Length);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(Length, sizeof(QuotaLimits));

    /* Trace the returned data (2) */
    trace("[2] QuotaLimits.PagedPoolLimit = %Iu\n", QuotaLimits.PagedPoolLimit);
    trace("[2] QuotaLimits.NonPagedPoolLimit = %Iu\n", QuotaLimits.NonPagedPoolLimit);
    trace("[2] QuotaLimits.MinimumWorkingSetSize = %Iu\n", QuotaLimits.MinimumWorkingSetSize);
    trace("[2] QuotaLimits.MaximumWorkingSetSize = %Iu\n", QuotaLimits.MaximumWorkingSetSize);
    trace("[2] QuotaLimits.PagefileLimit = %Iu\n", QuotaLimits.PagefileLimit);
    trace("[2] QuotaLimits.TimeLimit = %I64d\n", QuotaLimits.TimeLimit.QuadPart);
}

static
void
Test_ProcessQuotaLimitsEx(void)
{
    NTSTATUS Status;
    ULONG Length;
    QUOTA_LIMITS_EX QuotaLimitsEx;

    /* Right size, invalid process handle */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimitsEx),
                                       NULL);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Unaligned buffer, correct size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       (PVOID)2,
                                       sizeof(QuotaLimitsEx),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Buffer too small */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimitsEx) - 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Right buffer size but NULL pointer */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimitsEx),
                                       NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Buffer too large */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimitsEx) + 1,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Buffer too small, ask for length */
    Length = 0x55555555;
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       NULL,
                                       sizeof(QuotaLimitsEx) - 1,
                                       &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok_dec(Length, 0x55555555);

    /* Valid parameters, no return length */
    RtlFillMemory(&QuotaLimitsEx, sizeof(QuotaLimitsEx), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       &QuotaLimitsEx,
                                       sizeof(QuotaLimitsEx),
                                       NULL);
    ok_hex(Status, STATUS_SUCCESS);

    /* Trace the returned data (1) */
    trace("[1] QuotaLimitsEx.PagedPoolLimit = %Iu\n", QuotaLimitsEx.PagedPoolLimit);
    trace("[1] QuotaLimitsEx.NonPagedPoolLimit = %Iu\n", QuotaLimitsEx.NonPagedPoolLimit);
    trace("[1] QuotaLimitsEx.MinimumWorkingSetSize = %Iu\n", QuotaLimitsEx.MinimumWorkingSetSize);
    trace("[1] QuotaLimitsEx.MaximumWorkingSetSize = %Iu\n", QuotaLimitsEx.MaximumWorkingSetSize);
    trace("[1] QuotaLimitsEx.PagefileLimit = %Iu\n", QuotaLimitsEx.PagefileLimit);
    trace("[1] QuotaLimitsEx.TimeLimit = %I64d\n", QuotaLimitsEx.TimeLimit.QuadPart);
    //trace("[1] QuotaLimitsEx.WorkingSetLimit = %Iu\n", QuotaLimitsEx.WorkingSetLimit); // Not used on Win2k3
    trace("[1] QuotaLimitsEx.Flags = %lx\n", QuotaLimitsEx.Flags);
    trace("[1] QuotaLimitsEx.CpuRateLimit.RateData = %lx\n", QuotaLimitsEx.CpuRateLimit.RateData);

    /* Again, this time with a return length */
    Length = 0x55555555;
    RtlFillMemory(&QuotaLimitsEx, sizeof(QuotaLimitsEx), 0x55);
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessQuotaLimits,
                                       &QuotaLimitsEx,
                                       sizeof(QuotaLimitsEx),
                                       &Length);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(Length, sizeof(QuotaLimitsEx));

    /* Trace the returned data (2) */
    trace("[2] QuotaLimitsEx.PagedPoolLimit = %Iu\n", QuotaLimitsEx.PagedPoolLimit);
    trace("[2] QuotaLimitsEx.NonPagedPoolLimit = %Iu\n", QuotaLimitsEx.NonPagedPoolLimit);
    trace("[2] QuotaLimitsEx.MinimumWorkingSetSize = %Iu\n", QuotaLimitsEx.MinimumWorkingSetSize);
    trace("[2] QuotaLimitsEx.MaximumWorkingSetSize = %Iu\n", QuotaLimitsEx.MaximumWorkingSetSize);
    trace("[2] QuotaLimitsEx.PagefileLimit = %Iu\n", QuotaLimitsEx.PagefileLimit);
    trace("[2] QuotaLimitsEx.TimeLimit = %I64d\n", QuotaLimitsEx.TimeLimit.QuadPart);
    //trace("[2] QuotaLimitsEx.WorkingSetLimit = %Iu\n", QuotaLimitsEx.WorkingSetLimit); // Not used on Win2k3
    trace("[2] QuotaLimitsEx.Flags = %lx\n", QuotaLimitsEx.Flags);
    trace("[2] QuotaLimitsEx.CpuRateLimit.RateData = %lx\n", QuotaLimitsEx.CpuRateLimit.RateData);
}

static
void
Test_ProcessPriorityClassAlignment(void)
{
    NTSTATUS Status;
    PPROCESS_PRIORITY_CLASS ProcPriority;

    /* Allocate some memory for the priority class structure */
    ProcPriority = malloc(sizeof(PROCESS_PRIORITY_CLASS));
    if (ProcPriority == NULL)
    {
        skip("Failed to allocate memory for PROCESS_PRIORITY_CLASS!\n");
        return;
    }

    /*
     * Initialize the PriorityClass member to ensure the test won't randomly succeed (if such data is uninitialized).
     * Filling 85 to the data member makes sure that if the test fails continously then NtQueryInformationProcess()
     * didn't initialize the structure with data.
     */
    RtlFillMemory(&ProcPriority->PriorityClass, sizeof(ProcPriority->PriorityClass), 0x55);

    /* Unaligned buffer -- wrong size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessPriorityClass,
                                       (PVOID)1,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer -- correct size */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessPriorityClass,
                                       (PVOID)1,
                                       sizeof(PROCESS_PRIORITY_CLASS),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Unaligned buffer -- wrong size (but this time do with an alignment of 2) */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessPriorityClass,
                                       (PVOID)2,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Unaligned buffer -- correct size (but this time do with an alignment of 2) */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessPriorityClass,
                                       (PVOID)2,
                                       sizeof(PROCESS_PRIORITY_CLASS),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Do not care for the length but expect to return the priority class */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessPriorityClass,
                                       ProcPriority,
                                       sizeof(PROCESS_PRIORITY_CLASS),
                                       NULL);
    ok_hex(Status, STATUS_SUCCESS);

    /* Make sure the returned priority class is a valid number (non negative) but also it should be within the PROCESS_PRIORITY_CLASS range */
    ok(ProcPriority->PriorityClass > PROCESS_PRIORITY_CLASS_INVALID && ProcPriority->PriorityClass <= PROCESS_PRIORITY_CLASS_ABOVE_NORMAL,
       "Expected a valid number from priority class range but got %d\n", ProcPriority->PriorityClass);
    free(ProcPriority);
}

static
void
Test_ProcessWx86Information(void)
{
    NTSTATUS Status;
    ULONG VdmPower = 1, ReturnLength;

    /* Everything is NULL */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessWx86Information,
                                       NULL,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Given an invalid process handle */
    Status = NtQueryInformationProcess(NULL,
                                       ProcessWx86Information,
                                       &VdmPower,
                                       sizeof(VdmPower),
                                       NULL);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Don't query anything */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessWx86Information,
                                       NULL,
                                       sizeof(VdmPower),
                                       NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* The buffer is misaligned and information length is wrong */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessWx86Information,
                                       (PVOID)1,
                                       0,
                                       NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* The buffer is misaligned */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessWx86Information,
                                       (PVOID)1,
                                       sizeof(VdmPower),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* The buffer is misaligned -- try with an alignment size of 2 */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessWx86Information,
                                       (PVOID)2,
                                       sizeof(VdmPower),
                                       NULL);
    ok_hex(Status, STATUS_DATATYPE_MISALIGNMENT);

    /* Query the VDM power */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessWx86Information,
                                       &VdmPower,
                                       sizeof(VdmPower),
                                       NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(VdmPower == 0 || VdmPower == 1, "The VDM power value must be within the boundary between 0 and 1, not anything else! Got %lu\n", VdmPower);

    /* Same but with ReturnLength */
    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessWx86Information,
                                       &VdmPower,
                                       sizeof(VdmPower),
                                       &ReturnLength);
    ok_hex(Status, STATUS_SUCCESS);
    ok(ReturnLength != 0, "ReturnLength shouldn't be 0!\n");
    ok(VdmPower == 0 || VdmPower == 1, "The VDM power value must be within the boundary between 0 and 1, not anything else! Got %lu\n", VdmPower);

    /* Trace the VDM power value and returned length */
    trace("ReturnLength = %lu\n", ReturnLength);
    trace("VdmPower = %lu\n", VdmPower);
}

static
void
Test_ProcQueryAlignmentProbe(void)
{
    ULONG InfoClass;

    /* Iterate over the process info classes and begin the tests */
    for (InfoClass = 0; InfoClass < _countof(PsProcessInfoClass); InfoClass++)
    {
        /* The buffer is misaligned */
        QuerySetProcessValidator(QUERY,
                                 InfoClass,
                                 (PVOID)(ULONG_PTR)1,
                                 PsProcessInfoClass[InfoClass].RequiredSizeQUERY,
                                 STATUS_DATATYPE_MISALIGNMENT);

        /* We query an invalid buffer address */
        QuerySetProcessValidator(QUERY,
                                 InfoClass,
                                 (PVOID)(ULONG_PTR)PsProcessInfoClass[InfoClass].AlignmentQUERY,
                                 PsProcessInfoClass[InfoClass].RequiredSizeQUERY,
                                 STATUS_ACCESS_VIOLATION);

        /* The information length is wrong */
        QuerySetProcessValidator(QUERY,
                                 InfoClass,
                                 (PVOID)(ULONG_PTR)PsProcessInfoClass[InfoClass].AlignmentQUERY,
                                 PsProcessInfoClass[InfoClass].RequiredSizeQUERY - 1,
                                 STATUS_INFO_LENGTH_MISMATCH);
    }
}

/* The process list, in a heap buffer that grows as needed */
static
PSYSTEM_PROCESS_INFORMATION
QueryProcessList(VOID)
{
    PVOID Buffer;
    ULONG Size = 0x40000, Length, Try;
    NTSTATUS Status;

    for (Try = 0; Try < 8; Try++)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Buffer)
            break;
        Length = 0;
        Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Size, &Length);
        if (NT_SUCCESS(Status))
            return Buffer;
        HeapFree(GetProcessHeap(), 0, Buffer);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
        {
            ok_hex(Status, STATUS_SUCCESS);
            return NULL;
        }
        Size = max(Size * 2, Length + 0x10000);
    }
    ok(FALSE, "Cannot query the process list\n");
    return NULL;
}

typedef struct _PRIVATE_COMMIT
{
    /* ProcessVmCounters with a VM_COUNTERS_EX */
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
    /* The entry of the current process in SystemProcessInformation */
    SIZE_T SpiPagefileUsage;
    SIZE_T SpiPeakPagefileUsage;
    SIZE_T SpiPrivatePageCount;
} PRIVATE_COMMIT, *PPRIVATE_COMMIT;

static BOOL IsWow64;

/* Read the private commit charge of the current process through both queries */
static
BOOLEAN
QueryPrivateCommit(
    _Out_ PPRIVATE_COMMIT Commit,
    _In_ PCSTR What)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    VM_COUNTERS_EX Counters, ShortCounters;
    PSYSTEM_PROCESS_INFORMATION List, ProcessInfo;
    ULONG Length, Try;

    for (Try = 0; Try < 8; Try++)
    {
        /* Allocated before the counters are read, so that the buffer is in both */
        List = QueryProcessList();
        if (!List)
            return FALSE;

        RtlFillMemory(&Counters, sizeof(Counters), 0x55);
        Length = 0x55555555;
        Status = NtQueryInformationProcess(NtCurrentProcess(), ProcessVmCounters, &Counters, sizeof(Counters), &Length);
        ok_hex(Status, STATUS_SUCCESS);
        ok_dec(Length, sizeof(Counters));

        /* VM_COUNTERS has no PrivateUsage, and nothing is written after it */
        RtlFillMemory(&ShortCounters, sizeof(ShortCounters), 0x55);
        Length = 0x55555555;
        Status = NtQueryInformationProcess(NtCurrentProcess(), ProcessVmCounters, &ShortCounters, sizeof(VM_COUNTERS), &Length);
        ok_hex(Status, STATUS_SUCCESS);
        ok_dec(Length, sizeof(VM_COUNTERS));
        ok(ShortCounters.PrivateUsage == (SIZE_T)0x5555555555555555ULL, "%s: PrivateUsage written: %Iu\n",
           What, ShortCounters.PrivateUsage);
        ok(ShortCounters.PagefileUsage + 1024 * 1024 > Counters.PagefileUsage &&
           ShortCounters.PagefileUsage < Counters.PagefileUsage + 1024 * 1024,
           "%s: VM_COUNTERS PagefileUsage %Iu, VM_COUNTERS_EX %Iu\n", What, ShortCounters.PagefileUsage, Counters.PagefileUsage);

        /* A new process or thread may have grown the list meanwhile; then sample again */
        Status = NtQuerySystemInformation(SystemProcessInformation, List, (ULONG)HeapSize(GetProcessHeap(), 0, List), &Length);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            break;
        HeapFree(GetProcessHeap(), 0, List);
    }
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            HeapFree(GetProcessHeap(), 0, List);
        return FALSE;
    }

    for (ProcessInfo = List; ; )
    {
        if (ProcessInfo->UniqueProcessId == NtCurrentTeb()->ClientId.UniqueProcess)
            break;
        if (ProcessInfo->NextEntryOffset == 0)
        {
            ok(FALSE, "%s: the current process is not in the process list\n", What);
            HeapFree(GetProcessHeap(), 0, List);
            return FALSE;
        }
        ProcessInfo = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)ProcessInfo + ProcessInfo->NextEntryOffset);
    }
    Commit->PagefileUsage = Counters.PagefileUsage;
    Commit->PeakPagefileUsage = Counters.PeakPagefileUsage;
    Commit->PrivateUsage = Counters.PrivateUsage;
    Commit->SpiPagefileUsage = ProcessInfo->PagefileUsage;
    Commit->SpiPeakPagefileUsage = ProcessInfo->PeakPagefileUsage;
    Commit->SpiPrivatePageCount = ProcessInfo->PrivatePageCount;
    HeapFree(GetProcessHeap(), 0, List);

    /* The fields of one query are one value */
    ok(Commit->PrivateUsage == Commit->PagefileUsage, "%s: PrivateUsage %Iu, PagefileUsage %Iu\n",
       What, Commit->PrivateUsage, Commit->PagefileUsage);
    ok(Commit->PeakPagefileUsage >= Commit->PagefileUsage, "%s: PeakPagefileUsage %Iu, PagefileUsage %Iu\n",
       What, Commit->PeakPagefileUsage, Commit->PagefileUsage);
    ok(Commit->SpiPrivatePageCount == Commit->SpiPagefileUsage, "%s: SystemProcessInformation PrivatePageCount %Iu, PagefileUsage %Iu\n",
       What, Commit->SpiPrivatePageCount, Commit->SpiPagefileUsage);
    ok(Commit->SpiPeakPagefileUsage >= Commit->SpiPagefileUsage, "%s: SystemProcessInformation PeakPagefileUsage %Iu, PagefileUsage %Iu\n",
       What, Commit->SpiPeakPagefileUsage, Commit->SpiPagefileUsage);

    /* Both queries report the same charge, give or take what another thread commits meanwhile.
       Under WOW64 they differ by a constant amount. */
    if (!IsWow64)
    {
        ok(Commit->SpiPagefileUsage + 1024 * 1024 > Commit->PagefileUsage &&
           Commit->SpiPagefileUsage < Commit->PagefileUsage + 1024 * 1024,
           "%s: SystemProcessInformation PagefileUsage %Iu, ProcessVmCounters %Iu\n",
           What, Commit->SpiPagefileUsage, Commit->PagefileUsage);
    }
    return TRUE;
}

/* The charge must have changed by Size, give or take 1 MB (Windows also charges page tables,
   and other threads of the process may commit or free memory meanwhile) */
#define ok_delta(Before, After, Size, What) \
    ok((SSIZE_T)((After) - (Before)) > (SSIZE_T)(Size) - 1024 * 1024 && \
       (SSIZE_T)((After) - (Before)) < (SSIZE_T)(Size) + 1024 * 1024, \
       "%s: the charge changed by %Id bytes, expected %Id\n", What, (SSIZE_T)((After) - (Before)), (SSIZE_T)(Size))

/* Both queries must have changed by Size */
#define ok_commit_delta(Before, After, Size, What) \
    do { \
        ok_delta((Before).PrivateUsage, (After).PrivateUsage, Size, What); \
        ok_delta((Before).SpiPagefileUsage, (After).SpiPagefileUsage, Size, What " (SystemProcessInformation)"); \
    } while (0)

static
void
Test_ProcessVmCounters(void)
{
    NTSTATUS Status;
    PVOID Committed = NULL, Reserved = NULL;
    SIZE_T Size;
    PRIVATE_COMMIT Start, Previous, Commit;
    const SIZE_T Mb = 1024 * 1024;

    if (!IsWow64Process(GetCurrentProcess(), &IsWow64))
        IsWow64 = FALSE;

    /* The charge is in bytes */
    if (!QueryPrivateCommit(&Start, "start"))
        return;
    ok(Start.PrivateUsage >= 64 * 1024, "PrivateUsage %Iu is less than 64 KB\n", Start.PrivateUsage);
    ok((Start.PrivateUsage % PAGE_SIZE) == 0, "PrivateUsage %Iu is not a multiple of the page size\n", Start.PrivateUsage);
    ok((Start.SpiPagefileUsage % PAGE_SIZE) == 0, "SystemProcessInformation PagefileUsage %Iu is not a multiple of the page size\n",
       Start.SpiPagefileUsage);

    /* A committed reservation is charged in full, before it is touched */
    Size = 16 * Mb;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Committed, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !QueryPrivateCommit(&Commit, "16 MB reserved and committed"))
        goto Cleanup;
    ok_commit_delta(Start, Commit, 16 * Mb, "16 MB reserved and committed");
    Previous = Commit;

    /* A reservation alone is not charged (page tables aside) */
    Size = 16 * Mb;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Reserved, 0, &Size, MEM_RESERVE, PAGE_READWRITE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !QueryPrivateCommit(&Commit, "16 MB reserved"))
        goto Cleanup;
    ok_commit_delta(Previous, Commit, 0, "16 MB reserved");
    Previous = Commit;

    /* Committing part of it charges that part */
    Size = 8 * Mb;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Reserved, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !QueryPrivateCommit(&Commit, "8 MB committed"))
        goto Cleanup;
    ok_commit_delta(Previous, Commit, 8 * Mb, "8 MB committed");
    Previous = Commit;

    /* Decommitting returns the charge */
    Size = 4 * Mb;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Reserved, &Size, MEM_DECOMMIT);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !QueryPrivateCommit(&Commit, "4 MB decommitted"))
        goto Cleanup;
    ok_commit_delta(Previous, Commit, -(SSIZE_T)(4 * Mb), "4 MB decommitted");

    /* Releasing both returns to the start; the peak stays */
    Size = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Reserved, &Size, MEM_RELEASE);
    ok_hex(Status, STATUS_SUCCESS);
    Reserved = NULL;
    Size = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Committed, &Size, MEM_RELEASE);
    ok_hex(Status, STATUS_SUCCESS);
    Committed = NULL;
    if (QueryPrivateCommit(&Commit, "both released"))
    {
        ok_commit_delta(Start, Commit, 0, "both released");
        ok(Commit.PeakPagefileUsage >= Start.PagefileUsage + 23 * Mb, "PeakPagefileUsage %Iu, expected at least %Iu\n",
           Commit.PeakPagefileUsage, Start.PagefileUsage + 23 * Mb);
        ok(Commit.SpiPeakPagefileUsage >= Start.SpiPagefileUsage + 23 * Mb,
           "SystemProcessInformation PeakPagefileUsage %Iu, expected at least %Iu\n",
           Commit.SpiPeakPagefileUsage, Start.SpiPagefileUsage + 23 * Mb);
    }

Cleanup:
    Size = 0;
    if (Reserved)
        NtFreeVirtualMemory(NtCurrentProcess(), &Reserved, &Size, MEM_RELEASE);
    Size = 0;
    if (Committed)
        NtFreeVirtualMemory(NtCurrentProcess(), &Committed, &Size, MEM_RELEASE);
}

START_TEST(NtQueryInformationProcess)
{
    NTSTATUS Status;

    /* Make sure that some time has passed since process creation, even if the resolution of our NtQuerySystemTime is low. */
    Sleep(1);

    Status = NtQuerySystemTime(&TestStartTime);
    ok_hex(Status, STATUS_SUCCESS);

    Test_ProcessTimes();
    Test_ProcessBasicInformation();
    Test_ProcessQuotaLimits();
    Test_ProcessQuotaLimitsEx();
    Test_ProcessPriorityClassAlignment();
    Test_ProcessWx86Information();
    Test_ProcQueryAlignmentProbe();
    Test_ProcessVmCounters();
}
