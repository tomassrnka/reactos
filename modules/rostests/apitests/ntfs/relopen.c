/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS: opens relative to a directory handle after the directory was renamed
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#ifndef REPARSE_DATA_BUFFER_HEADER_SIZE
#define REPARSE_DATA_BUFFER_HEADER_SIZE FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)
#endif

/* NtCreateFile of Name relative to the directory handle Root. */
static NTSTATUS RelOpen(HANDLE Root, PCWSTR Name, ACCESS_MASK Access, ULONG Disposition, ULONG Options, PHANDLE Handle)
{
    UNICODE_STRING Str;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    HANDLE H = NULL;
    NTSTATUS Status;

    if (Handle)
        *Handle = NULL;
    RtlInitUnicodeString(&Str, Name);
    InitializeObjectAttributes(&Oa, &Str, OBJ_CASE_INSENSITIVE, Root, NULL);
    Status = NtCreateFile(&H, Access | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, SHARE_ALL, Disposition,
                          Options | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (NT_SUCCESS(Status))
    {
        if (Handle)
            *Handle = H;
        else
            NtClose(H);
    }
    return Status;
}

/* Renames the file or directory of Handle to the Win32 path To. */
static NTSTATUS RenameHandle(HANDLE Handle, PCWSTR To)
{
    UCHAR Buffer[sizeof(FILE_RENAME_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    PFILE_RENAME_INFORMATION R = (PFILE_RENAME_INFORMATION)Buffer;
    UNICODE_STRING Target;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    if (!RtlDosPathNameToNtPathName_U(To, &Target, NULL, NULL))
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    RtlZeroMemory(Buffer, sizeof(Buffer));
    R->ReplaceIfExists = FALSE;
    R->RootDirectory = NULL;
    R->FileNameLength = Target.Length;
    RtlCopyMemory(R->FileName, Target.Buffer, Target.Length);
    Status = NtSetInformationFile(Handle, &Iosb, R, sizeof(Buffer), FileRenameInformation);
    RtlFreeUnicodeString(&Target);
    return Status;
}

/* Makes the new empty directory Path a junction to the directory Target (a Win32 path). */
static NTSTATUS MakeJunctionDisposition(PCWSTR Path, PCWSTR Target, ULONG Disposition);

static NTSTATUS MakeJunction(PCWSTR Path, PCWSTR Target)
{
    return MakeJunctionDisposition(Path, Target, FILE_CREATE);
}

/* Makes Path (a new directory for FILE_CREATE, an existing empty one for FILE_OPEN) a junction to Target. */
static NTSTATUS MakeJunctionDisposition(PCWSTR Path, PCWSTR Target, ULONG Disposition)
{
    ULONGLONG Buffer[(REPARSE_DATA_BUFFER_HEADER_SIZE + 8 + 4 * MAX_PATH * sizeof(WCHAR)) / sizeof(ULONGLONG)];
    PREPARSE_DATA_BUFFER Rp = (PREPARSE_DATA_BUFFER)Buffer;
    WCHAR Sub[MAX_PATH + 8];
    USHORT SubLen, PrintLen;
    IO_STATUS_BLOCK Iosb;
    HANDLE H;
    NTSTATUS Status;

    Status = NtOpen(Path, FILE_WRITE_ATTRIBUTES | FILE_WRITE_DATA | SYNCHRONIZE, SHARE_ALL, Disposition,
                    FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &H);
    if (!NT_SUCCESS(Status))
        return Status;
    _snwprintf(Sub, RTL_NUMBER_OF(Sub) - 1, L"\\??\\%ls", Target);
    Sub[RTL_NUMBER_OF(Sub) - 1] = 0;
    SubLen = (USHORT)(wcslen(Sub) * sizeof(WCHAR));
    PrintLen = (USHORT)(wcslen(Target) * sizeof(WCHAR));
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Rp->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    Rp->MountPointReparseBuffer.SubstituteNameOffset = 0;
    Rp->MountPointReparseBuffer.SubstituteNameLength = SubLen;
    Rp->MountPointReparseBuffer.PrintNameOffset = SubLen + sizeof(WCHAR);
    Rp->MountPointReparseBuffer.PrintNameLength = PrintLen;
    RtlCopyMemory(Rp->MountPointReparseBuffer.PathBuffer, Sub, SubLen);
    RtlCopyMemory((PUCHAR)Rp->MountPointReparseBuffer.PathBuffer + SubLen + sizeof(WCHAR), Target, PrintLen);
    Rp->ReparseDataLength = (USHORT)(FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) -
                                     REPARSE_DATA_BUFFER_HEADER_SIZE + SubLen + PrintLen + 2 * sizeof(WCHAR));
    Status = NtFsControlFile(H, NULL, NULL, NULL, &Iosb, FSCTL_SET_REPARSE_POINT, Rp,
                             REPARSE_DATA_BUFFER_HEADER_SIZE + Rp->ReparseDataLength, NULL, 0);
    NtClose(H);
    return Status;
}

static BOOLEAN Exists(PCWSTR Dir, PCWSTR Name)
{
    WCHAR Path[MAX_PATH];
    _snwprintf(Path, MAX_PATH - 1, L"%ls\\%ls", Dir, Name);
    Path[MAX_PATH - 1] = 0;
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

/* Number of entries of Dir besides "." and "..". */
static ULONG CountEntries(PCWSTR Dir)
{
    WCHAR Pattern[MAX_PATH];
    WIN32_FIND_DATAW Fd;
    HANDLE Find;
    ULONG Count = 0;
    _snwprintf(Pattern, MAX_PATH - 1, L"%ls\\*", Dir);
    Pattern[MAX_PATH - 1] = 0;
    Find = FindFirstFileW(Pattern, &Fd);
    if (Find == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        if (wcscmp(Fd.cFileName, L".") && wcscmp(Fd.cFileName, L".."))
            Count++;
    } while (FindNextFileW(Find, &Fd));
    FindClose(Find);
    return Count;
}

/* A thread that keeps renaming a directory between two names through its handle. */
typedef struct _RENAMER
{
    HANDLE Handle;
    PCWSTR Names[2];
    ULONG Current;              /* index of the name the directory has */
    ULONG Renames;
    volatile LONG Stop;
} RENAMER;

static DWORD WINAPI RenamerThread(PVOID Param)
{
    RENAMER *R = Param;
    while (!R->Stop)
    {
        if (NT_SUCCESS(RenameHandle(R->Handle, R->Names[R->Current ^ 1])))
        {
            R->Current ^= 1;
            R->Renames++;
        }
    }
    return 0;
}

/* The name Handle reports, in Out (MAX_PATH characters); FALSE if the query fails. */
static BOOLEAN QueryName(HANDLE Handle, PWSTR Out)
{
    ULONGLONG Buffer[(sizeof(FILE_NAME_INFORMATION) + MAX_PATH * sizeof(WCHAR)) / sizeof(ULONGLONG) + 1];
    PFILE_NAME_INFORMATION N = (PFILE_NAME_INFORMATION)Buffer;
    IO_STATUS_BLOCK Iosb;
    ULONG Chars;
    if (!NT_SUCCESS(NtQueryInformationFile(Handle, &Iosb, N, sizeof(Buffer) - sizeof(WCHAR), FileNameInformation)))
        return FALSE;
    Chars = min(N->FileNameLength / sizeof(WCHAR), MAX_PATH - 1);
    RtlCopyMemory(Out, N->FileName, Chars * sizeof(WCHAR));
    Out[Chars] = 0;
    return TRUE;
}

/* TRUE if the name Handle reports ends with "\Tail". */
static BOOLEAN NameEndsWith(HANDLE Handle, PCWSTR Tail)
{
    ULONGLONG Buffer[(sizeof(FILE_NAME_INFORMATION) + MAX_PATH * sizeof(WCHAR)) / sizeof(ULONGLONG) + 1];
    PFILE_NAME_INFORMATION N = (PFILE_NAME_INFORMATION)Buffer;
    IO_STATUS_BLOCK Iosb;
    ULONG Chars, TailChars = (ULONG)wcslen(Tail);
    NTSTATUS Status;

    RtlZeroMemory(Buffer, sizeof(Buffer));
    Status = NtQueryInformationFile(Handle, &Iosb, N, sizeof(Buffer) - sizeof(WCHAR), FileNameInformation);
    if (!NT_SUCCESS(Status))
    {
        trace("FileNameInformation: 0x%08lx\n", Status);
        return FALSE;
    }
    Chars = N->FileNameLength / sizeof(WCHAR);
    trace("name %.*ls\n", (int)Chars, N->FileName);
    return Chars > TailChars && N->FileName[Chars - TailChars - 1] == L'\\' &&
           !_wcsnicmp(N->FileName + Chars - TailChars, Tail, TailChars);
}

START_TEST(NtfsRelativeOpen)
{
    WCHAR D[MAX_PATH], D2[MAX_PATH], D3[MAX_PATH], T[MAX_PATH], E[MAX_PATH], E2[MAX_PATH], T2[MAX_PATH], Path[MAX_PATH];
    FILE_DISPOSITION_INFORMATION Disp = { TRUE };
    IO_STATUS_BLOCK Iosb;
    HANDLE H, Hr, Hx, He, Hen;
    ULONGLONG IdD, IdT, IdE;
    NTSTATUS Status;

    if (!NtInit("NtfsRelativeOpen"))
        return;
    NtPath(D, L"d");
    NtPath(D2, L"d2");
    NtPath(D3, L"d3");
    NtPath(T, L"t");
    NtPath(E, L"e");
    NtPath(E2, L"e2");
    NtPath(T2, L"t2");
    ok_hex(NtMakeDir(D, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(NtMakeDir(T, NULL, 0, FALSE), STATUS_SUCCESS);
    NtPath(Path, L"t\\victim.txt");
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "victim"), STATUS_SUCCESS);
    NtPath(Path, L"t\\del.txt");
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "t-del"), STATUS_SUCCESS);
    NtPath(Path, L"d\\del.txt");
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "d-del"), STATUS_SUCCESS);

    /* H: the handle the opens go through; Hr: another handle to the same directory, which renames it. */
    Status = NtOpen(D, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | SYNCHRONIZE,
                    SHARE_ALL, FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &H);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtOpen(D, DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE | SYNCHRONIZE, SHARE_ALL, FILE_OPEN,
                    FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &Hr);
    ok_hex(Status, STATUS_SUCCESS);
    if (!H || !Hr)
    {
        if (H) NtClose(H);
        if (Hr) NtClose(Hr);
        NtCleanup();
        return;
    }
    IdD = NtFileId(H);
    ok_hex(RenameHandle(Hr, D2), STATUS_SUCCESS);
    ok(!Exists(NtTestDir(), L"d") && Exists(NtTestDir(), L"d2"), "d was not renamed to d2\n");

    /* (a) A create relative to H lands in the directory H is open on, now named d2. */
    ok_hex(RelOpen(H, L"x1.txt", FILE_GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(Exists(D2, L"x1.txt"), "x1.txt is not in d2\n");
    /* The same with a new directory at the old name. */
    ok_hex(NtMakeDir(D, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(RelOpen(H, L"x2.txt", FILE_GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(Exists(D2, L"x2.txt"), "x2.txt is not in d2\n");
    ok(!Exists(D, L"x2.txt"), "x2.txt was created in the new d\n");
    ok_dec(CountEntries(D), 0);
    /* An open of an existing name, and of H's own directory (empty name). */
    ok_hex(RelOpen(H, L"del.txt", FILE_GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok_hex(RelOpen(H, L"", FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &Hx), STATUS_SUCCESS);
    if (Hx)
    {
        ok(NtFileId(Hx) == IdD, "The empty relative name opened %I64x, not d2 %I64x\n", NtFileId(Hx), IdD);
        NtClose(Hx);
    }
    /* What H reports as its name after another handle renamed it. */
    ok(NameEndsWith(H, L"d2"), "H does not report the new name\n");
    NtRemove(D);

    /* (b) A junction at the old name, to t: nothing is created in t, nothing of t is opened. */
    ok_hex(MakeJunction(D, T), STATUS_SUCCESS);
    ok_hex(NtOpen(D, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, 0, NULL, &Hx), STATUS_SUCCESS);
    if (Hx)
    {
        IdT = NtFileId(Hx);
        NtClose(Hx);
    }
    else
        IdT = 0;
    ok(IdT != IdD, "d (the junction to t) is d2\n");
    ok_hex(RelOpen(H, L"x3.txt", FILE_GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(Exists(D2, L"x3.txt"), "x3.txt is not in d2\n");
    ok(!Exists(T, L"x3.txt"), "x3.txt was created in t, through the junction at the old name\n");
    ok_hex(RelOpen(H, L"sub", FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(Exists(D2, L"sub"), "sub is not in d2\n");
    ok(!Exists(T, L"sub"), "sub was created in t\n");
    ok_hex(RelOpen(H, L"victim.txt", FILE_GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL), STATUS_OBJECT_NAME_NOT_FOUND);
    ok_hex(RelOpen(H, L"", FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &Hx), STATUS_SUCCESS);
    if (Hx)
    {
        ok(NtFileId(Hx) == IdD, "The empty relative name opened %I64x, not d2 %I64x\n", NtFileId(Hx), IdD);
        NtClose(Hx);
    }
    ok_hex(RelOpen(H, L"x1.txt", FILE_GENERIC_READ, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(!Exists(T, L"x1.txt"), "x1.txt was created in t\n");

    /* (c) Deletes relative to H: d2's file goes, t's file of the same name stays. */
    ok_hex(RelOpen(H, L"del.txt", DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL), STATUS_SUCCESS);
    ok(!Exists(D2, L"del.txt"), "d2\\del.txt was not deleted\n");
    ok(Exists(T, L"del.txt"), "t\\del.txt was deleted through the junction at the old name\n");
    Status = RelOpen(H, L"x2.txt", DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &Hx);
    ok_hex(Status, STATUS_SUCCESS);
    if (Hx)
    {
        ok_hex(NtSetInformationFile(Hx, &Iosb, &Disp, sizeof(Disp), FileDispositionInformation), STATUS_SUCCESS);
        NtClose(Hx);
    }
    ok(!Exists(D2, L"x2.txt"), "d2\\x2.txt was not deleted\n");
    ok_dec(CountEntries(T), 2);
    ok(Exists(T, L"victim.txt"), "t\\victim.txt is gone\n");

    /* (d) The renaming handle keeps working, also through a second rename, and so does H. */
    ok_hex(RelOpen(Hr, L"y1.txt", FILE_GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(Exists(D2, L"y1.txt"), "y1.txt is not in d2\n");
    ok(NameEndsWith(Hr, L"d2"), "the renaming handle does not report the new name\n");
    ok_hex(RenameHandle(Hr, D3), STATUS_SUCCESS);
    ok_hex(RelOpen(Hr, L"y2.txt", FILE_GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(Exists(D3, L"y2.txt"), "y2.txt is not in d3\n");
    ok_hex(RelOpen(H, L"y3.txt", FILE_GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL), STATUS_SUCCESS);
    ok(Exists(D3, L"y3.txt"), "y3.txt is not in d3\n");
    ok(NameEndsWith(H, L"d3"), "H does not report the second new name\n");
    ok_dec(CountEntries(T), 2);

    /* Creates relative to H while another thread keeps renaming the directory: each one lands in
     * it, whatever its name is at that moment. */
    {
        RENAMER R;
        HANDLE Thread;
        ULONG i, Created = 0, Failed = 0, Found = 0, Checked = 0, Wrong = 0;
        WCHAR File[16];
        RtlZeroMemory(&R, sizeof(R));
        R.Handle = Hr;
        R.Names[0] = D3;
        R.Names[1] = D2;
        Thread = CreateThread(NULL, 0, RenamerThread, &R, 0, NULL);
        ok(Thread != NULL, "CreateThread failed %lu\n", GetLastError());
        /* At least 400 creates, and on until the thread has renamed the directory 50 times. */
        for (i = 0; Thread && i < 5000 && (i < 400 || R.Renames < 50); i++, Created++)
        {
            _snwprintf(File, RTL_NUMBER_OF(File) - 1, L"r%04lu", i);
            File[RTL_NUMBER_OF(File) - 1] = 0;
            Status = RelOpen(H, File, FILE_GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &Hx);
            if (!NT_SUCCESS(Status) && Failed++ < 5)
                ok(FALSE, "Creating %ls relative to the renamed directory: 0x%08lx\n", File, Status);
            if (Hx && !(i % 8))
            {
                /* While the file is open the directory is not renamed: the file's name is the
                 * directory's name now, whichever it had when the file was created. */
                WCHAR DirName[MAX_PATH], FileName[MAX_PATH], Expect[MAX_PATH];
                if (QueryName(H, DirName) && QueryName(Hx, FileName))
                {
                    _snwprintf(Expect, MAX_PATH - 1, L"%ls\\%ls", DirName, File);
                    Expect[MAX_PATH - 1] = 0;
                    Checked++;
                    if (_wcsicmp(Expect, FileName) && Wrong++ < 5)
                        ok(FALSE, "%ls reports %ls, its directory %ls\n", File, FileName, DirName);
                }
            }
            if (Hx)
                NtClose(Hx);
        }
        if (Thread)
        {
            R.Stop = TRUE;
            WaitForSingleObject(Thread, INFINITE);
            CloseHandle(Thread);
            trace("%lu renames during %lu creates\n", R.Renames, Created);
            ok(R.Renames > 0, "The directory was never renamed during the creates\n");
            ok_dec(Failed, 0);
            ok(Checked > 0, "No name checked\n");
            ok_dec(Wrong, 0);
            /* By name: the directory is open with DELETE, which a listing open without FILE_SHARE_DELETE would not share. */
            for (i = 0; i < Created; i++)
            {
                _snwprintf(File, RTL_NUMBER_OF(File) - 1, L"r%04lu", i);
                Found += Exists(R.Names[R.Current], File);
            }
            ok_dec(Found, Created);
            ok(NameEndsWith(H, R.Current ? L"d2" : L"d3"), "H does not report the name the thread left\n");
            ok_dec(CountEntries(T), 2);
        }
    }
    NtClose(Hr);
    NtClose(H);

    /* A delete through a handle whose directory another handle renamed removes that directory, not
     * what was put at its old name. */
    ok_hex(NtMakeDir(E, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(NtMakeDir(T2, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(NtOpen(E, DELETE | FILE_LIST_DIRECTORY | SYNCHRONIZE, SHARE_ALL, FILE_OPEN,
                  FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &He), STATUS_SUCCESS);
    ok_hex(NtOpen(E, DELETE | SYNCHRONIZE, SHARE_ALL, FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                  NULL, &Hen), STATUS_SUCCESS);
    if (He && Hen)
    {
        IdE = NtFileId(He);
        ok_hex(RenameHandle(Hen, E2), STATUS_SUCCESS);
        NtClose(Hen);
        ok_hex(MakeJunction(E, T2), STATUS_SUCCESS);
        ok_hex(NtSetInformationFile(He, &Iosb, &Disp, sizeof(Disp), FileDispositionInformation), STATUS_SUCCESS);
        NtClose(He);
        ok(!Exists(NtTestDir(), L"e2"), "e2 (renamed through another handle) was not deleted\n");
        ok(Exists(NtTestDir(), L"e"), "the junction e at the old name was deleted\n");
        ok(Exists(NtTestDir(), L"t2"), "t2 was deleted\n");
        Status = NtOpen(E, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, FILE_OPEN_REPARSE_POINT, NULL, &Hx);
        ok_hex(Status, STATUS_SUCCESS);
        if (Hx)
        {
            ok(NtFileId(Hx) != IdE, "e is still the renamed directory\n");
            NtClose(Hx);
        }
    }
    else
    {
        if (He) NtClose(He);
        if (Hen) NtClose(Hen);
    }
    NtRemove(D);
    NtRemove(E);
    NtCleanup();
}

/*
 * A junction whose own DACL denies Everyone the listing (as the legacy profile junctions do), opened
 * itself with FILE_OPEN_REPARSE_POINT, then reopened relative to that handle with an empty name and
 * without FILE_OPEN_REPARSE_POINT. Windows never follows the junction there: Windows 10 refuses the
 * listing by the junction's DACL (STATUS_ACCESS_DENIED), Server 2008 R2 and both of them for any other
 * access fail the reparse (STATUS_IO_REPARSE_DATA_INVALID). ntfsng follows it like a path open, so it
 * must at least check the junction's DACL like one: the listing must not reach the target.
 */
START_TEST(NtfsRelativeOpenJunctionAcl)
{
    WCHAR J[MAX_PATH], T[MAX_PATH], Path[MAX_PATH];
    NT_ACE Aces[3];
    HANDLE Hj = NULL, Hx = NULL;
    ULONGLONG IdJ = 0, IdT = 0;
    NTSTATUS Status;

    if (!NtInit("NtfsRelativeOpenJunctionAcl"))
        return;
    NtPath(J, L"j");
    NtPath(T, L"t");
    ok_hex(NtMakeDir(T, NULL, 0, FALSE), STATUS_SUCCESS);
    NtPath(Path, L"t\\inside.txt");
    ok_hex(NtMakeFile(Path, NULL, 0, FALSE, "target"), STATUS_SUCCESS);
    ok_hex(NtOpen(T, FILE_READ_ATTRIBUTES, SHARE_ALL, FILE_OPEN, FILE_DIRECTORY_FILE, NULL, &Hx), STATUS_SUCCESS);
    if (Hx)
    {
        IdT = NtFileId(Hx);
        NtClose(Hx);
        Hx = NULL;
    }

    /* j: Everyone is denied the listing, administrators and SYSTEM get the rest; then j becomes a junction to t. */
    Aces[0].Sid = SidEveryone; Aces[0].Mask = FILE_LIST_DIRECTORY; Aces[0].Flags = 0; Aces[0].Deny = TRUE;
    Aces[1].Sid = SidAdmins; Aces[1].Mask = FILE_ALL_ACCESS; Aces[1].Flags = 0; Aces[1].Deny = FALSE;
    Aces[2].Sid = SidSystem; Aces[2].Mask = FILE_ALL_ACCESS; Aces[2].Flags = 0; Aces[2].Deny = FALSE;
    ok_hex(NtMakeDir(J, Aces, 3, TRUE), STATUS_SUCCESS);
    Status = MakeJunctionDisposition(J, T, FILE_OPEN);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtCleanup();
        return;
    }

    /* The path open of j for listing is refused by the DACL of j. */
    Status = NtOpen(J, FILE_LIST_DIRECTORY | SYNCHRONIZE, SHARE_ALL, FILE_OPEN,
                    FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &Hx);
    ok_hex(Status, STATUS_ACCESS_DENIED);
    if (Hx)
    {
        ok(NtFileId(Hx) != IdT, "The listing open of j reached t\n");
        NtClose(Hx);
        Hx = NULL;
    }

    /* j itself, not followed. */
    Status = NtOpen(J, FILE_READ_ATTRIBUTES | SYNCHRONIZE, SHARE_ALL, FILE_OPEN,
                    FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &Hj);
    ok_hex(Status, STATUS_SUCCESS);
    if (!Hj)
    {
        NtCleanup();
        return;
    }
    IdJ = NtFileId(Hj);
    ok(IdJ != IdT, "The FILE_OPEN_REPARSE_POINT open of j followed it\n");

    /* The empty-name reopen for listing: refused, t is not reached. */
    Status = RelOpen(Hj, L"", FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &Hx);
    ok(Status == STATUS_ACCESS_DENIED || Status == STATUS_IO_REPARSE_DATA_INVALID,
       "The empty-name reopen of j for listing: 0x%08lx\n", Status);
    if (Hx)
    {
        ok(NtFileId(Hx) != IdT, "The empty-name reopen of j for listing reached t\n");
        NtClose(Hx);
        Hx = NULL;
    }

    /* The same reopen for an access that j grants: Windows fails the reparse; ntfsng follows j to t. */
    Status = RelOpen(Hj, L"", FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_DIRECTORY_FILE, &Hx);
    ok(Status == STATUS_IO_REPARSE_DATA_INVALID || (is_reactos() && Status == STATUS_SUCCESS),
       "The empty-name reopen of j for attributes: 0x%08lx\n", Status);
    if (Hx)
    {
        ok(NtFileId(Hx) == IdT, "The empty-name reopen of j opened %I64x, not t %I64x\n", NtFileId(Hx), IdT);
        NtClose(Hx);
        Hx = NULL;
    }

    /* With FILE_OPEN_REPARSE_POINT the reopen is j itself. */
    Status = RelOpen(Hj, L"", FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_OPEN_REPARSE_POINT, &Hx);
    ok_hex(Status, STATUS_SUCCESS);
    if (Hx)
    {
        ok(NtFileId(Hx) == IdJ, "The FILE_OPEN_REPARSE_POINT reopen of j opened %I64x, not j %I64x\n", NtFileId(Hx), IdJ);
        NtClose(Hx);
        Hx = NULL;
    }

    NtClose(Hj);
    NtCleanup();
}
