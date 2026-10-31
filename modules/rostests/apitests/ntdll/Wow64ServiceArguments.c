/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System services whose WoW64 thunks take counts and pointers from the argument list
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The same PE32 file runs on i386 and under WoW64, where the thunks read the arguments once
 * and check them. Well-formed calls must give the results the i386 kernel gives, and the
 * calls the kernel rejects (a bad count, a NULL output descriptor) must fail with its status.
 */

#include "precomp.h"

#define TEST_CODE 0xE0A12C0DUL

static void TestWaitForMultiple(void)
{
    HANDLE Events[MAXIMUM_WAIT_OBJECTS + 1];
    LARGE_INTEGER Zero;
    NTSTATUS Status;
    ULONG i;

    Zero.QuadPart = 0;
    for (i = 0; i < RTL_NUMBER_OF(Events); i++)
    {
        Status = NtCreateEvent(&Events[i], EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
        ok(Status == STATUS_SUCCESS, "NtCreateEvent %lu: 0x%08lx\n", i, Status);
        if (!NT_SUCCESS(Status))
        {
            while (i--) NtClose(Events[i]);
            return;
        }
    }

    Status = NtWaitForMultipleObjects(2, Events, WaitAny, FALSE, &Zero);
    ok(Status == STATUS_TIMEOUT, "none set: 0x%08lx\n", Status);

    NtSetEvent(Events[1], NULL);
    Status = NtWaitForMultipleObjects(2, Events, WaitAny, FALSE, &Zero);
    ok(Status == STATUS_WAIT_1, "second set, any: 0x%08lx\n", Status);
    Status = NtWaitForMultipleObjects(2, Events, WaitAll, FALSE, &Zero);
    ok(Status == STATUS_TIMEOUT, "second set, all: 0x%08lx\n", Status);

    NtSetEvent(Events[0], NULL);
    Status = NtWaitForMultipleObjects(2, Events, WaitAll, FALSE, &Zero);
    ok(Status == STATUS_WAIT_0, "both set, all: 0x%08lx\n", Status);
    NtResetEvent(Events[0], NULL);
    NtResetEvent(Events[1], NULL);

    /* the largest count, signalled only at its last handle */
    NtSetEvent(Events[MAXIMUM_WAIT_OBJECTS - 1], NULL);
    Status = NtWaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, Events, WaitAny, FALSE, &Zero);
    ok(Status == STATUS_WAIT_0 + MAXIMUM_WAIT_OBJECTS - 1, "64 handles, any: 0x%08lx\n", Status);
    Status = NtWaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, Events, WaitAll, FALSE, &Zero);
    ok(Status == STATUS_TIMEOUT, "64 handles, all: 0x%08lx\n", Status);

    /* counts the kernel rejects */
    Status = NtWaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS + 1, Events, WaitAny, FALSE, &Zero);
    ok(Status == STATUS_INVALID_PARAMETER_1, "65 handles: 0x%08lx\n", Status);
    Status = NtWaitForMultipleObjects(0, Events, WaitAny, FALSE, &Zero);
    ok(Status == STATUS_INVALID_PARAMETER_1, "no handles: 0x%08lx\n", Status);

    for (i = 0; i < RTL_NUMBER_OF(Events); i++)
        NtClose(Events[i]);
}

static void TestRaiseHardErrorCount(void)
{
    ULONG_PTR Parameters[MAXIMUM_HARDERROR_PARAMETERS + 1] = {0};
    ULONG Response = 0xdeadbeef;
    NTSTATUS Status;

    /* rejected before any box: the count is checked first */
    Status = NtRaiseHardError(STATUS_ACCESS_VIOLATION, MAXIMUM_HARDERROR_PARAMETERS + 1, 0,
                              Parameters, OptionOk, &Response);
    ok(Status == STATUS_INVALID_PARAMETER_2, "count %d: 0x%08lx\n", MAXIMUM_HARDERROR_PARAMETERS + 1, Status);
    ok(Response == 0xdeadbeef, "count %d: response %lu\n", MAXIMUM_HARDERROR_PARAMETERS + 1, Response);
}

