/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS: FSCTL_SET/GET/DELETE_REPARSE_POINT reach the reparse handler and complete the request
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef REPARSE_DATA_BUFFER_HEADER_SIZE
#define REPARSE_DATA_BUFFER_HEADER_SIZE FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)
#endif

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define SENTINEL_STATUS ((NTSTATUS)0x7EADBEEF)

/*
 * Issues the FSCTL on H with an IO_STATUS_BLOCK that holds a sentinel.  A successful request completes the block;
 * the I/O manager need not write it for a request that fails synchronously, so only the status is checked then.
 */
static NTSTATUS Fsctl(HANDLE H, ULONG Code, PVOID In, ULONG InLen, PVOID Out, ULONG OutLen, PULONG_PTR Info)
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    Iosb.Status = SENTINEL_STATUS;
    Iosb.Information = 0xDEAD;
    Status = NtFsControlFile(H, NULL, NULL, NULL, &Iosb, Code, In, InLen, Out, OutLen);
    if (NT_SUCCESS(Status))
    {
        ok(Iosb.Status == Status, "FSCTL %08lx: returned 0x%08lx, status block 0x%08lx\n", Code, Status, Iosb.Status);
        if (Info)
            *Info = Iosb.Information;
    }
    return Status;
}

START_TEST(NtfsReparseRouting)
{
    ULONGLONG Buffer[(REPARSE_DATA_BUFFER_HEADER_SIZE + 8 + 4 * MAX_PATH * sizeof(WCHAR)) / sizeof(ULONGLONG)];
    ULONGLONG Back[sizeof(Buffer) / sizeof(ULONGLONG)];
    PREPARSE_DATA_BUFFER Rp = (PREPARSE_DATA_BUFFER)Buffer, Got = (PREPARSE_DATA_BUFFER)Back;
    WCHAR J[MAX_PATH], T[MAX_PATH], Sub[MAX_PATH + 8];
    USHORT SubLen, PrintLen;
    ULONG_PTR Info;
    HANDLE H = NULL;
    NTSTATUS Status;

    if (!NtInit("NtfsReparseRouting"))
        return;
    NtPath(J, L"j");
    NtPath(T, L"t");
    ok_hex(NtMakeDir(T, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(NtMakeDir(J, NULL, 0, FALSE), STATUS_SUCCESS);
    Status = NtOpen(J, FILE_WRITE_ATTRIBUTES | FILE_WRITE_DATA | SYNCHRONIZE, SHARE_ALL, FILE_OPEN,
                    FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &H);
    ok_hex(Status, STATUS_SUCCESS);
    if (!H)
    {
        NtCleanup();
        return;
    }

    _snwprintf(Sub, RTL_NUMBER_OF(Sub) - 1, L"\\??\\%ls", T);
    Sub[RTL_NUMBER_OF(Sub) - 1] = 0;
    SubLen = (USHORT)(wcslen(Sub) * sizeof(WCHAR));
    PrintLen = (USHORT)(wcslen(T) * sizeof(WCHAR));
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Rp->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    Rp->MountPointReparseBuffer.SubstituteNameOffset = 0;
    Rp->MountPointReparseBuffer.SubstituteNameLength = SubLen;
    Rp->MountPointReparseBuffer.PrintNameOffset = SubLen + sizeof(WCHAR);
    Rp->MountPointReparseBuffer.PrintNameLength = PrintLen;
    RtlCopyMemory(Rp->MountPointReparseBuffer.PathBuffer, Sub, SubLen);
    RtlCopyMemory((PUCHAR)Rp->MountPointReparseBuffer.PathBuffer + SubLen + sizeof(WCHAR), T, PrintLen);
    Rp->ReparseDataLength = (USHORT)(FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) -
                                     REPARSE_DATA_BUFFER_HEADER_SIZE + SubLen + PrintLen + 2 * sizeof(WCHAR));

    /* Before the fix the request never reached the reparse handler: STATUS_INVALID_DEVICE_REQUEST, block unwritten. */
    Status = Fsctl(H, FSCTL_SET_REPARSE_POINT, Rp, REPARSE_DATA_BUFFER_HEADER_SIZE + Rp->ReparseDataLength, NULL, 0, NULL);
    ok(Status != STATUS_INVALID_DEVICE_REQUEST, "FSCTL_SET_REPARSE_POINT did not reach the reparse handler\n");
    ok_hex(Status, STATUS_SUCCESS);

    RtlZeroMemory(Back, sizeof(Back));
    Status = Fsctl(H, FSCTL_GET_REPARSE_POINT, NULL, 0, Back, sizeof(Back), &Info);
    ok(Status != STATUS_INVALID_DEVICE_REQUEST, "FSCTL_GET_REPARSE_POINT did not reach the reparse handler\n");
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_hex(Got->ReparseTag, IO_REPARSE_TAG_MOUNT_POINT);
        ok_dec(Got->ReparseDataLength, Rp->ReparseDataLength);
        ok_dec((ULONG)Info, REPARSE_DATA_BUFFER_HEADER_SIZE + Rp->ReparseDataLength);
        ok(Got->MountPointReparseBuffer.SubstituteNameLength == SubLen &&
           !memcmp(Got->MountPointReparseBuffer.PathBuffer, Sub, SubLen), "the substitute name does not round-trip\n");
    }

    RtlZeroMemory(Back, REPARSE_DATA_BUFFER_HEADER_SIZE);
    Got->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    Status = Fsctl(H, FSCTL_DELETE_REPARSE_POINT, Back, REPARSE_DATA_BUFFER_HEADER_SIZE, NULL, 0, NULL);
    ok(Status != STATUS_INVALID_DEVICE_REQUEST, "FSCTL_DELETE_REPARSE_POINT did not reach the reparse handler\n");
    ok_hex(Status, STATUS_SUCCESS);
    Status = Fsctl(H, FSCTL_GET_REPARSE_POINT, NULL, 0, Back, sizeof(Back), NULL);
    ok_hex(Status, STATUS_NOT_A_REPARSE_POINT);

    NtClose(H);
    NtCleanup();
}
