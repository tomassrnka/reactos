/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS: system files, the paging file, sharing, file IDs, hard links, directory renames
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

/* "X:\" of the test directory's volume. */
static VOID VolumeRoot(PWSTR Out, PCWSTR Name)
{
    WCHAR Volume[MAX_PATH];
    GetVolumePathNameW(NtTestDir(), Volume, MAX_PATH);
    _snwprintf(Out, MAX_PATH - 1, L"%ls%ls", Volume, Name);
}

/*
 * The metadata files are never opened for modification. Every probe here only opens and closes;
 * nothing is written, truncated or deleted, whatever the open returns.
 */
START_TEST(NtfsSystemFiles)
{
    static const PCWSTR Names[] = { L"$MFT", L"$MFTMirr", L"$LogFile", L"$Volume", L"$AttrDef", L"$Bitmap", L"$Boot",
                                    L"$BadClus", L"$Secure", L"$UpCase", L"$Extend" };
    WCHAR Path[MAX_PATH];
    HANDLE Root;
    NTSTATUS Status;
    ULONG i;

    if (!NtInit("NtfsSystemFiles"))
        return;
    for (i = 0; i < RTL_NUMBER_OF(Names); i++)
    {
        VolumeRoot(Path, Names[i]);
        /* \$Volume is a volume open on Windows (write access is granted to an administrator);
         * ntfsng refuses it, which is recorded, not asserted. */
        BOOLEAN IsVolume = !wcscmp(Names[i], L"$Volume");
        Status = NtOpen(Path, FILE_WRITE_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL);
        ok(IsVolume || Status == STATUS_ACCESS_DENIED || Status == STATUS_SHARING_VIOLATION,
           "%ls for write: 0x%08lx\n", Names[i], Status);
        trace("%ls for write: 0x%08lx\n", Names[i], Status);
        Status = NtOpen(Path, FILE_APPEND_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL);
        ok(IsVolume || Status == STATUS_ACCESS_DENIED || Status == STATUS_SHARING_VIOLATION,
           "%ls for append: 0x%08lx\n", Names[i], Status);
        Status = NtOpen(Path, DELETE, SHARE_ALL, FILE_OPEN, 0, NULL, NULL);
        ok(Status == STATUS_ACCESS_DENIED || Status == STATUS_SHARING_VIOLATION,
           "%ls for delete: 0x%08lx\n", Names[i], Status);
        Status = NtOpen(Path, FILE_READ_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL);
        trace("%ls for read: 0x%08lx\n", Names[i], Status);
        Status = NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, 0, NULL, NULL);
        trace("%ls for attributes: 0x%08lx\n", Names[i], Status);
    }
    /* No named stream of a metadata file can be opened.  FILE_OPEN keeps the probe from creating
     * one on a driver that would allow it, so this does not test the refusal to create a stream;
     * that refusal is in the create path and was checked by code review only. */
    VolumeRoot(Path, L"$MFT:ntfsapitest");
    Status = NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, 0, NULL, NULL);
    ok(!NT_SUCCESS(Status), "Stream on $MFT: 0x%08lx\n", Status);
    /* The same files by their file ID (records 0-11). */
    VolumeRoot(Path, L"");
    Status = NtOpen(Path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, SHARE_ALL, FILE_OPEN, FILE_DIRECTORY_FILE, NULL, &Root);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < 16; i++)
        {
            if (i == 3 || i == 5)
                continue;   /* $Volume (a volume open on Windows) and the root directory */
            ok_hex(NtOpenById(Root, i, FILE_WRITE_DATA, FILE_OPEN, NULL), STATUS_INVALID_PARAMETER);
            ok_hex(NtOpenById(Root, i, FILE_READ_ATTRIBUTES, FILE_OPEN, NULL), STATUS_INVALID_PARAMETER);
        }
        NtClose(Root);
    }
    NtCleanup();
}

