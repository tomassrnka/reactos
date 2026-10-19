/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     RTL_DEBUG_INFORMATION implementation
 * COPYRIGHT:   Copyright James Tabor
 *              Copyright 2020 Mark Jansen (mark.jansen@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>
#include <heap.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

PVOID
NTAPI
RtlpDebugBufferCommit(_Inout_ PRTL_DEBUG_INFORMATION Buffer,
                     _In_ SIZE_T Size)
{
    ULONG Remaining = Buffer->CommitSize - Buffer->OffsetFree;
    PVOID Result;
    NTSTATUS Status;

    if (Size > MAXLONG)
        return NULL;

    /* Beyond the reserved range there may be anything else */
    if (Size > Buffer->ViewSize || Buffer->OffsetFree > Buffer->ViewSize - Size)
        return NULL;

    if (Remaining < Size)
    {
        PVOID Buf;
        SIZE_T CommitSize;

        Buf = (PVOID)((ULONG_PTR)Buffer->ViewBaseClient + Buffer->CommitSize);
        CommitSize = Size - Remaining;

        /* this is not going to end well.. */
        if (CommitSize > MAXLONG)
            return NULL;

        Status = NtAllocateVirtualMemory(NtCurrentProcess(), (PVOID*)&Buf, 0, &CommitSize, MEM_COMMIT, PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
            return NULL;

        Buffer->CommitSize += CommitSize;
        Remaining = Buffer->CommitSize - Buffer->OffsetFree;
        /* Sanity check */
        ASSERT(Remaining >= Size);
        if (Remaining < Size)
            return NULL;
    }

    Result = (PBYTE)Buffer->ViewBaseClient + Buffer->OffsetFree;
    Buffer->OffsetFree += Size;

    return Result;
}


/*
 * @unimplemented
 */
PRTL_DEBUG_INFORMATION
NTAPI
RtlCreateQueryDebugBuffer(_In_ ULONG Size,
                          _In_ BOOLEAN EventPair)
{
    NTSTATUS Status;
    PRTL_DEBUG_INFORMATION Buf = NULL;
    SIZE_T AllocationSize = Size ? Size : 0x400 * PAGE_SIZE;
    SIZE_T CommitSize = sizeof(*Buf);

    /* Reserve the memory */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), (PVOID*)&Buf, 0, &AllocationSize, MEM_RESERVE, PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
        return NULL;

    /* Commit the first data, CommitSize is updated with the actual committed data */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), (PVOID*)&Buf, 0, &CommitSize, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        RtlDestroyQueryDebugBuffer(Buf);
        return NULL;
    }

    /* Fill out the minimum data required */
    Buf->ViewBaseClient = Buf;
    Buf->ViewSize = AllocationSize;
    Buf->CommitSize = CommitSize;
    Buf->OffsetFree = sizeof(*Buf);

    return Buf;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
RtlDestroyQueryDebugBuffer(_In_ PRTL_DEBUG_INFORMATION Buf)
{
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T ViewSize = 0;

    if (NULL != Buf)
    {
        Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                     (PVOID*)&Buf,
                                     &ViewSize,
                                     MEM_RELEASE);
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("RtlDQDB: Failed to free VM!\n");
    }
    return Status;
}

#ifdef _WIN64
/* The fixed head of a 32-bit LDR_DATA_TABLE_ENTRY, as a WOW64 target keeps it in its 32-bit
   address space. The three LIST_ENTRY32 come first, then the base and size, the names (a
   32-bit UNICODE_STRING is a length, a maximum and a 32-bit pointer) and the flags. */
#include <pshpack4.h>
typedef struct _RTLP_LDR_ENTRY32
{
    ULONG  InLoadOrderFlink;        /* +0x00 */
    ULONG  Reserved0[5];            /* the rest of the three LIST_ENTRY32, +0x04..0x17 */
    ULONG  DllBase;                 /* +0x18 */
    ULONG  EntryPoint;              /* +0x1c */
    ULONG  SizeOfImage;             /* +0x20 */
    USHORT FullDllNameLength;       /* +0x24 */
    USHORT FullDllNameMaximum;      /* +0x26 */
    ULONG  FullDllNameBuffer;       /* +0x28 */
    ULONG  BaseDllName[2];          /* +0x2c..0x33 */
    ULONG  Flags;                   /* +0x34 */
    USHORT LoadCount;               /* +0x38 */
    USHORT TlsIndex;                /* +0x3a */
} RTLP_LDR_ENTRY32;
#include <poppack.h>

C_ASSERT(FIELD_OFFSET(RTLP_LDR_ENTRY32, DllBase) == 0x18);
C_ASSERT(FIELD_OFFSET(RTLP_LDR_ENTRY32, SizeOfImage) == 0x20);
C_ASSERT(FIELD_OFFSET(RTLP_LDR_ENTRY32, FullDllNameLength) == 0x24);
C_ASSERT(FIELD_OFFSET(RTLP_LDR_ENTRY32, FullDllNameBuffer) == 0x28);
C_ASSERT(FIELD_OFFSET(RTLP_LDR_ENTRY32, Flags) == 0x34);
C_ASSERT(FIELD_OFFSET(RTLP_LDR_ENTRY32, LoadCount) == 0x38);

/*
 * Appends the 32-bit loader module list of a WOW64 target to the list a 64-bit caller is
 * building. The 32-bit PEB holds the loader data pointer at offset 0x0c, the 32-bit
 * PEB_LDR_DATA holds the load-order list at 0x0c, and an entry is an RTLP_LDR_ENTRY32. The
 * running count, used size and write cursor are those of the native walk, so the two lists
 * are sized and filled by the same rules.
 */
