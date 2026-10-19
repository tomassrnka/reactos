/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite registry flush failure test user-mode part:
 *              a hive flush that the disk fails must fail, and must work again
 *              once the disk does
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include <ndk/setypes.h>

#include "CmFlush.h"

#define HIVE_FILE   CMFLUSH_DISK_NAME L"\\CMFLUSH.HIV"
#define SOURCE_KEY  L"\\Registry\\Machine\\SOFTWARE\\KmtCmFlushSource"
#define TARGET_KEY  L"\\Registry\\Machine\\KmtCmFlush"

static
NTSTATUS
OpenKey(
    _Out_ PHANDLE Key,
    _In_ PCWSTR Name,
    _In_ BOOLEAN Create)
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES ObjectAttributes;

    *Key = NULL;
    RtlInitUnicodeString(&Path, Name);
    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (Create)
        return NtCreateKey(Key, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    return NtOpenKey(Key, KEY_ALL_ACCESS, &ObjectAttributes);
}

static
NTSTATUS
SetValue(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Data)
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    return NtSetValueKey(Key, &ValueName, 0, REG_DWORD, &Data, sizeof(Data));
}

static
NTSTATUS
GetValue(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _Out_ PULONG Data)
{
    UNICODE_STRING ValueName;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    ULONG Length;
    NTSTATUS Status;

    *Data = 0;
    RtlInitUnicodeString(&ValueName, Name);
    Status = NtQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &Length);
    if (NT_SUCCESS(Status) && Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
        RtlCopyMemory(Data, Info->Data, sizeof(ULONG));
    return Status;
}

static
ULONG
GetFlushCount(VOID)
{
    DWORD Count = 0;
    DWORD Length = sizeof(Count);
    DWORD Error;

    Error = KmtSendBufferToDriver(IOCTL_CMFLUSH_GET_FLUSH_COUNT, &Count, 0, &Length);
    ok_eq_int(Error, ERROR_SUCCESS);
    return Count;
}

static
DWORD
SetFlushStatus(
    _In_ NTSTATUS Status)
{
    DWORD Error;

    Error = KmtSendUlongToDriver(IOCTL_CMFLUSH_SET_FLUSH_STATUS, (DWORD)Status);
    ok_eq_int(Error, ERROR_SUCCESS);
    return Error;
}

/* Saves a small key into a hive file on the test disk */
static
NTSTATUS
MakeHiveFile(VOID)
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Source, File;
    NTSTATUS Status;

    Status = OpenKey(&Source, SOURCE_KEY, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SetValue(Source, L"Seed", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);

    RtlInitUnicodeString(&Path, HIVE_FILE);
    InitializeObjectAttributes(&ObjectAttributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtCreateFile(&File, GENERIC_WRITE | SYNCHRONIZE, &ObjectAttributes, &IoStatus, NULL,
                          FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtSaveKey(Source, File);
        ok_eq_hex(Status, STATUS_SUCCESS);
        NtClose(File);
    }

    NtDeleteKey(Source);
    NtClose(Source);
    return Status;
}

static
NTSTATUS
LoadHive(VOID)
{
    UNICODE_STRING TargetName, FileName;
    OBJECT_ATTRIBUTES Target, File;

    RtlInitUnicodeString(&TargetName, TARGET_KEY);
    InitializeObjectAttributes(&Target, &TargetName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    RtlInitUnicodeString(&FileName, HIVE_FILE);
    InitializeObjectAttributes(&File, &FileName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtLoadKey(&Target, &File);
}

static
NTSTATUS
UnloadHive(VOID)
{
    UNICODE_STRING TargetName;
    OBJECT_ATTRIBUTES Target;

    RtlInitUnicodeString(&TargetName, TARGET_KEY);
    InitializeObjectAttributes(&Target, &TargetName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtUnloadKey(&Target);
}

START_TEST(CmFlush)
{
    DWORD Error;
    NTSTATUS Status;
    BOOLEAN OldBackup = FALSE, OldRestore = FALSE;
    HANDLE Key = NULL, Machine = NULL;
    ULONG Before, Data;

    /* Once its disk exists, the driver is never unloaded: see CmFlush_drv.c */
    Error = KmtLoadAndOpenDriver(L"CmFlush", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = SetFlushStatus(STATUS_SUCCESS);
    if (Error != ERROR_SUCCESS)
    {
        KmtCloseDriver();

        /* The driver has no disk: unload it, so that a later run loads it again */
        if (Error == ERROR_NOT_READY)
            KmtUnloadDriver();
        return;
    }

    Status = RtlAdjustPrivilege(SE_BACKUP_PRIVILEGE, TRUE, FALSE, &OldBackup);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No backup privilege\n"))
        goto Cleanup;
    Status = RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE, TRUE, FALSE, &OldRestore);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No restore privilege\n"))
        goto Cleanup;

    /* A leftover from an earlier run in this boot */
    UnloadHive();

    Status = MakeHiveFile();
    if (skip(NT_SUCCESS(Status), "No hive file\n"))
        goto Cleanup;

    Status = LoadHive();
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "Hive not loaded\n"))
        goto Cleanup;

    Status = OpenKey(&Key, TARGET_KEY, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = OpenKey(&Machine, L"\\Registry\\Machine", FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(Key != NULL && Machine != NULL, "No key handles\n"))
        goto Unload;

    /* A flush of the hive reaches the disk */
    Before = GetFlushCount();
    Status = SetValue(Key, L"Value1", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtFlushKey(Key);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(GetFlushCount() > Before, "The hive flush sent no flush to the disk\n");

    /* The disk fails its flushes: so must a flush of the hive and of all hives */
    SetFlushStatus(STATUS_IO_DEVICE_ERROR);
    Status = SetValue(Key, L"Value2", 2);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtFlushKey(Key);
    ok_eq_hex(Status, STATUS_REGISTRY_IO_FAILED);
    Status = NtFlushKey(Machine);
    ok_eq_hex(Status, STATUS_REGISTRY_IO_FAILED);

    /* The disk recovers: the next flush writes the hive again */
    SetFlushStatus(STATUS_SUCCESS);
    Status = NtFlushKey(Key);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /*
     * The disk fails its flushes again. The flush of all hives must still
     * succeed, as the hive has nothing left to write, and the reload shows
     * what reached the hive files.
     */
    SetFlushStatus(STATUS_IO_DEVICE_ERROR);
    Status = NtFlushKey(Machine);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtClose(Key);
    Key = NULL;
    Status = UnloadHive();
    ok_eq_hex(Status, STATUS_SUCCESS);
    SetFlushStatus(STATUS_SUCCESS);

    /* What the flush wrote is in the file */
    Status = LoadHive();
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!skip(NT_SUCCESS(Status), "Hive not loaded again\n"))
    {
        Status = OpenKey(&Key, TARGET_KEY, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Status = GetValue(Key, L"Value1", &Data);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok_eq_ulong(Data, 1UL);
            Status = GetValue(Key, L"Value2", &Data);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok_eq_ulong(Data, 2UL);
        }
    }

Unload:
    if (Key)
        NtClose(Key);
    if (Machine)
        NtClose(Machine);
    UnloadHive();

Cleanup:
    SetFlushStatus(STATUS_SUCCESS);
    RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE, OldRestore, FALSE, &OldRestore);
    RtlAdjustPrivilege(SE_BACKUP_PRIVILEGE, OldBackup, FALSE, &OldBackup);
    KmtCloseDriver();
}
