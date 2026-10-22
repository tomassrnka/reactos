/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Maximum sizes of data sections on a file
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define FILE_SIZE 0x100

static LONGLONG GetSize(HANDLE hFile)
{
    LARGE_INTEGER Size;

    if (!GetFileSizeEx(hFile, &Size))
        return -1;
    return Size.QuadPart;
}

static void TestSize(HANDLE hFile, LONGLONG MaximumSize, ULONG Protect, NTSTATUS Expected, LONGLONG ExpectedFileSize)
{
    HANDLE hSection = NULL;
    LARGE_INTEGER Size;
    NTSTATUS Status;

    Size.QuadPart = MaximumSize;
    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, &Size, Protect, SEC_COMMIT, hFile);
    ok(Status == Expected, "Size 0x%I64x protect 0x%lx: Status 0x%08lx, expected 0x%08lx\n",
       MaximumSize, Protect, Status, Expected);
    if (NT_SUCCESS(Status))
        NtClose(hSection);
    ok(GetSize(hFile) == ExpectedFileSize, "Size 0x%I64x protect 0x%lx: file size 0x%I64x, expected 0x%I64x\n",
       MaximumSize, Protect, GetSize(hFile), ExpectedFileSize);
}

START_TEST(NtCreateSectionSize)
{
    WCHAR TempDir[MAX_PATH], Path[MAX_PATH];
    BYTE Data[FILE_SIZE];
    HANDLE hFile;
    DWORD Written;

    if (!GetTempPathW(ARRAYSIZE(TempDir), TempDir) ||
        !GetTempFileNameW(TempDir, L"ncz", 0, Path))
    {
        skip("No temporary file: %lu\n", GetLastError());
        return;
    }
    hFile = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        skip("Cannot create %ls: %lu\n", Path, GetLastError());
        return;
    }
    memset(Data, 0x5A, sizeof(Data));
    ok(WriteFile(hFile, Data, sizeof(Data), &Written, NULL) && Written == sizeof(Data), "WriteFile failed: %lu\n", GetLastError());

    /* A negative size never reaches the file system, whatever the protection */
    TestSize(hFile, -1, PAGE_READWRITE, STATUS_SECTION_TOO_BIG, FILE_SIZE);
    TestSize(hFile, -0x100000000LL, PAGE_READWRITE, STATUS_SECTION_TOO_BIG, FILE_SIZE);
    TestSize(hFile, -0x100000000LL + FILE_SIZE, PAGE_READWRITE, STATUS_SECTION_TOO_BIG, FILE_SIZE);
    TestSize(hFile, -1, PAGE_READONLY, STATUS_SECTION_TOO_BIG, FILE_SIZE);
    TestSize(hFile, -0x100000000LL, PAGE_WRITECOPY, STATUS_SECTION_TOO_BIG, FILE_SIZE);

    /* Larger than the file: only a writable section extends it */
    TestSize(hFile, FILE_SIZE + 1, PAGE_READONLY, STATUS_SECTION_TOO_BIG, FILE_SIZE);
    TestSize(hFile, FILE_SIZE + 1, PAGE_WRITECOPY, STATUS_SECTION_TOO_BIG, FILE_SIZE);
    TestSize(hFile, FILE_SIZE, PAGE_READONLY, STATUS_SUCCESS, FILE_SIZE);
    TestSize(hFile, 0x200, PAGE_READWRITE, STATUS_SUCCESS, 0x200);

    CloseHandle(hFile);
}