static NTSTATUS
RtlpQueryRemoteProcessModules32(HANDLE ProcessHandle,
                               ULONG_PTR Peb32,
                               PRTL_PROCESS_MODULES Modules,
                               ULONG Size,
                               PULONG UsedSize,
                               PRTL_PROCESS_MODULE_INFORMATION *ModulePtrOut)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG LoaderData32 = 0, Head, Entry;
    PRTL_PROCESS_MODULE_INFORMATION ModulePtr = *ModulePtrOut;

    /* The 32-bit loader list is additive: when it cannot be read - the WOW64 loader has
       not published it yet (the pointer is still NULL), or the target is damaged or racing -
       keep the modules the native walk already collected rather than failing the whole query.
       Only a buffer that is too small for the extra modules is reported to the caller.
       The list lives in the target's 32-bit address space, so its pointers are 32-bit. */
    if (!NT_SUCCESS(NtReadVirtualMemory(ProcessHandle, (PVOID)(Peb32 + 0x0c),
                                        &LoaderData32, sizeof(LoaderData32), NULL)) ||
        LoaderData32 == 0)
    {
        return STATUS_SUCCESS;
    }

    /* InLoadOrderModuleList at PEB_LDR_DATA32 + 0x0c; read its first Flink */
    Head = LoaderData32 + 0x0c;
    if (!NT_SUCCESS(NtReadVirtualMemory(ProcessHandle, (PVOID)(ULONG_PTR)Head, &Entry, sizeof(Entry), NULL)))
    {
        return STATUS_SUCCESS;
    }

    while (Entry != Head)
    {
        RTLP_LDR_ENTRY32 Module32;

        /* read the current 32-bit module entry; a failed read ends the (best-effort) walk */
        if (!NT_SUCCESS(NtReadVirtualMemory(ProcessHandle, (PVOID)(ULONG_PTR)Entry, &Module32, sizeof(Module32), NULL)))
            break;

        if (Modules != NULL &&
            FIELD_OFFSET(RTL_PROCESS_MODULES, Modules) +
            ((SIZE_T)Modules->NumberOfModules + 1) * sizeof(RTL_PROCESS_MODULE_INFORMATION) > Size)
        {
            /* The list has grown since it was sized: do not write past the buffer */
            Status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        else if (Modules != NULL)
        {
            UNICODE_STRING Unicode;
            ANSI_STRING AnsiString;
            WCHAR Buffer[256];
            PCHAR p;

            /* Import the module name from the target. The target controls the length,
               so it is clamped to leave room for the ANSI string and its terminator. */
            Unicode.Length = min(Module32.FullDllNameLength,
                                 (USHORT)((sizeof(ModulePtr->FullPathName) - 1) * sizeof(WCHAR))) & ~(USHORT)1;
            Unicode.MaximumLength = sizeof(Buffer);
            Unicode.Buffer = Buffer;
            if (Unicode.Length != 0 &&
                !NT_SUCCESS(NtReadVirtualMemory(ProcessHandle,
                                                (PVOID)(ULONG_PTR)Module32.FullDllNameBuffer,
                                                Unicode.Buffer,
                                                Unicode.Length,
                                                NULL)))
            {
                /* the name could not be read: stop before committing this entry */
                break;
            }

            ModulePtr->Section        = 0;
            ModulePtr->MappedBase     = NULL;
            ModulePtr->ImageBase      = (PVOID)(ULONG_PTR)Module32.DllBase;
            ModulePtr->ImageSize      = Module32.SizeOfImage;
            ModulePtr->Flags          = Module32.Flags;
            ModulePtr->LoadOrderIndex = 0;
            ModulePtr->InitOrderIndex = 0;
            ModulePtr->LoadCount      = Module32.LoadCount;

            AnsiString.Length        = 0;
            AnsiString.MaximumLength = sizeof(ModulePtr->FullPathName);
            AnsiString.Buffer        = ModulePtr->FullPathName;
            RtlUnicodeStringToAnsiString(&AnsiString, &Unicode, FALSE);

            p = strrchr(ModulePtr->FullPathName, '\\');
            if (p != NULL)
                ModulePtr->OffsetToFileName = (USHORT)(p - ModulePtr->FullPathName + 1);
            else
                ModulePtr->OffsetToFileName = 0;

            ModulePtr++;
            Modules->NumberOfModules++;
        }
        *UsedSize += sizeof(RTL_PROCESS_MODULE_INFORMATION);

        /* When sizing, a list that does not fit the caller's room cannot be returned */
        if (Modules == NULL && *UsedSize > Size)
            return STATUS_NO_MEMORY;

        /* address of the next module in the list */
        Entry = Module32.InLoadOrderFlink;
    }

    *ModulePtrOut = ModulePtr;
    return Status;
}
#endif /* _WIN64 */

/*
 *  Based on lib/epsapi/enum/modules.c by KJK::Hyperion.
 */