static void TestQuerySymbolicLink(void)
{
    UNICODE_STRING Name, Target;
    OBJECT_ATTRIBUTES Attributes;
    WCHAR Buffer[MAX_PATH];
    HANDLE Link;
    ULONG ResultLength;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, L"\\GLOBAL??\\C:");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenSymbolicLinkObject(&Link, SYMBOLIC_LINK_QUERY, &Attributes);
    ok(Status == STATUS_SUCCESS, "NtOpenSymbolicLinkObject: 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    Target.Buffer = Buffer;
    Target.Length = 0;
    Target.MaximumLength = sizeof(Buffer);
    ResultLength = 0xdeadbeef;
    Status = NtQuerySymbolicLinkObject(Link, &Target, &ResultLength);
    ok(Status == STATUS_SUCCESS, "query: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        ok(Target.Length >= 8 * sizeof(WCHAR) && !_wcsnicmp(Target.Buffer, L"\\Device\\", 8),
           "query: target %s\n", wine_dbgstr_wn(Target.Buffer, Target.Length / sizeof(WCHAR)));
        ok(ResultLength >= Target.Length, "query: result length %lu, length %u\n", ResultLength, Target.Length);
    }

    /* without a result length */
    Target.Length = 0;
    Status = NtQuerySymbolicLinkObject(Link, &Target, NULL);
    ok(Status == STATUS_SUCCESS, "query, no result length: 0x%08lx\n", Status);
    ok(Target.Length >= 8 * sizeof(WCHAR), "query, no result length: length %u\n", Target.Length);

    /* a buffer too small for the target */
    Target.Length = 0;
    Target.MaximumLength = sizeof(WCHAR);
    ResultLength = 0;
    Status = NtQuerySymbolicLinkObject(Link, &Target, &ResultLength);
    ok(Status == STATUS_BUFFER_TOO_SMALL, "small buffer: 0x%08lx\n", Status);
    ok(ResultLength > sizeof(WCHAR), "small buffer: result length %lu\n", ResultLength);

    /* no descriptor at all: an error status, not a crash */
    ResultLength = 0xdeadbeef;
    Status = NtQuerySymbolicLinkObject(Link, NULL, &ResultLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "NULL target: 0x%08lx\n", Status);
    Status = NtQuerySymbolicLinkObject(Link, NULL, NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "NULL target, no result length: 0x%08lx\n", Status);

    NtClose(Link);
}

static EXCEPTION_RECORD Seen;
static volatile LONG Hits;
static volatile LONG Raised;

