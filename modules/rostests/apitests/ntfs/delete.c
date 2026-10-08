/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS: the rights that delete, rename and replace need
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

/* Deletes Path the way DeleteFile does (open for DELETE, set the disposition), as the current thread. */
static NTSTATUS DeleteByDisposition(PCWSTR Path)
{
    FILE_DISPOSITION_INFORMATION Disp = { TRUE };
    IO_STATUS_BLOCK Iosb;
    HANDLE H;
    NTSTATUS Status = NtOpen(Path, DELETE, SHARE_ALL, FILE_OPEN, FILE_OPEN_REPARSE_POINT, NULL, &H);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtSetInformationFile(H, &Iosb, &Disp, sizeof(Disp), FileDispositionInformation);
    NtClose(H);
    return Status;
}

static NTSTATUS Rename(PCWSTR From, PCWSTR To, BOOLEAN Replace, ACCESS_MASK Access)
{
    UCHAR Buffer[sizeof(FILE_RENAME_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    PFILE_RENAME_INFORMATION R = (PFILE_RENAME_INFORMATION)Buffer;
    UNICODE_STRING Target;
    IO_STATUS_BLOCK Iosb;
    HANDLE H;
    NTSTATUS Status;

    if (!RtlDosPathNameToNtPathName_U(To, &Target, NULL, NULL))
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    Status = NtOpen(From, Access, SHARE_ALL, FILE_OPEN, 0, NULL, &H);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Buffer, sizeof(Buffer));
        R->ReplaceIfExists = Replace;
        R->RootDirectory = NULL;
        R->FileNameLength = Target.Length;
        RtlCopyMemory(R->FileName, Target.Buffer, Target.Length);
        Status = NtSetInformationFile(H, &Iosb, R, sizeof(Buffer), FileRenameInformation);
        NtClose(H);
    }
    RtlFreeUnicodeString(&Target);
    return Status;
}

