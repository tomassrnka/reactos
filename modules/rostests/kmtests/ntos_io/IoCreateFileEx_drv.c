/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IoCreateFileEx test driver: the extra create parameters (ECP list) of
 *              the driver context reach the file system in the create IRP
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#include "IoCreateFileEx.h"

/* Declared by the headers only for newer NTDDI_VERSIONs than this driver is built for */
typedef struct _TEST_ECP_LIST *PTEST_ECP_LIST;
typedef NTSTATUS (NTAPI *P_FsRtlAllocateExtraCreateParameterList)(ULONG Flags, PTEST_ECP_LIST *EcpList);
typedef VOID (NTAPI *P_FsRtlFreeExtraCreateParameterList)(PTEST_ECP_LIST EcpList);
typedef NTSTATUS (NTAPI *P_FsRtlAllocateExtraCreateParameter)(const GUID *EcpType, ULONG SizeOfContext, ULONG Flags,
                                                             PVOID Cleanup, ULONG PoolTag, PVOID *EcpContext);
typedef NTSTATUS (NTAPI *P_FsRtlInsertExtraCreateParameter)(PTEST_ECP_LIST EcpList, PVOID EcpContext);
typedef NTSTATUS (NTAPI *P_FsRtlFindExtraCreateParameter)(PTEST_ECP_LIST EcpList, const GUID *EcpType,
                                                         PVOID *EcpContext, PULONG EcpContextSize);
typedef NTSTATUS (NTAPI *P_FsRtlGetEcpListFromIrp)(PIRP Irp, PTEST_ECP_LIST *EcpList);
typedef struct _TEST_DRIVER_CONTEXT
{
    CSHORT Size;
    PTEST_ECP_LIST ExtraCreateParameter;
    PVOID DeviceObjectHint;
    PVOID TxnParameters;
} TEST_DRIVER_CONTEXT;
typedef NTSTATUS (NTAPI *P_IoCreateFileEx)(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                          PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                          ULONG ShareAccess, ULONG Disposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength,
                                          CREATE_FILE_TYPE CreateFileType, PVOID InternalParameters, ULONG Options,
                                          TEST_DRIVER_CONTEXT *DriverContext);

static P_FsRtlAllocateExtraCreateParameterList pAllocateList;
static P_FsRtlFreeExtraCreateParameterList pFreeList;
static P_FsRtlAllocateExtraCreateParameter pAllocateEcp;
static P_FsRtlInsertExtraCreateParameter pInsertEcp;
static P_FsRtlFindExtraCreateParameter pFindEcp;
static P_FsRtlGetEcpListFromIrp pGetEcpListFromIrp;
static P_IoCreateFileEx pIoCreateFileEx;

static const GUID EcpTypeA = { 0x11111111, 0x1111, 0x1111, { 1, 1, 1, 1, 1, 1, 1, 1 } };
static const GUID EcpTypeB = { 0x22222222, 0x2222, 0x2222, { 2, 2, 2, 2, 2, 2, 2, 2 } };

static KMT_IRP_HANDLER TestIrpHandler;
static KMT_MESSAGE_HANDLER TestMessageHandler;

/* What the create handler saw in the last create IRP */
static struct
{
    ULONG Creates;
    NTSTATUS GetStatus;
    PTEST_ECP_LIST List;
    NTSTATUS FindAStatus;
    PVOID FoundA;
    ULONG SizeA;
    NTSTATUS FindBStatus;
    UCHAR Data[8];
    PDEVICE_OBJECT Device;
} Seen;

