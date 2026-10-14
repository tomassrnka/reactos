/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT User-Mode DLL
 * FILE:            lib/ntdll/rtl/libsup.c
 * PURPOSE:         RTL Support Routines
 * PROGRAMMERS:     Alex Ionescu (alex@relsoft.net)
 *                  Gunnar Dalsnes
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>
#include <apisets.h>

#define NDEBUG
#include <debug.h>

SIZE_T RtlpAllocDeallocQueryBufferSize = PAGE_SIZE;
PTEB LdrpTopLevelDllBeingLoadedTeb = NULL;
PVOID MmHighestUserAddress = (PVOID)MI_HIGHEST_USER_ADDRESS;

/* FUNCTIONS ***************************************************************/

BOOLEAN
NTAPI
RtlpCheckForActiveDebugger(VOID)
{
    /* Return the flag in the PEB */
    return NtCurrentPeb()->BeingDebugged;
}

BOOLEAN
NTAPI
RtlpSetInDbgPrint(VOID)
{
    /* Check if it's already set and return TRUE if so */
#if (NTDDI_VERSION < NTDDI_LONGHORN)
    if (NtCurrentTeb()->InDbgPrint) return TRUE;

    /* Set it and return */
    NtCurrentTeb()->InDbgPrint = TRUE;
#else
    if (NtCurrentTeb()->DbgInDebugPrint) return TRUE;

    /* Set it and return */
    NtCurrentTeb()->DbgInDebugPrint = TRUE;
#endif
    return FALSE;
}

VOID
NTAPI
RtlpClearInDbgPrint(VOID)
{
#if (NTDDI_VERSION < NTDDI_LONGHORN)
    /* Clear the flag */
    NtCurrentTeb()->InDbgPrint = FALSE;
#else
    NtCurrentTeb()->DbgInDebugPrint = FALSE;
#endif
}

KPROCESSOR_MODE
NTAPI
RtlpGetMode(VOID)
{
   return UserMode;
}

/*
 * @implemented
 */
PPEB
NTAPI
RtlGetCurrentPeb(VOID)
{
    return NtCurrentPeb();
}

/*
 * @implemented
 */
VOID NTAPI
RtlAcquirePebLock(VOID)
{
   PPEB Peb = NtCurrentPeb ();
   RtlEnterCriticalSection(Peb->FastPebLock);
}

/*
 * @implemented
 */
VOID NTAPI
RtlReleasePebLock(VOID)
{
   PPEB Peb = NtCurrentPeb ();
   RtlLeaveCriticalSection(Peb->FastPebLock);
}

/*
* @implemented
*/
ULONG
NTAPI
RtlGetNtGlobalFlags(VOID)
{
    PPEB pPeb = NtCurrentPeb();
    return pPeb->NtGlobalFlag;
}

NTSTATUS
NTAPI
RtlDeleteHeapLock(IN OUT PHEAP_LOCK Lock)
{
    return RtlDeleteCriticalSection(&Lock->CriticalSection);
}

NTSTATUS
NTAPI
RtlEnterHeapLock(IN OUT PHEAP_LOCK Lock, IN BOOLEAN Exclusive)
{
    UNREFERENCED_PARAMETER(Exclusive);

    return RtlEnterCriticalSection(&Lock->CriticalSection);
}

BOOLEAN
NTAPI
RtlTryEnterHeapLock(IN OUT PHEAP_LOCK Lock, IN BOOLEAN Exclusive)
{
    UNREFERENCED_PARAMETER(Exclusive);

    return RtlTryEnterCriticalSection(&Lock->CriticalSection);
}

NTSTATUS
NTAPI
RtlInitializeHeapLock(IN OUT PHEAP_LOCK *Lock)
{
    return RtlInitializeCriticalSection(&(*Lock)->CriticalSection);
}

NTSTATUS
NTAPI
RtlLeaveHeapLock(IN OUT PHEAP_LOCK Lock)
{
    return RtlLeaveCriticalSection(&Lock->CriticalSection);
}

PVOID
NTAPI
RtlpAllocateMemory(UINT Bytes,
                   ULONG Tag)
{
    UNREFERENCED_PARAMETER(Tag);

    return RtlAllocateHeap(RtlGetProcessHeap(),
                           0,
                           Bytes);
}


VOID
NTAPI
RtlpFreeMemory(PVOID Mem,
               ULONG Tag)
{
    UNREFERENCED_PARAMETER(Tag);

    RtlFreeHeap(RtlGetProcessHeap(),
                0,
                Mem);
}


#if DBG
VOID FASTCALL
CHECK_PAGED_CODE_RTL(char *file, int line)
{
  /* meaningless in user mode */
}
#endif

VOID
NTAPI
RtlpSetHeapParameters(IN PRTL_HEAP_PARAMETERS Parameters)
{
    PPEB Peb;

    /* Get PEB */
    Peb = RtlGetCurrentPeb();

    /* Apply defaults for non-set parameters */
    if (!Parameters->SegmentCommit) Parameters->SegmentCommit = Peb->HeapSegmentCommit;
    if (!Parameters->SegmentReserve) Parameters->SegmentReserve = Peb->HeapSegmentReserve;
    if (!Parameters->DeCommitFreeBlockThreshold) Parameters->DeCommitFreeBlockThreshold = Peb->HeapDeCommitFreeBlockThreshold;
    if (!Parameters->DeCommitTotalFreeThreshold) Parameters->DeCommitTotalFreeThreshold = Peb->HeapDeCommitTotalFreeThreshold;
}

BOOLEAN
NTAPI
RtlpHandleDpcStackException(IN PEXCEPTION_REGISTRATION_RECORD RegistrationFrame,
                            IN ULONG_PTR RegistrationFrameEnd,
                            IN OUT PULONG_PTR StackLow,
                            IN OUT PULONG_PTR StackHigh)
{
    /* There's no such thing as a DPC stack in user-mode */
    return FALSE;
}

VOID
NTAPI
RtlpCheckLogException(IN PEXCEPTION_RECORD ExceptionRecord,
                      IN PCONTEXT ContextRecord,
                      IN PVOID ContextData,
                      IN ULONG Size)
{
    /* Exception logging is not done in user-mode */
}

