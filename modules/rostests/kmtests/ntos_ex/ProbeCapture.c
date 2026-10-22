/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite for ProbeAndCaptureUnicodeString /
 *                  ReleaseCapturedUnicodeString (sdk/include/reactos/probe.h)
 *
 * The pool allocator used by the helper is intercepted so that an allocation
 * failure can be forced and every free can be validated: the helper must
 * never hand a pointer it did not allocate to ExFreePoolWithTag.
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

static BOOLEAN FailNextAlloc;
static PVOID LastAlloc;
static ULONG AllocCount;
static ULONG FreeCount;
static ULONG ForeignFreeCount;

static
PVOID
NTAPI
ProbeTestAlloc(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T Size,
    _In_ ULONG Tag)
{
    PVOID Ptr;

    if (FailNextAlloc)
    {
        FailNextAlloc = FALSE;
        return NULL;
    }

    Ptr = ExAllocatePoolWithTag(PoolType, Size, Tag);
    if (Ptr)
    {
        LastAlloc = Ptr;
        AllocCount++;
    }
    return Ptr;
}

static
VOID
NTAPI
ProbeTestFree(
    _In_ PVOID Ptr,
    _In_ ULONG Tag)
{
    FreeCount++;
    if (Ptr != LastAlloc)
    {
        /* Not ours: record it, but do not pass it on (would bugcheck) */
        ForeignFreeCount++;
        return;
    }
    LastAlloc = NULL;
    ExFreePoolWithTag(Ptr, Tag);
}

#define _NTOSKRNL_ /* probe.h is guarded; this test deliberately uses it */
#define ExAllocatePoolWithTag ProbeTestAlloc
#define ExFreePoolWithTag ProbeTestFree
#include <reactos/probe.h>
#undef ExAllocatePoolWithTag
#undef ExFreePoolWithTag
#undef _NTOSKRNL_

static
VOID
ResetCounters(VOID)
{
    FailNextAlloc = FALSE;
    LastAlloc = NULL;
    AllocCount = FreeCount = ForeignFreeCount = 0;
}