static
BOOLEAN
ResolveExports(VOID)
{
    UNICODE_STRING Name;

#define GET(var, name) \
    RtlInitUnicodeString(&Name, L ## name); \
    var = MmGetSystemRoutineAddress(&Name); \
    if (var == NULL) { ok(FALSE, name " is not exported\n"); return FALSE; }
    GET(pAllocateList, "FsRtlAllocateExtraCreateParameterList")
    GET(pFreeList, "FsRtlFreeExtraCreateParameterList")
    GET(pAllocateEcp, "FsRtlAllocateExtraCreateParameter")
    GET(pInsertEcp, "FsRtlInsertExtraCreateParameter")
    GET(pFindEcp, "FsRtlFindExtraCreateParameter")
    GET(pGetEcpListFromIrp, "FsRtlGetEcpListFromIrp")
    GET(pIoCreateFileEx, "IoCreateFileEx")
#undef GET
    return TRUE;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"IoCreateFileEx";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    KmtRegisterIrpHandler(IRP_MJ_CREATE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_CLEANUP, NULL, TestIrpHandler);
    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
}

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    PAGED_CODE();

    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_CREATE)
    {
        ULONG Size = 0;

        Seen.Creates++;
        Seen.Device = DeviceObject;
        Seen.List = (PTEST_ECP_LIST)(ULONG_PTR)0xDEADBEEF;
        Seen.FoundA = NULL;
        Seen.SizeA = 0;
        Seen.FindAStatus = Seen.FindBStatus = STATUS_UNSUCCESSFUL;
        RtlZeroMemory(Seen.Data, sizeof(Seen.Data));

        /* the opens of the test program itself come before the exports are resolved */
        Seen.GetStatus = (pGetEcpListFromIrp != NULL) ? pGetEcpListFromIrp(Irp, &Seen.List) : STATUS_UNSUCCESSFUL;
        if (NT_SUCCESS(Seen.GetStatus) && Seen.List != NULL)
        {
            PVOID Context;

            Seen.FindAStatus = pFindEcp(Seen.List, &EcpTypeA, &Seen.FoundA, &Size);
            Seen.SizeA = Size;
            if (NT_SUCCESS(Seen.FindAStatus) && Size >= sizeof(Seen.Data))
                RtlCopyMemory(Seen.Data, Seen.FoundA, sizeof(Seen.Data));
            Seen.FindBStatus = pFindEcp(Seen.List, &EcpTypeB, &Context, NULL);
        }
        Irp->IoStatus.Information = FILE_OPENED;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
DoCreate(
    _In_ PUNICODE_STRING Name,
    _In_ ULONG Options,
    _In_opt_ TEST_DRIVER_CONTEXT *DriverContext)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE Handle;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = pIoCreateFileEx(&Handle, GENERIC_READ, &ObjectAttributes, &IoStatusBlock, NULL, FILE_ATTRIBUTE_NORMAL,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT,
                             NULL, 0, CreateFileTypeNone, NULL, Options, DriverContext);
    if (NT_SUCCESS(Status))
        ObCloseHandle(Handle, KernelMode);
    return Status;
}

static
VOID
RunEcpTest(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\Kmtest-IoCreateFileEx\\probe");
    TEST_DRIVER_CONTEXT Context;
    PTEST_ECP_LIST List;
    PVOID A;
    ULONG Before;
    NTSTATUS Status;

    Status = pAllocateList(0, &List);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = pAllocateEcp(&EcpTypeA, 8, 0, NULL, 'tseT', &A);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        pFreeList(List);
        return;
    }
    RtlCopyMemory(A, "ECPDATA!", 8);
    Status = pInsertEcp(List, A);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* A create without a driver context carries no list */
    Before = Seen.Creates;
    Status = DoCreate(&Name, IO_NO_PARAMETER_CHECKING, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Seen.Creates, Before + 1);
    ok_eq_hex(Seen.GetStatus, STATUS_SUCCESS);
    ok_eq_pointer(Seen.List, NULL);

    /* The list of the driver context is the one of the create IRP, with its ECPs */
    RtlZeroMemory(&Context, sizeof(Context));
    Context.Size = sizeof(Context);
    Context.ExtraCreateParameter = List;
    Before = Seen.Creates;
    Status = DoCreate(&Name, IO_NO_PARAMETER_CHECKING, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Seen.Creates, Before + 1);
    ok_eq_hex(Seen.GetStatus, STATUS_SUCCESS);
    ok_eq_pointer(Seen.List, List);
    ok_eq_hex(Seen.FindAStatus, STATUS_SUCCESS);
    ok_eq_pointer(Seen.FoundA, A);
    ok_eq_ulong(Seen.SizeA, 8);
    ok(RtlCompareMemory(Seen.Data, "ECPDATA!", 8) == 8, "ECP context content differs\n");
    ok_eq_hex(Seen.FindBStatus, STATUS_NOT_FOUND);

    /* A driver context without a list: none is delivered */
    Context.ExtraCreateParameter = NULL;
    Before = Seen.Creates;
    Status = DoCreate(&Name, IO_NO_PARAMETER_CHECKING, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Seen.Creates, Before + 1);
    ok_eq_pointer(Seen.List, NULL);

    /* A context too small to hold the list is ignored, like the hint */
    Context.ExtraCreateParameter = List;
    Context.Size = sizeof(CSHORT);
    Before = Seen.Creates;
    Status = DoCreate(&Name, IO_NO_PARAMETER_CHECKING, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Seen.Creates, Before + 1);
    ok_eq_pointer(Seen.List, NULL);

    pFreeList(List);
}

static
NTSTATUS
TestMessageHandler(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG ControlCode,
    IN PVOID Buffer OPTIONAL,
    IN SIZE_T InLength,
    IN OUT PSIZE_T OutLength)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    switch (ControlCode)
    {
        case IOCTL_RUN_ECP_TEST:
            if (!ResolveExports())
                return STATUS_SUCCESS;
            RunEcpTest();
            return STATUS_SUCCESS;
        default:
            return STATUS_NOT_SUPPORTED;
    }
}
