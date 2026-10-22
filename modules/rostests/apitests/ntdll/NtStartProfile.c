/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtStartProfile
 * COPYRIGHT:   Copyright 2021 Thomas Faber (thomas.faber@reactos.org)
 */

#include "precomp.h"
#include <versionhelpers.h>

#define SIZEOF_MDL (5 * sizeof(PVOID) + 2 * sizeof(ULONG))
typedef ULONG_PTR PFN_NUMBER;
/* Maximum size that can be described by an MDL on 2003 and earlier */
#define MAX_MDL_BUFFER_SIZE ((MAXUSHORT - SIZEOF_MDL) / sizeof(PFN_NUMBER) * PAGE_SIZE + PAGE_SIZE - 1)

static BOOL IsWow64;
static KAFFINITY SystemAffinityMask;
static DWORD_PTR CurrentProcessAffinity;
static ULONG DummyBuffer[4096];

/* The "Buffer[Offset]++;" should likely be within 128 bytes of the start
 * of the function on any architecture we support
 */
#define LOOP_FUNCTION_SIZE_SHIFT 7
#define LOOP_FUNCTION_SIZE (1UL << LOOP_FUNCTION_SIZE_SHIFT)
C_ASSERT(LOOP_FUNCTION_SIZE == 128);
typedef void LOOP_FUNCTION(volatile ULONG *, ULONG, ULONG);
/* The profiled range must hold the code that runs */
static
DECLSPEC_NOINLINE
void
LoopFunction(
    _Inout_updates_all_(BufferSize) volatile ULONG *Buffer,
    _In_ ULONG BufferSizeInElements,
    _In_ ULONG LoopCount)
{
    ULONG i;
    ULONG Offset;

    for (i = 0; i < LoopCount; i++)
    {
        for (Offset = 0; Offset < BufferSizeInElements; Offset++)
        {
            Buffer[Offset]++;
        }
    }
}

static
void
ProfileLoopFunction(
    _In_ LOOP_FUNCTION *Function,
    _Out_writes_bytes_(BufferSize) PULONG Buffer,
    _In_ ULONG BufferSize,
    _In_range_(0, BufferSize / sizeof(ULONG)) ULONG BufferOffset
    )
{
    NTSTATUS Status;
    HANDLE ProfileHandle;
    ULONG Buffer1Value;

    Status = NtCreateProfile(&ProfileHandle,
                             NtCurrentProcess(),
                             (PVOID)((ULONG_PTR)Function - LOOP_FUNCTION_SIZE),
                             3 * LOOP_FUNCTION_SIZE,
                             LOOP_FUNCTION_SIZE_SHIFT,
                             Buffer,
                             BufferSize,
                             ProfileTime,
                             SystemAffinityMask);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to create profile\n");
        return;
    }

    Status = NtStartProfile(ProfileHandle);
    ok_hex(Status, STATUS_SUCCESS);

    /* Can't validate Buffer contents here, since we don't know what's next to our function */

    /*
     * This takes around 10-12 seconds on my machine, which is not ideal.
     * But on my Win2003 VM it only results in counts of 10-12,
     * which means we can't really make it shorter.
     */

    /* Run a long loop */
    Function(DummyBuffer,
             RTL_NUMBER_OF(DummyBuffer),
             1000000);

    /* The buffer should get live updates */
    Buffer1Value = Buffer[BufferOffset];
    ok(Buffer1Value != 0, "Buffer[%lu] = %lu\n", BufferOffset, Buffer1Value);

    /* Run a shorter loop, we should see a smaller increase */
    Function(DummyBuffer,
             RTL_NUMBER_OF(DummyBuffer),
             200000);

    ok(Buffer[BufferOffset] > Buffer1Value,
       "Buffer[%lu] = %lu, expected more than %lu\n",
       BufferOffset, Buffer[BufferOffset], Buffer1Value);

    Status = NtStopProfile(ProfileHandle);
    ok_hex(Status, STATUS_SUCCESS);

    /* The expectation is that Buffer[BufferOffset] is somewhere around 20% larger than Buffer1Value.
     * Allow anywhere from one more count to twice as many to make the test robust.
     */
    ok(Buffer[BufferOffset] > Buffer1Value,
       "Buffer[%lu] = %lu, expected more than %lu\n", BufferOffset, Buffer[BufferOffset], Buffer1Value);
    ok(Buffer[BufferOffset] < 2 * Buffer1Value,
       "Buffer[%lu] = %lu, expected less than %lu\n", BufferOffset, Buffer[BufferOffset], 2 * Buffer1Value);

    trace("Buffer1Value = %lu\n", Buffer1Value);
    trace("Buffer[%lu] = %lu\n", BufferOffset - 1, Buffer[BufferOffset - 1]);
    trace("Buffer[%lu] = %lu\n", BufferOffset, Buffer[BufferOffset]);
    trace("Buffer[%lu] = %lu\n", BufferOffset + 1, Buffer[BufferOffset + 1]);

    Status = NtClose(ProfileHandle);
    ok_hex(Status, STATUS_SUCCESS);
}