static
VOID
TestCapture(
    _In_ PUNICODE_STRING UserString,
    _In_ PWCHAR UserBuffer)
{
    UNICODE_STRING Dest;
    NTSTATUS Status;

    /* Normal path: private copy, NUL terminated, then freed exactly once */
    ResetCounters();
    UserString->Buffer = UserBuffer;
    UserString->Length = 5 * sizeof(WCHAR);
    UserString->MaximumLength = 5 * sizeof(WCHAR);
    Status = ProbeAndCaptureUnicodeString(&Dest, UserMode, UserString);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Dest.Buffer != UserBuffer, "Buffer was not copied\n");
    ok_eq_uint(Dest.Length, 5 * sizeof(WCHAR));
    ok_eq_uint(Dest.MaximumLength, 6 * sizeof(WCHAR));
    ok_eq_uint(AllocCount, 1);
    if (Dest.Buffer && Dest.Buffer != UserBuffer)
    {
        ok(RtlEqualMemory(Dest.Buffer, UserBuffer, Dest.Length), "Wrong contents\n");
        ok_eq_uint(Dest.Buffer[5], UNICODE_NULL);
    }
    ReleaseCapturedUnicodeString(&Dest, UserMode);
    ok_eq_uint(FreeCount, 1);
    ok_eq_uint(ForeignFreeCount, 0);
    ok(Dest.Buffer == NULL, "Release did not clear Buffer\n");
    ok_eq_uint(Dest.Length, 0);
    ok_eq_uint(Dest.MaximumLength, 0);

    /* Allocation failure: Dest must be safe, status must be correct */
    ResetCounters();
    FailNextAlloc = TRUE;
    Status = ProbeAndCaptureUnicodeString(&Dest, UserMode, UserString);
    ok_eq_hex(Status, STATUS_INSUFFICIENT_RESOURCES);
    ok(Dest.Buffer == NULL, "Dest.Buffer = %p after failed capture, user pointer was %p\n",
       Dest.Buffer, UserBuffer);
    ok_eq_uint(Dest.Length, 0);
    ok_eq_uint(Dest.MaximumLength, 0);
    /* The pattern used by callers (e.g. NtSetValueKey): release on failure */
    ReleaseCapturedUnicodeString(&Dest, UserMode);
    ok_eq_uint(FreeCount, 0);
    ok_eq_uint(ForeignFreeCount, 0);
    ok_eq_uint(AllocCount, 0);
    /* ... and release is idempotent */
    ReleaseCapturedUnicodeString(&Dest, UserMode);
    ok_eq_uint(ForeignFreeCount, 0);

    /* Odd length is rounded down */
    ResetCounters();
    UserString->Length = 5;
    Status = ProbeAndCaptureUnicodeString(&Dest, UserMode, UserString);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(Dest.Length, 4);
    ReleaseCapturedUnicodeString(&Dest, UserMode);
    ok_eq_uint(ForeignFreeCount, 0);
    ok_eq_uint(FreeCount, 1);

    /* Zero length with a buffer: no allocation, sanitized */
    ResetCounters();
    UserString->Length = 0;
    Status = ProbeAndCaptureUnicodeString(&Dest, UserMode, UserString);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Dest.Buffer == NULL, "Buffer = %p\n", Dest.Buffer);
    ok_eq_uint(AllocCount, 0);
    ReleaseCapturedUnicodeString(&Dest, UserMode);
    ok_eq_uint(FreeCount, 0);

    /* NULL buffer */
    ResetCounters();
    UserString->Buffer = NULL;
    UserString->Length = 4;
    Status = ProbeAndCaptureUnicodeString(&Dest, UserMode, UserString);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Dest.Buffer == NULL, "Buffer = %p\n", Dest.Buffer);
    ok_eq_uint(Dest.Length, 0);
    ok_eq_uint(AllocCount, 0);

    /* Kernel pointer in a user string: exception path, zeroed */
    ResetCounters();
    UserString->Buffer = (PWCHAR)(ULONG_PTR)MmSystemRangeStart;
    UserString->Length = 4;
    Status = ProbeAndCaptureUnicodeString(&Dest, UserMode, UserString);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);
    ok(Dest.Buffer == NULL, "Buffer = %p\n", Dest.Buffer);
    ok_eq_uint(Dest.Length, 0);
    ReleaseCapturedUnicodeString(&Dest, UserMode);
    ok_eq_uint(FreeCount, 0);
    ok_eq_uint(ForeignFreeCount, 0);

    /* KernelMode: structure copied verbatim, nothing allocated or freed */
    ResetCounters();
    UserString->Buffer = UserBuffer;
    UserString->Length = 4;
    Status = ProbeAndCaptureUnicodeString(&Dest, KernelMode, UserString);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Dest.Buffer == UserBuffer, "Buffer = %p\n", Dest.Buffer);
    ok_eq_uint(AllocCount, 0);
    ReleaseCapturedUnicodeString(&Dest, KernelMode);
    ok_eq_uint(FreeCount, 0);
}

START_TEST(ProbeCapture)
{
    NTSTATUS Status;
    PVOID Base = NULL;
    SIZE_T Size = PAGE_SIZE;
    PUNICODE_STRING UserString;
    PWCHAR UserBuffer;
    ULONG i;

    /* Need real user-mode memory: the test runs in the context of kmtest.exe */
    Status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &Base, 0, &Size,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    UserString = Base;
    UserBuffer = (PWCHAR)((PUCHAR)Base + 256);
    _SEH2_TRY
    {
        for (i = 0; i < 16; i++)
            UserBuffer[i] = L'A' + i;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ok(0, "Exception writing user memory\n");
        _SEH2_YIELD(return);
    }
    _SEH2_END;

    _SEH2_TRY
    {
        TestCapture(UserString, UserBuffer);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ok(0, "Unexpected exception 0x%lx\n", _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    Size = 0;
    ZwFreeVirtualMemory(ZwCurrentProcess(), &Base, &Size, MEM_RELEASE);
}
