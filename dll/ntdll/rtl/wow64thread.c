/* Guest-only thread creation API. SPDX-License-Identifier: GPL-2.0-or-later */
#include <ntdll.h>

NTSTATUS NTAPI RtlpCreateUserStack(HANDLE, SIZE_T, SIZE_T, ULONG, PINITIAL_TEB);
VOID NTAPI RtlpFreeUserStack(HANDLE, PINITIAL_TEB);

typedef struct
{
    ULONG Attribute, Size;
    PVOID Value;
    PULONG ReturnLength;
} GUEST_THREAD_ATTRIBUTE;

typedef struct
{
    ULONG TotalLength;
    GUEST_THREAD_ATTRIBUTE Attributes[1];
} GUEST_THREAD_ATTRIBUTES;

static VOID NTAPI
Wow64UserThreadStart(PTHREAD_START_ROUTINE Routine, PVOID Argument)
{
    NTSTATUS Status;
    _SEH2_TRY { Status = Routine(Argument); }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { Status = _SEH2_GetExceptionCode(); }
    _SEH2_END;
    RtlExitUserThread(Status);
}

NTSTATUS NTAPI
NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK Access, POBJECT_ATTRIBUTES ObjectAttributes,
                 HANDLE Process, PTHREAD_START_ROUTINE Routine, PVOID Argument, ULONG Flags,
                 ULONG_PTR ZeroBits, SIZE_T Commit, SIZE_T Reserve, GUEST_THREAD_ATTRIBUTES *Attributes)
{
    INITIAL_TEB Stack = {0};
    CONTEXT Context;
    CLIENT_ID Id;
    THREAD_BASIC_INFORMATION Basic;
    HANDLE Thread = NULL, Result = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG i, Count = 0;
    PVOID Arguments[3] = {NULL, Routine, Argument};

    if (Flags & ~1) return STATUS_NOT_SUPPORTED;
    _SEH2_TRY
    {
        /* Capture and validate output buffers before allocating a thread. */
        *(volatile HANDLE *)ThreadHandle = *(volatile HANDLE *)ThreadHandle;
        if (Attributes)
        {
            if (Attributes->TotalLength < sizeof(ULONG) ||
                (Attributes->TotalLength - sizeof(ULONG)) % sizeof(GUEST_THREAD_ATTRIBUTE))
                _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
            Count = (Attributes->TotalLength - sizeof(ULONG)) / sizeof(GUEST_THREAD_ATTRIBUTE);
            if (Count > 16) _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
            for (i = 0; i < Count; ++i)
            {
                GUEST_THREAD_ATTRIBUTE *Attr = &Attributes->Attributes[i];
                ULONG j, Size;
                if (Attr->Attribute == 0x10003) Size = sizeof(CLIENT_ID);
                else if (Attr->Attribute == 0x10004) Size = sizeof(PVOID);
                else _SEH2_YIELD(return STATUS_NOT_SUPPORTED);
                if (Attr->Size != Size) _SEH2_YIELD(return STATUS_INFO_LENGTH_MISMATCH);
                for (j = 0; j < Size; ++j)
                    ((volatile BYTE *)Attr->Value)[j] = ((volatile BYTE *)Attr->Value)[j];
                if (Attr->ReturnLength)
                    *(volatile ULONG *)Attr->ReturnLength = *(volatile ULONG *)Attr->ReturnLength;
            }
        }
        Status = RtlpCreateUserStack(Process, Reserve, Commit, ZeroBits, &Stack);
        if (NT_SUCCESS(Status))
        {
            RtlZeroMemory(&Context, sizeof(Context));
            RtlInitializeContext(Process, &Context, NULL, (PTHREAD_START_ROUTINE)Wow64UserThreadStart, Stack.StackBase);
            Context.Esp = (ULONG_PTR)Stack.StackBase - sizeof(Arguments);
            Status = NtWriteVirtualMemory(Process, (PVOID)Context.Esp, Arguments, sizeof(Arguments), NULL);
        }
        if (NT_SUCCESS(Status))
            Status = NtCreateThread(&Thread, THREAD_ALL_ACCESS, ObjectAttributes, Process, &Id, &Context, &Stack, TRUE);
        if (NT_SUCCESS(Status))
        {
            for (i = 0; i < Count; ++i)
            {
                GUEST_THREAD_ATTRIBUTE *Attr = &Attributes->Attributes[i];
                if (Attr->Attribute == 0x10003) *(CLIENT_ID *)Attr->Value = Id;
                else
                {
                    Status = NtQueryInformationThread(Thread, ThreadBasicInformation, &Basic, sizeof(Basic), NULL);
                    if (!NT_SUCCESS(Status)) break;
                    *(PVOID *)Attr->Value = Basic.TebBaseAddress;
                }
                if (Attr->ReturnLength) *Attr->ReturnLength = Attr->Size;
            }
            if (NT_SUCCESS(Status))
                Status = NtDuplicateObject(NtCurrentProcess(), Thread, NtCurrentProcess(), &Result,
                                            Access, ObjectAttributes ? ObjectAttributes->Attributes & OBJ_INHERIT : 0, 0);
            if (NT_SUCCESS(Status)) *ThreadHandle = Result;
            if (NT_SUCCESS(Status) && !(Flags & 1)) Status = NtResumeThread(Thread, NULL);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { Status = _SEH2_GetExceptionCode(); }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
    {
        if (Result) NtClose(Result);
        if (Thread)
        {
            NtTerminateThread(Thread, Status);
            NtWaitForSingleObject(Thread, FALSE, NULL);
            NtClose(Thread);
        }
        if (Stack.AllocatedStackBase) RtlpFreeUserStack(Process, &Stack);
    }
    else NtClose(Thread);
    return Status;
}
