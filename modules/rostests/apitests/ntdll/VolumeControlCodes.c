/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the mount manager, volume and storage control codes of DeviceIoControl
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#include <winioctl.h>
#include <ntddstor.h>
#include <mountdev.h>

START_TEST(VolumeControlCodes)
{
    union
    {
        MOUNTDEV_NAME Name;
        UCHAR Buffer[512];
    } Mount;
    VOLUME_DISK_EXTENTS Extents;
    STORAGE_PROPERTY_QUERY Query;
    STORAGE_DESCRIPTOR_HEADER Header;
    DISK_PERFORMANCE Performance;
    WCHAR VolumeName[64];
    HANDLE Volume, Disk;
    DWORD Returned;
    BOOL Ret;

    /* The mount manager's query for the unique name of a drive letter */
    SetLastError(0xdeadbeef);
    Ret = GetVolumeNameForVolumeMountPointW(L"C:\\", VolumeName, RTL_NUMBER_OF(VolumeName));
    ok(Ret, "GetVolumeNameForVolumeMountPoint failed, error %lu\n", GetLastError());
    if (Ret)
    {
        ok(!wcsncmp(VolumeName, L"\\\\?\\Volume{", 11), "name is %S\n", VolumeName);
        ok_eq_ulong((ULONG)wcslen(VolumeName), 49);
        ok(VolumeName[48] == L'\\', "no backslash at the end of %S\n", VolumeName);
    }

    Volume = CreateFileW(L"\\\\.\\C:", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Volume != INVALID_HANDLE_VALUE, "opening the volume failed, error %lu\n", GetLastError());
    if (Volume != INVALID_HANDLE_VALUE)
    {
        RtlFillMemory(&Mount, sizeof(Mount), 0x55);
        Returned = 0;
        Ret = DeviceIoControl(Volume, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, &Mount, sizeof(Mount), &Returned, NULL);
        ok(Ret, "IOCTL_MOUNTDEV_QUERY_DEVICE_NAME failed, error %lu\n", GetLastError());
        if (Ret)
        {
            ok(Mount.Name.NameLength >= 16 && Mount.Name.NameLength < 200, "name length %u\n", Mount.Name.NameLength);
            ok(!_wcsnicmp(Mount.Name.Name, L"\\Device\\", 8), "name is %.*S\n", Mount.Name.NameLength / 2, Mount.Name.Name);
            ok_eq_ulong(Returned, sizeof(USHORT) + Mount.Name.NameLength);
        }

        RtlFillMemory(&Extents, sizeof(Extents), 0x55);
        Returned = 0;
        Ret = DeviceIoControl(Volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &Extents, sizeof(Extents),
                              &Returned, NULL);
        ok(Ret, "IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS failed, error %lu\n", GetLastError());
        if (Ret)
        {
            ok(Extents.NumberOfDiskExtents >= 1, "%lu extents\n", Extents.NumberOfDiskExtents);
            ok_eq_ulong(Returned, sizeof(Extents));
            ok(Extents.Extents[0].ExtentLength.QuadPart > 0, "empty extent\n");
        }
        else
            ok_eq_ulong(GetLastError(), ERROR_MORE_DATA);

        RtlZeroMemory(&Query, sizeof(Query));
        Query.PropertyId = StorageDeviceProperty;
        Query.QueryType = PropertyStandardQuery;
        RtlFillMemory(&Header, sizeof(Header), 0x55);
        Returned = 0;
        Ret = DeviceIoControl(Volume, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query), &Header, sizeof(Header),
                              &Returned, NULL);
        ok(Ret, "IOCTL_STORAGE_QUERY_PROPERTY failed, error %lu\n", GetLastError());
        if (Ret)
        {
            ok_eq_ulong(Returned, sizeof(Header));
            ok(Header.Size >= sizeof(STORAGE_DEVICE_DESCRIPTOR), "descriptor size %lu\n", Header.Size);
            ok(Header.Version != 0x55555555, "version was not set\n");
        }
        CloseHandle(Volume);
    }

    Disk = CreateFileW(L"\\\\.\\PhysicalDrive0", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Disk == INVALID_HANDLE_VALUE)
    {
        skip("Cannot open PhysicalDrive0: %lu\n", GetLastError());
        return;
    }
    RtlFillMemory(&Performance, sizeof(Performance), 0x55);
    Returned = 0;
    Ret = DeviceIoControl(Disk, IOCTL_DISK_PERFORMANCE, NULL, 0, &Performance, sizeof(Performance), &Returned, NULL);
    ok(Ret, "IOCTL_DISK_PERFORMANCE failed, error %lu\n", GetLastError());
    if (Ret)
    {
        ok_eq_ulong(Returned, sizeof(Performance));
        ok(Performance.QueryTime.QuadPart > 0, "QueryTime is %I64d\n", Performance.QueryTime.QuadPart);
        ok(Performance.StorageDeviceNumber == 0, "StorageDeviceNumber is %lu\n", Performance.StorageDeviceNumber);
    }
    CloseHandle(Disk);
}
