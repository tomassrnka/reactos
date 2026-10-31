/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for IOCTL_DISK_PERFORMANCE on \\.\PhysicalDrive0
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expectations were measured on Windows 10 22H2 with a read-only query of \\.\PhysicalDrive0: the record
 * is DISK_PERFORMANCE (88 bytes on x64), "Partmgr " names the manager, times are in units of 100 ns, and a
 * buffer that is too small gives ERROR_INSUFFICIENT_BUFFER.
 */

#include "precomp.h"

#include <winioctl.h>

static BOOL
Query(HANDLE Disk, DISK_PERFORMANCE *Perf, DWORD *Error)
{
    DWORD Returned = 0;
    BOOL Ok;

    memset(Perf, 0xCC, sizeof(*Perf));
    SetLastError(0xDEADBEEF);
    Ok = DeviceIoControl(Disk, IOCTL_DISK_PERFORMANCE, NULL, 0, Perf, sizeof(*Perf), &Returned, NULL);
    *Error = GetLastError();
    if (Ok)
        ok(Returned == sizeof(*Perf), "returned %lu bytes, expected %lu\n", Returned, (ULONG)sizeof(*Perf));
    return Ok;
}

START_TEST(DiskPerformance)
{
    static const WCHAR ManagerName[8] = L"Partmgr ";
    DISK_PERFORMANCE Before, After;
    FILETIME Now;
    ULONGLONG Time;
    HANDLE Disk;
    DWORD Error, Returned;
    CHAR Small[16];
    BOOL Ok;
    PVOID Buffer;
    ULONG Read = 0, i;
    LARGE_INTEGER Offset;

    Disk = CreateFileW(L"\\\\.\\PhysicalDrive0", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Disk == INVALID_HANDLE_VALUE)
    {
        skip("Cannot open PhysicalDrive0: %lu\n", GetLastError());
        return;
    }

    Ok = Query(Disk, &Before, &Error);
    if (!Ok && Error == ERROR_INVALID_FUNCTION)
        ok(0, "IOCTL_DISK_PERFORMANCE is not supported\n");
    ok(Ok, "IOCTL_DISK_PERFORMANCE failed with %lu\n", Error);
    if (!Ok)
    {
        CloseHandle(Disk);
        return;
    }

    ok(Before.StorageDeviceNumber == 0, "StorageDeviceNumber is %lu\n", Before.StorageDeviceNumber);
    ok(!memcmp(Before.StorageManagerName, ManagerName, sizeof(ManagerName)), "StorageManagerName is %.8ls\n", Before.StorageManagerName);
    ok(Before.QueueDepth < 1000, "QueueDepth is %lu\n", Before.QueueDepth);
    ok(Before.BytesRead.QuadPart > 0 && Before.ReadCount > 0, "nothing was read from the disk since it started: %I64d bytes, %lu requests\n",
       Before.BytesRead.QuadPart, Before.ReadCount);
    ok(Before.BytesRead.QuadPart >= (LONGLONG)Before.ReadCount * 512, "BytesRead %I64d is less than a sector for each of %lu requests\n",
       Before.BytesRead.QuadPart, Before.ReadCount);
    ok(Before.ReadTime.QuadPart > 0, "ReadTime is %I64d\n", Before.ReadTime.QuadPart);
    ok(Before.IdleTime.QuadPart > 0, "IdleTime is %I64d\n", Before.IdleTime.QuadPart);

    /* QueryTime is the system time */
    GetSystemTimeAsFileTime(&Now);
    Time = ((ULONGLONG)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
    ok((ULONGLONG)Before.QueryTime.QuadPart <= Time + 10000000ULL && (ULONGLONG)Before.QueryTime.QuadPart + 600 * 10000000ULL >= Time,
       "QueryTime %I64d is not the system time %I64u\n", Before.QueryTime.QuadPart, Time);

    /* A buffer that is too small */
    Returned = 0;
    SetLastError(0xDEADBEEF);
    Ok = DeviceIoControl(Disk, IOCTL_DISK_PERFORMANCE, NULL, 0, Small, sizeof(Small), &Returned, NULL);
    ok(!Ok, "a %lu byte buffer succeeded\n", (ULONG)sizeof(Small));
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "error is %lu\n", GetLastError());
    CloseHandle(Disk);

    /* Reads show: 64 reads of 64 KiB, unbuffered, from a place most likely not read before */
    Disk = CreateFileW(L"\\\\.\\PhysicalDrive0", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_NO_BUFFERING, NULL);
    if (Disk == INVALID_HANDLE_VALUE)
    {
        skip("Cannot open PhysicalDrive0 for reading: %lu\n", GetLastError());
        return;
    }
    Buffer = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Offset.QuadPart = 0;
    SetFilePointerEx(Disk, Offset, NULL, FILE_BEGIN);
    for (i = 0; i < 64; i++)
    {
        DWORD Got = 0;
        if (!ReadFile(Disk, Buffer, 65536, &Got, NULL) || Got != 65536)
            break;
        Read += Got;
    }
    if (Read != 64 * 65536)
    {
        skip("Read only %lu bytes of the disk\n", Read);
        VirtualFree(Buffer, 0, MEM_RELEASE);
        CloseHandle(Disk);
        return;
    }
    VirtualFree(Buffer, 0, MEM_RELEASE);

    Ok = Query(Disk, &After, &Error);
    ok(Ok, "IOCTL_DISK_PERFORMANCE failed with %lu\n", Error);
    if (Ok)
    {
        /* Others may read as well: at least what this test read */
        ok(After.BytesRead.QuadPart - Before.BytesRead.QuadPart >= Read, "BytesRead rose by %I64d, expected at least %lu\n",
           After.BytesRead.QuadPart - Before.BytesRead.QuadPart, Read);
        ok(After.ReadCount - Before.ReadCount >= 64, "ReadCount rose by %lu, expected at least 64\n", After.ReadCount - Before.ReadCount);
        ok(After.ReadTime.QuadPart > Before.ReadTime.QuadPart, "ReadTime did not rise: %I64d, %I64d\n",
           Before.ReadTime.QuadPart, After.ReadTime.QuadPart);
        ok(After.IdleTime.QuadPart >= Before.IdleTime.QuadPart, "IdleTime fell: %I64d, %I64d\n",
           Before.IdleTime.QuadPart, After.IdleTime.QuadPart);
        ok(After.WriteCount >= Before.WriteCount, "WriteCount fell\n");
        ok(After.BytesWritten.QuadPart >= Before.BytesWritten.QuadPart, "BytesWritten fell\n");
        ok(After.QueryTime.QuadPart >= Before.QueryTime.QuadPart, "QueryTime fell\n");
    }
    CloseHandle(Disk);
}
