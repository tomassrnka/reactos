/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite: delete and attribute queries on a name that is not a file
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define CALL_COUNT 10

static
ULONG
GetHandleCount(
    _In_ HANDLE Handle)
{
    NTSTATUS Status;
    PUBLIC_OBJECT_BASIC_INFORMATION ObjectInfo;

    RtlZeroMemory(&ObjectInfo, sizeof(ObjectInfo));
    Status = ZwQueryObject(Handle,
                           ObjectBasicInformation,
                           &ObjectInfo,
                           sizeof(ObjectInfo),
                           NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return ObjectInfo.HandleCount;
}

static
NTSTATUS
CallDeleteFile(
    _In_ POBJECT_ATTRIBUTES ObjectAttributes)
{
    return ZwDeleteFile(ObjectAttributes);
}

static
NTSTATUS
CallQueryFullAttributesFile(
    _In_ POBJECT_ATTRIBUTES ObjectAttributes)
{
    FILE_NETWORK_OPEN_INFORMATION NetworkInfo;
    return ZwQueryFullAttributesFile(ObjectAttributes, &NetworkInfo);
}

static
NTSTATUS
CallFastQueryNetworkAttributes(
    _In_ POBJECT_ATTRIBUTES ObjectAttributes)
{
    BOOLEAN Result;
    IO_STATUS_BLOCK IoStatus;
    FILE_NETWORK_OPEN_INFORMATION NetworkInfo;

    IoStatus.Status = STATUS_PENDING;
    Result = IoFastQueryNetworkAttributes(ObjectAttributes,
                                          FILE_READ_ATTRIBUTES,
                                          0,
                                          &IoStatus,
                                          &NetworkInfo);
    ok_bool_true(Result, "IoFastQueryNetworkAttributes returned");
    return IoStatus.Status;
}

static
VOID
TestNonFileName(
    _In_ PCSTR CallName,
    _In_ NTSTATUS (*Call)(POBJECT_ATTRIBUTES),
    _In_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ HANDLE EventHandle)
{
    NTSTATUS Status;
    ULONG Before, After, i, Failures = 0;

    Before = GetHandleCount(EventHandle);
    for (i = 0; i < CALL_COUNT; i++)
    {
        Status = Call(ObjectAttributes);
        if (Status != STATUS_OBJECT_TYPE_MISMATCH)
        {
            if (Failures++ == 0)
                trace("%s: Status = 0x%lx\n", CallName, Status);
        }
    }
    After = GetHandleCount(EventHandle);

    ok(Failures == 0, "%s: %lu of %u calls did not return STATUS_OBJECT_TYPE_MISMATCH\n",
       CallName, Failures, CALL_COUNT);
    ok(After == Before, "%s: event handle count went from %lu to %lu over %u calls\n",
       CallName, Before, After, CALL_COUNT);
}

START_TEST(IoNonFileName)
{
    NTSTATUS Status;
    HANDLE EventHandle;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING EventName = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\KmtestIoNonFileNameEvent");

    /* Handles leaked by an earlier failing run keep the event alive */
    InitializeObjectAttributes(&ObjectAttributes,
                               &EventName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_OPENIF,
                               NULL,
                               NULL);
    Status = ZwCreateEvent(&EventHandle,
                           EVENT_ALL_ACCESS,
                           &ObjectAttributes,
                           NotificationEvent,
                           FALSE);
    ok(Status == STATUS_SUCCESS || Status == STATUS_OBJECT_NAME_EXISTS,
       "ZwCreateEvent returned 0x%lx\n", Status);
    if (skip(NT_SUCCESS(Status), "No event\n"))
        return;

    /* The name resolves to an event, which the I/O manager does not parse */
    InitializeObjectAttributes(&ObjectAttributes,
                               &EventName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    TestNonFileName("ZwDeleteFile", CallDeleteFile, &ObjectAttributes, EventHandle);
    TestNonFileName("ZwQueryFullAttributesFile", CallQueryFullAttributesFile, &ObjectAttributes, EventHandle);
    TestNonFileName("IoFastQueryNetworkAttributes", CallFastQueryNetworkAttributes, &ObjectAttributes, EventHandle);

    ZwClose(EventHandle);
}
