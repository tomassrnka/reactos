/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that NtRaiseException does not hand kernel stack back to user mode
 */

#include "precomp.h"
#include <string.h>

/*
 * NtRaiseException copies the caller's record into a kernel stack local and
 * dispatches that local, all of it, back to the user-mode handlers. The slots
 * past NumberParameters and the padding must therefore come back as the caller
 * passed them (zero here), not as whatever an earlier call left on the kernel
 * stack. A raise with all 15 parameters set to a pattern runs first, through
 * the same system call, so an unzeroed local is seen as that pattern.
 */

#define TEST_CODE       0xE0F15EEDUL
#define PATTERN         ((ULONG_PTR)0xA5A5A5A5A5A5A5A5ULL)

static EXCEPTION_RECORD Seen;
static volatile LONG Hits;
static volatile LONG Raised;

static
LONG
CALLBACK
RaiseHandler(PEXCEPTION_POINTERS Pointers)
{
    if (Pointers->ExceptionRecord->ExceptionCode != TEST_CODE)
        return EXCEPTION_CONTINUE_SEARCH;
    Seen = *Pointers->ExceptionRecord;
    Hits++;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Raise Record through the kernel, first chance, and return once the vectored
 * handler has continued. The context resumes after RtlCaptureContext. */
static
DECLSPEC_NOINLINE
NTSTATUS
RaiseOnce(PEXCEPTION_RECORD Record)
{
    static CONTEXT Context;
    static volatile NTSTATUS Status;

    Raised = 0;
    Status = STATUS_SUCCESS;
    RtlCaptureContext(&Context);
    if (!Raised)
    {
        Raised = 1;
        Status = NtRaiseException(Record, &Context, TRUE);
    }
    return Status;
}

static
VOID
TestCount(ULONG Count)
{
    EXCEPTION_RECORD Record, Expected;
    NTSTATUS Status;
    ULONG i;

    /* Prime: all parameters set, through the same path */
    RtlZeroMemory(&Record, sizeof(Record));
    Record.ExceptionCode = TEST_CODE;
    Record.ExceptionAddress = (PVOID)RaiseOnce;
    Record.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS;
    for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
        Record.ExceptionInformation[i] = PATTERN;
    Hits = 0;
    Status = RaiseOnce(&Record);
    ok(Hits == 1 && Status == STATUS_SUCCESS, "Prime: hits %ld, status 0x%lx\n", Hits, Status);

    /* The record under test: Count parameters, the rest zero */
    RtlZeroMemory(&Record, sizeof(Record));
    Record.ExceptionCode = TEST_CODE;
    Record.ExceptionAddress = (PVOID)RaiseOnce;
    Record.NumberParameters = Count;
    for (i = 0; i < Count; i++)
        Record.ExceptionInformation[i] = 0x1000 + i;
    Hits = 0;
    RtlFillMemory(&Seen, sizeof(Seen), 0x5A);
    Status = RaiseOnce(&Record);
    ok(Hits == 1, "%lu parameters: hits %ld, status 0x%lx\n", Count, Hits, Status);
    if (Hits != 1)
        return;

    Expected = Record;
    Expected.ExceptionFlags = Seen.ExceptionFlags;
    ok(Seen.NumberParameters == Count, "%lu parameters: got %lu\n", Count, Seen.NumberParameters);
    for (i = Count; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
    {
        ok(Seen.ExceptionInformation[i] == 0, "%lu parameters: slot %lu is 0x%Ix\n",
           Count, i, Seen.ExceptionInformation[i]);
    }
    /* Byte for byte, the padding after NumberParameters included */
    ok(memcmp(&Seen, &Expected, sizeof(Expected)) == 0,
       "%lu parameters: the record differs from the one raised\n", Count);
}

START_TEST(RaiseExceptionLeak)
{
    static const ULONG Counts[] = { 0, 1, 7, 14, EXCEPTION_MAXIMUM_PARAMETERS };
    PVOID Handler;
    ULONG i;

    Handler = AddVectoredExceptionHandler(1, RaiseHandler);
    ok(Handler != NULL, "AddVectoredExceptionHandler failed\n");
    if (!Handler)
        return;
    for (i = 0; i < _countof(Counts); i++)
        TestCount(Counts[i]);
    RemoveVectoredExceptionHandler(Handler);
}
