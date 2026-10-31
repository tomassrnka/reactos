/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtSetEaFile and NtQueryEaFile
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define PATTERN 0x55

START_TEST(NtEaFile)
{
    union
    {
        FILE_FULL_EA_INFORMATION Ea;
        UCHAR Buffer[64];
    } Set, Get;
    WCHAR Path[MAX_PATH];
    IO_STATUS_BLOCK Io;
    NTSTATUS Status;
    HANDLE File;
    ULONG Length;
    ULONG_PTR Pattern;

    RtlFillMemory(&Pattern, sizeof(Pattern), PATTERN);
    Length = GetTempPathW(RTL_NUMBER_OF(Path), Path);
    StringCbPrintfW(Path + Length, sizeof(Path) - Length * sizeof(WCHAR), L"wow64pf_ea_%lu.tmp", GetCurrentProcessId());
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;

    RtlZeroMemory(&Set, sizeof(Set));
    Set.Ea.NextEntryOffset = 0;
    Set.Ea.Flags = 0;
    Set.Ea.EaNameLength = 6;
    Set.Ea.EaValueLength = 3;
    RtlCopyMemory(Set.Ea.EaName, "PFTEST", 7);
    RtlCopyMemory(Set.Ea.EaName + 7, "abc", 3);
    Length = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + 7 + 3;

    RtlFillMemory(&Io, sizeof(Io), PATTERN);
    Status = NtSetEaFile(File, &Io, &Set, Length);
    if (Status == STATUS_NOT_IMPLEMENTED || Status == STATUS_EAS_NOT_SUPPORTED)
    {
        skip("The file system keeps no extended attributes (%lx)\n", Status);
    }
    else
    {
        ok_hex(Status, STATUS_SUCCESS);
        ok_hex(Io.Status, STATUS_SUCCESS);

        RtlFillMemory(&Get, sizeof(Get), PATTERN);
        RtlFillMemory(&Io, sizeof(Io), PATTERN);
        Status = NtQueryEaFile(File, &Io, &Get, sizeof(Get), FALSE, NULL, 0, NULL, TRUE);
        ok_hex(Status, STATUS_SUCCESS);
        ok_hex(Io.Status, STATUS_SUCCESS);
        ok_eq_ulong(Get.Ea.NextEntryOffset, 0);
        ok_eq_int(Get.Ea.EaNameLength, 6);
        ok_eq_int(Get.Ea.EaValueLength, 3);
        ok(!memcmp(Get.Ea.EaName, "PFTEST", 7), "name is %.6s\n", Get.Ea.EaName);
        ok(!memcmp(Get.Ea.EaName + 7, "abc", 3), "value is wrong\n");

        /* Too small for the entry */
        RtlFillMemory(&Io, sizeof(Io), PATTERN);
        Status = NtQueryEaFile(File, &Io, &Get, 8, FALSE, NULL, 0, NULL, TRUE);
        ok_hex(Status, STATUS_BUFFER_TOO_SMALL);
    }

    RtlFillMemory(&Io, sizeof(Io), PATTERN);
    Status = NtSetEaFile((HANDLE)(ULONG_PTR)0x12344, &Io, &Set, Length);
    /* The ReactOS kernel has no extended attributes at all, and does not look at the handle */
    ok(Status == STATUS_INVALID_HANDLE || Status == STATUS_NOT_IMPLEMENTED, "NtSetEaFile gave %lx\n", Status);
    ok_eq_hex((ULONG)Io.Status, PATTERN * 0x01010101);
    ok_eq_ulongptr(Io.Information, Pattern);
    Status = NtQueryEaFile((HANDLE)(ULONG_PTR)0x12344, &Io, &Get, sizeof(Get), FALSE, NULL, 0, NULL, TRUE);
    ok(Status == STATUS_INVALID_HANDLE || Status == STATUS_NOT_IMPLEMENTED, "NtQueryEaFile gave %lx\n", Status);

    CloseHandle(File);
    DeleteFileW(Path);
}
