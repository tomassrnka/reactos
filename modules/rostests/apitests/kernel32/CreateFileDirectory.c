/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for CreateFile directory creation (POSIX semantics) and junctions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Windows' CreateFile creates a *new directory* (not a file) when the caller
 * passes CREATE_NEW together with FILE_ATTRIBUTE_DIRECTORY and
 * FILE_FLAG_POSIX_SEMANTICS; kernelbase maps that combination to the native
 * FILE_DIRECTORY_FILE create option (see get_nt_file_options). Without
 * FILE_FLAG_POSIX_SEMANTICS the same call creates a regular file. WinFsp's
 * installer "InstallJunctions" custom action relies on this exact behaviour to
 * create its junction node before stamping an IO_REPARSE_TAG_MOUNT_POINT. These
 * tests pin that contract (and the junction round-trip on reparse-capable
 * volumes).
 */

#include "precomp.h"
#include <winioctl.h>

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif

#define REPARSE_DATA_BUFFER_HEADER_SIZE 8

typedef struct
{
    DWORD ReparseTag;
    WORD  ReparseDataLength;
    WORD  Reserved;
    WORD  SubstituteNameOffset;
    WORD  SubstituteNameLength;
    WORD  PrintNameOffset;
    WORD  PrintNameLength;
    WCHAR PathBuffer[1];
} TEST_REPARSE_MOUNTPOINT;

static WCHAR TempDir[MAX_PATH];
static WCHAR TargetDir[MAX_PATH];

static void BuildPath(WCHAR *Out, PCWSTR Leaf)
{
    wcscpy(Out, TempDir);
    wcscat(Out, Leaf);
}

static void Cleanup(PCWSTR Path)
{
    SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(Path);
    DeleteFileW(Path);
}

/* Core contract: POSIX + DIRECTORY + CREATE_NEW creates a directory. */
static void Test_PosixDirectoryCreate(void)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    DWORD Attr;

    BuildPath(Path, L"cfd_posixdir");
    Cleanup(Path);

    Handle = CreateFileW(Path,
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_DIRECTORY | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_POSIX_SEMANTICS,
        NULL);
    ok(Handle != INVALID_HANDLE_VALUE,
       "CreateFileW(POSIX|DIRECTORY, CREATE_NEW) failed with %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);

    Attr = GetFileAttributesW(Path);
    ok(Attr != INVALID_FILE_ATTRIBUTES, "GetFileAttributesW failed with %lu\n", GetLastError());
    ok(Attr != INVALID_FILE_ATTRIBUTES && (Attr & FILE_ATTRIBUTE_DIRECTORY),
       "expected a directory, attributes = 0x%lx\n", Attr);

    Cleanup(Path);
}

/* Gate precision: without POSIX the same call creates a regular file. */
static void Test_NoPosixCreatesFile(void)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    DWORD Attr;

    BuildPath(Path, L"cfd_nofile");
    Cleanup(Path);

    Handle = CreateFileW(Path,
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_DIRECTORY | FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    ok(Handle != INVALID_HANDLE_VALUE,
       "CreateFileW(DIRECTORY, no POSIX, CREATE_NEW) failed with %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);

    Attr = GetFileAttributesW(Path);
    ok(Attr != INVALID_FILE_ATTRIBUTES, "GetFileAttributesW failed with %lu\n", GetLastError());
    ok(Attr != INVALID_FILE_ATTRIBUTES && !(Attr & FILE_ATTRIBUTE_DIRECTORY),
       "without POSIX this must be a file, attributes = 0x%lx\n", Attr);

    Cleanup(Path);
}

/* CREATE_NEW must still collide with an existing name. */
static void Test_CreateNewCollision(void)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;

    BuildPath(Path, L"cfd_collide");
    Cleanup(Path);

    ok(CreateDirectoryW(Path, NULL), "CreateDirectoryW failed with %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    Handle = CreateFileW(Path,
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_DIRECTORY | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_POSIX_SEMANTICS,
        NULL);
    ok(Handle == INVALID_HANDLE_VALUE, "CREATE_NEW over an existing name must fail\n");
    if (Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);

    Cleanup(Path);
}