/* The active paging file: only Mm writes it. Opens only; nothing is written. */
START_TEST(NtfsPagingFile)
{
    WCHAR Path[MAX_PATH];

    if (!NtInit("NtfsPagingFile"))
        return;
    VolumeRoot(Path, L"pagefile.sys");
    if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
    {
        skip("No paging file on this volume\n");
        NtCleanup();
        return;
    }
    /* Every other open of the paging file is a sharing violation, also one for attributes only. */
    ok_hex(NtOpen(Path, FILE_WRITE_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    ok_hex(NtOpen(Path, FILE_READ_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    ok_hex(NtOpen(Path, DELETE, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    ok_hex(NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    ok_hex(NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OVERWRITE, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    NtCleanup();
}

/* Overwrite and supersede conflict with handles that deny writing (or deleting). */
START_TEST(NtfsSharing)
{
    WCHAR Path[MAX_PATH];
    FILE_STANDARD_INFORMATION Std;
    IO_STATUS_BLOCK Iosb;
    HANDLE A;
    NTSTATUS Status;

    if (!NtInit("NtfsSharing"))
        return;
    NtPath(Path, L"shared.txt");
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "0123456789"), STATUS_SUCCESS);
    Status = NtOpen(Path, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ, FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT, NULL, &A);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    ok_hex(NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OVERWRITE, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    ok_hex(NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OVERWRITE_IF, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    ok_hex(NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_SUPERSEDE, 0, NULL, NULL), STATUS_SHARING_VIOLATION);
    ok_hex(NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SUCCESS);
    ok_hex(NtQueryInformationFile(A, &Iosb, &Std, sizeof(Std), FileStandardInformation), STATUS_SUCCESS);
    ok(Std.EndOfFile.QuadPart == 10, "File size %I64d after refused overwrites\n", Std.EndOfFile.QuadPart);
    NtClose(A);
    /* With write sharing an attributes-only overwrite is allowed and truncates. */
    Status = NtOpen(Path, FILE_READ_DATA | SYNCHRONIZE, SHARE_ALL, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, &A);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_hex(NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OVERWRITE, 0, NULL, NULL), STATUS_SUCCESS);
        NtClose(A);
    }
    NtCleanup();
}

/* Open by file ID opens the file the ID names, also after the name moved to another file. */
START_TEST(NtfsOpenById)
{
    WCHAR Path[MAX_PATH], Moved[MAX_PATH];
    ULONGLONG Id, Got;
    HANDLE Dir, H, F;
    NTSTATUS Status;
    DWORD Done;

    if (!NtInit("NtfsOpenById"))
        return;
    NtPath(Path, L"first.txt");
    NtPath(Moved, L"moved.txt");
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "first"), STATUS_SUCCESS);
    Status = NtOpen(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, 0, NULL, &H);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Id = NtFileId(H);
    NtClose(H);
    ok(MoveFileW(Path, Moved), "MoveFile failed %lu\n", GetLastError());
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "second"), STATUS_SUCCESS);
    ok_hex(NtOpen(NtTestDir(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, SHARE_ALL, FILE_OPEN, FILE_DIRECTORY_FILE, NULL, &Dir),
           STATUS_SUCCESS);
    Status = NtOpenById(Dir, Id, FILE_READ_ATTRIBUTES, FILE_OPEN, &H);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Got = NtFileId(H);
        ok(Got == Id, "Opened ID %I64x for %I64x\n", Got, Id);
        NtClose(H);
    }
    /* A stale sequence number names nothing. */
    Status = NtOpenById(Dir, Id + (1ULL << 48), FILE_READ_ATTRIBUTES, FILE_OPEN, NULL);
    ok(!NT_SUCCESS(Status), "Open with a wrong sequence number: 0x%08lx\n", Status);
    /* A file created just now and still open: its record has not been written back yet. */
    NtPath(Path, L"fresh.txt");
    F = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(F != INVALID_HANDLE_VALUE, "CreateFile failed %lu\n", GetLastError());
    if (F != INVALID_HANDLE_VALUE)
    {
        ok(WriteFile(F, "fresh", 5, &Done, NULL) && Done == 5, "WriteFile failed %lu\n", GetLastError());
        Id = NtFileId(F);
        Status = NtOpenById(Dir, Id, FILE_READ_DATA, FILE_OPEN, &H);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Got = NtFileId(H);
            ok(Got == Id, "Opened ID %I64x for %I64x\n", Got, Id);
            NtClose(H);
        }
        CloseHandle(F);
    }
    NtClose(Dir);
    NtCleanup();
}

