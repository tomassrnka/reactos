/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Contract test: MountMgr IOCTLs must reject malformed, undersized
 *              or out-of-bounds buffered input with STATUS_INVALID_PARAMETER,
 *              while well-formed requests keep being processed normally.
 *              Only rejected/non-matching requests are sent: nothing here
 *              modifies the mount point database.
 */

#include "precomp.h"

#define BAD_VOLUME  L"\\??\\Volume{00000000-0000-0000-0000-000000000000}"

static NTSTATUS
Ioctl(
    _In_ HANDLE MountMgrHandle,
    _In_ ULONG Code,
    _In_ PVOID In,
    _In_ ULONG InLength,
    _In_ PVOID Out,
    _In_ ULONG OutLength)
{
    IO_STATUS_BLOCK IoStatusBlock;

    return NtDeviceIoControlFile(MountMgrHandle, NULL, NULL, NULL,
                                 &IoStatusBlock, Code,
                                 In, InLength, Out, OutLength);
}

static VOID
Test_QueryPoints(
    _In_ HANDLE MountMgrHandle)
{
    NTSTATUS Status;
    MOUNTMGR_MOUNT_POINTS Out;
    struct
    {
        MOUNTMGR_MOUNT_POINT;
        WCHAR Buffer[16];
    } In;
    const ULONG Hdr = sizeof(MOUNTMGR_MOUNT_POINT);

    /* Undersized input */
    RtlZeroMemory(&In, sizeof(In));
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, Hdr - 1, &Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    /* Small length, but offset far beyond the buffer */
    RtlZeroMemory(&In, sizeof(In));
    In.DeviceNameOffset = 0xFFF0;
    In.DeviceNameLength = 8;
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, sizeof(In), &Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    RtlZeroMemory(&In, sizeof(In));
    In.SymbolicLinkNameOffset = 0xFFF0;
    In.SymbolicLinkNameLength = 8;
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, sizeof(In), &Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    RtlZeroMemory(&In, sizeof(In));
    In.UniqueIdOffset = 0xFFF0;
    In.UniqueIdLength = 8;
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, sizeof(In), &Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    /* One byte-pair past the end of the input buffer */
    RtlZeroMemory(&In, sizeof(In));
    In.DeviceNameOffset = Hdr;
    In.DeviceNameLength = sizeof(In.Buffer) + sizeof(WCHAR);
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, sizeof(In), &Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    /* Exactly fitting string: must NOT be rejected as malformed
     * (the unknown device is simply not found / has no points). */
    RtlZeroMemory(&In, sizeof(In));
    RtlCopyMemory(In.Buffer, L"\\Device\\NoSuch", sizeof(L"\\Device\\NoSuch") - sizeof(WCHAR));
    In.DeviceNameOffset = Hdr;
    In.DeviceNameLength = sizeof(L"\\Device\\NoSuch") - sizeof(WCHAR);
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, sizeof(In), &Out, sizeof(Out));
    ok(Status != STATUS_INVALID_PARAMETER, "Valid request rejected as malformed\n");

    /* Odd lengths / offsets */
    In.DeviceNameLength |= 1;
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, sizeof(In), &Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    /* Plain "enumerate everything" query must still work (size probe) */
    RtlZeroMemory(&In, sizeof(In));
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_QUERY_POINTS, &In, Hdr, &Out, sizeof(Out));
    ok(Status == STATUS_BUFFER_OVERFLOW || NT_SUCCESS(Status),
       "Enumeration probe failed, Status 0x%08lx\n", Status);
}

static VOID
Test_DeletePoints(
    _In_ HANDLE MountMgrHandle)
{
    NTSTATUS Status;
    ULONG Out[64];
    struct
    {
        MOUNTMGR_MOUNT_POINT;
        WCHAR Buffer[16];
    } In;

    /* Only malformed requests: they must be rejected before anything is deleted */
    RtlZeroMemory(&In, sizeof(In));
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_DELETE_POINTS,
                   &In, sizeof(MOUNTMGR_MOUNT_POINT) - 1, Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    RtlZeroMemory(&In, sizeof(In));
    In.SymbolicLinkNameOffset = 0xFFF0;
    In.SymbolicLinkNameLength = 8;
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_DELETE_POINTS,
                   &In, sizeof(In), Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

    RtlZeroMemory(&In, sizeof(In));
    In.UniqueIdOffset = sizeof(MOUNTMGR_MOUNT_POINT);
    In.UniqueIdLength = sizeof(In.Buffer) + sizeof(WCHAR);
    Status = Ioctl(MountMgrHandle, IOCTL_MOUNTMGR_DELETE_POINTS,
                   &In, sizeof(In), Out, sizeof(Out));
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
}