BOOLEAN
NTAPI
RtlpCaptureStackLimits(IN ULONG_PTR Ebp,
                       IN ULONG_PTR *StackBegin,
                       IN ULONG_PTR *StackEnd)
{
    /* FIXME: Verify */
    *StackBegin = (ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit;
    *StackEnd = (ULONG_PTR)NtCurrentTeb()->NtTib.StackBase;
    return TRUE;
}

#ifndef _M_AMD64
/*
 * @implemented
 */
ULONG
NTAPI
RtlWalkFrameChain(OUT PVOID *Callers,
                  IN ULONG Count,
                  IN ULONG Flags)
{
    ULONG_PTR Stack, NewStack, StackBegin, StackEnd = 0;
    ULONG Eip;
    BOOLEAN Result, StopSearch = FALSE;
    ULONG i = 0;

    /* Get current EBP */
#if defined(_M_IX86)
#if defined __GNUC__
    __asm__("mov %%ebp, %0" : "=r" (Stack) : );
#elif defined(_MSC_VER)
    __asm mov Stack, ebp
#endif
#elif defined(_M_MIPS)
        __asm__("move $sp, %0" : "=r" (Stack) : );
#elif defined(_M_PPC)
    __asm__("mr %0,1" : "=r" (Stack) : );
#elif defined(_M_ARM)
#if defined __GNUC__
    __asm__("mov sp, %0" : "=r"(Stack) : );
#elif defined(_MSC_VER)
    // FIXME: Hack. Probably won't work if this ever actually manages to run someday.
    Stack = (ULONG_PTR)&Stack;
#endif
#else
#error Unknown architecture
#endif

    /* Set it as the stack begin limit as well */
    StackBegin = (ULONG_PTR)Stack;

    /* Check if we're called for non-logging mode */
    if (!Flags)
    {
        /* Get the actual safe limits */
        Result = RtlpCaptureStackLimits((ULONG_PTR)Stack,
                                        &StackBegin,
                                        &StackEnd);
        if (!Result) return 0;
    }

    /* Use a SEH block for maximum protection */
    _SEH2_TRY
    {
        /* Loop the frames */
        for (i = 0; i < Count; i++)
        {
            /*
             * Leave if we're past the stack,
             * if we're before the stack,
             * or if we've reached ourselves.
             */
            if ((Stack >= StackEnd) ||
                (!i ? (Stack < StackBegin) : (Stack <= StackBegin)) ||
                ((StackEnd - Stack) < (2 * sizeof(ULONG_PTR))))
            {
                /* We're done or hit a bad address */
                break;
            }

            /* Get new stack and EIP */
            NewStack = *(PULONG_PTR)Stack;
            Eip = *(PULONG_PTR)(Stack + sizeof(ULONG_PTR));

            /* Check if the new pointer is above the oldone and past the end */
            if (!((Stack < NewStack) && (NewStack < StackEnd)))
            {
                /* Stop searching after this entry */
                StopSearch = TRUE;
            }

            /* Also make sure that the EIP isn't a stack address */
            if ((StackBegin < Eip) && (Eip < StackEnd)) break;

            /* FIXME: Check that EIP is inside a loaded module */

            /* Save this frame */
            Callers[i] = (PVOID)Eip;

            /* Check if we should continue */
            if (StopSearch)
            {
                /* Return the next index */
                i++;
                break;
            }

            /* Move to the next stack */
            Stack = NewStack;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* No index */
        i = 0;
    }
    _SEH2_END;

    /* Return frames parsed */
    return i;
}
#endif

VOID
NTAPI
RtlpGetStackLimits(
    OUT PULONG_PTR LowLimit,
    OUT PULONG_PTR HighLimit)
{
    *LowLimit = (ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit;
    *HighLimit = (ULONG_PTR)NtCurrentTeb()->NtTib.StackBase;
}

BOOLEAN
NTAPI
RtlIsThreadWithinLoaderCallout(VOID)
{
    return LdrpTopLevelDllBeingLoadedTeb == NtCurrentTeb();
}

/* RTL Atom Tables ************************************************************/

typedef struct _RTL_ATOM_HANDLE
{
   RTL_HANDLE_TABLE_ENTRY Handle;
   PRTL_ATOM_TABLE_ENTRY AtomEntry;
} RTL_ATOM_HANDLE, *PRTL_ATOM_HANDLE;

NTSTATUS
RtlpInitAtomTableLock(PRTL_ATOM_TABLE AtomTable)
{
   RtlInitializeCriticalSection(&AtomTable->CriticalSection);
   return STATUS_SUCCESS;
}


VOID
RtlpDestroyAtomTableLock(PRTL_ATOM_TABLE AtomTable)
{
   RtlDeleteCriticalSection(&AtomTable->CriticalSection);
}


BOOLEAN
RtlpLockAtomTable(PRTL_ATOM_TABLE AtomTable)
{
   RtlEnterCriticalSection(&AtomTable->CriticalSection);
   return TRUE;
}


VOID
RtlpUnlockAtomTable(PRTL_ATOM_TABLE AtomTable)
{
   RtlLeaveCriticalSection(&AtomTable->CriticalSection);
}


/* handle functions */

BOOLEAN
RtlpCreateAtomHandleTable(PRTL_ATOM_TABLE AtomTable)
{
   RtlInitializeHandleTable(0xCFFF,
                            sizeof(RTL_ATOM_HANDLE),
                            &AtomTable->RtlHandleTable);

   return TRUE;
}

VOID
RtlpDestroyAtomHandleTable(PRTL_ATOM_TABLE AtomTable)
{
   RtlDestroyHandleTable(&AtomTable->RtlHandleTable);
}

PRTL_ATOM_TABLE
RtlpAllocAtomTable(ULONG Size)
{
   return (PRTL_ATOM_TABLE)RtlAllocateHeap(RtlGetProcessHeap(),
                                           HEAP_ZERO_MEMORY,
                                           Size);
}

VOID
RtlpFreeAtomTable(PRTL_ATOM_TABLE AtomTable)
{
   RtlFreeHeap(RtlGetProcessHeap(),
               0,
               AtomTable);
}

PRTL_ATOM_TABLE_ENTRY
RtlpAllocAtomTableEntry(ULONG Size)
{
   return (PRTL_ATOM_TABLE_ENTRY)RtlAllocateHeap(RtlGetProcessHeap(),
                                                 HEAP_ZERO_MEMORY,
                                                 Size);
}

VOID
RtlpFreeAtomTableEntry(PRTL_ATOM_TABLE_ENTRY Entry)
{
   RtlFreeHeap(RtlGetProcessHeap(),
               0,
               Entry);
}

VOID
RtlpFreeAtomHandle(PRTL_ATOM_TABLE AtomTable, PRTL_ATOM_TABLE_ENTRY Entry)
{
   PRTL_HANDLE_TABLE_ENTRY RtlHandleEntry;

   if (RtlIsValidIndexHandle(&AtomTable->RtlHandleTable,
                             (ULONG)Entry->HandleIndex,
                             &RtlHandleEntry))
   {
      RtlFreeHandle(&AtomTable->RtlHandleTable,
                    RtlHandleEntry);
   }
}

BOOLEAN
RtlpCreateAtomHandle(PRTL_ATOM_TABLE AtomTable, PRTL_ATOM_TABLE_ENTRY Entry)
{
   ULONG HandleIndex;
   PRTL_HANDLE_TABLE_ENTRY RtlHandle;

   RtlHandle = RtlAllocateHandle(&AtomTable->RtlHandleTable,
                                 &HandleIndex);
   if (RtlHandle != NULL)
   {
      PRTL_ATOM_HANDLE AtomHandle = (PRTL_ATOM_HANDLE)RtlHandle;

      /* FIXME - Handle Indexes >= 0xC000 ?! */
      if (HandleIndex < 0xC000)
      {
         Entry->HandleIndex = (USHORT)HandleIndex;
         Entry->Atom = 0xC000 + (USHORT)HandleIndex;

         AtomHandle->AtomEntry = Entry;
         AtomHandle->Handle.Flags = RTL_HANDLE_VALID;

         return TRUE;
      }
      else
      {
         /* set the valid flag, otherwise RtlFreeHandle will fail! */
         AtomHandle->Handle.Flags = RTL_HANDLE_VALID;

         RtlFreeHandle(&AtomTable->RtlHandleTable,
                       RtlHandle);
      }
   }

   return FALSE;
}

PRTL_ATOM_TABLE_ENTRY
RtlpGetAtomEntry(PRTL_ATOM_TABLE AtomTable, ULONG Index)
{
   PRTL_HANDLE_TABLE_ENTRY RtlHandle;

   if (RtlIsValidIndexHandle(&AtomTable->RtlHandleTable,
                             Index,
                             &RtlHandle))
   {
      PRTL_ATOM_HANDLE AtomHandle = (PRTL_ATOM_HANDLE)RtlHandle;

      return AtomHandle->AtomEntry;
   }

   return NULL;
}

/* Ldr SEH-Protected access to IMAGE_NT_HEADERS */

/* Rtl SEH-Free version of this */
NTSTATUS
NTAPI
RtlpImageNtHeaderEx(
    _In_ ULONG Flags,
    _In_ PVOID Base,
    _In_ ULONG64 Size,
    _Out_ PIMAGE_NT_HEADERS *OutHeaders);


/*
 * @implemented
 * @note: This is here, so that we do not drag SEH into rosload, freeldr and bootmgfw
 */
NTSTATUS
NTAPI
RtlImageNtHeaderEx(
    _In_ ULONG Flags,
    _In_ PVOID Base,
    _In_ ULONG64 Size,
    _Out_ PIMAGE_NT_HEADERS *OutHeaders)
{
    NTSTATUS Status;

    /* Assume failure. This is also done in RtlpImageNtHeaderEx, but this is guarded by SEH. */
    if (OutHeaders != NULL)
        *OutHeaders = NULL;

    _SEH2_TRY
    {
        Status = RtlpImageNtHeaderEx(Flags, Base, Size, OutHeaders);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* Fail with the SEH error */
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

/*
 * Ldr Resource support code
 */

IMAGE_RESOURCE_DIRECTORY *find_entry_by_name( IMAGE_RESOURCE_DIRECTORY *dir,
                                              LPCWSTR name, void *root,
                                              int want_dir );
IMAGE_RESOURCE_DIRECTORY *find_entry_by_id( IMAGE_RESOURCE_DIRECTORY *dir,
                                            WORD id, void *root, int want_dir );
IMAGE_RESOURCE_DIRECTORY *find_first_entry( IMAGE_RESOURCE_DIRECTORY *dir,
                                            void *root, int want_dir );
int push_language( USHORT *list, ULONG pos, WORD lang );

/**********************************************************************
 *  find_entry
 *
 * Find a resource entry
 */
NTSTATUS find_entry( PVOID BaseAddress, LDR_RESOURCE_INFO *info,
                     ULONG level, void **ret, int want_dir )
{
    ULONG size;
    void *root;
    IMAGE_RESOURCE_DIRECTORY *resdirptr;
    USHORT list[9];  /* list of languages to try */
    int i, pos = 0;
    LCID user_lcid, system_lcid;

    root = RtlImageDirectoryEntryToData( BaseAddress, TRUE, IMAGE_DIRECTORY_ENTRY_RESOURCE, &size );
    if (!root) return STATUS_RESOURCE_DATA_NOT_FOUND;
    if (size < sizeof(*resdirptr)) return STATUS_RESOURCE_DATA_NOT_FOUND;
    resdirptr = root;

    if (!level--) goto done;
    if (!(*ret = find_entry_by_name( resdirptr, (LPCWSTR)info->Type, root, want_dir || level )))
        return STATUS_RESOURCE_TYPE_NOT_FOUND;
    if (!level--) return STATUS_SUCCESS;

    resdirptr = *ret;
    if (!(*ret = find_entry_by_name( resdirptr, (LPCWSTR)info->Name, root, want_dir || level )))
        return STATUS_RESOURCE_NAME_NOT_FOUND;
    if (!level--) return STATUS_SUCCESS;
    if (level) return STATUS_INVALID_PARAMETER;  /* level > 3 */

    /* 1. specified language */
    pos = push_language( list, pos, info->Language );

    /* 2. specified language with neutral sublanguage */
    pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(info->Language), SUBLANG_NEUTRAL ) );

    /* 3. neutral language with neutral sublanguage */
    pos = push_language( list, pos, MAKELANGID( LANG_NEUTRAL, SUBLANG_NEUTRAL ) );

    /* if no explicitly specified language, try some defaults */
    if (PRIMARYLANGID(info->Language) == LANG_NEUTRAL)
    {
        /* user defaults, unless SYS_DEFAULT sublanguage specified  */
        if (SUBLANGID(info->Language) != SUBLANG_SYS_DEFAULT)
        {
            /* 4. current thread locale language */
            pos = push_language( list, pos, LANGIDFROMLCID(NtCurrentTeb()->CurrentLocale) );

            if (NT_SUCCESS(NtQueryDefaultLocale(TRUE, &user_lcid)))
            {
                /* 5. user locale language */
                pos = push_language( list, pos, LANGIDFROMLCID(user_lcid) );

                /* 6. user locale language with neutral sublanguage  */
                pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(user_lcid), SUBLANG_NEUTRAL ) );
            }
        }

        /* now system defaults */

        if (NT_SUCCESS(NtQueryDefaultLocale(FALSE, &system_lcid)))
        {
            /* 7. system locale language */
            pos = push_language( list, pos, LANGIDFROMLCID( system_lcid ) );

            /* 8. system locale language with neutral sublanguage */
            pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(system_lcid), SUBLANG_NEUTRAL ) );
        }

        /* 9. English */
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }

    resdirptr = *ret;
    for (i = 0; i < pos; i++)
        if ((*ret = find_entry_by_id( resdirptr, list[i], root, want_dir ))) return STATUS_SUCCESS;

    /* if no explicitly specified language, return the first entry */
    if (PRIMARYLANGID(info->Language) == LANG_NEUTRAL)
    {
        if ((*ret = find_first_entry( resdirptr, root, want_dir ))) return STATUS_SUCCESS;
    }
    return STATUS_RESOURCE_LANG_NOT_FOUND;

