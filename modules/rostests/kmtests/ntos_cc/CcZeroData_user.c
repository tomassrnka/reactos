/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CcZeroData test, user-mode part
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

START_TEST(CcZeroData)
{
    HANDLE Handle;
    NTSTATUS Status;
    LARGE_INTEGER ByteOffset;
    IO_STATUS_BLOCK IoStatusBlock;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UCHAR Buffer[16] = { 0 };
    UNICODE_STRING TestFile = RTL_CONSTANT_STRING(L"\\Device\\Kmtest-CcZeroData\\TestFile");
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"CcZeroData", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    InitializeObjectAttributes(&ObjectAttributes, &TestFile, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, FILE_ALL_ACCESS, &ObjectAttributes, &IoStatusBlock, 0, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* The driver runs the test in its write handler */
        ByteOffset.QuadPart = 0;
        Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatusBlock, Buffer, sizeof(Buffer), &ByteOffset, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        NtClose(Handle);
    }

    KmtCloseDriver();
    KmtUnloadDriver();
}