static VOID
Test_VolumeMountPoint(
    _In_ HANDLE MountMgrHandle)
{
    NTSTATUS Status;
    ULONG Out[16];
    struct
    {
        MOUNTMGR_VOLUME_MOUNT_POINT;
        WCHAR Source[64];
        WCHAR Target[64];
    } In;
    const ULONG Hdr = sizeof(MOUNTMGR_VOLUME_MOUNT_POINT);
    const ULONG SrcLen = sizeof(BAD_VOLUME) - sizeof(WCHAR);
    ULONG Codes[2] = { IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED,
                       IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_DELETED };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Codes); ++i)
    {
        /* Undersized input */
        RtlZeroMemory(&In, sizeof(In));
        Status = Ioctl(MountMgrHandle, Codes[i], &In, Hdr - 1, Out, sizeof(Out));
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

        /* Offsets pointing outside of the input buffer */
        RtlZeroMemory(&In, sizeof(In));
        In.SourceVolumeNameOffset = 0xFFF0;
        In.SourceVolumeNameLength = 8;
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(In), Out, sizeof(Out));
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

        RtlZeroMemory(&In, sizeof(In));
        In.TargetVolumeNameOffset = 0xFFF0;
        In.TargetVolumeNameLength = 8;
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(In), Out, sizeof(Out));
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

        /* Lengths that overrun the input by one character */
        RtlZeroMemory(&In, sizeof(In));
        In.SourceVolumeNameOffset = Hdr;
        In.SourceVolumeNameLength = sizeof(In.Source) + sizeof(In.Target) + sizeof(WCHAR);
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(In), Out, sizeof(Out));
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

        /* A well-formed request (non-existent volume) is accepted as
         * well-formed: it must fail later for a different reason. This
         * also covers the formerly inverted length comparison, which
         * rejected exactly the requests that fit. */
        RtlZeroMemory(&In, sizeof(In));
        RtlCopyMemory(In.Source, BAD_VOLUME, SrcLen);
        In.SourceVolumeNameOffset = Hdr;
        In.SourceVolumeNameLength = SrcLen;
        RtlCopyMemory(In.Target, BAD_VOLUME, SrcLen);
        In.TargetVolumeNameOffset = Hdr + sizeof(In.Source);
        In.TargetVolumeNameLength = SrcLen;
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(In), Out, sizeof(Out));
        ok(Status != STATUS_INVALID_PARAMETER && !NT_SUCCESS(Status),
           "IOCTL 0x%lx: well-formed request for missing volume, Status 0x%08lx\n",
           Codes[i], Status);
    }
}

static VOID
Test_QueryDosVolumePath(
    _In_ HANDLE MountMgrHandle)
{
    NTSTATUS Status;
    ULONG Out[32];
    struct
    {
        USHORT NameLength;
        WCHAR Name[16];
    } In;
    ULONG Codes[2] = { IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH,
                       IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATHS };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Codes); ++i)
    {
        RtlZeroMemory(&In, sizeof(In));
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(USHORT) - 1, Out, sizeof(Out));
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

        /* Name longer than what was supplied */
        In.NameLength = sizeof(In.Name) + sizeof(WCHAR);
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(In), Out, sizeof(Out));
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

        /* Odd name length */
        In.NameLength = 3;
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(In), Out, sizeof(Out));
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);

        /* Well-formed, unknown device: not rejected as malformed */
        RtlCopyMemory(In.Name, L"\\Device\\NoSuch", sizeof(L"\\Device\\NoSuch") - sizeof(WCHAR));
        In.NameLength = sizeof(L"\\Device\\NoSuch") - sizeof(WCHAR);
        Status = Ioctl(MountMgrHandle, Codes[i], &In, sizeof(In), Out, sizeof(Out));
        ok(Status != STATUS_INVALID_PARAMETER && !NT_SUCCESS(Status),
           "IOCTL 0x%lx: unknown device, Status 0x%08lx\n", Codes[i], Status);
    }
}

START_TEST(MalformedInput)
{
    HANDLE MountMgrHandle;

    MountMgrHandle = GetMountMgrHandle(FILE_READ_DATA | FILE_WRITE_DATA);
    if (!MountMgrHandle)
    {
        win_skip("MountMgr unavailable (read/write): %lu\n", GetLastError());
        return;
    }

    Test_QueryPoints(MountMgrHandle);
    Test_DeletePoints(MountMgrHandle);
    Test_VolumeMountPoint(MountMgrHandle);
    Test_QueryDosVolumePath(MountMgrHandle);

    CloseHandle(MountMgrHandle);
}
