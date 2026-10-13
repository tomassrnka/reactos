/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power request objects (PowerCreateRequest, PowerSetRequest, PowerClearRequest)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

typedef struct _POP_POWER_REQUEST
{
    /* Number of PowerSetRequest calls not yet balanced by PowerClearRequest, per type */
    ULONG Count[PowerRequestMaximum];
} POP_POWER_REQUEST, *PPOP_POWER_REQUEST;

POBJECT_TYPE PopPowerRequestObjectType;

/* Requests of each type that are set, over all objects; protected by PopPowerRequestLock */
static ULONG PopPowerRequestActive[PowerRequestMaximum];
static KSPIN_LOCK PopPowerRequestLock;

static GENERIC_MAPPING PopPowerRequestMapping =
{
    STANDARD_RIGHTS_READ,
    STANDARD_RIGHTS_WRITE | POWER_REQUEST_MODIFY_STATE,
    STANDARD_RIGHTS_EXECUTE,
    POWER_REQUEST_ALL_ACCESS
};

/* PRIVATE FUNCTIONS *********************************************************/

static
VOID
NTAPI
PopDeletePowerRequest(
    _In_ PVOID ObjectBody)
{
    PPOP_POWER_REQUEST Request = ObjectBody;
    KIRQL OldIrql;
    ULONG Type;

    /* Closing the last handle drops whatever the request still holds */
    KeAcquireSpinLock(&PopPowerRequestLock, &OldIrql);
    for (Type = 0; Type < PowerRequestMaximum; Type++)
    {
        if (Request->Count[Type])
        {
            ASSERT(PopPowerRequestActive[Type] > 0);
            PopPowerRequestActive[Type]--;
        }
    }
    KeReleaseSpinLock(&PopPowerRequestLock, OldIrql);
}

CODE_SEG("INIT")
BOOLEAN
NTAPI
PopInitializePowerRequests(VOID)
{
    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    UNICODE_STRING Name;
    NTSTATUS Status;

    KeInitializeSpinLock(&PopPowerRequestLock);

    RtlZeroMemory(&ObjectTypeInitializer, sizeof(ObjectTypeInitializer));
    RtlInitUnicodeString(&Name, L"PowerRequest");
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(POP_POWER_REQUEST);
    ObjectTypeInitializer.GenericMapping = PopPowerRequestMapping;
    ObjectTypeInitializer.PoolType = NonPagedPool;
    ObjectTypeInitializer.ValidAccessMask = POWER_REQUEST_ALL_ACCESS;
    ObjectTypeInitializer.InvalidAttributes = OBJ_OPENLINK | OBJ_PERMANENT;
    ObjectTypeInitializer.DeleteProcedure = PopDeletePowerRequest;
    Status = ObCreateObjectType(&Name, &ObjectTypeInitializer, NULL, &PopPowerRequestObjectType);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create the PowerRequest object type: 0x%lx\n", Status);
        return FALSE;
    }

    return TRUE;
}

/* Handles NtPowerInformation(PowerRequestCreate); OutputBuffer is probed by the caller */
NTSTATUS
NTAPI
PopCreatePowerRequest(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    PPOP_POWER_REQUEST Request;
    HANDLE Handle;
    NTSTATUS Status;

    if (!OutputBuffer || OutputBufferLength != sizeof(HANDLE))
        return STATUS_INVALID_PARAMETER;

    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    Status = ObCreateObject(KernelMode,
                            PopPowerRequestObjectType,
                            &ObjectAttributes,
                            PreviousMode,
                            NULL,
                            sizeof(POP_POWER_REQUEST),
                            0,
                            0,
                            (PVOID*)&Request);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Request, sizeof(*Request));

    Status = ObInsertObject(Request, NULL, POWER_REQUEST_ALL_ACCESS, 0, NULL, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *(PHANDLE)OutputBuffer = Handle;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* The handle cannot be returned, so it must not stay open */
        ObCloseHandle(Handle, PreviousMode);
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

/* Handles NtPowerInformation(PowerRequestAction); InputBuffer is probed by the caller */
NTSTATUS
NTAPI
PopPowerRequestAction(
    _In_reads_bytes_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    POWER_REQUEST_ACTION Action;
    PPOP_POWER_REQUEST Request;
    KIRQL OldIrql;
    NTSTATUS Status;

    if (!InputBuffer || InputBufferLength != sizeof(POWER_REQUEST_ACTION))
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        Action = *(PPOWER_REQUEST_ACTION)InputBuffer;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    /* Windows answers an unknown type, and a clear of a type that is not set, with STATUS_NOT_SUPPORTED */
    if ((ULONG)Action.RequestType >= PowerRequestMaximum)
        return STATUS_NOT_SUPPORTED;

    Status = ObReferenceObjectByHandle(Action.PowerRequestHandle,
                                       POWER_REQUEST_MODIFY_STATE,
                                       PopPowerRequestObjectType,
                                       PreviousMode,
                                       (PVOID*)&Request,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    KeAcquireSpinLock(&PopPowerRequestLock, &OldIrql);
    if (Action.SetAction)
    {
        if (Request->Count[Action.RequestType] == MAXULONG)
        {
            Status = STATUS_INTEGER_OVERFLOW;
        }
        else if (Request->Count[Action.RequestType]++ == 0)
        {
            PopPowerRequestActive[Action.RequestType]++;
        }
    }
    else
    {
        if (Request->Count[Action.RequestType] == 0)
        {
            /* Clearing a request type that is not set */
            Status = STATUS_NOT_SUPPORTED;
        }
        else if (--Request->Count[Action.RequestType] == 0)
        {
            ASSERT(PopPowerRequestActive[Action.RequestType] > 0);
            PopPowerRequestActive[Action.RequestType]--;
        }
    }
    KeReleaseSpinLock(&PopPowerRequestLock, OldIrql);

    ObDereferenceObject(Request);
    return Status;
}

/* The execution state the active power requests ask for, as ES_* flags */
EXECUTION_STATE
NTAPI
PopQueryPowerRequestState(VOID)
{
    EXECUTION_STATE State = 0;
    KIRQL OldIrql;

    KeAcquireSpinLock(&PopPowerRequestLock, &OldIrql);
    if (PopPowerRequestActive[PowerRequestDisplayRequired])
        State |= ES_DISPLAY_REQUIRED;
    if (PopPowerRequestActive[PowerRequestSystemRequired])
        State |= ES_SYSTEM_REQUIRED;
    if (PopPowerRequestActive[PowerRequestAwayModeRequired])
        State |= ES_AWAYMODE_REQUIRED;
    KeReleaseSpinLock(&PopPowerRequestLock, OldIrql);

    return State;
}
