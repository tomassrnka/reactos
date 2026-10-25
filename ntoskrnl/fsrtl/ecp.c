/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Extra Create Parameters (ECP) lists and contexts
 * COPYRIGHT:       Copyright 2026 Tomas Srnka
 *
 * This is the container part of the ECP API: allocating, inserting, finding
 * and freeing ECPs. FsRtlGetEcpListFromIrp and FsRtlGetNextExtraCreateParameter
 * are in sdk/lib/drivers/ntoskrnl_vista/fsrtl.c, and the layouts here must match
 * that file. IoCreateFileEx does not deliver a list to the file system yet, so a
 * create IRP carries none.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* ntoskrnl is built for an older NTDDI_VERSION, which hides these declarations */
typedef VOID
(*PFSRTL_EXTRA_CREATE_PARAMETER_CLEANUP_CALLBACK)(
    _Inout_ PVOID EcpContext,
    _In_ const GUID *EcpType);
typedef struct _ECP_LIST ECP_LIST, *PECP_LIST;
typedef ULONG FSRTL_ALLOCATE_ECPLIST_FLAGS;
typedef ULONG FSRTL_ALLOCATE_ECP_FLAGS;
#define FSRTL_ALLOCATE_ECPLIST_FLAG_CHARGE_QUOTA 0x00000001
#define FSRTL_ALLOCATE_ECP_FLAG_CHARGE_QUOTA     0x00000001
#define FSRTL_ALLOCATE_ECP_FLAG_NONPAGED_POOL    0x00000002

VOID
NTAPI
FsRtlFreeExtraCreateParameter(
    _In_ PVOID EcpContext);

#define TAG_ECP_LIST 'LpcE'
#define TAG_ECP      'pcE '

#define ECP_LIST_SIGNATURE   'LPCE'
#define ECP_HEADER_SIGNATURE 'HPCE'

#define ECP_FLAG_NONPAGED       0x00000001
#define ECP_FLAG_ACKNOWLEDGED   0x00000002
#define ECP_FLAG_FROM_USERMODE  0x00000004
#define ECP_FLAG_IN_LIST        0x00000008

/* Same layout as in sdk/lib/drivers/ntoskrnl_vista/fsrtl.c */
struct _ECP_LIST
{
    ULONG Signature;
    ULONG Flags;
    LIST_ENTRY EcpList;
};

typedef struct _ECP_HEADER
{
    ULONG Signature;
    ULONG PoolTag; /* ntoskrnl_vista calls this Spare */
    LIST_ENTRY ListEntry;
    GUID EcpType;
    PFSRTL_EXTRA_CREATE_PARAMETER_CLEANUP_CALLBACK CleanupCallback;
    ULONG Flags;
    ULONG Size; /* Header and context */
    PVOID ListAllocatedFrom;
    PVOID Filter;
} ECP_HEADER, *PECP_HEADER;

#define ECP_FROM_CONTEXT(Context) ((PECP_HEADER)((ULONG_PTR)(Context) - sizeof(ECP_HEADER)))
#define ECP_CONTEXT(Ecp)          ((PVOID)((ULONG_PTR)(Ecp) + sizeof(ECP_HEADER)))

/* PUBLIC FUNCTIONS ***********************************************************/