/* Full WinFsp-style junction round-trip; skipped on non-reparse volumes. */
static void Test_JunctionRoundTrip(void)
{
    WCHAR Junction[MAX_PATH];
    WCHAR Substitute[MAX_PATH];
    HANDLE Handle;
    BYTE Buffer[sizeof(TEST_REPARSE_MOUNTPOINT) + 2 * MAX_PATH * sizeof(WCHAR)];
    TEST_REPARSE_MOUNTPOINT *Reparse = (TEST_REPARSE_MOUNTPOINT *)Buffer;
    size_t SubLen, PrnLen;
    DWORD Bytes = 0, Attr, Err;
    BOOL Ok;

    BuildPath(Junction, L"cfd_junction");
    Cleanup(Junction);

    /* target directory (reuse a known-good directory) */
    ok(CreateDirectoryW(TargetDir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS,
       "CreateDirectoryW(target) failed with %lu\n", GetLastError());

    Handle = CreateFileW(Junction,
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_DIRECTORY | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_POSIX_SEMANTICS,
        NULL);
    ok(Handle != INVALID_HANDLE_VALUE,
       "CreateFileW(junction node) failed with %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return;

    wcscpy(Substitute, L"\\??\\");
    wcscat(Substitute, TargetDir);
    SubLen = wcslen(Substitute) * sizeof(WCHAR);
    PrnLen = wcslen(TargetDir) * sizeof(WCHAR);

    ZeroMemory(Buffer, sizeof(Buffer));
    Reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    Reparse->SubstituteNameOffset = 0;
    Reparse->SubstituteNameLength = (WORD)SubLen;
    Reparse->PrintNameOffset = (WORD)(SubLen + sizeof(WCHAR));
    Reparse->PrintNameLength = (WORD)PrnLen;
    memcpy(Reparse->PathBuffer, Substitute, SubLen + sizeof(WCHAR));
    memcpy((BYTE *)Reparse->PathBuffer + Reparse->PrintNameOffset, TargetDir, PrnLen + sizeof(WCHAR));
    Reparse->ReparseDataLength = (WORD)(8 + Reparse->PrintNameOffset + PrnLen + sizeof(WCHAR));

    SetLastError(0xdeadbeef);
    Ok = DeviceIoControl(Handle, FSCTL_SET_REPARSE_POINT,
        Reparse, REPARSE_DATA_BUFFER_HEADER_SIZE + Reparse->ReparseDataLength,
        NULL, 0, &Bytes, NULL);
    Err = GetLastError();
    CloseHandle(Handle);

    if (!Ok && (Err == ERROR_NOT_SUPPORTED || Err == ERROR_INVALID_FUNCTION))
    {
        skip("Volume does not support reparse points (err %lu); skipping junction round-trip\n", Err);
        Cleanup(Junction);
        return;
    }

    ok(Ok, "FSCTL_SET_REPARSE_POINT failed with %lu\n", Err);

    Attr = GetFileAttributesW(Junction);
    ok(Attr != INVALID_FILE_ATTRIBUTES &&
       (Attr & FILE_ATTRIBUTE_DIRECTORY) &&
       (Attr & FILE_ATTRIBUTE_REPARSE_POINT),
       "junction must be a reparse-point directory, attributes = 0x%lx\n", Attr);

    Cleanup(Junction);
}

START_TEST(CreateFileDirectory)
{
    DWORD Len = GetTempPathW(ARRAYSIZE(TempDir), TempDir);
    ok(Len != 0 && Len < ARRAYSIZE(TempDir), "GetTempPathW failed with %lu\n", GetLastError());

    BuildPath(TargetDir, L"cfd_target");

    Test_PosixDirectoryCreate();
    Test_NoPosixCreatesFile();
    Test_CreateNewCollision();
    Test_JunctionRoundTrip();

    Cleanup(TargetDir);
}
