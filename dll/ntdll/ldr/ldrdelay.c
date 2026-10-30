/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     LdrResolveDelayLoadedAPI, the loader half of delay-load import resolution
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* TYPES ********************************************************************/

/* IMAGE_DELAYLOAD_DESCRIPTOR, DELAYLOAD_INFO and the hook types, as in the SDK's winnt.h and
 * delayloadhandler.h (the NDK headers used here do not carry them) */
typedef struct _LDRP_DELAYLOAD_DESCRIPTOR
{
    ULONG Attributes;           /* bit 0: the other fields are RVAs, not addresses */
    ULONG DllNameRVA;
    ULONG ModuleHandleRVA;
    ULONG ImportAddressTableRVA;
    ULONG ImportNameTableRVA;
    ULONG BoundImportAddressTableRVA;
    ULONG UnloadInformationTableRVA;
    ULONG TimeDateStamp;
} LDRP_DELAYLOAD_DESCRIPTOR;

typedef struct _LDRP_DELAYLOAD_INFO
{
    ULONG Size;
    const LDRP_DELAYLOAD_DESCRIPTOR *DelayloadDescriptor;
    PIMAGE_THUNK_DATA ThunkAddress;
    LPCSTR TargetDllName;
    ULONG ImportDescribedByName;
    union
    {
        LPCSTR Name;
        ULONG Ordinal;
    } Description;
    PVOID TargetModuleBase;
    PVOID Unused;
    ULONG LastError;
} LDRP_DELAYLOAD_INFO;

typedef PVOID (NTAPI *PLDRP_DELAYLOAD_FAILURE_DLL_CALLBACK)(ULONG, LDRP_DELAYLOAD_INFO *);
typedef PVOID (NTAPI *PLDRP_DELAYLOAD_FAILURE_SYSTEM_ROUTINE)(LPCSTR, LPCSTR);

#define LDRP_DELAYLOAD_RVA_BASED    0x1
#define LDRP_DELAYLOAD_GPA_FAILURE  4

/* FUNCTIONS ****************************************************************/

static
VOID
LdrpDelayLoadWriteThunk(
    _In_ PIMAGE_THUNK_DATA Thunk,
    _In_ PVOID Value)
{
    PVOID Address = Thunk;
    SIZE_T Size = sizeof(*Thunk);
    ULONG OldProtect;

    /* The delay-load import table can sit in a read-only section until it is first resolved */
    if (NT_SUCCESS(NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READWRITE, &OldProtect)))
    {
        Thunk->u1.Function = (ULONG_PTR)Value;
        Address = Thunk;
        Size = sizeof(*Thunk);
        NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, OldProtect, &OldProtect);
        return;
    }

    _SEH2_TRY
    {
        Thunk->u1.Function = (ULONG_PTR)Value;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
    }
    _SEH2_END;
}

/*
 * Resolves one import of a delay-load descriptor: loads the DLL (unless the module handle slot
 * already has it), looks the import up and stores the address in the import address table.
 *
 * Behaviour of Windows 10 22H2, probed with a hand-built descriptor:
 *  - a thunk that no longer points into the parent image is already resolved and is returned as it is
 *  - the DLL handle is stored in the descriptor's module slot, also when the import is then not found
 *  - on a failure the result is NULL and nothing is raised; the DLL hook is called first (reason 4,
 *    last error ERROR_MOD_NOT_FOUND, ERROR_PROC_NOT_FOUND or ERROR_INVALID_ORDINAL), its result is
 *    used unless it is NULL, then the system routine is called with the DLL name and the import
 *    name or ordinal. A failed import by name leaves the table untouched; for a failed
 *    import by ordinal a non-NULL hook result is stored in the table.
 */