NTSTATUS
NTAPI
FsRtlAllocateExtraCreateParameterList(
    _In_ FSRTL_ALLOCATE_ECPLIST_FLAGS Flags,
    _Outptr_ PECP_LIST *EcpList)
{
    PECP_LIST List;

    PAGED_CODE();

    *EcpList = NULL;
    if (Flags & ~FSRTL_ALLOCATE_ECPLIST_FLAG_CHARGE_QUOTA)
        return STATUS_INVALID_PARAMETER;

    List = ExAllocatePoolWithTag(NonPagedPool, sizeof(*List), TAG_ECP_LIST);
    if (List == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    List->Signature = ECP_LIST_SIGNATURE;
    List->Flags = 0;
    InitializeListHead(&List->EcpList);
    *EcpList = List;
    return STATUS_SUCCESS;
}

VOID
NTAPI
FsRtlFreeExtraCreateParameterList(
    _In_ PECP_LIST EcpList)
{
    PAGED_CODE();

    /* The list owns its ECPs */
    while (!IsListEmpty(&EcpList->EcpList))
    {
        PECP_HEADER Ecp = CONTAINING_RECORD(RemoveHeadList(&EcpList->EcpList), ECP_HEADER, ListEntry);

        Ecp->Flags &= ~ECP_FLAG_IN_LIST;
        FsRtlFreeExtraCreateParameter(ECP_CONTEXT(Ecp));
    }
    ExFreePoolWithTag(EcpList, TAG_ECP_LIST);
}

NTSTATUS
NTAPI
FsRtlAllocateExtraCreateParameter(
    _In_ const GUID * EcpType,
    _In_ ULONG SizeOfContext,
    _In_ FSRTL_ALLOCATE_ECP_FLAGS Flags,
    _In_opt_ PFSRTL_EXTRA_CREATE_PARAMETER_CLEANUP_CALLBACK CleanupCallback,
    _In_ ULONG PoolTag,
    _Outptr_result_bytebuffer_(SizeOfContext) PVOID *EcpContext)
{
    PECP_HEADER Ecp;

    PAGED_CODE();

    *EcpContext = NULL;
    if (Flags & ~(FSRTL_ALLOCATE_ECP_FLAG_CHARGE_QUOTA | FSRTL_ALLOCATE_ECP_FLAG_NONPAGED_POOL))
        return STATUS_INVALID_PARAMETER;
    if (SizeOfContext > MAXULONG - sizeof(ECP_HEADER))
        return STATUS_INVALID_PARAMETER;

    Ecp = ExAllocatePoolWithTag((Flags & FSRTL_ALLOCATE_ECP_FLAG_NONPAGED_POOL) ? NonPagedPool : PagedPool,
                                sizeof(*Ecp) + SizeOfContext,
                                PoolTag ? PoolTag : TAG_ECP);
    if (Ecp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Ecp, sizeof(*Ecp) + SizeOfContext);
    Ecp->Signature = ECP_HEADER_SIGNATURE;
    Ecp->EcpType = *EcpType;
    Ecp->CleanupCallback = CleanupCallback;
    Ecp->Size = sizeof(*Ecp) + SizeOfContext;
    Ecp->PoolTag = PoolTag ? PoolTag : TAG_ECP;
    Ecp->Flags = (Flags & FSRTL_ALLOCATE_ECP_FLAG_NONPAGED_POOL) ? ECP_FLAG_NONPAGED : 0;

    *EcpContext = ECP_CONTEXT(Ecp);
    return STATUS_SUCCESS;
}

VOID
NTAPI
FsRtlFreeExtraCreateParameter(
    _In_ PVOID EcpContext)
{
    PECP_HEADER Ecp = ECP_FROM_CONTEXT(EcpContext);

    PAGED_CODE();

    /* A linked ECP belongs to its list, which frees it */
    ASSERT(!(Ecp->Flags & ECP_FLAG_IN_LIST));

    if (Ecp->CleanupCallback != NULL)
        Ecp->CleanupCallback(EcpContext, &Ecp->EcpType);

    ExFreePoolWithTag(Ecp, Ecp->PoolTag);
}

NTSTATUS
NTAPI
FsRtlInsertExtraCreateParameter(
    _Inout_ PECP_LIST EcpList,
    _Inout_ PVOID EcpContext)
{
    PECP_HEADER Ecp = ECP_FROM_CONTEXT(EcpContext);
    PLIST_ENTRY Entry;

    PAGED_CODE();

    if (Ecp->Flags & ECP_FLAG_IN_LIST)
        return STATUS_INVALID_PARAMETER;

    /* One ECP per type */
    for (Entry = EcpList->EcpList.Flink; Entry != &EcpList->EcpList; Entry = Entry->Flink)
    {
        if (RtlEqualMemory(&CONTAINING_RECORD(Entry, ECP_HEADER, ListEntry)->EcpType, &Ecp->EcpType, sizeof(GUID)))
            return STATUS_OBJECT_NAME_COLLISION;
    }

    InsertTailList(&EcpList->EcpList, &Ecp->ListEntry);
    Ecp->Flags |= ECP_FLAG_IN_LIST;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
FsRtlFindExtraCreateParameter(
    _In_ PECP_LIST EcpList,
    _In_ const GUID * EcpType,
    _Outptr_opt_ PVOID *EcpContext,
    _Out_opt_ ULONG *EcpContextSize)
{
    PLIST_ENTRY Entry;

    PAGED_CODE();

    if (EcpContext != NULL)
        *EcpContext = NULL;
    if (EcpContextSize != NULL)
        *EcpContextSize = 0;

    for (Entry = EcpList->EcpList.Flink; Entry != &EcpList->EcpList; Entry = Entry->Flink)
    {
        PECP_HEADER Ecp = CONTAINING_RECORD(Entry, ECP_HEADER, ListEntry);

        if (RtlEqualMemory(&Ecp->EcpType, EcpType, sizeof(GUID)))
        {
            if (EcpContext != NULL)
                *EcpContext = ECP_CONTEXT(Ecp);
            if (EcpContextSize != NULL)
                *EcpContextSize = Ecp->Size - sizeof(ECP_HEADER);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

NTSTATUS
NTAPI
FsRtlRemoveExtraCreateParameter(
    _Inout_ PECP_LIST EcpList,
    _In_ const GUID * EcpType,
    _Outptr_ PVOID *EcpContext,
    _Out_opt_ ULONG *EcpContextSize)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = FsRtlFindExtraCreateParameter(EcpList, EcpType, EcpContext, EcpContextSize);
    if (NT_SUCCESS(Status))
    {
        PECP_HEADER Ecp = ECP_FROM_CONTEXT(*EcpContext);

        RemoveEntryList(&Ecp->ListEntry);
        Ecp->Flags &= ~ECP_FLAG_IN_LIST;
    }
    return Status;
}

VOID
NTAPI
FsRtlAcknowledgeEcp(
    _In_ PVOID EcpContext)
{
    ECP_FROM_CONTEXT(EcpContext)->Flags |= ECP_FLAG_ACKNOWLEDGED;
}

BOOLEAN
NTAPI
FsRtlIsEcpAcknowledged(
    _In_ PVOID EcpContext)
{
    return (ECP_FROM_CONTEXT(EcpContext)->Flags & ECP_FLAG_ACKNOWLEDGED) != 0;
}

BOOLEAN
NTAPI
FsRtlIsEcpFromUserMode(
    _In_ PVOID EcpContext)
{
    return (ECP_FROM_CONTEXT(EcpContext)->Flags & ECP_FLAG_FROM_USERMODE) != 0;
}

/*
 * The create IRP carries its list in UserBuffer, where IoGetIrpExtraCreateParameter
 * of ntoskrnl_vista (behind FsRtlGetEcpListFromIrp) looks for it. Defining it here
 * keeps ntoskrnl from linking that library's io.c, which duplicates other kernel exports.
 */
NTSTATUS
NTAPI
IoGetIrpExtraCreateParameter(
    _In_ PIRP Irp,
    _Outptr_result_maybenull_ PECP_LIST *ExtraCreateParameter)
{
    if (!BooleanFlagOn(Irp->Flags, IRP_CREATE_OPERATION))
        return STATUS_INVALID_PARAMETER;

    *ExtraCreateParameter = Irp->UserBuffer;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
IoSetIrpExtraCreateParameter(
    _Inout_ PIRP Irp,
    _In_ PECP_LIST ExtraCreateParameter)
{
    if (!BooleanFlagOn(Irp->Flags, IRP_CREATE_OPERATION))
        return STATUS_INVALID_PARAMETER;

    Irp->UserBuffer = ExtraCreateParameter;
    return STATUS_SUCCESS;
}

VOID
NTAPI
IoClearIrpExtraCreateParameter(
    _Inout_ PIRP Irp)
{
    if (BooleanFlagOn(Irp->Flags, IRP_CREATE_OPERATION))
        Irp->UserBuffer = NULL;
}

/* EOF */