done:
    *ret = resdirptr;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
PVOID NTAPI
RtlPcToFileHeader(IN PVOID PcValue,
                  PVOID* BaseOfImage)
{
    PLIST_ENTRY ModuleListHead;
    PLIST_ENTRY Entry;
    PLDR_DATA_TABLE_ENTRY Module;
    PVOID ImageBase = NULL;

    RtlEnterCriticalSection (NtCurrentPeb()->LoaderLock);
    ModuleListHead = &NtCurrentPeb()->Ldr->InLoadOrderModuleList;
    Entry = ModuleListHead->Flink;
    while (Entry != ModuleListHead)
    {
        Module = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

        if ((ULONG_PTR)PcValue >= (ULONG_PTR)Module->DllBase &&
                (ULONG_PTR)PcValue < (ULONG_PTR)Module->DllBase + Module->SizeOfImage)
        {
            ImageBase = Module->DllBase;
            break;
        }
        Entry = Entry->Flink;
    }
    RtlLeaveCriticalSection (NtCurrentPeb()->LoaderLock);

    *BaseOfImage = ImageBase;
    return ImageBase;
}

NTSTATUS get_buffer(LPWSTR *buffer, SIZE_T needed, PUNICODE_STRING CallerBuffer, BOOLEAN bAllocateBuffer)
{
    WCHAR *p;

    if (CallerBuffer && CallerBuffer->MaximumLength > needed)
    {
        p = CallerBuffer->Buffer;
        CallerBuffer->Length = needed - sizeof(WCHAR);
    }
    else
    {
        if (!bAllocateBuffer)
            return STATUS_BUFFER_TOO_SMALL;

        if (CallerBuffer)
            CallerBuffer->Buffer[0] = 0;

        p = RtlAllocateHeap(RtlGetProcessHeap(), 0, needed );
        if (!p)
            return STATUS_NO_MEMORY;
    }
    *buffer = p;

    return STATUS_SUCCESS;
}

