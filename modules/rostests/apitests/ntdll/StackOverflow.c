/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for stack overflow
 * PROGRAMMER:      Jérôme Gardou
 */

#include "precomp.h"

#include <pseh/pseh2.h>

#ifdef _MSC_VER
#pragma warning(disable : 4717) // disable warning about recursive function
#elif defined(__GNUC__) && (__GNUC__ >= 12)
#pragma GCC diagnostic ignored "-Winfinite-recursion"
#endif

static int iteration = 0;
static PVOID StackAllocationBase;
static PVOID LastStackAllocation;
static ULONG StackSize;

static
void
infinite_recursive(void)
{
    MEMORY_BASIC_INFORMATION MemoryBasicInfo;
    NTSTATUS Status;
    char Buffer[0x500];

    sprintf(Buffer, "Iteration %d.\n", iteration++);

    Status = NtQueryVirtualMemory(
        NtCurrentProcess(),
        &Buffer[0],
        MemoryBasicInformation,
        &MemoryBasicInfo,
        sizeof(MemoryBasicInfo),
        NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    /* This never changes */
    ok_ptr(MemoryBasicInfo.AllocationBase, StackAllocationBase);
    /* Stack is committed one page at a time */
    ok_ptr(MemoryBasicInfo.BaseAddress, (PVOID)PAGE_ROUND_DOWN(&Buffer[0]));
    /* This is the protection of the memory when it was reserved. */
    ok_long(MemoryBasicInfo.AllocationProtect, PAGE_READWRITE);
    /* Windows commits the whole used stack at once, +2 pages. */
#if 0
    ok_long(MemoryBasicInfo.RegionSize, ((ULONG_PTR)StackAllocationBase + StackSize + 2* PAGE_SIZE) - PAGE_ROUND_DOWN(&Buffer[0]));
#endif
    /* This is the state of the queried address */
    ok_long(MemoryBasicInfo.State, MEM_COMMIT);
    /* This is the protection of the queried address */
    ok_long(MemoryBasicInfo.Protect, PAGE_READWRITE);
    /* Of course this is private memory. */
    ok_long(MemoryBasicInfo.Type, MEM_PRIVATE);

    LastStackAllocation = &Buffer[-0x500];

    infinite_recursive();
}

START_TEST(StackOverflow)
{
    NTSTATUS Status;
    MEMORY_BASIC_INFORMATION MemoryBasicInfo;

    /* Get the base of the stack */
    Status = NtQueryVirtualMemory(
        NtCurrentProcess(),
        &Status,
        MemoryBasicInformation,
        &MemoryBasicInfo,
        sizeof(MemoryBasicInfo),
        NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    StackAllocationBase = MemoryBasicInfo.AllocationBase;
    trace("Stack allocation base is %p.\n", StackAllocationBase);
    StackSize = MemoryBasicInfo.RegionSize;

    /* Check TEB attributes */
    ok_ptr(NtCurrentTeb()->DeallocationStack, StackAllocationBase);
    ok_ptr(NtCurrentTeb()->NtTib.StackBase, (PVOID)((ULONG_PTR)MemoryBasicInfo.BaseAddress + MemoryBasicInfo.RegionSize));
#ifdef _WIN64
    ok_ptr(NtCurrentTeb()->NtTib.StackLimit, (PVOID)((ULONG_PTR)MemoryBasicInfo.BaseAddress - 2 * PAGE_SIZE));
#else
    ok_ptr(NtCurrentTeb()->NtTib.StackLimit, (PVOID)((ULONG_PTR)MemoryBasicInfo.BaseAddress - PAGE_SIZE));
#endif
    trace("Guaranteed stack size is %lu.\n", NtCurrentTeb()->GuaranteedStackBytes);

    /* Get its size */
    Status = NtQueryVirtualMemory(
        NtCurrentProcess(),
        StackAllocationBase,
        MemoryBasicInformation,
        &MemoryBasicInfo,
        sizeof(MemoryBasicInfo),
        NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* This is the complete stack size */
    StackSize += MemoryBasicInfo.RegionSize;
    trace("Stack size is 0x%lx.\n", StackSize);

    trace("Stack limit %p, stack base %p.\n", NtCurrentTeb()->NtTib.StackLimit, NtCurrentTeb()->NtTib.StackBase);

    /* Take a look at what is beyond the stack limit */
    Status = NtQueryVirtualMemory(
        NtCurrentProcess(),
        NtCurrentTeb()->NtTib.StackLimit,
        MemoryBasicInformation,
        &MemoryBasicInfo,
        sizeof(MemoryBasicInfo),
        NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);

    ok_ptr(MemoryBasicInfo.BaseAddress, NtCurrentTeb()->NtTib.StackLimit);
    ok_ptr(MemoryBasicInfo.AllocationBase, StackAllocationBase);
    ok_long(MemoryBasicInfo.AllocationProtect, PAGE_READWRITE);
#ifdef _WIN64
    ok_long(MemoryBasicInfo.RegionSize, 3 * PAGE_SIZE);
#else
    ok_long(MemoryBasicInfo.RegionSize, 2 * PAGE_SIZE);
#endif
    ok_long(MemoryBasicInfo.State, MEM_COMMIT);
    ok_long(MemoryBasicInfo.Protect, PAGE_READWRITE);
    ok_long(MemoryBasicInfo.Type, MEM_PRIVATE);

    /* Accessing below stack limit is OK, as long as we don't starve the reserved space. */
    _SEH2_TRY
    {
        volatile CHAR* Pointer = (PVOID)((ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit - PAGE_SIZE / 2);
        CHAR Value = *Pointer;
        (void)Value;
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_ntstatus(Status, STATUS_SUCCESS);

    _SEH2_TRY
    {
        infinite_recursive();
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        trace("Exception after %d iteration.\n", iteration);
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    ok_ntstatus(Status, STATUS_STACK_OVERFLOW);

    /* Windows lets 2 pages between the reserved memory and the smallest possible stack address */
    ok((ULONG_PTR)LastStackAllocation > (ULONG_PTR)StackAllocationBase, "\n");
#ifdef _WIN64
    ok_long(PAGE_ROUND_DOWN(LastStackAllocation), (ULONG_PTR)StackAllocationBase + 4 * PAGE_SIZE);
#else
    ok_long(PAGE_ROUND_DOWN(LastStackAllocation), (ULONG_PTR)StackAllocationBase + 3 * PAGE_SIZE);
#endif

    /* And in fact, this is the true condition of the stack overflow */
    ok_ptr(NtCurrentTeb()->NtTib.StackLimit, (PVOID)((ULONG_PTR)StackAllocationBase + PAGE_SIZE));

    trace("Stack limit %p, stack base %p.\n", NtCurrentTeb()->NtTib.StackLimit, NtCurrentTeb()->NtTib.StackBase);

    /* Of course, accessing above the stack limit is OK. */
    _SEH2_TRY
    {
        volatile CHAR* Pointer = (PVOID)((ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit + PAGE_SIZE / 2);
        CHAR Value = *Pointer;
        (void)Value;
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* But once stack is starved, it's starved. */
    _SEH2_TRY
    {
        volatile CHAR* Pointer = (PVOID)((ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit - PAGE_SIZE / 2);
        CHAR Value = *Pointer;
        (void)Value;
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        trace("Exception after %d iteration.\n", iteration);
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);
}

/* The same overflow on a thread created with the default stack, the stack of
 * every thread but the first one (a 32-bit thread under WoW64 included) */
static DWORD WINAPI StackOverflowThreadProc(PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    iteration = 0;
    func_StackOverflow();
    return 0x50f;
}

START_TEST(StackOverflowThread)
{
    HANDLE Thread;
    DWORD Exit = 0;

    Thread = CreateThread(NULL, 0, StackOverflowThreadProc, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return;
    ok(WaitForSingleObject(Thread, 60000) == WAIT_OBJECT_0, "The thread did not end\n");
    ok(GetExitCodeThread(Thread, &Exit), "GetExitCodeThread failed: %lu\n", GetLastError());
    ok(Exit == 0x50f, "Thread exit code 0x%lx\n", Exit);
    CloseHandle(Thread);
}

/* Deep use of the first thread's stack that does not overflow it: 900 KiB in
 * frames below a page each, so the stack grows one guard page at a time.
 * The test runs in its own process, so this is the first thread's stack. */
static ULONG_PTR DeepTarget;
static ULONG_PTR DeepLowest;

static
ULONG
deep_recursive(ULONG Depth)
{
    volatile CHAR Buffer[0xe00];
    ULONG Sum;

    Buffer[0] = (CHAR)Depth;
    Buffer[sizeof(Buffer) - 1] = (CHAR)Depth;
    DeepLowest = (ULONG_PTR)&Buffer[0];
    if ((ULONG_PTR)&Buffer[0] > DeepTarget)
        Sum = deep_recursive(Depth + 1);
    else
        Sum = Depth;
    return Sum + Buffer[0] - Buffer[sizeof(Buffer) - 1];
}

START_TEST(StackDeepRecursion)
{
    ULONG_PTR Base = (ULONG_PTR)NtCurrentTeb()->NtTib.StackBase;
    ULONG_PTR Reserve = (ULONG_PTR)NtCurrentTeb()->DeallocationStack;
    MEMORY_BASIC_INFORMATION Info;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Depth = 0;

    trace("Stack %p-%p, limit %p\n", (PVOID)Reserve, (PVOID)Base, NtCurrentTeb()->NtTib.StackLimit);
    /* The stack is not committed in full: a guard page is below the limit */
    ok((ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit > Reserve + 2 * PAGE_SIZE, "Stack limit %p, reserve %p\n",
       NtCurrentTeb()->NtTib.StackLimit, (PVOID)Reserve);
    Status = NtQueryVirtualMemory(NtCurrentProcess(), (PUCHAR)NtCurrentTeb()->NtTib.StackLimit - PAGE_SIZE,
                                  MemoryBasicInformation, &Info, sizeof(Info), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_long(Info.State, MEM_COMMIT);
    ok_long(Info.Protect, PAGE_READWRITE | PAGE_GUARD);
    /* 900 KiB, or all but 64 KiB of a smaller stack */
    if (Base - Reserve < 128 * 1024)
    {
        skip("The stack reserve is 0x%Ix\n", Base - Reserve);
        return;
    }
    DeepTarget = Base - min(900 * 1024, Base - Reserve - 64 * 1024);
    Status = STATUS_SUCCESS;
    _SEH2_TRY
    {
        Depth = deep_recursive(0);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(Depth > (Base - DeepTarget) / 0x1000, "Depth %lu\n", Depth);
    ok(DeepLowest <= DeepTarget, "Lowest frame %p, target %p\n", (PVOID)DeepLowest, (PVOID)DeepTarget);
    /* The stack grew to the deepest frame and still has a guard page below */
    ok((ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit <= DeepLowest, "Stack limit %p, lowest frame %p\n",
       NtCurrentTeb()->NtTib.StackLimit, (PVOID)DeepLowest);
    ok((ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit > Reserve + 2 * PAGE_SIZE, "Stack limit %p, reserve %p\n",
       NtCurrentTeb()->NtTib.StackLimit, (PVOID)Reserve);
    Status = NtQueryVirtualMemory(NtCurrentProcess(), (PUCHAR)NtCurrentTeb()->NtTib.StackLimit - PAGE_SIZE,
                                  MemoryBasicInformation, &Info, sizeof(Info), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_long(Info.State, MEM_COMMIT);
    ok_long(Info.Protect, PAGE_READWRITE | PAGE_GUARD);
}
