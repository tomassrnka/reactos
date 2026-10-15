/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test driver for user-mode accesses to special pool
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include "MmSpecialPool.h"

#define TAG_EXACT 'pSmK'

static KMT_MESSAGE_HANDLER TestMessageHandler;
static PUCHAR LiveBuffer;

NTSTATUS
TestEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PCUNICODE_STRING RegistryPath,
    OUT PCWSTR *DeviceName,
    IN OUT INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"MmSpecialPool";
    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);
    return STATUS_SUCCESS;
}

VOID
TestUnload(
    IN PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    if (LiveBuffer)
    {
        ExFreePoolWithTag(LiveBuffer, TAG_EXACT);
        LiveBuffer = NULL;
    }
}

/* Special pool puts the data at the end of its page (overruns) or at its start (underruns), next to an unmapped page */
static
BOOLEAN
IsSpecial(
    _In_ PUCHAR Buffer,
    _In_ SIZE_T Size,
    _Out_ PUCHAR *Guard)
{
    PUCHAR PageEnd = (PUCHAR)PAGE_ALIGN(Buffer) + PAGE_SIZE;

    if ((PageEnd - (Buffer + Size) < 2 * sizeof(PVOID)) && !MmIsAddressValid(PageEnd))
    {
        *Guard = PageEnd;
        return TRUE;
    }
    if ((PAGE_ALIGN(Buffer) == Buffer) && !MmIsAddressValid(Buffer - 1))
    {
        *Guard = Buffer - 1;
        return TRUE;
    }
    return FALSE;
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
    PSPECIAL_POOL_ADDRESSES Addresses = Buffer;
    PUCHAR Freed, Guard = NULL, FreedGuard;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (ControlCode != IOCTL_GET_ADDRESSES)
        return STATUS_NOT_SUPPORTED;
    if (!Addresses || InLength < sizeof(*Addresses) || *OutLength < sizeof(*Addresses))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Addresses, sizeof(*Addresses));
    *OutLength = sizeof(*Addresses);

    if (!LiveBuffer)
    {
        LiveBuffer = ExAllocatePoolWithTag(NonPagedPool, 24, TAG_EXACT);
        if (!LiveBuffer)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!IsSpecial(LiveBuffer, 24, &Guard))
        return STATUS_SUCCESS;

    Freed = ExAllocatePoolWithTag(NonPagedPool, 24, TAG_EXACT);
    if (!Freed)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!IsSpecial(Freed, 24, &FreedGuard))
    {
        ExFreePoolWithTag(Freed, TAG_EXACT);
        return STATUS_SUCCESS;
    }
    ExFreePoolWithTag(Freed, TAG_EXACT);

    Addresses->Live = (ULONG_PTR)LiveBuffer;
    Addresses->Guard = (ULONG_PTR)Guard;
    Addresses->Freed = (ULONG_PTR)Freed;
    Addresses->Special = TRUE;
    return STATUS_SUCCESS;
}