/* NOTE: Remove this one once our actctx support becomes better */
NTSTATUS find_actctx_dll( PUNICODE_STRING pnameW, LPWSTR *fullname, PUNICODE_STRING CallerBuffer, BOOLEAN bAllocateBuffer)
{
    static const WCHAR winsxsW[] = {'\\','w','i','n','s','x','s','\\'};
    static const WCHAR dotManifestW[] = {'.','m','a','n','i','f','e','s','t',0};

    ACTIVATION_CONTEXT_ASSEMBLY_DETAILED_INFORMATION *info;
    ACTCTX_SECTION_KEYED_DATA data;
    NTSTATUS status;
    SIZE_T needed, size = 1024;
    WCHAR *p;

    data.cbSize = sizeof(data);
    status = RtlFindActivationContextSectionString( FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX, NULL,
                                                    ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION,
                                                    pnameW, &data );
    if (status != STATUS_SUCCESS)
    {
        //DPRINT1("RtlFindActivationContextSectionString returned 0x%x for %wZ\n", status, pnameW);
        return status;
    }

    for (;;)
    {
        if (!(info = RtlAllocateHeap( RtlGetProcessHeap(), 0, size )))
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        status = RtlQueryInformationActivationContext( 0, data.hActCtx, &data.ulAssemblyRosterIndex,
                                                       AssemblyDetailedInformationInActivationContext,
                                                       info, size, &needed );
        if (status == STATUS_SUCCESS) break;
        if (status != STATUS_BUFFER_TOO_SMALL) goto done;
        RtlFreeHeap( RtlGetProcessHeap(), 0, info );
        size = needed;
    }

    DPRINT("manifestpath === %S\n", info->lpAssemblyManifestPath);
    DPRINT("DirectoryName === %S\n", info->lpAssemblyDirectoryName);
    if (!info->lpAssemblyManifestPath /*|| !info->lpAssemblyDirectoryName*/)
    {
        status = STATUS_SXS_KEY_NOT_FOUND;
        goto done;
    }

    if ((p = wcsrchr( info->lpAssemblyManifestPath, '\\' )))
    {
        DWORD dirlen = info->ulAssemblyDirectoryNameLength / sizeof(WCHAR);

        p++;
        if (!info->lpAssemblyDirectoryName || _wcsnicmp( p, info->lpAssemblyDirectoryName, dirlen ) || _wcsicmp( p + dirlen, dotManifestW ))
        {
            /* manifest name does not match directory name, so it's not a global
             * windows/winsxs manifest; use the manifest directory name instead */
            dirlen = p - info->lpAssemblyManifestPath;
            needed = (dirlen + 1) * sizeof(WCHAR) + pnameW->Length;

            status = get_buffer(fullname, needed, CallerBuffer, bAllocateBuffer);
            if (!NT_SUCCESS(status))
                goto done;

            p = *fullname;

            memcpy( p, info->lpAssemblyManifestPath, dirlen * sizeof(WCHAR) );
            p += dirlen;
            memcpy( p, pnameW->Buffer, pnameW->Length);
            p += (pnameW->Length / sizeof(WCHAR));
            *p = L'\0';

            goto done;
        }
    }

    needed = (wcslen(SharedUserData->NtSystemRoot) * sizeof(WCHAR) +
              sizeof(winsxsW) + info->ulAssemblyDirectoryNameLength + pnameW->Length + 2*sizeof(WCHAR));

    status = get_buffer(fullname, needed, CallerBuffer, bAllocateBuffer);
    if (!NT_SUCCESS(status))
        goto done;

    p = *fullname;

    wcscpy( p, SharedUserData->NtSystemRoot );
    p += wcslen(p);
    memcpy( p, winsxsW, sizeof(winsxsW) );
    p += sizeof(winsxsW) / sizeof(WCHAR);
    memcpy( p, info->lpAssemblyDirectoryName, info->ulAssemblyDirectoryNameLength );
    p += info->ulAssemblyDirectoryNameLength / sizeof(WCHAR);
    *p++ = L'\\';
    memcpy( p, pnameW->Buffer, pnameW->Length);
    p += (pnameW->Length / sizeof(WCHAR));
    *p = L'\0';

done:
    RtlFreeHeap( RtlGetProcessHeap(), 0, info );
    RtlReleaseActivationContext( data.hActCtx );
    DPRINT("%S\n", fullname);
    return status;
}

