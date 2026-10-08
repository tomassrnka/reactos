/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS access checks: open, create in a directory, traverse, backup and restore
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

static NTSTATUS OpenAs(PCWSTR Path, ACCESS_MASK Access, ULONG Disposition, ULONG Options, PACCESS_MASK Granted)
{
    HANDLE H;
    NTSTATUS Status = NtOpen(Path, Access, SHARE_ALL, Disposition, Options, NULL, &H);
    if (Granted)
        *Granted = 0;
    if (NT_SUCCESS(Status))
    {
        if (Granted)
            *Granted = NtGrantedAccess(H);
        NtClose(H);
    }
    return Status;
}

START_TEST(NtfsAccess)
{
    WCHAR ReadFile[MAX_PATH], NoneFile[MAX_PATH], EmptyFile[MAX_PATH], WriteFile[MAX_PATH];
    NT_ACE Aces[3];
    ACCESS_MASK Granted;
    NTSTATUS Status;

    if (!NtInit("NtfsAccess") || !NtHaveUser())
        return;
    NtPath(ReadFile, L"read.txt");
    NtPath(NoneFile, L"none.txt");
    NtPath(EmptyFile, L"empty.txt");
    NtPath(WriteFile, L"write.txt");
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = 0; Aces[0].Deny = FALSE;
    Aces[1].Sid = SidUser; Aces[1].Mask = FILE_GENERIC_READ; Aces[1].Flags = 0; Aces[1].Deny = FALSE;
    ok_hex(NtMakeFile(ReadFile, Aces, 2, TRUE, "read"), STATUS_SUCCESS);
    ok_hex(NtMakeFile(NoneFile, Aces, 1, TRUE, "none"), STATUS_SUCCESS);
    ok_hex(NtMakeFile(EmptyFile, Aces, 0, TRUE, NULL), STATUS_SUCCESS);
    Aces[1].Mask = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
    ok_hex(NtMakeFile(WriteFile, Aces, 2, TRUE, "write"), STATUS_SUCCESS);
    {
        /* empty.txt: a DACL without ACEs */
        PSECURITY_DESCRIPTOR Sd = NtMakeSd(NULL, 0, TRUE);
        HANDLE H;
        Status = NtOpen(EmptyFile, WRITE_DAC, SHARE_ALL, FILE_OPEN, 0, NULL, &H);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_hex(NtSetSecurityObject(H, DACL_SECURITY_INFORMATION, Sd), STATUS_SUCCESS);
            NtClose(H);
        }
        LocalFree(Sd);
    }

    /* The administrator: full access where the DACL grants it, nothing (but owner rights) where it does not. */
    ok_hex(OpenAs(ReadFile, GENERIC_ALL, FILE_OPEN, 0, NULL), STATUS_SUCCESS);
    ok_hex(OpenAs(EmptyFile, FILE_READ_DATA, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    Status = OpenAs(EmptyFile, READ_CONTROL | WRITE_DAC, FILE_OPEN, 0, &Granted);
    trace("admin, empty DACL, owner rights: 0x%08lx granted 0x%08lx\n", Status, Granted);
    ok_hex(OpenAs(EmptyFile, FILE_READ_DATA, FILE_OPEN, FILE_OPEN_FOR_BACKUP_INTENT, NULL), STATUS_SUCCESS);

    NtBeginUser();
    ok_hex(OpenAs(ReadFile, GENERIC_READ, FILE_OPEN, 0, NULL), STATUS_SUCCESS);
    ok_hex(OpenAs(ReadFile, FILE_READ_DATA, FILE_OPEN, 0, NULL), STATUS_SUCCESS);
    ok_hex(OpenAs(ReadFile, READ_CONTROL, FILE_OPEN, 0, NULL), STATUS_SUCCESS);
    ok_hex(OpenAs(ReadFile, GENERIC_WRITE, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, FILE_WRITE_DATA, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, FILE_APPEND_DATA, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, FILE_WRITE_ATTRIBUTES, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, WRITE_DAC, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, WRITE_OWNER, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, DELETE, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, GENERIC_ALL, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    Status = OpenAs(ReadFile, MAXIMUM_ALLOWED, FILE_OPEN, 0, &Granted);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Granted, FILE_GENERIC_READ);
    /* Overwrite and supersede need write access (and supersede DELETE) even when not asked for. */
    ok_hex(OpenAs(ReadFile, FILE_READ_DATA, FILE_OVERWRITE, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, FILE_READ_DATA, FILE_OVERWRITE_IF, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(ReadFile, FILE_READ_DATA, FILE_SUPERSEDE, 0, NULL), STATUS_ACCESS_DENIED);
    Status = OpenAs(WriteFile, FILE_READ_DATA, FILE_SUPERSEDE, 0, NULL);
    trace("user, supersede with write but no DELETE: 0x%08lx\n", Status);
    ok_hex(OpenAs(WriteFile, FILE_READ_DATA, FILE_OVERWRITE, 0, &Granted), STATUS_SUCCESS);
    ok_hex(Granted, FILE_READ_DATA);

    ok_hex(OpenAs(NoneFile, FILE_READ_DATA, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(NoneFile, READ_CONTROL, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    /* FILE_LIST_DIRECTORY on the directory grants FILE_READ_ATTRIBUTES on its files. */
    Status = OpenAs(NoneFile, FILE_READ_ATTRIBUTES, FILE_OPEN, 0, &Granted);
    ok_hex(Status, STATUS_SUCCESS);
    Status = OpenAs(NoneFile, MAXIMUM_ALLOWED, FILE_OPEN, 0, &Granted);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Granted, FILE_READ_ATTRIBUTES);
    ok_hex(OpenAs(EmptyFile, FILE_READ_DATA, FILE_OPEN, 0, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(EmptyFile, FILE_READ_DATA, FILE_OPEN, FILE_OPEN_FOR_BACKUP_INTENT, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(WriteFile, GENERIC_READ | GENERIC_WRITE, FILE_OPEN, 0, NULL), STATUS_SUCCESS);
    NtEndUser();

    /* The directory itself: the user may list and traverse it, not add to it. */
    NtBeginUser();
    ok_hex(OpenAs(NtTestDir(), FILE_LIST_DIRECTORY | FILE_TRAVERSE, FILE_OPEN, FILE_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok_hex(OpenAs(NtTestDir(), FILE_ADD_FILE, FILE_OPEN, FILE_DIRECTORY_FILE, NULL), STATUS_ACCESS_DENIED);
    ok_hex(OpenAs(NtTestDir(), FILE_DELETE_CHILD, FILE_OPEN, FILE_DIRECTORY_FILE, NULL), STATUS_ACCESS_DENIED);
    NtEndUser();
    NtCleanup();
}

START_TEST(NtfsCreate)
{
    WCHAR RoDir[MAX_PATH], AddFileDir[MAX_PATH], AddDirDir[MAX_PATH], Path[MAX_PATH];
    NT_ACE Aces[2];
    ACCESS_MASK Granted;
    HANDLE H;
    NTSTATUS Status;

    if (!NtInit("NtfsCreate") || !NtHaveUser())
        return;
    NtPath(RoDir, L"ro");
    NtPath(AddFileDir, L"addfile");
    NtPath(AddDirDir, L"adddir");
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE; Aces[0].Deny = FALSE;
    Aces[1].Sid = SidUser; Aces[1].Mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    Aces[1].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE; Aces[1].Deny = FALSE;
    ok_hex(NtMakeDir(RoDir, Aces, 2, TRUE), STATUS_SUCCESS);
    Aces[1].Mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_ADD_FILE;
    Aces[1].Flags = 0;
    ok_hex(NtMakeDir(AddFileDir, Aces, 2, TRUE), STATUS_SUCCESS);
    Aces[1].Mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | FILE_ADD_SUBDIRECTORY;
    ok_hex(NtMakeDir(AddDirDir, Aces, 2, TRUE), STATUS_SUCCESS);
    NtPath(Path, L"ro\\existing.txt");
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "existing"), STATUS_SUCCESS);

    NtBeginUser();
    NtPath(Path, L"ro\\new.txt");
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_SUPERSEDE, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(GetFileAttributesW(Path), INVALID_FILE_ATTRIBUTES);
    NtPath(Path, L"ro\\newdir");
    ok_hex(NtOpen(Path, FILE_LIST_DIRECTORY, 0, FILE_CREATE, FILE_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    /* A new named stream needs write access to its file. */
    NtPath(Path, L"ro\\existing.txt:stream");
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    NtPath(Path, L"ro\\existing.txt");
    ok_hex(NtOpen(Path, FILE_GENERIC_READ, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SUCCESS);

    NtPath(Path, L"addfile\\new.txt");
    Status = NtOpen(Path, FILE_GENERIC_WRITE | FILE_GENERIC_READ, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, &H);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_hex(NtGrantedAccess(H), FILE_GENERIC_WRITE | FILE_GENERIC_READ);
        NtClose(H);
    }
    NtPath(Path, L"addfile\\newdir");
    ok_hex(NtOpen(Path, FILE_LIST_DIRECTORY, 0, FILE_CREATE, FILE_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    NtPath(Path, L"adddir\\newdir");
    ok_hex(NtOpen(Path, FILE_LIST_DIRECTORY, 0, FILE_CREATE, FILE_DIRECTORY_FILE, NULL, NULL), STATUS_SUCCESS);
    NtPath(Path, L"adddir\\new.txt");
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);

    /* The creator gets the access it asked for, even if the new descriptor grants nothing. */
    {
        PSECURITY_DESCRIPTOR Sd = NtMakeSd(NULL, 0, TRUE);
        IO_STATUS_BLOCK Iosb;
        NtPath(Path, L"addfile\\denyall.txt");
        Status = NtOpen(Path, FILE_GENERIC_WRITE | FILE_GENERIC_READ, 0, FILE_CREATE,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, Sd, &H);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_hex(NtWriteFile(H, NULL, NULL, NULL, &Iosb, "x", 1, NULL, NULL), STATUS_SUCCESS);
            NtClose(H);
        }
        LocalFree(Sd);
        ok_hex(NtOpen(Path, FILE_READ_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_ACCESS_DENIED);
        Status = NtOpen(Path, READ_CONTROL | WRITE_DAC, SHARE_ALL, FILE_OPEN, 0, NULL, &H);
        ok_hex(Status, STATUS_SUCCESS);   /* the owner's implicit rights */
        if (NT_SUCCESS(Status))
        {
            Granted = NtGrantedAccess(H);
            ok_hex(Granted, READ_CONTROL | WRITE_DAC);
            NtClose(H);
        }
    }
    /* MAXIMUM_ALLOWED on a new file: everything. */
    NtPath(Path, L"addfile\\max.txt");
    Status = NtOpen(Path, MAXIMUM_ALLOWED, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, &H);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_hex(NtGrantedAccess(H), FILE_ALL_ACCESS);
        NtClose(H);
    }
    NtEndUser();

    /* The administrator creates in all of them. */
    NtPath(Path, L"ro\\admin.txt");
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_SUCCESS);
    NtCleanup();
}

START_TEST(NtfsTraverse)
{
    WCHAR Path[MAX_PATH];
    NT_ACE Aces[2];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    HANDLE Dir, H, Thread;
    NTSTATUS Status;

    if (!NtInit("NtfsTraverse") || !NtHaveUser())
        return;
    NtPath(Path, L"closed");
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = 0; Aces[0].Deny = FALSE;
    ok_hex(NtMakeDir(Path, Aces, 1, TRUE), STATUS_SUCCESS);
    NtPath(Path, L"closed\\open.txt");
    Aces[1].Sid = SidUser; Aces[1].Mask = FILE_GENERIC_READ; Aces[1].Flags = 0; Aces[1].Deny = FALSE;
    ok_hex(NtMakeFile(Path, Aces, 2, TRUE, "open"), STATUS_SUCCESS);

    /* Opened relative to the test directory: only "closed" is traversed. */
    ok_hex(NtOpen(NtTestDir(), FILE_TRAVERSE | SYNCHRONIZE, SHARE_ALL, FILE_OPEN, FILE_DIRECTORY_FILE, NULL, &Dir), STATUS_SUCCESS);
    RtlInitUnicodeString(&Name, L"closed\\open.txt");
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, Dir, NULL);
    NtBeginUser();
    Status = NtCreateFile(&H, FILE_READ_DATA, &Oa, &Iosb, NULL, 0, SHARE_ALL, FILE_OPEN, 0, NULL, 0);
    ok_hex(Status, STATUS_SUCCESS);     /* SeChangeNotifyPrivilege bypasses traverse checking */
    if (NT_SUCCESS(Status))
        NtClose(H);
    if (OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &Thread))
    {
        ok(NtSetPrivilege(Thread, L"SeChangeNotifyPrivilege", FALSE), "Disabling SeChangeNotifyPrivilege failed\n");
        Status = NtCreateFile(&H, FILE_READ_DATA, &Oa, &Iosb, NULL, 0, SHARE_ALL, FILE_OPEN, 0, NULL, 0);
        ok_hex(Status, STATUS_ACCESS_DENIED);
        if (NT_SUCCESS(Status))
            NtClose(H);
        /* The directory itself is opened without traversing it. */
        RtlInitUnicodeString(&Name, L"closed");
        Status = NtCreateFile(&H, FILE_READ_ATTRIBUTES, &Oa, &Iosb, NULL, 0, SHARE_ALL, FILE_OPEN, 0, NULL, 0);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            NtClose(H);
        NtSetPrivilege(Thread, L"SeChangeNotifyPrivilege", TRUE);
        CloseHandle(Thread);
    }
    else
    {
        skip("OpenThreadToken failed %lu\n", GetLastError());
    }
    NtEndUser();
    NtClose(Dir);
    NtCleanup();
}

START_TEST(NtfsBackup)
{
    WCHAR File[MAX_PATH], Dir[MAX_PATH], Path[MAX_PATH];
    NT_ACE Aces[1];
    HANDLE Process;
    NTSTATUS Status;

    if (!NtInit("NtfsBackup"))
        return;
    NtPath(File, L"nobody.txt");
    NtPath(Dir, L"nobody");
    Aces[0].Sid = SidSystem; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = 0; Aces[0].Deny = FALSE;
    ok_hex(NtMakeFile(File, Aces, 1, TRUE, "nobody"), STATUS_SUCCESS);
    ok_hex(NtMakeDir(Dir, Aces, 1, TRUE), STATUS_SUCCESS);
    /* The test enabled both privileges in its process token. */
    ok_hex(NtOpen(File, FILE_READ_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(File, FILE_READ_DATA, SHARE_ALL, FILE_OPEN, FILE_OPEN_FOR_BACKUP_INTENT, NULL, NULL), STATUS_SUCCESS);
    ok_hex(NtOpen(File, FILE_WRITE_DATA, SHARE_ALL, FILE_OPEN, FILE_OPEN_FOR_BACKUP_INTENT, NULL, NULL), STATUS_SUCCESS);
    NtPath(Path, L"nobody\\restored.txt");
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(Path, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT, NULL, NULL),
           STATUS_SUCCESS);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Process))
    {
        NtSetPrivilege(Process, L"SeBackupPrivilege", FALSE);
        NtSetPrivilege(Process, L"SeRestorePrivilege", FALSE);
        Status = NtOpen(File, FILE_READ_DATA, SHARE_ALL, FILE_OPEN, FILE_OPEN_FOR_BACKUP_INTENT, NULL, NULL);
        ok_hex(Status, STATUS_ACCESS_DENIED);
        NtSetPrivilege(Process, L"SeBackupPrivilege", TRUE);
        NtSetPrivilege(Process, L"SeRestorePrivilege", TRUE);
        CloseHandle(Process);
    }
    NtCleanup();
}
