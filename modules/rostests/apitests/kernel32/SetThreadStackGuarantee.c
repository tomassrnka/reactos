/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for SetThreadStackGuarantee
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/pstypes.h>
#include <pseh/pseh2.h>

#ifdef _MSC_VER
#pragma warning(disable : 4717) // disable warning about recursive function
#elif defined(__GNUC__) && (__GNUC__ >= 12)
#pragma GCC diagnostic ignored "-Winfinite-recursion"
#endif

#define TEST_PAGE_SIZE 0x1000
#define GUARANTEE (64 * 1024)
#define HANDLER_STACK (48 * 1024)

static ULONG Depth;
static BOOL HandlerRan;

static
DECLSPEC_NOINLINE
ULONG
Recurse(volatile CHAR *Previous)
{
    volatile CHAR Buffer[0x400];

    Buffer[0] = Previous[0] + 1;
    Depth++;

    /* Use the result so that this is not a tail call */
    return Recurse(Buffer) + Buffer[0];
}

static
DECLSPEC_NOINLINE
LONG
UseHandlerStack(void)
{
    volatile CHAR Buffer[HANDLER_STACK];
    ULONG i;

    for (i = sizeof(Buffer); i > 0; i -= TEST_PAGE_SIZE)
        Buffer[i - 1] = 1;

    return Buffer[0] + Buffer[sizeof(Buffer) - 1];
}

static
LONG
OverflowFilter(NTSTATUS Code)
{
    /* The filter runs on the overflowed stack, so this needs the guarantee */
    if (Code == STATUS_STACK_OVERFLOW)
    {
        UseHandlerStack();
        HandlerRan = TRUE;
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

static
DWORD
WINAPI
GuaranteeThread(PVOID Parameter)
{
    NTSTATUS Status;
    ULONG Size;
    BOOL Ret;

    Size = 0;
    Ret = SetThreadStackGuarantee(&Size);
    ok(Ret, "SetThreadStackGuarantee failed with %lu\n", GetLastError());
    ok_long(Size, 0);
    ok_long(NtCurrentTeb()->GuaranteedStackBytes, 0);

    /* Larger than the stack reservation */
    Size = 0x10000000;
    SetLastError(0xdeadbeef);
    Ret = SetThreadStackGuarantee(&Size);
    ok(!Ret, "SetThreadStackGuarantee succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER || GetLastError() == ERROR_INVALID_ADDRESS,
       "Wrong error %lu\n", GetLastError());
    ok_long(Size, 0);
    ok_long(NtCurrentTeb()->GuaranteedStackBytes, 0);

    Size = GUARANTEE;
    Ret = SetThreadStackGuarantee(&Size);
    ok(Ret, "SetThreadStackGuarantee failed with %lu\n", GetLastError());
    ok_long(Size, 0);
    ok_long(NtCurrentTeb()->GuaranteedStackBytes, GUARANTEE);

    /* Keep testing the stack overflow handling if the call did not store the guarantee */
    if (NtCurrentTeb()->GuaranteedStackBytes != GUARANTEE)
        NtCurrentTeb()->GuaranteedStackBytes = GUARANTEE;

    Status = STATUS_SUCCESS;
    _SEH2_TRY
    {
        Recurse((volatile CHAR *)&Status);
    }
    _SEH2_EXCEPT(OverflowFilter(_SEH2_GetExceptionCode()))
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    trace("Stack overflow after %lu calls\n", Depth);
    ok_ntstatus(Status, STATUS_STACK_OVERFLOW);
    ok(HandlerRan, "The filter could not use %u bytes of stack\n", HANDLER_STACK);

    return 0;
}

START_TEST(SetThreadStackGuarantee)
{
    HANDLE Thread;
    DWORD Wait;

    /* Overflow the stack of a new thread, so this one keeps its stack */
    Thread = CreateThread(NULL, 1024 * 1024, GuaranteeThread, NULL,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    ok(Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!Thread)
        return;

    Wait = WaitForSingleObject(Thread, 60 * 1000);
    ok_long(Wait, WAIT_OBJECT_0);
    CloseHandle(Thread);
}