/*
 * @unimplemented
 */
NTSYSAPI
NTSTATUS
NTAPI
RtlDosApplyFileIsolationRedirection_Ustr(IN ULONG Flags,
                                         IN PUNICODE_STRING OriginalName,
                                         IN PUNICODE_STRING Extension,
                                         IN OUT PUNICODE_STRING StaticString,
                                         IN OUT PUNICODE_STRING DynamicString,
                                         IN OUT PUNICODE_STRING *NewName,
                                         IN PULONG NewFlags,
                                         IN PSIZE_T FileNameSize,
                                         IN PSIZE_T RequiredLength)
{
    NTSTATUS Status;
    LPWSTR fullname;
    WCHAR buffer [MAX_PATH];
    UNICODE_STRING localStr, localStr2, *pstrParam;
    WCHAR *p;
    BOOLEAN GotExtension;
    WCHAR c;
    C_ASSERT(sizeof(UNICODE_NULL) == sizeof(WCHAR));


    /* Check for invalid parameters */
    if (!OriginalName)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!DynamicString && !StaticString)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((DynamicString) && (StaticString) && !(NewName))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!OriginalName->Buffer || OriginalName->Length == 0)
    {
        return STATUS_SXS_KEY_NOT_FOUND;
    }

    if (StaticString && (OriginalName == StaticString || OriginalName->Buffer == StaticString->Buffer))
    {
        return STATUS_SXS_KEY_NOT_FOUND;
    }

    if ((Flags & RTL_DOS_APPLY_FILE_REDIRECTION_USTR_FLAG_RESPECT_DOT_LOCAL) &&
        NtCurrentPeb()->ProcessParameters &&
        (NtCurrentPeb()->ProcessParameters->Flags & RTL_USER_PROCESS_PARAMETERS_PRIVATE_DLL_PATH))
    {
        UNICODE_STRING RealName, LocalName;
        WCHAR RealNameBuf[MAX_PATH], LocalNameBuf[MAX_PATH];

        RtlInitEmptyUnicodeString(&RealName, RealNameBuf, sizeof(RealNameBuf));
        RtlInitEmptyUnicodeString(&LocalName, LocalNameBuf, sizeof(LocalNameBuf));

        Status = RtlComputePrivatizedDllName_U(OriginalName, &RealName, &LocalName);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("RtlComputePrivatizedDllName_U failed for %wZ: 0x%lx\n", OriginalName, Status);
            return Status;
        }

        if (RtlDoesFileExists_UStr(&LocalName))
        {
            Status = get_buffer(&fullname, LocalName.Length + sizeof(UNICODE_NULL), StaticString, DynamicString != NULL);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(fullname, LocalName.Buffer, LocalName.Length + sizeof(UNICODE_NULL));
            }
            else
            {
                DPRINT1("Error while retrieving buffer for %wZ: 0x%lx\n", OriginalName, Status);
            }
        }
        else if (RtlDoesFileExists_UStr(&RealName))
        {
            Status = get_buffer(&fullname, RealName.Length + sizeof(UNICODE_NULL), StaticString, DynamicString != NULL);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(fullname, RealName.Buffer, RealName.Length + sizeof(UNICODE_NULL));
            }
            else
            {
                DPRINT1("Error while retrieving buffer for %wZ: 0x%lx\n", OriginalName, Status);
            }
        }
        else
        {
            Status = STATUS_NOT_FOUND;
        }

        if (RealName.Buffer != RealNameBuf)
            RtlFreeUnicodeString(&RealName);
        if (LocalName.Buffer != LocalNameBuf)
            RtlFreeUnicodeString(&LocalName);

        if (NT_SUCCESS(Status))
        {
            DPRINT("Redirecting %wZ to %S\n", OriginalName, fullname);
            if (!StaticString || StaticString->Buffer != fullname)
            {
                RtlInitUnicodeString(DynamicString, fullname);
                *NewName = DynamicString;
            }
            else
            {
                *NewName = StaticString;
            }
            return Status;
        }
    }

    pstrParam = OriginalName;

    /* Get the file name with an extension */
    p = OriginalName->Buffer + OriginalName->Length / sizeof(WCHAR) - 1;
    GotExtension = FALSE;
    while (p >= OriginalName->Buffer)
    {
        c = *p--;
        if (c == L'.')
        {
            GotExtension = TRUE;
        }
        else if (c == L'\\')
        {
            localStr.Buffer = p + 2;
            localStr.Length = OriginalName->Length - ((ULONG_PTR)localStr.Buffer - (ULONG_PTR)OriginalName->Buffer);
            localStr.MaximumLength = OriginalName->MaximumLength - ((ULONG_PTR)localStr.Buffer - (ULONG_PTR)OriginalName->Buffer);
            pstrParam = &localStr;
            break;
        }
    }

    if (!GotExtension)
    {
        if (!Extension)
        {
            return STATUS_SXS_KEY_NOT_FOUND;
        }

        if (pstrParam->Length + Extension->Length > sizeof(buffer))
        {
            //FIXME!
            return STATUS_NO_MEMORY;
        }

        RtlInitEmptyUnicodeString(&localStr2, buffer, sizeof(buffer));
        RtlAppendUnicodeStringToString(&localStr2, pstrParam);
        RtlAppendUnicodeStringToString(&localStr2, Extension);
        pstrParam = &localStr2;
    }

    /* Use wine's function as long as we use wine's sxs implementation in ntdll */
    Status = find_actctx_dll(pstrParam, &fullname, StaticString, DynamicString != NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    DPRINT("Redirecting %wZ to %S\n", OriginalName, fullname);

    if (!StaticString || StaticString->Buffer != fullname)
    {
        RtlInitUnicodeString(DynamicString, fullname);
        *NewName = DynamicString;
    }
    else
    {
        *NewName = StaticString;
    }

    return Status;
}

#ifndef TAG_USTR
#define TAG_USTR 'RTSU'
#endif
#ifndef RtlpAllocateStringMemory
#define RtlpAllocateStringMemory(Bytes, Tag) RtlpAllocateMemory(Bytes, Tag)
#endif