static
void
TestProfileInterval(void)
{
    NTSTATUS Status;
    ULONG OldInterval, Interval, Counts, ElapsedMs, RunMs;
    ULONG Buffer[3] = { 0 };
    HANDLE ProfileHandle;
    LARGE_INTEGER Frequency, Start, End;
    FILETIME Creation, Exit, Kernel0, User0, Kernel1, User1;

    Status = NtQueryIntervalProfile(ProfileTime, &OldInterval);
    ok_hex(Status, STATUS_SUCCESS);
    trace("Default interval %lu\n", OldInterval);

    Status = NtSetIntervalProfile(10000, ProfileTime);
    if (Status == STATUS_PRIVILEGE_NOT_HELD)
    {
        skip("Cannot set the profile interval\n");
        return;
    }
    ok_hex(Status, STATUS_SUCCESS);

    Status = NtQueryIntervalProfile(ProfileTime, &Interval);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Interval >= 5000 && Interval <= 20000, "Interval = %lu\n", Interval);
    if (Interval == 0)
        Interval = 10000;

    Status = NtCreateProfile(&ProfileHandle,
                             NtCurrentProcess(),
                             (PVOID)((ULONG_PTR)LoopFunction - LOOP_FUNCTION_SIZE),
                             3 * LOOP_FUNCTION_SIZE,
                             LOOP_FUNCTION_SIZE_SHIFT,
                             Buffer,
                             sizeof(Buffer),
                             ProfileTime,
                             SystemAffinityMask);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtStartProfile(ProfileHandle);
        ok_hex(Status, STATUS_SUCCESS);

        QueryPerformanceFrequency(&Frequency);
        GetThreadTimes(GetCurrentThread(), &Creation, &Exit, &Kernel0, &User0);
        QueryPerformanceCounter(&Start);
        LoopFunction(DummyBuffer, RTL_NUMBER_OF(DummyBuffer), 200000);
        QueryPerformanceCounter(&End);
        GetThreadTimes(GetCurrentThread(), &Creation, &Exit, &Kernel1, &User1);

        Status = NtStopProfile(ProfileHandle);
        ok_hex(Status, STATUS_SUCCESS);
        Status = NtClose(ProfileHandle);
        ok_hex(Status, STATUS_SUCCESS);

        /* About one count per interval of the thread's run time, never more than wall time allows */
        Counts = Buffer[0] + Buffer[1] + Buffer[2];
        ElapsedMs = (ULONG)((End.QuadPart - Start.QuadPart) * 1000 / Frequency.QuadPart);
        RunMs = (ULONG)(((((ULONGLONG)Kernel1.dwHighDateTime << 32) | Kernel1.dwLowDateTime) -
                          (((ULONGLONG)Kernel0.dwHighDateTime << 32) | Kernel0.dwLowDateTime) +
                          (((ULONGLONG)User1.dwHighDateTime << 32) | User1.dwLowDateTime) -
                          (((ULONGLONG)User0.dwHighDateTime << 32) | User0.dwLowDateTime)) / 10000);
        trace("%lu counts in %lu ms (%lu ms run) at interval %lu\n", Counts, ElapsedMs, RunMs, Interval);
        ok(Counts >= (ULONGLONG)RunMs * 10000 / Interval / 4, "%lu counts in %lu ms run\n", Counts, RunMs);
        ok(Counts <= (ULONGLONG)ElapsedMs * 10000 / Interval * 4 + 10, "%lu counts in %lu ms\n", Counts, ElapsedMs);
    }

    Status = NtSetIntervalProfile(OldInterval, ProfileTime);
    ok_hex(Status, STATUS_SUCCESS);
}