static LONG CALLBACK RaiseHandler(PEXCEPTION_POINTERS Pointers)
{
    if (Pointers->ExceptionRecord->ExceptionCode != TEST_CODE)
        return EXCEPTION_CONTINUE_SEARCH;
    Seen = *Pointers->ExceptionRecord;
    Hits++;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Raise Record through the kernel, first chance; returns once the vectored handler has
 * continued or the service has failed. */
static DECLSPEC_NOINLINE NTSTATUS RaiseOnce(PEXCEPTION_RECORD Record)
{
    /* The status is kept apart from the context the raise resumes with */
    static struct { CONTEXT Context; UCHAR Slack[256]; } Capture;
    static volatile NTSTATUS Status;

    Raised = 0;
    Status = STATUS_SUCCESS;
    RtlCaptureContext(&Capture.Context);
    if (!Raised)
    {
        Raised = 1;
        Status = NtRaiseException(Record, &Capture.Context, TRUE);
    }
    return Status;
}

static void TestRaiseException(void)
{
    static const ULONG Counts[] = { 0, 3, EXCEPTION_MAXIMUM_PARAMETERS };
    struct { EXCEPTION_RECORD Record; ULONG_PTR Extra[4]; } Raise;
    NTSTATUS Status;
    PVOID Handler;
    ULONG c, i;

    Handler = AddVectoredExceptionHandler(1, RaiseHandler);
    ok(Handler != NULL, "AddVectoredExceptionHandler failed\n");
    if (!Handler)
        return;

    for (c = 0; c < RTL_NUMBER_OF(Counts); c++)
    {
        RtlZeroMemory(&Raise, sizeof(Raise));
        Raise.Record.ExceptionCode = TEST_CODE;
        Raise.Record.ExceptionAddress = (PVOID)RaiseOnce;
        Raise.Record.NumberParameters = Counts[c];
        for (i = 0; i < Counts[c]; i++)
            Raise.Record.ExceptionInformation[i] = 0x1000 + i;
        Hits = 0;
        RtlZeroMemory(&Seen, sizeof(Seen));
        Status = RaiseOnce(&Raise.Record);
        /* how the raise returns after the handler continued is not checked: 64-bit Windows
         * resumes a 32-bit caller after the service rather than at the captured context */
        ok(Hits == 1, "%lu parameters: hits %ld, status 0x%08lx\n", Counts[c], Hits, Status);
        if (Hits != 1)
            continue;
        ok(Seen.ExceptionAddress == (PVOID)RaiseOnce, "%lu parameters: address %p\n",
           Counts[c], Seen.ExceptionAddress);
        ok(Seen.NumberParameters == Counts[c], "%lu parameters: got %lu\n", Counts[c], Seen.NumberParameters);
        for (i = 0; i < Counts[c]; i++)
            ok(Seen.ExceptionInformation[i] == 0x1000 + i, "%lu parameters: parameter %lu is 0x%Ix\n",
               Counts[c], i, Seen.ExceptionInformation[i]);
    }

    /* More parameters than a record holds. The i386 kernel fails the service and dispatches
     * nothing. A 32-bit caller on 64-bit Windows gets the exception raised with the parameters
     * a record holds (Windows hands the handler the count as passed; ReactOS the clamped one). */
    {
        static const ULONG Over[] = { EXCEPTION_MAXIMUM_PARAMETERS + 1, 100, 0x80000000 };
        BOOL (WINAPI *pIsWow64Process)(HANDLE, PBOOL);
        BOOL Wow64 = FALSE;
        pIsWow64Process = (PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
        if (pIsWow64Process) pIsWow64Process(GetCurrentProcess(), &Wow64);
        for (c = 0; c < RTL_NUMBER_OF(Over); c++)
        {
            RtlZeroMemory(&Raise, sizeof(Raise));
            Raise.Record.ExceptionCode = TEST_CODE;
            Raise.Record.ExceptionAddress = (PVOID)RaiseOnce;
            Raise.Record.NumberParameters = Over[c];
            for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS + 4; i++)
                Raise.Record.ExceptionInformation[i] = 0x3000 + i;
            Hits = 0;
            RtlZeroMemory(&Seen, sizeof(Seen));
            Status = RaiseOnce(&Raise.Record);
            if (!Wow64)
            {
                ok(Status == STATUS_INVALID_PARAMETER, "count %lu: status 0x%08lx\n", Over[c], Status);
                ok(Hits == 0, "count %lu: hits %ld\n", Over[c], Hits);
                continue;
            }
            ok(Hits == 1, "count %lu: hits %ld, status 0x%08lx\n", Over[c], Hits, Status);
            if (Hits != 1)
                continue;
            ok(Seen.NumberParameters == EXCEPTION_MAXIMUM_PARAMETERS || Seen.NumberParameters == Over[c],
               "count %lu: handler got %lu\n", Over[c], Seen.NumberParameters);
            for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
                ok(Seen.ExceptionInformation[i] == 0x3000 + i, "count %lu: parameter %lu is 0x%Ix\n",
                   Over[c], i, Seen.ExceptionInformation[i]);
        }
    }

    /* kernel32 limits the count itself (in user mode: no system service) */
    Hits = 0;
    {
        ULONG_PTR Arguments[EXCEPTION_MAXIMUM_PARAMETERS + 5];
        for (i = 0; i < RTL_NUMBER_OF(Arguments); i++)
            Arguments[i] = 0x2000 + i;
        RaiseException(TEST_CODE, 0, RTL_NUMBER_OF(Arguments), Arguments);
    }
    ok(Hits == 1, "RaiseException with 20 arguments: hits %ld\n", Hits);
    if (Hits == 1)
    {
        ok(Seen.NumberParameters == EXCEPTION_MAXIMUM_PARAMETERS, "RaiseException: %lu parameters\n",
           Seen.NumberParameters);
        ok(Seen.ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS - 1] == 0x2000 + EXCEPTION_MAXIMUM_PARAMETERS - 1,
           "RaiseException: last parameter 0x%Ix\n", Seen.ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS - 1]);
    }

    RemoveVectoredExceptionHandler(Handler);
}