/* Windows resolves API set names from the schema of the running system: the supportedOS entries of the application
   manifest do not change which names load. ReactOS has no schema of its own, so every version in the table counts. */
static const DWORD LdrpApisetVersion = APISET_WIN7 | APISET_WIN8 | APISET_WIN81 | APISET_WIN10;

/* A contract that Windows 7 does not have is an ordinary DLL name there, so an application that ships its own copy
   (the app-local deployment of the Universal CRT, for example) runs with that copy. Keep that: the table is only the
   fallback for such a name when the directory of the executable has no file of that name. The directory is read once,
   so that every lookup of a name (load, handle lookup, reference counts) gives the same answer for the whole process.
   If it cannot be read, each name is decided once by opening the file directly and that decision is kept. */
#define LDRP_APP_CONTRACTS_UNREAD ((PWSTR)(ULONG_PTR)1)
static WCHAR LdrpNoAppContracts[1];
static PWSTR LdrpAppContracts;
static UNICODE_STRING LdrpAppDirectory;
static PWSTR LdrpAppContractDecisions;
static SIZE_T LdrpAppContractDecisionsUsed, LdrpAppContractDecisionsSize;

/* Makes room for a name of Length bytes and the two terminators, so that the following append cannot fail */
static BOOLEAN
LdrpReserveName(
    _Inout_ PWSTR *List,
    _In_ SIZE_T Used,
    _Inout_ PSIZE_T Size,
    _In_ SIZE_T Length)
{
    SIZE_T Needed = Length + 2 * sizeof(WCHAR);

    if (Used + Needed > *Size)
    {
        SIZE_T NewSize = max(*Size * 2, Used + Needed + 1024);
        PWSTR NewList = *List ? RtlReAllocateHeap(RtlGetProcessHeap(), 0, *List, NewSize)
                              : RtlAllocateHeap(RtlGetProcessHeap(), 0, NewSize);
        if (!NewList)
            return FALSE;
        *List = NewList;
        *Size = NewSize;
    }
    return TRUE;
}

static BOOLEAN
LdrpAppendName(
    _Inout_ PWSTR *List,
    _Inout_ PSIZE_T Used,
    _Inout_ PSIZE_T Size,
    _In_reads_bytes_(Length) PCWSTR Name,
    _In_ SIZE_T Length)
{
    if (!LdrpReserveName(List, *Used, Size, Length))
        return FALSE;
    RtlCopyMemory((PUCHAR)*List + *Used, Name, Length);
    *Used += Length;
    (*List)[*Used / sizeof(WCHAR)] = UNICODE_NULL;
    *Used += sizeof(WCHAR);
    (*List)[*Used / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

/* The directory of the executable as an NT path with its trailing backslash: \??\C:\app\, \??\Volume{...}\, ... */
static BOOLEAN
LdrpGetAppDirectory(
    _Out_ PUNICODE_STRING Directory)
{
    PUNICODE_STRING ImagePath = &NtCurrentPeb()->ProcessParameters->ImagePathName;
    PWSTR DosPath;
    USHORT Length;

    RtlInitEmptyUnicodeString(Directory, NULL, 0);
    if (ImagePath->Length < 2 * sizeof(WCHAR))
        return FALSE;

    /* \??\..., \Device\...: already an NT path */
    if (ImagePath->Buffer[0] == L'\\' && ImagePath->Buffer[1] != L'\\')
    {
        Directory->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, ImagePath->Length + sizeof(WCHAR));
        if (!Directory->Buffer)
            return FALSE;
        RtlCopyMemory(Directory->Buffer, ImagePath->Buffer, ImagePath->Length);
        Directory->Length = ImagePath->Length;
        Directory->MaximumLength = ImagePath->Length + sizeof(WCHAR);
    }
    else
    {
        DosPath = RtlAllocateHeap(RtlGetProcessHeap(), 0, ImagePath->Length + sizeof(WCHAR));
        if (!DosPath)
            return FALSE;
        RtlCopyMemory(DosPath, ImagePath->Buffer, ImagePath->Length);
        DosPath[ImagePath->Length / sizeof(WCHAR)] = UNICODE_NULL;
        if (!RtlDosPathNameToNtPathName_U(DosPath, Directory, NULL, NULL))
            RtlInitEmptyUnicodeString(Directory, NULL, 0);
        RtlFreeHeap(RtlGetProcessHeap(), 0, DosPath);
        if (!Directory->Buffer)
            return FALSE;
    }

    Length = Directory->Length;
    while (Length && Directory->Buffer[Length / sizeof(WCHAR) - 1] != L'\\')
        Length -= sizeof(WCHAR);
    Directory->Length = Length;
    if (Length <= sizeof(WCHAR))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Directory->Buffer);
        RtlInitEmptyUnicodeString(Directory, NULL, 0);
        return FALSE;
    }
    return TRUE;
}

