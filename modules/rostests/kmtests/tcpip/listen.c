/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Cancel a TDI_LISTEN request after its address file was closed
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include <tdikrnl.h>
#include <ndk/rtlfuncs.h>

#include "tcpip.h"

#define TAG_TEST 'tseT'

static
NTSTATUS
NTAPI
ListenCompletionRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NETWORK_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
OpenTcpFile(
    _Out_ PHANDLE Handle,
    _In_ PCSTR EaName,
    _In_ USHORT EaNameLength,
    _In_ PVOID EaValue,
    _In_ USHORT EaValueLength)
{
    UNICODE_STRING TcpDeviceName = RTL_CONSTANT_STRING(L"\\Device\\Tcp");
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK StatusBlock;
    PFILE_FULL_EA_INFORMATION FileInfo;
    ULONG FileInfoSize;
    NTSTATUS Status;

    FileInfoSize = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName[EaNameLength]) + 1 + EaValueLength;
    FileInfo = ExAllocatePoolZero(NonPagedPool, FileInfoSize, TAG_TEST);
    if (!FileInfo)
        return STATUS_INSUFFICIENT_RESOURCES;

    FileInfo->EaNameLength = (UCHAR)EaNameLength;
    FileInfo->EaValueLength = EaValueLength;
    RtlCopyMemory(&FileInfo->EaName[0], EaName, EaNameLength);
    RtlCopyMemory(&FileInfo->EaName[EaNameLength + 1], EaValue, EaValueLength);

    InitializeObjectAttributes(&ObjectAttributes,
                               &TcpDeviceName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwCreateFile(Handle,
                          GENERIC_READ | GENERIC_WRITE,
                          &ObjectAttributes,
                          &StatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          0,
                          FileInfo,
                          FileInfoSize);

    ExFreePoolWithTag(FileInfo, TAG_TEST);
    return Status;
}

static
VOID
TestListenCancelAfterAddressClose(void)
{
    HANDLE AddressHandle, ConnectionHandle;
    PFILE_OBJECT ConnectionFileObject;
    PDEVICE_OBJECT DeviceObject;
    TA_IP_ADDRESS Address;
    TA_IP_ADDRESS ReturnAddress;
    TDI_CONNECTION_INFORMATION ReturnInfo;
    CONNECTION_CONTEXT ConnectionContext = (CONNECTION_CONTEXT)(ULONG_PTR)0x5157;
    LARGE_INTEGER Timeout;
    BOOLEAN Cancelled;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    RtlZeroMemory(&Address, sizeof(Address));
    Address.TAAddressCount = 1;
    Address.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    Address.Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    Address.Address[0].Address[0].in_addr = 0x0100007F; /* 127.0.0.1 */

    Status = OpenTcpFile(&AddressHandle,
                         TdiTransportAddress,
                         TDI_TRANSPORT_ADDRESS_LENGTH,
                         &Address,
                         sizeof(Address));
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = OpenTcpFile(&ConnectionHandle,
                         TdiConnectionContext,
                         TDI_CONNECTION_CONTEXT_LENGTH,
                         &ConnectionContext,
                         sizeof(ConnectionContext));
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(AddressHandle);
        return;
    }

    Status = ObReferenceObjectByHandle(ConnectionHandle,
                                       GENERIC_READ,
                                       *IoFileObjectType,
                                       KernelMode,
                                       (PVOID*)&ConnectionFileObject,
                                       NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(ConnectionHandle);
        ZwClose(AddressHandle);
        return;
    }
    DeviceObject = IoGetRelatedDeviceObject(ConnectionFileObject);

    /* Associate the connection with the address */
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (skip(Irp != NULL, "IoAllocateIrp failed\n"))
        goto Cleanup;
    TdiBuildAssociateAddress(Irp, DeviceObject, ConnectionFileObject, NULL, NULL, AddressHandle);
    IoSetCompletionRoutine(Irp, ListenCompletionRoutine, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    IoFreeIrp(Irp);

    /* Queue a listen request on the connection */
    KeClearEvent(&Event);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (skip(Irp != NULL, "IoAllocateIrp failed\n"))
        goto Cleanup;
    RtlZeroMemory(&ReturnInfo, sizeof(ReturnInfo));
    RtlZeroMemory(&ReturnAddress, sizeof(ReturnAddress));
    ReturnInfo.RemoteAddressLength = sizeof(ReturnAddress);
    ReturnInfo.RemoteAddress = &ReturnAddress;
    TdiBuildListen(Irp, DeviceObject, ConnectionFileObject, NULL, NULL, 0, NULL, &ReturnInfo);
    IoSetCompletionRoutine(Irp, ListenCompletionRoutine, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(DeviceObject, Irp);
    ok_eq_hex(Status, STATUS_PENDING);
    ok(KeReadStateEvent(&Event) == 0, "The listen request completed early\n");

    /* Closing the address file closes its listener and flushes the
     * listen request; then cancel the request */
    ZwClose(AddressHandle);
    AddressHandle = NULL;
    Cancelled = IoCancelIrp(Irp);
    trace("IoCancelIrp returned %u\n", Cancelled);

    Timeout.QuadPart = -10 * 1000 * 1000 * 10LL;
    Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
    {
        /* The completion routine uses Event: do not return before it runs */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }
    ok_eq_hex(Irp->IoStatus.Status, STATUS_CANCELLED);
    ok_eq_size(Irp->IoStatus.Information, 0);
    IoFreeIrp(Irp);

Cleanup:
    ObDereferenceObject(ConnectionFileObject);
    ZwClose(ConnectionHandle);
    if (AddressHandle)
        ZwClose(AddressHandle);
}

static KSTART_ROUTINE RunTest;
static
VOID
NTAPI
RunTest(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    TestListenCancelAfterAddressClose();
}

KMT_MESSAGE_HANDLER TestListenCancel;
NTSTATUS
TestListenCancel(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    PKTHREAD Thread;

    Thread = KmtStartThread(RunTest, NULL);
    KmtFinishThread(Thread, NULL);

    return STATUS_SUCCESS;
}