START_TEST(NtfsDelete)
{
    WCHAR DcDir[MAX_PATH], NoDcDir[MAX_PATH], Path[MAX_PATH];
    NT_ACE Aces[2];
    NTSTATUS Status;

    if (!NtInit("NtfsDelete") || !NtHaveUser())
        return;
    NtPath(DcDir, L"deletechild");
    NtPath(NoDcDir, L"nodeletechild");
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE; Aces[0].Deny = FALSE;
    Aces[1].Sid = SidUser; Aces[1].Mask = FILE_TRAVERSE | FILE_LIST_DIRECTORY | FILE_DELETE_CHILD | SYNCHRONIZE; Aces[1].Flags = 0; Aces[1].Deny = FALSE;
    ok_hex(NtMakeDir(DcDir, Aces, 2, TRUE), STATUS_SUCCESS);
    Aces[1].Mask = FILE_TRAVERSE | FILE_LIST_DIRECTORY | SYNCHRONIZE;
    ok_hex(NtMakeDir(NoDcDir, Aces, 2, TRUE), STATUS_SUCCESS);

    NtPath(Path, L"deletechild\\a.txt");
    ok_hex(NtMakeFile(Path, Aces, 1, TRUE, "a"), STATUS_SUCCESS);
    NtPath(Path, L"deletechild\\b.txt");
    ok_hex(NtMakeFile(Path, Aces, 1, TRUE, "b"), STATUS_SUCCESS);
    NtPath(Path, L"nodeletechild\\own.txt");
    Aces[1].Mask = DELETE | SYNCHRONIZE;
    ok_hex(NtMakeFile(Path, Aces, 2, TRUE, "own"), STATUS_SUCCESS);
    NtPath(Path, L"nodeletechild\\keep.txt");
    Aces[1].Mask = FILE_GENERIC_READ;
    ok_hex(NtMakeFile(Path, Aces, 2, TRUE, "keep"), STATUS_SUCCESS);
    NtPath(Path, L"deletechild\\deny.txt");
    Aces[1].Mask = DELETE; Aces[1].Deny = TRUE;
    ok_hex(NtMakeFile(Path, Aces, 2, TRUE, "deny"), STATUS_SUCCESS);
    Aces[1].Deny = FALSE;

    NtBeginUser();
    /* FILE_DELETE_CHILD on the directory: any file in it, whatever its own DACL says. */
    NtPath(Path, L"deletechild\\a.txt");
    ok_hex(DeleteByDisposition(Path), STATUS_SUCCESS);
    NtPath(Path, L"deletechild\\b.txt");
    ok_hex(NtOpen(Path, DELETE, SHARE_ALL, FILE_OPEN, FILE_DELETE_ON_CLOSE, NULL, NULL), STATUS_SUCCESS);
    /* DELETE on the file itself. */
    NtPath(Path, L"nodeletechild\\own.txt");
    ok_hex(DeleteByDisposition(Path), STATUS_SUCCESS);
    /* Neither. */
    NtPath(Path, L"nodeletechild\\keep.txt");
    ok_hex(DeleteByDisposition(Path), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(Path, DELETE, SHARE_ALL, FILE_OPEN, FILE_DELETE_ON_CLOSE, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(Path, FILE_READ_DATA, SHARE_ALL, FILE_OPEN, FILE_DELETE_ON_CLOSE, NULL, NULL), STATUS_INVALID_PARAMETER);
    NtEndUser();

    NtPath(Path, L"deletechild\\a.txt");
    ok_hex(GetFileAttributesW(Path), INVALID_FILE_ATTRIBUTES);
    NtPath(Path, L"deletechild\\b.txt");
    ok_hex(GetFileAttributesW(Path), INVALID_FILE_ATTRIBUTES);
    NtPath(Path, L"nodeletechild\\own.txt");
    ok_hex(GetFileAttributesW(Path), INVALID_FILE_ATTRIBUTES);
    NtPath(Path, L"nodeletechild\\keep.txt");
    ok(GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES, "keep.txt was deleted\n");

    /* FILE_DELETE_CHILD on the directory also overrides a DELETE the file's DACL denies. */
    NtBeginUser();
    NtPath(Path, L"deletechild\\deny.txt");
    Status = DeleteByDisposition(Path);
    ok_hex(Status, STATUS_SUCCESS);
    NtEndUser();
    NtCleanup();
}

START_TEST(NtfsRename)
{
    WCHAR Src[MAX_PATH], Ro[MAX_PATH], Path[MAX_PATH], To[MAX_PATH];
    NT_ACE Aces[2];

    if (!NtInit("NtfsRename") || !NtHaveUser())
        return;
    NtPath(Src, L"src");
    NtPath(Ro, L"ro");
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE; Aces[0].Deny = FALSE;
    Aces[1].Sid = SidUser; Aces[1].Mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD;
    Aces[1].Flags = 0; Aces[1].Deny = FALSE;
    ok_hex(NtMakeDir(Src, Aces, 2, TRUE), STATUS_SUCCESS);
    Aces[1].Mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    ok_hex(NtMakeDir(Ro, Aces, 2, TRUE), STATUS_SUCCESS);
    NtPath(Path, L"src\\a.txt");
    ok_hex(NtMakeFile(Path, Aces, 1, TRUE, "a"), STATUS_SUCCESS);
    NtPath(Path, L"src\\locked.txt");
    Aces[1].Mask = FILE_GENERIC_READ;
    ok_hex(NtMakeFile(Path, Aces, 2, TRUE, "locked"), STATUS_SUCCESS);
    NtPath(Path, L"src\\d");
    ok_hex(NtMakeDir(Path, Aces, 1, TRUE), STATUS_SUCCESS);
    NtPath(Path, L"ro\\target.txt");
    ok_hex(NtMakeFile(Path, Aces, 2, TRUE, "target"), STATUS_SUCCESS);

    NtBeginUser();
    /* Within src: DELETE through FILE_DELETE_CHILD, FILE_ADD_FILE on src. */
    NtPath(Path, L"src\\a.txt");
    NtPath(To, L"src\\b.txt");
    ok_hex(Rename(Path, To, FALSE, DELETE), STATUS_SUCCESS);
    /* Into ro: no FILE_ADD_FILE there. */
    NtPath(To, L"ro\\b.txt");
    NtPath(Path, L"src\\b.txt");
    ok_hex(Rename(Path, To, FALSE, DELETE), STATUS_ACCESS_DENIED);
    /* A directory needs FILE_ADD_SUBDIRECTORY on the target directory. */
    NtPath(Path, L"src\\d");
    NtPath(To, L"src\\e");
    ok_hex(Rename(Path, To, FALSE, DELETE), STATUS_SUCCESS);
    NtPath(Path, L"src\\e");
    NtPath(To, L"ro\\e");
    ok_hex(Rename(Path, To, FALSE, DELETE), STATUS_ACCESS_DENIED);
    /* Replacing a file needs the right to delete it. */
    NtPath(Path, L"src\\b.txt");
    NtPath(To, L"src\\locked.txt");
    ok_hex(Rename(Path, To, TRUE, DELETE), STATUS_SUCCESS);   /* FILE_DELETE_CHILD on src */
    NtEndUser();
    NtPath(Path, L"src\\locked.txt");
    ok(GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES, "Renamed file missing\n");

    /* A target directory that grants adding files but not deleting them: no replace of a file the user may not delete. */
    NtPath(Path, L"repl");
    Aces[1].Mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_ADD_FILE;
    ok_hex(NtMakeDir(Path, Aces, 2, TRUE), STATUS_SUCCESS);
    NtPath(Path, L"repl\\victim.txt");
    Aces[1].Mask = FILE_GENERIC_READ;
    ok_hex(NtMakeFile(Path, Aces, 2, TRUE, "victim"), STATUS_SUCCESS);
    NtBeginUser();
    NtPath(Path, L"src\\locked.txt");
    NtPath(To, L"repl\\victim.txt");
    ok_hex(Rename(Path, To, TRUE, DELETE), STATUS_ACCESS_DENIED);
    NtEndUser();
    NtPath(Path, L"repl\\victim.txt");
    ok(GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES, "The replace target was deleted\n");
    NtCleanup();
}