static NTSTATUS
LdrpOpenAppDirectory(
    _In_ PUNICODE_STRING Directory,
    _Out_ PHANDLE Handle)
{
    UNICODE_STRING Name = *Directory;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    /* Without the trailing backslash first; a volume root (\??\Volume{...}\, \??\C:\) needs it */
    Name.Length -= sizeof(WCHAR);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(Handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &ObjectAttributes, &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(Status) || Status == STATUS_ACCESS_DENIED)
        return Status;
    ObjectAttributes.ObjectName = Directory;
    return NtOpenFile(Handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &ObjectAttributes, &IoStatusBlock,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

static NTSTATUS
LdrpAddAppContracts(
    _In_ HANDLE Handle,
    _In_ PUNICODE_STRING Pattern,
    _Inout_ PWSTR *List,
    _Inout_ PSIZE_T Used,
    _Inout_ PSIZE_T Size)
{
    IO_STATUS_BLOCK IoStatusBlock;
    const ULONG BufferSize = 4096;
    PFILE_DIRECTORY_INFORMATION Info;
    BOOLEAN First = TRUE;
    NTSTATUS Status;
    PVOID Buffer;

    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
    if (!Buffer)
        return STATUS_NO_MEMORY;

    for (;;)
    {
        Status = NtQueryDirectoryFile(Handle, NULL, NULL, NULL, &IoStatusBlock, Buffer, BufferSize,
                                      FileDirectoryInformation, FALSE, First ? Pattern : NULL, First);
        First = FALSE;
        if (!NT_SUCCESS(Status))
            break;

        for (Info = Buffer;; Info = (PFILE_DIRECTORY_INFORMATION)((ULONG_PTR)Info + Info->NextEntryOffset))
        {
            if (!(Info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Info->FileNameLength &&
                !LdrpAppendName(List, Used, Size, Info->FileName, Info->FileNameLength))
            {
                Status = STATUS_NO_MEMORY;
                goto Quit;
            }
            if (!Info->NextEntryOffset)
                break;
        }
    }
    if (Status == STATUS_NO_MORE_FILES || Status == STATUS_NO_SUCH_FILE)
        Status = STATUS_SUCCESS;

Quit:
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    return Status;
}

/* Returns the list of contract DLL names in the directory, or LDRP_APP_CONTRACTS_UNREAD if it could not be read
   completely; never a partial list */
static PWSTR
LdrpReadAppContracts(VOID)
{
    UNICODE_STRING Patterns[] = { RTL_CONSTANT_STRING(L"api-*.dll"), RTL_CONSTANT_STRING(L"ext-*.dll") };
    UNICODE_STRING Directory;
    PWSTR List = NULL, Result, Old;
    SIZE_T Used = 0, Size = 0;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    HANDLE Handle;
    ULONG n;

    if (LdrpGetAppDirectory(&Directory))
    {
        for (n = 0; n < RTL_NUMBER_OF(Patterns); ++n)
        {
            /* A handle per pattern: the pattern of a handle is fixed by its first query */
            Status = LdrpOpenAppDirectory(&Directory, &Handle);
            if (!NT_SUCCESS(Status))
                break;
            Status = LdrpAddAppContracts(Handle, &Patterns[n], &List, &Used, &Size);
            NtClose(Handle);
            if (!NT_SUCCESS(Status))
                break;
        }
    }

    if (NT_SUCCESS(Status))
    {
        Result = List ? List : LdrpNoAppContracts;
    }
    else
    {
        DPRINT1("Contract DLLs next to %wZ not read (0x%lx): each name is opened directly\n",
                &NtCurrentPeb()->ProcessParameters->ImagePathName, Status);
        if (List)
            RtlFreeHeap(RtlGetProcessHeap(), 0, List);
        List = NULL;
        Result = LDRP_APP_CONTRACTS_UNREAD;
    }

    RtlAcquirePebLock();
    Old = LdrpAppContracts;
    if (!Old)
    {
        LdrpAppContracts = Result;
        if (Directory.Buffer)
            LdrpAppDirectory = Directory;
        Directory.Buffer = NULL;
        List = NULL;
    }
    RtlReleasePebLock();

    if (Old)
        Result = Old;
    if (List)
        RtlFreeHeap(RtlGetProcessHeap(), 0, List);
    if (Directory.Buffer)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Directory.Buffer);
    return Result;
}

static BOOLEAN
LdrpFindName(
    _In_opt_ PCWSTR List,
    _In_ PUNICODE_STRING Name,
    _In_ BOOLEAN Tagged,
    _Out_opt_ PWCHAR Tag)
{
    UNICODE_STRING Entry;

    for (; List && *List; List += wcslen(List) + 1)
    {
        RtlInitUnicodeString(&Entry, Tagged ? List + 1 : List);
        if (RtlEqualUnicodeString(&Entry, Name, TRUE))
        {
            if (Tag)
                *Tag = List[0];
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * The directory could not be read: decide the name by opening the file next to the executable. Only a conclusive
 * answer is kept for the life of the process, and it is stored before it is used:
 * - the file opens: the copy is used (Windows 7);
 * - it is absent (STATUS_OBJECT_NAME_NOT_FOUND, STATUS_OBJECT_PATH_NOT_FOUND) or a directory
 *   (STATUS_FILE_IS_A_DIRECTORY, not a copy either): the name goes to its host.
 * Anything else (no memory, access denied, a sharing violation, any other I/O error) is inconclusive and is not kept:
 * the name is then not redirected, so the ordinary DLL search decides as on Windows 7, and the next lookup asks again.
 */
static BOOLEAN
LdrpDecideAppLocalCopy(
    _In_ PUNICODE_STRING Candidate)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING Path;
    WCHAR Tag, Entry[MAX_PATH + 1];
    NTSTATUS Status = STATUS_NO_MEMORY;
    HANDLE Handle;

    RtlAcquirePebLock();
    if (LdrpFindName(LdrpAppContractDecisions, Candidate, TRUE, &Tag))
    {
        RtlReleasePebLock();
        return Tag == L'L';
    }

    /* The entry must fit before anything is decided */
    if (!LdrpReserveName(&LdrpAppContractDecisions, LdrpAppContractDecisionsUsed, &LdrpAppContractDecisionsSize,
                         Candidate->Length + sizeof(WCHAR)))
    {
        RtlReleasePebLock();
        DPRINT1("%wZ: no memory to keep a decision, left to the DLL search\n", Candidate);
        return TRUE;
    }

    Path.MaximumLength = LdrpAppDirectory.Length + Candidate->Length;
    Path.Length = 0;
    Path.Buffer = NULL;
    if (!LdrpAppDirectory.Buffer || (ULONG)LdrpAppDirectory.Length + Candidate->Length > MAXUSHORT)
        Status = STATUS_OBJECT_PATH_INVALID;
    else
        Path.Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Path.MaximumLength);
    if (Path.Buffer)
    {
        RtlAppendUnicodeStringToString(&Path, &LdrpAppDirectory);
        RtlAppendUnicodeStringToString(&Path, Candidate);
        InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
        Status = NtOpenFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes, &IoStatusBlock,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
        if (NT_SUCCESS(Status))
            NtClose(Handle);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Path.Buffer);
    }

    if (NT_SUCCESS(Status))
    {
        Tag = L'L';
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_OBJECT_PATH_NOT_FOUND ||
             Status == STATUS_FILE_IS_A_DIRECTORY)
    {
        Tag = L'S';
        DPRINT1("%wZ: no copy next to the executable (0x%lx), using the system host\n", Candidate, Status);
    }
    else
    {
        RtlReleasePebLock();
        DPRINT1("%wZ: the copy next to the executable could not be checked (0x%lx), left to the DLL search\n",
                Candidate, Status);
        return TRUE;
    }

    Entry[0] = Tag;
    RtlCopyMemory(Entry + 1, Candidate->Buffer, Candidate->Length);
    LdrpAppendName(&LdrpAppContractDecisions, &LdrpAppContractDecisionsUsed, &LdrpAppContractDecisionsSize,
                   Entry, Candidate->Length + sizeof(WCHAR));
    RtlReleasePebLock();
    return Tag == L'L';
}

static BOOLEAN
LdrpApisetHasAppLocalCopy(
    _In_ PUNICODE_STRING Name,
    _In_opt_ PUNICODE_STRING Extension)
{
    UNICODE_STRING Host, Candidate;
    WCHAR Buffer[MAX_PATH];
    BOOLEAN Windows7, HasExtension = FALSE;
    PWSTR List;
    USHORT n;

    if (NT_SUCCESS(ApiSetResolveToHostEx(APISET_WIN7, TRUE, Name, &Windows7, &Host)) && Windows7)
        return FALSE;

    for (n = 0; n < Name->Length / sizeof(WCHAR); ++n)
    {
        if (Name->Buffer[n] == L'.')
            HasExtension = TRUE;
    }
    RtlInitEmptyUnicodeString(&Candidate, Buffer, sizeof(Buffer));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&Candidate, Name)))
        return FALSE;
    if (!HasExtension && Extension && !NT_SUCCESS(RtlAppendUnicodeStringToString(&Candidate, Extension)))
        return FALSE;

    List = LdrpAppContracts;
    if (!List)
        List = LdrpReadAppContracts();
    if (List == LDRP_APP_CONTRACTS_UNREAD)
        return LdrpDecideAppLocalCopy(&Candidate);
    return LdrpFindName(List, &Candidate, FALSE, NULL);
}