PVOID
NTAPI
LdrResolveDelayLoadedAPI(
    _In_ PVOID ParentModuleBase,
    _In_ const LDRP_DELAYLOAD_DESCRIPTOR *Descriptor,
    _In_opt_ PLDRP_DELAYLOAD_FAILURE_DLL_CALLBACK FailureDllHook,
    _In_opt_ PLDRP_DELAYLOAD_FAILURE_SYSTEM_ROUTINE FailureSystemHook,
    _Out_ PIMAGE_THUNK_DATA ThunkAddress,
    _Reserved_ ULONG Flags)
{
    PIMAGE_NT_HEADERS NtHeaders;
    PIMAGE_THUNK_DATA Iat, Int;
    PVOID *ModuleSlot, Module, Result, Procedure = NULL;
    LPCSTR DllName;
    ULONG_PTR Current, Index;
    ANSI_STRING AnsiName, ProcName;
    UNICODE_STRING WideName;
    LDRP_DELAYLOAD_INFO Info;
    NTSTATUS Status;
    BOOLEAN ByName;

#define DELAYLOAD_PTR(Field) \
    ((PVOID)(((Descriptor)->Attributes & LDRP_DELAYLOAD_RVA_BASED) ? \
             (ULONG_PTR)ParentModuleBase + (Field) : (ULONG_PTR)(Field)))

    NtHeaders = RtlImageNtHeader(ParentModuleBase);
    if (!NtHeaders)
        return NULL;

    /* Another thread, or an earlier call, already resolved it */
    Current = ThunkAddress->u1.Function;
    if (Current < (ULONG_PTR)ParentModuleBase ||
        Current - (ULONG_PTR)ParentModuleBase >= NtHeaders->OptionalHeader.SizeOfImage)
    {
        return (PVOID)Current;
    }

    Iat = DELAYLOAD_PTR(Descriptor->ImportAddressTableRVA);
    Int = DELAYLOAD_PTR(Descriptor->ImportNameTableRVA);
    ModuleSlot = DELAYLOAD_PTR(Descriptor->ModuleHandleRVA);
    DllName = DELAYLOAD_PTR(Descriptor->DllNameRVA);
    if (ThunkAddress < Iat)
        return NULL;
    Index = ThunkAddress - Iat;

    RtlZeroMemory(&Info, sizeof(Info));
    Info.Size = sizeof(Info);
    Info.DelayloadDescriptor = Descriptor;
    Info.ThunkAddress = ThunkAddress;
    Info.TargetDllName = DllName;

    /* The import: a name (hint, name) or an ordinal */
    if (IMAGE_SNAP_BY_ORDINAL(Int[Index].u1.Ordinal))
    {
        ByName = FALSE;
        Info.Description.Ordinal = IMAGE_ORDINAL(Int[Index].u1.Ordinal);
    }
    else
    {
        PIMAGE_IMPORT_BY_NAME ImportByName = DELAYLOAD_PTR(Int[Index].u1.AddressOfData);

        ByName = TRUE;
        Info.ImportDescribedByName = 1;
        Info.Description.Name = (LPCSTR)ImportByName->Name;
    }

    /* The DLL */
    Module = *ModuleSlot;
    if (!Module)
    {
        RtlInitAnsiString(&AnsiName, DllName);
        Status = RtlAnsiStringToUnicodeString(&WideName, &AnsiName, TRUE);
        if (NT_SUCCESS(Status))
        {
            Status = LdrLoadDll(NULL, NULL, &WideName, &Module);
            RtlFreeUnicodeString(&WideName);
        }
        if (!NT_SUCCESS(Status))
        {
            Info.LastError = (Status == STATUS_NO_MEMORY) ? ERROR_NOT_ENOUGH_MEMORY : ERROR_MOD_NOT_FOUND;
            goto Failure;
        }

        /* Several threads can get here: the first handle stays, the other references are dropped */
        Result = InterlockedCompareExchangePointer(ModuleSlot, Module, NULL);
        if (Result)
        {
            LdrUnloadDll(Module);
            Module = Result;
        }
    }
    Info.TargetModuleBase = Module;

    /* The import */
    if (ByName)
    {
        RtlInitAnsiString(&ProcName, Info.Description.Name);
        Status = LdrGetProcedureAddress(Module, &ProcName, 0, &Procedure);
    }
    else
    {
        Status = LdrGetProcedureAddress(Module, NULL, Info.Description.Ordinal, &Procedure);
    }
    if (!NT_SUCCESS(Status) || !Procedure)
    {
        Info.LastError = ByName ? ERROR_PROC_NOT_FOUND : ERROR_INVALID_ORDINAL;
        goto Failure;
    }

    LdrpDelayLoadWriteThunk(ThunkAddress, Procedure);
    return Procedure;

Failure:
    Result = NULL;
    if (FailureDllHook)
        Result = FailureDllHook(LDRP_DELAYLOAD_GPA_FAILURE, &Info);
    if (!Result && FailureSystemHook)
    {
        Result = FailureSystemHook(DllName, ByName ? Info.Description.Name :
                                            (LPCSTR)(ULONG_PTR)Info.Description.Ordinal);
    }
    if (Result && !ByName && Info.LastError == ERROR_INVALID_ORDINAL)
        LdrpDelayLoadWriteThunk(ThunkAddress, Result);
    return Result;

#undef DELAYLOAD_PTR
}