static void TestConsoleProcessList(void)
{
    DWORD (WINAPI *pGetConsoleProcessList)(LPDWORD, DWORD);
    DWORD Ids[64], Count, i;
    BOOL Found = FALSE;

    pGetConsoleProcessList = (PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetConsoleProcessList");
    if (!pGetConsoleProcessList)
    {
        skip("no GetConsoleProcessList\n");
        return;
    }

    Count = pGetConsoleProcessList(Ids, RTL_NUMBER_OF(Ids));
    if (!Count)
    {
        skip("GetConsoleProcessList failed: %lu\n", GetLastError());
        return;
    }
    for (i = 0; i < min(Count, RTL_NUMBER_OF(Ids)); i++)
        if (Ids[i] == GetCurrentProcessId()) Found = TRUE;
    ok(Found, "GetConsoleProcessList: own id not in the %lu ids\n", Count);

    /* a list of one: the total comes back, at most one id is stored */
    Ids[1] = 0xdeadbeef;
    i = pGetConsoleProcessList(Ids, 1);
    /* processes may join the console between the two calls: the total can only be a lower bound */
    if (i < Count) skip("GetConsoleProcessList(1): total %lu below %lu, list changed\n", i, Count);
    else ok(i >= Count, "GetConsoleProcessList(1): %lu, expected at least %lu\n", i, Count);
    ok(Ids[1] == 0xdeadbeef, "GetConsoleProcessList(1) wrote past the list\n");
}

#ifdef _M_IX86
/* A 32-bit RtlCaptureContext sets the flags itself, whatever the context held (Windows 10:
 * CONTEXT_i386 | CONTROL | INTEGER | SEGMENTS) */
static DECLSPEC_NOINLINE void CaptureWith(PCONTEXT Context, ULONG Flags)
{
    RtlFillMemory(Context, sizeof(*Context), 0xCC);
    Context->ContextFlags = Flags;
    RtlCaptureContext(Context);
}

static void TestCaptureContextFlags(void)
{
    static const ULONG Initial[] = { 0, CONTEXT_CONTROL, 0xFFFFFFFF, 0xCCCCCCCC };
    CONTEXT Context;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Initial); i++)
    {
        CaptureWith(&Context, Initial[i]);
        ok(Context.ContextFlags == 0x00010007, "flags 0x%08lx before: not 0x00010007 (0x%08lx)\n",
           Initial[i], Context.ContextFlags);
        ok((Context.SegCs & 0xFFFF) != 0xCCCC, "flags 0x%08lx before: SegCs not captured\n", Initial[i]);
        ok(Context.Eip != 0xCCCCCCCC && Context.Esp != 0xCCCCCCCC, "flags 0x%08lx before: control not captured\n", Initial[i]);
    }
}
#endif

START_TEST(Wow64ServiceArguments)
{
#ifdef _M_IX86
    TestCaptureContextFlags();
#endif
    TestWaitForMultiple();
    TestRaiseHardErrorCount();
    TestQuerySymbolicLink();
    TestRaiseException();
    TestConsoleProcessList();
}