NTSYSAPI
NTSTATUS
NTAPI
LdrpApplyFileNameRedirection(
    _In_ PUNICODE_STRING OriginalName,
    _In_ PUNICODE_STRING Extension,
    _Inout_opt_ PUNICODE_STRING StaticString,
    _Inout_opt_ PUNICODE_STRING DynamicString,
    _Inout_ PUNICODE_STRING *NewName,
    _Out_ PBOOLEAN RedirectedDll)
{

    /* Check for invalid parameters */
    if (!OriginalName)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!DynamicString && !StaticString)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!NewName)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *RedirectedDll = FALSE;

    PCUNICODE_STRING PrevNewName = *NewName;
    UNICODE_STRING ApisetName = {0};
    NTSTATUS Status = STATUS_SUCCESS;

    Status = ApiSetResolveToHost(LdrpApisetVersion, OriginalName, RedirectedDll, &ApisetName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ApiSetResolveToHost FAILED: (Status 0x%x)\n", Status);
        return Status;
    }
    if (*RedirectedDll && LdrpApisetHasAppLocalCopy(OriginalName, Extension))
    {
        DPRINT("%wZ: the application has its own copy\n", OriginalName);
        *RedirectedDll = FALSE;
    }

    if (*RedirectedDll)
    {
        UNICODE_STRING NtSystemRoot;
        static const UNICODE_STRING System32 = RTL_CONSTANT_STRING(L"\\System32\\");
        PUNICODE_STRING ResultPath = NULL;

        /* This is an apiset we can use */
        RtlInitUnicodeString(&NtSystemRoot, SharedUserData->NtSystemRoot);

        SIZE_T Needed = System32.Length + ApisetName.Length + NtSystemRoot.Length + sizeof(UNICODE_NULL);

        if (StaticString && StaticString->MaximumLength >= (USHORT)Needed)
        {
            StaticString->Length = 0;
            ResultPath = StaticString;
        }
        else if (DynamicString)
        {
            DynamicString->Buffer = RtlpAllocateStringMemory(Needed, TAG_USTR);
            if (DynamicString->Buffer == NULL)
            {
                DPRINT1("LdrpApplyFileNameRedirection out of memory\n");
                return STATUS_NO_MEMORY;
            }
            DynamicString->MaximumLength = (USHORT)Needed;
            DynamicString->Length = 0;

            ResultPath = DynamicString;
        }
        else
        {
            DPRINT1("ERROR: LdrpApplyFileNameRedirection no inputbuffer valid\n");
            return STATUS_INVALID_PARAMETER;
        }

        RtlAppendUnicodeStringToString(ResultPath, &NtSystemRoot);
        RtlAppendUnicodeStringToString(ResultPath, &System32);
        RtlAppendUnicodeStringToString(ResultPath, &ApisetName);
        DPRINT1("ApiSetResolveToHost redirected %wZ to %wZ\n", OriginalName, ResultPath);
        *NewName = ResultPath;
    }
    else
    {
        /* Check if the SxS Assemblies specify another file */
        Status = RtlDosApplyFileIsolationRedirection_Ustr(RTL_DOS_APPLY_FILE_REDIRECTION_USTR_FLAG_RESPECT_DOT_LOCAL, OriginalName, Extension, StaticString, DynamicString, NewName, NULL, NULL, NULL);

        /* Check success */
        if (NT_SUCCESS(Status))
        {
            /* Let Ldrp know */
            *RedirectedDll = TRUE;
        }
        else if (Status == STATUS_SXS_KEY_NOT_FOUND)
        {
            ASSERT(*NewName == PrevNewName);
            Status = STATUS_SUCCESS;
        }
        else
        {
            /* Unrecoverable SxS failure; did we get a string? */
            if (DynamicString && DynamicString->Buffer)
                RtlFreeUnicodeString(DynamicString);
            return Status;
        }
    }

    return Status;
}

/*
 * @implemented
 */
NTSYSAPI
NTSTATUS
NTAPI
RtlWow64EnableFsRedirection(IN BOOLEAN Wow64FsEnableRedirection)
{
    /* This is what Windows returns on x86 */
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
NTSYSAPI
NTSTATUS
NTAPI
RtlWow64EnableFsRedirectionEx(IN PVOID Wow64FsEnableRedirection,
                              OUT PVOID *OldFsRedirectionLevel)
{
    /* This is what Windows returns on x86 */
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSYSAPI
NTSTATUS
NTAPI
RtlComputeImportTableHash(IN HANDLE FileHandle,
                          OUT PCHAR Hash,
                          IN ULONG ImportTableHashSize)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
RtlpSafeCopyMemory(
   _Out_writes_bytes_all_(Length) VOID UNALIGNED *Destination,
   _In_reads_bytes_(Length) CONST VOID UNALIGNED *Source,
   _In_ SIZE_T Length)
{
    _SEH2_TRY
    {
        RtlCopyMemory(Destination, Source, Length);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

/* FIXME: code duplication with kernel32/client/time.c */
ULONG
NTAPI
RtlGetTickCount(VOID)
{
    LARGE_INTEGER TickCount;

    TickCount = KiReadSystemTime(&SharedUserData->TickCount);

    /* Convert to milliseconds */
    return KiTickCountToMs(TickCount);
}

/* EOF */
