/* ReactOS WoW64 thread translation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"

NTSTATUS Wow64CreateThread(HANDLE *Handle, ACCESS_MASK Access,
                           OBJECT_ATTRIBUTES *Attributes, HANDLE Process,
                           CLIENT_ID *Id, const WOW64_CONTEXT *Context,
                           const ULONG *InitialTeb, BOOLEAN Suspended)
{
    CONTEXT Native = {0};
    INITIAL_TEB Stack = {0}, GuestStack;
    WOW64_CONTEXT Captured = *Context;
    THREAD_BASIC_INFORMATION Info;
    TEB Teb;
    TEB32 Teb32;
    ULONG_PTR Peb32;
    SIZE_T Size = 1024 * 1024, Zero = 0;
    PVOID ContextAddress;
    HANDLE Thread = NULL;
    NTSTATUS Status;

    if ((Captured.ContextFlags & WOW64_CONTEXT_FULL) != WOW64_CONTEXT_FULL)
        return STATUS_INVALID_PARAMETER;
    Status = NtQueryInformationProcess(Process, ProcessWow64Information,
                                       &Peb32, sizeof(Peb32), NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (!Peb32) return STATUS_INVALID_PARAMETER;
    GuestStack.PreviousStackBase = PTR32(InitialTeb[0]);
    GuestStack.PreviousStackLimit = PTR32(InitialTeb[1]);
    GuestStack.StackBase = PTR32(InitialTeb[2]);
    GuestStack.StackLimit = PTR32(InitialTeb[3]);
    GuestStack.AllocatedStackBase = PTR32(InitialTeb[4]);
    if (GuestStack.PreviousStackBase || GuestStack.PreviousStackLimit)
    {
        GuestStack.StackBase = GuestStack.PreviousStackBase;
        GuestStack.StackLimit = GuestStack.PreviousStackLimit;
    }

    /* Every guest thread needs a separate native stack for the loader and
     * argument translators. Keep the startup context in that allocation. */
    Status = NtAllocateVirtualMemory(Process, &Stack.AllocatedStackBase, 0,
                                     &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(Status)) return Status;
    Stack.StackBase = (PUCHAR)Stack.AllocatedStackBase + Size;
    Stack.StackLimit = Stack.AllocatedStackBase;
    ContextAddress = (PUCHAR)Stack.StackBase - ((sizeof(Captured) + 15) & ~15);
    Native.ContextFlags = CONTEXT_FULL;
    Native.Rsp = (ULONG_PTR)ContextAddress;
    Native.Rcx = (ULONG_PTR)ContextAddress;
    Native.Rdx = WOW64_THREAD_START_MARKER;
    Native.SegCs = 0x33;
    Native.SegSs = 0x2b;
    Native.EFlags = 0x202;
    Status = NtWriteVirtualMemory(Process, ContextAddress, &Captured, sizeof(Captured), NULL);
    if (NT_SUCCESS(Status))
        Status = NtCreateThread(&Thread, Access, Attributes, Process, Id,
                                 &Native, &Stack, TRUE);
    if (NT_SUCCESS(Status))
    {
        ULONG_PTR StartAddress = Captured.Eax;
        Status = NtSetInformationThread(Thread, ThreadQuerySetWin32StartAddress,
                                        &StartAddress, sizeof(StartAddress));
    }
    if (NT_SUCCESS(Status))
        Status = NtQueryInformationThread(Thread, ThreadBasicInformation, &Info, sizeof(Info), NULL);
    if (NT_SUCCESS(Status))
        Status = NtReadVirtualMemory(Process, Info.TebBaseAddress, &Teb, sizeof(Teb), NULL);
    if (NT_SUCCESS(Status))
    {
        Wow64InitializeTeb(&Teb32, &Teb, Info.TebBaseAddress, &GuestStack);
        Status = NtWriteVirtualMemory(Process, WOW64_TEB32(Info.TebBaseAddress),
                                      &Teb32, sizeof(Teb32), NULL);
    }
    if (NT_SUCCESS(Status) && !Suspended) Status = NtResumeThread(Thread, NULL);
    if (!NT_SUCCESS(Status))
    {
        if (Thread)
        {
            NtTerminateThread(Thread, Status);
            NtWaitForSingleObject(Thread, FALSE, NULL);
            NtClose(Thread);
        }
        NtFreeVirtualMemory(Process, &Stack.AllocatedStackBase, &Zero, MEM_RELEASE);
        return Status;
    }
    *Handle = Thread;
    return STATUS_SUCCESS;
}

NTSTATUS Wow64QueryThread(HANDLE Thread, ULONG Class, PVOID Buffer, ULONG Length, PULONG ReturnLength)
{
    NTSTATUS Status;
    if (Class == ThreadBasicInformation)
    {
        THREAD_BASIC_INFORMATION Native;
        struct { ULONG Status, Teb, Process, Thread, Affinity; LONG Priority, BasePriority; } *Guest = Buffer;
        if (Length != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        Status = NtQueryInformationThread(Thread, Class, &Native, sizeof(Native), NULL);
        if (NT_SUCCESS(Status))
        {
            Guest->Status = Native.ExitStatus;
            Guest->Teb = Native.TebBaseAddress ? PtrToUlong(WOW64_TEB32(Native.TebBaseAddress)) : 0;
            Guest->Process = HandleToUlong(Native.ClientId.UniqueProcess);
            Guest->Thread = HandleToUlong(Native.ClientId.UniqueThread);
            Guest->Affinity = (ULONG)Native.AffinityMask;
            Guest->Priority = Native.Priority;
            Guest->BasePriority = Native.BasePriority;
            if (ReturnLength) *ReturnLength = sizeof(*Guest);
        }
        return Status;
    }
    if (Class == ThreadQuerySetWin32StartAddress || Class == ThreadAffinityMask)
    {
        ULONG_PTR Value;
        if (Length != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
        Status = NtQueryInformationThread(Thread, Class, &Value, sizeof(Value), NULL);
        if (NT_SUCCESS(Status)) *(ULONG *)Buffer = (ULONG)Value;
        if (ReturnLength) *ReturnLength = sizeof(ULONG);
        return Status;
    }
    switch (Class)
    {
    case ThreadTimes: case ThreadPriorityBoost: case ThreadAmILastThread:
    case ThreadIsIoPending: case ThreadHideFromDebugger: case ThreadBreakOnTermination:
    case ThreadIsTerminated: case ThreadPerformanceCount:
        return NtQueryInformationThread(Thread, Class, Buffer, Length, ReturnLength);
    default: return STATUS_NOT_SUPPORTED;
    }
}