static
ULONG
ProfileOnAffinity(
    _In_ KAFFINITY Affinity,
    _Out_ PNTSTATUS CreateStatus)
{
    NTSTATUS Status;
    HANDLE ProfileHandle;
    ULONG Buffer[3] = { 0 };

    *CreateStatus = NtCreateProfile(&ProfileHandle,
                                    NtCurrentProcess(),
                                    (PVOID)((ULONG_PTR)LoopFunction - LOOP_FUNCTION_SIZE),
                                    3 * LOOP_FUNCTION_SIZE,
                                    LOOP_FUNCTION_SIZE_SHIFT,
                                    Buffer,
                                    sizeof(Buffer),
                                    ProfileTime,
                                    Affinity);
    if (!NT_SUCCESS(*CreateStatus))
        return 0;

    Status = NtStartProfile(ProfileHandle);
    ok_hex(Status, STATUS_SUCCESS);
    LoopFunction(DummyBuffer, RTL_NUMBER_OF(DummyBuffer), 200000);
    Status = NtStopProfile(ProfileHandle);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtClose(ProfileHandle);
    ok_hex(Status, STATUS_SUCCESS);
    return Buffer[0] + Buffer[1] + Buffer[2];
}

static
void
TestProfileAffinity(void)
{
    KAFFINITY Run, Other;
    DWORD_PTR OldAffinity;
    NTSTATUS Status;
    ULONG Counts;

    Counts = ProfileOnAffinity(0, &Status);
    ok_hex(Status, STATUS_INVALID_PARAMETER);

    /* Run on the lowest processor this process may use, profile on another one */
    Run = CurrentProcessAffinity & SystemAffinityMask;
    Run &= ~Run + 1;
    Other = SystemAffinityMask & ~Run;
    Other &= ~Other + 1;
    if (!Other)
    {
        skip("Only one processor\n");
        return;
    }

    OldAffinity = SetThreadAffinityMask(GetCurrentThread(), Run);
    ok(OldAffinity != 0, "SetThreadAffinityMask failed with %lu\n", GetLastError());
    if (!OldAffinity)
        return;

    Counts = ProfileOnAffinity(Other, &Status);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Counts == 0, "%lu counts from an excluded processor\n", Counts);

    Counts = ProfileOnAffinity(Run, &Status);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Counts != 0, "No counts from the profiled processor\n");

    SetThreadAffinityMask(GetCurrentThread(), OldAffinity);
}

START_TEST(NtStartProfile)
{
    NTSTATUS Status;
    ULONG StackBuffer[3] = { 0 };

    IsWow64Process(GetCurrentProcess(), &IsWow64);

    GetProcessAffinityMask(GetCurrentProcess(), &CurrentProcessAffinity, &SystemAffinityMask);

    /* Parameter validation is pretty simple... */
    Status = NtStartProfile(NULL);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Do an actual simple profile */
    ProfileLoopFunction(LoopFunction,
                        StackBuffer,
                        sizeof(StackBuffer),
                        1);

    TestProfileInterval();
    TestProfileAffinity();
}