/* Deleting one hard link keeps the data written through it for the other link. */
START_TEST(NtfsHardLink)
{
    WCHAR A[MAX_PATH], B[MAX_PATH];
    static CHAR Data[65536];
    static CHAR Back[65536];
    HANDLE H;
    DWORD Done;
    ULONG i;

    if (!NtInit("NtfsHardLink"))
        return;
    NtPath(A, L"a.bin");
    NtPath(B, L"b.bin");
    for (i = 0; i < sizeof(Data); i++)
        Data[i] = (CHAR)(i * 7 + 1);
    H = CreateFileW(A, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(H != INVALID_HANDLE_VALUE, "CreateFile failed %lu\n", GetLastError());
    if (H == INVALID_HANDLE_VALUE)
        return;
    ok(WriteFile(H, Data, sizeof(Data), &Done, NULL) && Done == sizeof(Data), "WriteFile failed %lu\n", GetLastError());
    ok(CreateHardLinkW(B, A, NULL), "CreateHardLink failed %lu\n", GetLastError());
    CloseHandle(H);
    /* The data is still in the cache, not yet written back. */
    ok(DeleteFileW(A), "DeleteFile failed %lu\n", GetLastError());
    H = CreateFileW(B, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    ok(H != INVALID_HANDLE_VALUE, "CreateFile of the other link failed %lu\n", GetLastError());
    if (H != INVALID_HANDLE_VALUE)
    {
        ok(ReadFile(H, Back, sizeof(Back), &Done, NULL) && Done == sizeof(Back), "ReadFile failed %lu (%lu)\n", GetLastError(), Done);
        ok(!memcmp(Data, Back, sizeof(Data)), "The other link lost its data\n");
        CloseHandle(H);
    }
    NtCleanup();
}

/* A running program's file cannot be deleted, also not by delete-on-close. */
START_TEST(NtfsDeleteRunning)
{
    WCHAR Exe[MAX_PATH], Copy[MAX_PATH];
    STARTUPINFOW Si = { sizeof(Si) };
    PROCESS_INFORMATION Pi;
    NTSTATUS Status;

    if (!NtInit("NtfsDeleteRunning"))
        return;
    GetModuleFileNameW(NULL, Exe, MAX_PATH);
    NtPath(Copy, L"running.exe");
    ok(CopyFileW(Exe, Copy, FALSE), "CopyFile failed %lu\n", GetLastError());
    if (!CreateProcessW(Copy, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &Si, &Pi))
    {
        skip("CreateProcess failed %lu\n", GetLastError());
        NtCleanup();
        return;
    }
    ok(!DeleteFileW(Copy), "DeleteFile of a running program succeeded\n");
    Status = NtOpen(Copy, DELETE, SHARE_ALL, FILE_OPEN, FILE_DELETE_ON_CLOSE | FILE_NON_DIRECTORY_FILE, NULL, NULL);
    trace("Delete-on-close of a running program: 0x%08lx\n", Status);
    ok(GetFileAttributesW(Copy) != INVALID_FILE_ATTRIBUTES, "The running program's file is gone\n");
    TerminateProcess(Pi.hProcess, 0);
    WaitForSingleObject(Pi.hProcess, 10000);
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);
    ok(DeleteFileW(Copy), "DeleteFile after exit failed %lu\n", GetLastError());
    NtCleanup();
}

/* A directory with open files below it is not renamed; other directories are, however many files are open. */
START_TEST(NtfsDirRename)
{
    WCHAR Many[MAX_PATH], Other[MAX_PATH], Other2[MAX_PATH], Busy[MAX_PATH], Busy2[MAX_PATH], Path[MAX_PATH];
    HANDLE Handles[100], Held;
    ULONG i, Open = 0;

    if (!NtInit("NtfsDirRename"))
        return;
    NtPath(Many, L"many");
    NtPath(Other, L"other");
    NtPath(Other2, L"other2");
    NtPath(Busy, L"busy");
    NtPath(Busy2, L"busy2");
    ok_hex(NtMakeDir(Many, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(NtMakeDir(Other, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(NtMakeDir(Busy, NULL, 0, FALSE), STATUS_SUCCESS);
    for (i = 0; i < RTL_NUMBER_OF(Handles); i++)
    {
        _snwprintf(Path, MAX_PATH - 1, L"%ls\\f%03lu.txt", Many, i);
        Handles[i] = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
        if (Handles[i] != INVALID_HANDLE_VALUE)
            Open++;
    }
    ok(Open == RTL_NUMBER_OF(Handles), "%lu files open\n", Open);
    ok(MoveFileW(Other, Other2), "Renaming an unrelated directory failed %lu\n", GetLastError());
    _snwprintf(Path, MAX_PATH - 1, L"%ls\\held.txt", Busy);
    Held = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
    ok(Held != INVALID_HANDLE_VALUE, "CreateFile failed %lu\n", GetLastError());
    ok(!MoveFileW(Busy, Busy2), "Renaming a directory with an open file succeeded\n");
    ok_err(ERROR_ACCESS_DENIED);
    if (Held != INVALID_HANDLE_VALUE)
        CloseHandle(Held);
    ok(MoveFileW(Busy, Busy2), "Renaming after the close failed %lu\n", GetLastError());
    for (i = 0; i < RTL_NUMBER_OF(Handles); i++)
        if (Handles[i] != INVALID_HANDLE_VALUE)
            CloseHandle(Handles[i]);
    NtCleanup();
}