NTSTATUS
NTAPI
RtlpQueryRemoteProcessModules(HANDLE ProcessHandle,
                              IN PRTL_PROCESS_MODULES Modules OPTIONAL,
                              IN ULONG Size OPTIONAL,
                              OUT PULONG ReturnedSize)
{
    PROCESS_BASIC_INFORMATION pbiInfo;
    PPEB_LDR_DATA ppldLdrData;
    LDR_DATA_TABLE_ENTRY lmModule;
    PLIST_ENTRY pleListHead;
    PLIST_ENTRY pleCurEntry;

    PRTL_PROCESS_MODULE_INFORMATION ModulePtr = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG UsedSize = FIELD_OFFSET(RTL_PROCESS_MODULES, Modules);
    ANSI_STRING AnsiString;
    PCHAR p;

    DPRINT("RtlpQueryRemoteProcessModules Start\n");

    /* query the process basic information (includes the PEB address) */
    Status = NtQueryInformationProcess(ProcessHandle,
                                       ProcessBasicInformation,
                                       &pbiInfo,
                                       sizeof(PROCESS_BASIC_INFORMATION),
                                       NULL);

    if (!NT_SUCCESS(Status))
    {
        /* failure */
        DPRINT("NtQueryInformationProcess 1 0x%lx\n", Status);
        return Status;
    }

    if (Modules == NULL)
    {
        Status = STATUS_INFO_LENGTH_MISMATCH;
    }
    else if (Size < FIELD_OFFSET(RTL_PROCESS_MODULES, Modules))
    {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    else
    {
        Modules->NumberOfModules = 0;
        ModulePtr = &Modules->Modules[0];
        Status = STATUS_SUCCESS;
    }

    /* get the address of the PE Loader data */
    Status = NtReadVirtualMemory(ProcessHandle,
                                 &(pbiInfo.PebBaseAddress->Ldr),
                                 &ppldLdrData,
                                 sizeof(ppldLdrData),
                                 NULL);

    if (!NT_SUCCESS(Status))
    {
        /* failure */
        DPRINT("NtReadVirtualMemory 1 0x%lx\n", Status);
        return Status;
    }


    /* head of the module list: the last element in the list will point to this */
    pleListHead = &ppldLdrData->InLoadOrderModuleList;

    /* get the address of the first element in the list */
    Status = NtReadVirtualMemory(ProcessHandle,
                                 &(ppldLdrData->InLoadOrderModuleList.Flink),
                                 &pleCurEntry,
                                 sizeof(pleCurEntry),
                                 NULL);

    if (!NT_SUCCESS(Status))
    {
        /* failure */
        DPRINT("NtReadVirtualMemory 2 0x%lx\n", Status);
        return Status;
    }

    while(pleCurEntry != pleListHead)
    {
        UNICODE_STRING Unicode;
        WCHAR  Buffer[256 * sizeof(WCHAR)];

        /* read the current module */
        Status = NtReadVirtualMemory(ProcessHandle,
                                     CONTAINING_RECORD(pleCurEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks),
                                     &lmModule,
                                     sizeof(LDR_DATA_TABLE_ENTRY),
                                     NULL);

        if (!NT_SUCCESS(Status))
        {
            /* failure */
            DPRINT( "NtReadVirtualMemory 3 0x%lx\n", Status);
            return Status;
        }

        /* Import module name from remote Process user space. The process
           controls the length, so it must not exceed the buffer. */
        Unicode.Length = min(lmModule.FullDllName.Length, sizeof(Buffer)) & ~(USHORT)1;
        Unicode.MaximumLength = sizeof(Buffer);
        Unicode.Buffer = Buffer;

        Status = NtReadVirtualMemory(ProcessHandle,
                                     lmModule.FullDllName.Buffer,
                                     Unicode.Buffer,
                                     Unicode.Length,
                                     NULL);

        if (!NT_SUCCESS(Status))
        {
            /* failure */
            DPRINT( "NtReadVirtualMemory 3 0x%lx\n", Status);
            return Status;
        }

        DPRINT("  Module %wZ\n", &Unicode);

        if (Modules != NULL &&
            FIELD_OFFSET(RTL_PROCESS_MODULES, Modules) +
            ((SIZE_T)Modules->NumberOfModules + 1) * sizeof(RTL_PROCESS_MODULE_INFORMATION) > Size)
        {
            /* The list has grown since it was sized: do not write past the buffer */
            Status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        else if (Modules != NULL)
        {
            ModulePtr->Section        = 0;
            ModulePtr->MappedBase     = NULL;      // FIXME: ??
            ModulePtr->ImageBase      = lmModule.DllBase;
            ModulePtr->ImageSize      = lmModule.SizeOfImage;
            ModulePtr->Flags          = lmModule.Flags;
            ModulePtr->LoadOrderIndex = 0;      // FIXME:  ??
            ModulePtr->InitOrderIndex = 0;      // FIXME: ??
            ModulePtr->LoadCount      = lmModule.LoadCount;

            AnsiString.Length        = 0;
            AnsiString.MaximumLength = 256;
            AnsiString.Buffer        = ModulePtr->FullPathName;
            RtlUnicodeStringToAnsiString(&AnsiString,
                                         &Unicode,
                                         FALSE);

            p = strrchr(ModulePtr->FullPathName, '\\');
            if (p != NULL)
                ModulePtr->OffsetToFileName = (USHORT)(p - ModulePtr->FullPathName + 1);
            else
                ModulePtr->OffsetToFileName = 0;

            ModulePtr++;
            Modules->NumberOfModules++;
        }
        UsedSize += sizeof(RTL_PROCESS_MODULE_INFORMATION);

        /* The list is read from a process that can damage it, even make it
           a cycle. When sizing, Size is the room the caller has: a list that
           does not fit there cannot be returned, so stop the walk. */
        if (Modules == NULL && UsedSize > Size)
            return STATUS_NO_MEMORY;

        /* address of the next module in the list */
        pleCurEntry = lmModule.InLoadOrderLinks.Flink;
    }

#ifdef _WIN64
    /* A WOW64 target keeps a second, 32-bit loader module list in its 32-bit PEB. The walk
       above reads the 64-bit PEB, which for such a process holds only the native ntdll and
       the wow64 support DLLs, not the 32-bit modules the program actually loaded. A 64-bit
       caller appends the 32-bit list too, so toolhelp and tasklist report a 32-bit process'
       modules the way Windows does. */
    if (NT_SUCCESS(Status))
    {
        ULONG_PTR Peb32 = 0;

        if (NT_SUCCESS(NtQueryInformationProcess(ProcessHandle,
                                                 ProcessWow64Information,
                                                 &Peb32,
                                                 sizeof(Peb32),
                                                 NULL)) &&
            Peb32 != 0)
        {
            Status = RtlpQueryRemoteProcessModules32(ProcessHandle, Peb32, Modules, Size,
                                                     &UsedSize, &ModulePtr);
            if (!NT_SUCCESS(Status))
            {
                if (ReturnedSize != 0)
                    *ReturnedSize = UsedSize;
                return Status;
            }
        }
    }
#endif /* _WIN64 */

    if (ReturnedSize != 0)
        *ReturnedSize = UsedSize;

    DPRINT("RtlpQueryRemoteProcessModules End\n");

    /* A list that outgrew the buffer is reported, the sizing pass has no buffer */
    return (Modules != NULL) ? Status : STATUS_SUCCESS;
}


/* The list of heaps is the one the process keeps in its PEB. */
static NTSTATUS
RtlpQueryProcessHeapList(_In_opt_ HANDLE ProcessHandle,
                         _Inout_ PRTL_DEBUG_INFORMATION Buf)
{
    PPEB Peb, RemotePeb = NULL;
    PVOID ProcessHeap, *RemoteList;
    PHANDLE Handles;
    PRTL_PROCESS_HEAPS Heaps;
    NTSTATUS Status;
    LARGE_INTEGER RetryDelay;
    ULONG Count = 0, Read, Retry, i;

    RetryDelay.QuadPart = -10000; /* 1 ms */

    if (ProcessHandle == NULL)
    {
        Peb = RtlGetCurrentPeb();
        ProcessHeap = Peb->ProcessHeap;
    }
    else
    {
        PROCESS_BASIC_INFORMATION Info;

        Status = NtQueryInformationProcess(ProcessHandle, ProcessBasicInformation, &Info, sizeof(Info), NULL);
        if (!NT_SUCCESS(Status))
            return Status;

        /* The system process has no PEB */
        RemotePeb = Info.PebBaseAddress;
        if (RemotePeb == NULL)
            return STATUS_ACCESS_DENIED;

        Status = NtReadVirtualMemory(ProcessHandle, &RemotePeb->ProcessHeap, &ProcessHeap, sizeof(ProcessHeap), NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* The list can change while it is read: ask for the count, read the handles, and read the count again */
    for (Retry = 0; ; Retry++)
    {
        if (Retry == 8)
            return STATUS_UNSUCCESSFUL;

        if (ProcessHandle == NULL)
        {
            Count = RtlGetProcessHeaps(0, NULL);
        }
        else
        {
            Status = NtReadVirtualMemory(ProcessHandle, &RemotePeb->NumberOfHeaps, &Count, sizeof(Count), NULL);
            if (!NT_SUCCESS(Status))
                return Status;
        }

        /* A sanity limit, so that a damaged PEB cannot ask for gigabytes */
        if (Count > 0x10000)
            return STATUS_UNSUCCESSFUL;

        Handles = RtlpDebugBufferCommit(Buf, (SIZE_T)Count * sizeof(HANDLE));
        if (Handles == NULL)
            return STATUS_NO_MEMORY;

        if (ProcessHandle == NULL)
        {
            Read = RtlGetProcessHeaps(Count, Handles);
            if (Read <= Count)
            {
                Count = Read;
                break;
            }
        }
        else
        {
            Status = NtReadVirtualMemory(ProcessHandle, &RemotePeb->ProcessHeaps, &RemoteList, sizeof(RemoteList), NULL);
            if (!NT_SUCCESS(Status))
                return Status;

            if (Count != 0)
            {
                Status = NtReadVirtualMemory(ProcessHandle, RemoteList, Handles, (SIZE_T)Count * sizeof(HANDLE), NULL);
                if (!NT_SUCCESS(Status))
                    return Status;
            }

            Status = NtReadVirtualMemory(ProcessHandle, &RemotePeb->NumberOfHeaps, &Read, sizeof(Read), NULL);
            if (!NT_SUCCESS(Status))
                return Status;

            /* The target moves the entries before it lowers the count, which leaves a heap twice in a row, and clears the freed entry */
            if (Read == Count)
            {
                for (i = 0; i < Count; i++)
                {
                    if (Handles[i] == NULL || (i != 0 && Handles[i] == Handles[i - 1]))
                        break;
                }

                if (i >= Count)
                    break;
            }
        }

        /* Give the target a moment to finish the change */
        NtDelayExecution(FALSE, &RetryDelay);
    }

    Heaps = RtlpDebugBufferCommit(Buf, FIELD_OFFSET(RTL_PROCESS_HEAPS, Heaps) + (SIZE_T)Count * sizeof(RTL_HEAP_INFORMATION));
    if (Heaps == NULL)
        return STATUS_NO_MEMORY;

    RtlZeroMemory(Heaps, FIELD_OFFSET(RTL_PROCESS_HEAPS, Heaps) + (SIZE_T)Count * sizeof(RTL_HEAP_INFORMATION));
    Heaps->NumberOfHeaps = Count;
    for (i = 0; i < Count; i++)
        Heaps->Heaps[i].BaseAddress = Handles[i];

    Buf->Heaps = Heaps;
    Buf->ProcessHeap = ProcessHeap;
    return STATUS_SUCCESS;
}

/* Reads the memory of another process, a page at a time as the heap structures lie close together */
typedef struct _RTLP_REMOTE_READ
{
    HANDLE Process;
    ULONG_PTR WindowBase;
    BOOLEAN WindowValid;
    UCHAR Window[PAGE_SIZE];
} RTLP_REMOTE_READ, *PRTLP_REMOTE_READ;

static
NTSTATUS
NTAPI
RtlpHeapReadRemote(_In_opt_ PVOID Context,
                   _In_ PVOID Address,
                   _Out_writes_bytes_(Size) PVOID Buffer,
                   _In_ SIZE_T Size)
{
    PRTLP_REMOTE_READ Remote = Context;
    ULONG_PTR Start = (ULONG_PTR)Address;
    ULONG_PTR Page = Start & ~((ULONG_PTR)PAGE_SIZE - 1);
    SIZE_T Read = 0;
    NTSTATUS Status;

    if (Size != 0 && Size <= PAGE_SIZE && Start + Size > Start && Start + Size <= Page + PAGE_SIZE)
    {
        if (!Remote->WindowValid || Remote->WindowBase != Page)
        {
            Remote->WindowValid = FALSE;
            Status = NtReadVirtualMemory(Remote->Process, (PVOID)Page, Remote->Window, PAGE_SIZE, &Read);
            if (NT_SUCCESS(Status) && Read == PAGE_SIZE)
            {
                Remote->WindowBase = Page;
                Remote->WindowValid = TRUE;
            }
        }

        if (Remote->WindowValid)
        {
            RtlCopyMemory(Buffer, Remote->Window + (Start - Page), Size);
            return STATUS_SUCCESS;
        }
    }

    Status = NtReadVirtualMemory(Remote->Process, Address, Buffer, Size, &Read);
    if (NT_SUCCESS(Status) && Read != Size)
        Status = STATUS_PARTIAL_COPY;

    return Status;
}

typedef struct _RTLP_HEAP_BLOCKS
{
    PRTL_DEBUG_INFORMATION Buf;
    PRTL_HEAP_INFORMATION Heap;
    NTSTATUS Status;
    BOOLEAN AfterUncommitted;
} RTLP_HEAP_BLOCKS, *PRTLP_HEAP_BLOCKS;

/* Whoever wants the blocks of a heap finds them in an array, so every entry goes to the next place */
static
BOOLEAN
RtlpAddHeapBlock(_Inout_ PRTLP_HEAP_BLOCKS Blocks,
                 _In_ SIZE_T Size,
                 _In_ USHORT Flags,
                 _In_ USHORT BackTraceIndex,
                 _In_ ULONG_PTR First,
                 _In_ ULONG_PTR Second)
{
    PRTL_HEAP_INFORMATION Heap = Blocks->Heap;
    PRTL_HEAP_ENTRY Entry;

    Entry = RtlpDebugBufferCommit(Blocks->Buf, sizeof(*Entry));
    if (Entry == NULL)
    {
        Blocks->Status = STATUS_BUFFER_TOO_SMALL;
        return FALSE;
    }

    /* The memory may have been used by a walk that was thrown away */
    RtlZeroMemory(Entry, sizeof(*Entry));

    if (Heap->NumberOfEntries == 0)
    {
        Heap->Entries = Entry;
    }
    else if (Entry != Heap->Entries + Heap->NumberOfEntries)
    {
        Blocks->Status = STATUS_UNSUCCESSFUL;
        return FALSE;
    }

    Entry->Size = Size;
    Entry->Flags = Flags;
    Entry->AllocatorBackTraceIndex = BackTraceIndex;
    if (Flags & RTL_HEAP_WALK_REGION)
    {
        Entry->u.s2.CommittedSize = First;
        Entry->u.s2.FirstBlock = (PVOID)Second;
    }
    else
    {
        Entry->u.s1.Settable = First;
        Entry->u.s1.Tag = (ULONG)Second;
    }

    Heap->NumberOfEntries++;
    return TRUE;
}

/*
 * The segments of a heap start a run of entries: the first block is at the address that the segment
 * entry gives, and each next one follows the one before. Where a run begins that is not behind its
 * segment entry, because an uncommitted range comes first or the block is not in a segment, an entry
 * of a segment without a size says where.
 */
static
BOOLEAN
NTAPI
RtlpHeapBlocksCallback(_In_opt_ PVOID Context,
                       _In_ PRTL_HEAP_WALK_ENTRY Entry)
{
    PRTLP_HEAP_BLOCKS Blocks = Context;
    PRTL_HEAP_INFORMATION Heap = Blocks->Heap;
    ULONG_PTR Start;
    SIZE_T Size;

    if (Entry->Flags & RTL_HEAP_WALK_REGION)
    {
        Blocks->AfterUncommitted = FALSE;
        Size = (ULONG_PTR)Entry->Segment.LastEntry - (ULONG_PTR)Entry->DataAddress;
        Heap->BytesCommitted += Entry->Segment.CommittedSize;
        return RtlpAddHeapBlock(Blocks, Size, RTL_HEAP_WALK_REGION, 0,
                                Entry->Segment.CommittedSize, (ULONG_PTR)Entry->Segment.FirstEntry);
    }

    if (Entry->Flags & RTL_HEAP_WALK_UNCOMMITTED)
    {
        Blocks->AfterUncommitted = TRUE;
        return RtlpAddHeapBlock(Blocks, Entry->DataSize, RTL_HEAP_WALK_UNCOMMITTED, 0, 0, 0);
    }

    Size = Entry->DataSize + Entry->OverheadBytes;

    if (Entry->Flags & RTL_HEAP_WALK_VIRTUAL)
    {
        /* A block of its own is a region of its own */
        Start = (ULONG_PTR)Entry->DataAddress - sizeof(HEAP_ENTRY);
        Size = Entry->Block.Reserved[0];
        Heap->BytesCommitted += Size;
        if (!RtlpAddHeapBlock(Blocks, Size, RTL_HEAP_WALK_REGION, 0, Size, Start))
            return FALSE;
    }
    else if (Blocks->AfterUncommitted)
    {
        Start = (ULONG_PTR)Entry->DataAddress - ((Entry->Flags & RTL_HEAP_WALK_BUSY) ? sizeof(HEAP_ENTRY) : sizeof(HEAP_FREE_ENTRY));
        if (!RtlpAddHeapBlock(Blocks, 0, RTL_HEAP_WALK_REGION, 0, 0, Start))
            return FALSE;
    }

    Blocks->AfterUncommitted = FALSE;

    if (Entry->Flags & RTL_HEAP_WALK_BUSY)
        Heap->BytesAllocated += Size;

    return RtlpAddHeapBlock(Blocks, Size, Entry->Flags, Entry->Block.AllocatorBackTraceIndex,
                            (Entry->Flags & RTL_HEAP_WALK_BLOCK) ? Entry->Block.Settable : 0,
                            (Entry->Flags & RTL_HEAP_WALK_BLOCK) ? Entry->Block.TagIndex : 0);
}

/* The lock of a heap in another process is free, or its count is not -1 */
static
BOOLEAN
RtlpRemoteHeapIsLocked(_In_ PRTLP_REMOTE_READ Remote,
                       _In_ PVOID HeapAddress)
{
    ULONG_PTR Lock;
    LONG Count;
    ULONG Flags;
    SIZE_T Read;

    /* The window of the pages that were read must not answer */
    Remote->WindowValid = FALSE;

    if (!NT_SUCCESS(NtReadVirtualMemory(Remote->Process, (PUCHAR)HeapAddress + FIELD_OFFSET(HEAP, Flags), &Flags, sizeof(Flags), &Read)) ||
        Read != sizeof(Flags))
    {
        return TRUE;
    }

    if (Flags & HEAP_NO_SERIALIZE)
        return FALSE;

    if (!NT_SUCCESS(NtReadVirtualMemory(Remote->Process, (PUCHAR)HeapAddress + FIELD_OFFSET(HEAP, LockVariable), &Lock, sizeof(Lock), &Read)) ||
        Read != sizeof(Lock))
    {
        return TRUE;
    }

    if (Lock == 0)
        return FALSE;

    return !NT_SUCCESS(NtReadVirtualMemory(Remote->Process, (PUCHAR)Lock + FIELD_OFFSET(RTL_CRITICAL_SECTION, LockCount), &Count, sizeof(Count), &Read)) ||
           Read != sizeof(Count) ||
           Count != -1;
}

/*
 * Reads a heap of this process. The heap list lock is held for the whole read: RtlDestroyHeap takes a heap
 * off the list before it deletes the lock of the heap, so the lock of a heap that is on the list is there
 * until the read is done (its memory is released earlier, so a read may fault and is then left out). The lock
 * of the heap is taken without waiting, because a thread that holds it may be waiting for the list lock; the
 * list lock is given up for a moment and the heap tried again.
 */
static
NTSTATUS
RtlpEnumerateLocalHeap(_In_ PVOID HeapAddress,
                       _In_ PRTLP_HEAP_WALK_CALLBACK Callback,
                       _In_ PVOID Context,
                       _Out_ PULONG Flags)
{
    PPEB Peb = RtlGetCurrentPeb();
    PHEAP Heap = HeapAddress;
    PHEAP_LOCK HeapLock;
    LARGE_INTEGER RetryDelay;
    NTSTATUS Status;
    ULONG Index, Try;
    BOOLEAN Locked, Faulted;

    RetryDelay.QuadPart = -10000; /* 1 ms */

    for (Try = 0; Try < 10000; Try++)
    {
        Faulted = FALSE;
        RtlEnterCriticalSection(&RtlpProcessHeapsListLock);

        for (Index = 0; Index < Peb->NumberOfHeaps; Index++)
        {
            if (Peb->ProcessHeaps[Index] == HeapAddress)
                break;
        }

        /* The heap is gone: it has no blocks to report */
        if (Index == Peb->NumberOfHeaps)
        {
            RtlLeaveCriticalSection(&RtlpProcessHeapsListLock);
            return STATUS_INVALID_HANDLE;
        }

        /* A page heap and what is not a heap are left to the walk, which refuses them */
        HeapLock = NULL;
        Locked = TRUE;
        _SEH2_TRY
        {
            if (!(Heap->ForceFlags & HEAP_FLAG_PAGE_ALLOCS) && Heap->Signature == HEAP_SIGNATURE &&
                !(Heap->Flags & HEAP_NO_SERIALIZE) && Heap->LockVariable != NULL)
            {
                HeapLock = Heap->LockVariable;
                Locked = RtlTryEnterHeapLock(HeapLock, TRUE);
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            HeapLock = NULL;
            Locked = FALSE;
            Faulted = TRUE;
        }
        _SEH2_END;

        if (Faulted)
        {
            RtlLeaveCriticalSection(&RtlpProcessHeapsListLock);
            return STATUS_INVALID_HANDLE;
        }
        if (!Locked)
        {
            RtlLeaveCriticalSection(&RtlpProcessHeapsListLock);
            NtDelayExecution(FALSE, &RetryDelay);
            continue;
        }

        /* The walk takes the lock of the heap again, which it may as the owner */
        Status = RtlpEnumerateHeap(NULL, NULL, HeapAddress, Callback, Context, Flags);

        if (HeapLock)
            RtlLeaveHeapLock(HeapLock);
        RtlLeaveCriticalSection(&RtlpProcessHeapsListLock);
        return Status;
    }

    return STATUS_RETRY;
}

/*
 * The blocks of the heaps that the list holds. The heaps of this process are locked while they are
 * read. Those of another process cannot be: a heap is read when its lock is free, and again if the
 * lock was taken meanwhile or what was read does not add up. A heap that does not come out right is
 * left without blocks.
 */
static
NTSTATUS
RtlpQueryProcessHeapBlocks(_In_opt_ HANDLE ProcessHandle,
                           _Inout_ PRTL_DEBUG_INFORMATION Buf)
{
    PRTL_PROCESS_HEAPS Heaps = Buf->Heaps;
    PRTLP_REMOTE_READ Remote = NULL;
    RTLP_HEAP_BLOCKS Blocks;
    LARGE_INTEGER RetryDelay;
    ULONG Index, Retry, Flags, Saved;
    NTSTATUS Status;

    RetryDelay.QuadPart = -10000; /* 1 ms */

    if (ProcessHandle != NULL)
    {
        /* One page of cache is too much for the stack */
        Remote = RtlpDebugBufferCommit(Buf, sizeof(*Remote));
        if (Remote == NULL)
            return STATUS_BUFFER_TOO_SMALL;

        Remote->Process = ProcessHandle;
    }

    Blocks.Buf = Buf;

    for (Index = 0; Index < Heaps->NumberOfHeaps; Index++)
    {
        Blocks.Heap = &Heaps->Heaps[Index];
        Blocks.Heap->EntryOverhead = sizeof(HEAP_ENTRY);

        for (Retry = 0; Retry < 8; Retry++)
        {
            Saved = Buf->OffsetFree;
            Blocks.Status = STATUS_SUCCESS;
            Blocks.AfterUncommitted = FALSE;
            Blocks.Heap->Flags = 0;
            Blocks.Heap->BytesAllocated = 0;
            Blocks.Heap->BytesCommitted = 0;
            Blocks.Heap->NumberOfEntries = 0;
            Blocks.Heap->Entries = NULL;
            Flags = 0;

            if (Remote == NULL)
            {
                Status = RtlpEnumerateLocalHeap(Blocks.Heap->BaseAddress, RtlpHeapBlocksCallback, &Blocks, &Flags);
            }
            else if (RtlpRemoteHeapIsLocked(Remote, Blocks.Heap->BaseAddress))
            {
                Status = STATUS_RETRY;
            }
            else
            {
                Remote->WindowValid = FALSE;
                Status = RtlpEnumerateHeap(RtlpHeapReadRemote, Remote, Blocks.Heap->BaseAddress, RtlpHeapBlocksCallback, &Blocks, &Flags);
                if (NT_SUCCESS(Status) && Blocks.Status == STATUS_SUCCESS && RtlpRemoteHeapIsLocked(Remote, Blocks.Heap->BaseAddress))
                    Status = STATUS_RETRY;
            }

            if (Blocks.Status != STATUS_SUCCESS)
                return Blocks.Status;

            if (NT_SUCCESS(Status))
            {
                Blocks.Heap->Flags = Flags;
                break;
            }

            /* Not a heap that can be read now: forget what was stored, and try again if somebody else was at work */
            Buf->OffsetFree = Saved;
            Blocks.Heap->BytesAllocated = 0;
            Blocks.Heap->BytesCommitted = 0;
            Blocks.Heap->NumberOfEntries = 0;
            Blocks.Heap->Entries = NULL;

            if (Remote == NULL || (Status != STATUS_RETRY && Status != STATUS_INVALID_PARAMETER))
                break;

            NtDelayExecution(FALSE, &RetryDelay);
        }
    }

    return STATUS_SUCCESS;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
RtlQueryProcessDebugInformation(IN ULONG ProcessId,
                                IN ULONG DebugInfoMask,
                                IN OUT PRTL_DEBUG_INFORMATION Buf)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Pid = (ULONG)(ULONG_PTR) NtCurrentTeb()->ClientId.UniqueProcess;

    Buf->Flags = DebugInfoMask;
    Buf->OffsetFree = sizeof(RTL_DEBUG_INFORMATION);

    DPRINT("QueryProcessDebugInformation Start\n");

    /*
    Currently ROS can not read-only from kenrel space, and doesn't
    check for boundaries inside kernel space that are page protected
    from every one but the kernel. aka page 0 - 2
    */
    if (ProcessId == 0)
    {
        Status = STATUS_INVALID_HANDLE;
    }
    else if (ProcessId == 1)
    {
        Status = STATUS_ACCESS_VIOLATION;
    }
    else
        if (Pid == ProcessId)
        {
            if (DebugInfoMask & RTL_DEBUG_QUERY_MODULES)
            {
                PRTL_PROCESS_MODULES Mp;
                ULONG ReturnSize = 0;

                /* I like this better than the do & while loop. */
                Status = LdrQueryProcessModuleInformation(NULL,
                                                          0,
                                                          &ReturnSize);

                Mp = RtlpDebugBufferCommit(Buf, ReturnSize);
                if (!Mp)
                {
                    DPRINT1("RtlQueryProcessDebugInformation: Unable to commit %u\n", ReturnSize);
                }

                Status = LdrQueryProcessModuleInformation(Mp,
                                                          ReturnSize,
                                                          &ReturnSize);
                if (!NT_SUCCESS(Status))
                {
                    return Status;
                }

                Buf->Modules = Mp;
            }

            if (DebugInfoMask & RTL_DEBUG_QUERY_HEAPS)
            {
                Status = RtlpQueryProcessHeapList(NULL, Buf);
                if (!NT_SUCCESS(Status))
                {
                    return Status;
                }

                if (DebugInfoMask & RTL_DEBUG_QUERY_HEAP_BLOCKS)
                {
                    Status = RtlpQueryProcessHeapBlocks(NULL, Buf);
                    if (!NT_SUCCESS(Status))
                    {
                        return Status;
                    }
                }
            }

            if (DebugInfoMask & RTL_DEBUG_QUERY_LOCKS)
            {
                PRTL_PROCESS_LOCKS Lp;
                ULONG LSize;

                Lp = (PRTL_PROCESS_LOCKS)((PUCHAR)Buf + Buf->OffsetFree);
                LSize = sizeof(RTL_PROCESS_LOCKS);
                Buf->Locks = Lp;
                Buf->OffsetFree = Buf->OffsetFree + LSize;
            }

            DPRINT("QueryProcessDebugInformation end\n");
            DPRINT("QueryDebugInfo : 0x%lx\n", Buf->OffsetFree);
        }
        else
        {
            HANDLE hProcess;
            CLIENT_ID ClientId;
            OBJECT_ATTRIBUTES ObjectAttributes;

            Buf->TargetProcessHandle = NtCurrentProcess();

            ClientId.UniqueThread = 0;
            ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)ProcessId;
            InitializeObjectAttributes(&ObjectAttributes,
                                       NULL,
                                       0,
                                       NULL,
                                       NULL);

            Status = NtOpenProcess(&hProcess,
                                   (PROCESS_ALL_ACCESS),
                                   &ObjectAttributes,
                                   &ClientId );
            if (!NT_SUCCESS(Status))
            {
                return Status;
            }

            if (DebugInfoMask & RTL_DEBUG_QUERY_MODULES)
            {
                PRTL_PROCESS_MODULES Mp;
                ULONG ReturnSize = 0;

                /* Size the list, at most to the room left in the buffer */
                Status = RtlpQueryRemoteProcessModules(hProcess,
                                                       NULL,
                                                       (ULONG)min(Buf->ViewSize - Buf->OffsetFree, MAXLONG),
                                                       &ReturnSize);
                if (!NT_SUCCESS(Status))
                {
                    NtClose(hProcess);
                    return Status;
                }

                Mp = RtlpDebugBufferCommit(Buf, ReturnSize);
                if (!Mp)
                {
                    DPRINT1("RtlQueryProcessDebugInformation: Unable to commit %u\n", ReturnSize);
                    NtClose(hProcess);
                    return STATUS_NO_MEMORY;
                }

                Status = RtlpQueryRemoteProcessModules(hProcess,
                                                       Mp,
                                                       ReturnSize ,
                                                       &ReturnSize);
                if (!NT_SUCCESS(Status))
                {
                    NtClose(hProcess);
                    return Status;
                }

                Buf->Modules = Mp;
            }

            if (DebugInfoMask & RTL_DEBUG_QUERY_HEAPS)
            {
                Status = RtlpQueryProcessHeapList(hProcess, Buf);
                if (!NT_SUCCESS(Status))
                {
                    NtClose(hProcess);
                    return Status;
                }

                if (DebugInfoMask & RTL_DEBUG_QUERY_HEAP_BLOCKS)
                {
                    Status = RtlpQueryProcessHeapBlocks(hProcess, Buf);
                    if (!NT_SUCCESS(Status))
                    {
                        NtClose(hProcess);
                        return Status;
                    }
                }
            }

            if (DebugInfoMask & RTL_DEBUG_QUERY_LOCKS)
            {
                PRTL_PROCESS_LOCKS Lp;
                ULONG LSize;

                Lp = (PRTL_PROCESS_LOCKS)((PUCHAR)Buf + Buf->OffsetFree);
                LSize = sizeof(RTL_PROCESS_LOCKS);
                Buf->Locks = Lp;
                Buf->OffsetFree = Buf->OffsetFree + LSize;
            }

            NtClose(hProcess);

            DPRINT("QueryProcessDebugInformation end\n");
            DPRINT("QueryDebugInfo : 0x%lx\n", Buf->OffsetFree);
        }

    return Status;
}

/* EOL */
