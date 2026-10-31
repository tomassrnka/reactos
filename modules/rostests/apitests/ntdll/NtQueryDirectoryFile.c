/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQueryDirectoryFile
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define PATTERN 0x55
#define BUFFER_SIZE 4096

static WCHAR TestDirectory[MAX_PATH];

typedef struct
{
    ULONG Class;
    ULONG NameLengthOffset; /* the class's FileNameLength field, and where the name starts */
    ULONG NameOffset;
    ULONG EndOfFileOffset;  /* FILE_*_INFORMATION.EndOfFile, 0 when the class has none */
    ULONG AttributesOffset; /* FileAttributes */
} DIRECTORY_CLASS;

/* The record of each class has LARGE_INTEGERs at multiples of 8 and a variable name at its end:
 * the same bytes for a 32-bit and a 64-bit caller. */
static const DIRECTORY_CLASS Classes[] =
{
    { FileDirectoryInformation,       60, 64,  40, 56 },
    { FileFullDirectoryInformation,   60, 68,  40, 56 },
    { FileBothDirectoryInformation,   60, 94,  40, 56 },
    { FileNamesInformation,            8, 12,   0,  0 },
    { FileIdBothDirectoryInformation, 60, 104, 40, 56 },
    { FileIdFullDirectoryInformation, 60, 80,  40, 56 },
};

static
ULONG_PTR
PatternValue(void)
{
    ULONG_PTR Value;
    RtlFillMemory(&Value, sizeof(Value), PATTERN);
    return Value;
}

static
ULONG
Get32(const UCHAR *Buffer, ULONG Offset)
{
    ULONG Value;
    RtlCopyMemory(&Value, Buffer + Offset, sizeof(Value));
    return Value;
}

static
LONGLONG
Get64(const UCHAR *Buffer, ULONG Offset)
{
    LONGLONG Value;
    RtlCopyMemory(&Value, Buffer + Offset, sizeof(Value));
    return Value;
}

static
HANDLE
OpenDirectory(ULONG Flags)
{
    HANDLE Handle = CreateFileW(TestDirectory, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | Flags, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "opening %ls failed, error %lu\n", TestDirectory, GetLastError());
    return Handle == INVALID_HANDLE_VALUE ? NULL : Handle;
}

static
NTSTATUS
Query(
    HANDLE Directory,
    HANDLE Event,
    PIO_STATUS_BLOCK Iosb,
    PUCHAR Buffer,
    ULONG Length,
    ULONG Class,
    BOOLEAN Single,
    PCWSTR Mask,
    BOOLEAN Restart)
{
    UNICODE_STRING Name;

    RtlFillMemory(Buffer, BUFFER_SIZE + 16, PATTERN);
    RtlFillMemory(Iosb, sizeof(*Iosb), PATTERN);
    if (Mask) RtlInitUnicodeString(&Name, Mask);
    return NtQueryDirectoryFile(Directory, Event, NULL, NULL, Iosb, Buffer, Length, Class, Single,
                                Mask ? &Name : NULL, Restart);
}

typedef struct
{
    WCHAR Name[32];
    LONGLONG Size;
    BOOL Directory;
    BOOL Seen;
} EXPECTED;

static
void
CheckEntries(
    const DIRECTORY_CLASS *Class,
    const UCHAR *Buffer,
    ULONG_PTR Information,
    EXPECTED *Expected,
    ULONG Count,
    ULONG ExpectedEntries)
{
    ULONG Offset = 0, Entries = 0, Next, i;

    for (;;)
    {
        ULONG Length = Get32(Buffer, Offset + Class->NameLengthOffset);
        const WCHAR *Name = (const WCHAR *)(Buffer + Offset + Class->NameOffset);
        BOOL Found = FALSE;

        ok(!(Offset & 7), "class %lu: entry at %lu is not 8-byte aligned\n", Class->Class, Offset);
        ok(Offset + Class->NameOffset + Length <= Information, "class %lu: entry at %lu ends at %lu, beyond %Iu\n",
           Class->Class, Offset, Offset + Class->NameOffset + Length, Information);
        for (i = 0; i < Count; i++)
        {
            if (Length == wcslen(Expected[i].Name) * sizeof(WCHAR) && !_wcsnicmp(Name, Expected[i].Name, Length / sizeof(WCHAR)))
            {
                Found = TRUE;
                ok(!Expected[i].Seen, "class %lu: %ls twice\n", Class->Class, Expected[i].Name);
                Expected[i].Seen = TRUE;
                if (Class->AttributesOffset)
                {
                    ULONG Attributes = Get32(Buffer, Offset + Class->AttributesOffset);
                    ok(!!(Attributes & FILE_ATTRIBUTE_DIRECTORY) == Expected[i].Directory,
                       "class %lu: %ls attributes 0x%lx\n", Class->Class, Expected[i].Name, Attributes);
                    if (!Expected[i].Directory && Expected[i].Size >= 0)
                        ok(Get64(Buffer, Offset + Class->EndOfFileOffset) == Expected[i].Size,
                           "class %lu: %ls EndOfFile %I64d\n", Class->Class, Expected[i].Name,
                           Get64(Buffer, Offset + Class->EndOfFileOffset));
                }
            }
        }
        ok(Found, "class %lu: unexpected entry at %lu (%lu bytes of name)\n", Class->Class, Offset, Length);
        Entries++;
        Next = Get32(Buffer, Offset);
        if (!Next) break;
        Offset += Next;
        if (Offset >= Information) { ok(FALSE, "class %lu: chain leaves the data\n", Class->Class); break; }
    }
    ok(Entries == ExpectedEntries, "class %lu: %lu entries, expected %lu\n", Class->Class, Entries, ExpectedEntries);
    /* the data ends where the last name ends */
    ok(Information == Offset + Class->NameOffset + Get32(Buffer, Offset + Class->NameLengthOffset),
       "class %lu: Information = %Iu\n", Class->Class, Information);
}

static
void
Test_Classes(void)
{
    HANDLE Directory;
    UCHAR Buffer[BUFFER_SIZE + 16];
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    ULONG i, j;

    for (i = 0; i < RTL_NUMBER_OF(Classes); i++)
    {
        const DIRECTORY_CLASS *Class = &Classes[i];
        EXPECTED Expected[] =
        {
            { L".", -1, TRUE }, { L"..", -1, TRUE }, { L"a.txt", 0, FALSE }, { L"bb.txt", 3, FALSE },
            { L"a_much_longer_file_name.dat", 0, FALSE }, { L"sub", -1, TRUE },
        };
        EXPECTED TxtOnly[] = { { L"a.txt", 0, FALSE }, { L"bb.txt", 3, FALSE } };

        Directory = OpenDirectory(0);
        if (!Directory) return;

        Status = Query(Directory, NULL, &Iosb, Buffer, BUFFER_SIZE, Class->Class, FALSE, L"*", TRUE);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Iosb.Status == STATUS_SUCCESS, "class %lu: IoStatusBlock.Status = 0x%lx\n", Class->Class, Iosb.Status);
        if (NT_SUCCESS(Status))
        {
            CheckEntries(Class, Buffer, Iosb.Information, Expected, RTL_NUMBER_OF(Expected), RTL_NUMBER_OF(Expected));
            ok(Buffer[Iosb.Information] == PATTERN, "class %lu: wrote past Information\n", Class->Class);
        }
        /* the enumeration is over */
        Status = Query(Directory, NULL, &Iosb, Buffer, BUFFER_SIZE, Class->Class, FALSE, NULL, FALSE);
        ok_hex(Status, STATUS_NO_MORE_FILES);
        ok(Iosb.Status == STATUS_NO_MORE_FILES && Iosb.Information == 0, "class %lu: (0x%lx, %Iu)\n", Class->Class, Iosb.Status, Iosb.Information);
        CloseHandle(Directory);

        /* one entry per call, a mask, and a continued search */
        Directory = OpenDirectory(0);
        if (!Directory) return;
        for (j = 0; j < 3; j++)
        {
            Status = Query(Directory, NULL, &Iosb, Buffer, BUFFER_SIZE, Class->Class, TRUE, j == 0 ? L"*.txt" : NULL, j == 0);
            if (j < 2)
            {
                ok_hex(Status, STATUS_SUCCESS);
                if (NT_SUCCESS(Status))
                {
                    ok(Get32(Buffer, 0) == 0, "class %lu: single entry with NextEntryOffset %lu\n", Class->Class, Get32(Buffer, 0));
                    CheckEntries(Class, Buffer, Iosb.Information, TxtOnly, RTL_NUMBER_OF(TxtOnly), 1);
                }
            }
            else ok_hex(Status, STATUS_NO_MORE_FILES);
        }
        CloseHandle(Directory);

        /* nothing matches */
        Directory = OpenDirectory(0);
        if (!Directory) return;
        Status = Query(Directory, NULL, &Iosb, Buffer, BUFFER_SIZE, Class->Class, FALSE, L"nomatch*", TRUE);
        ok_hex(Status, STATUS_NO_SUCH_FILE);
        ok(Iosb.Status == 0x55555555 && Iosb.Information == PatternValue(), "class %lu: IoStatusBlock (0x%lx, %Iu)\n", Class->Class, Iosb.Status, Iosb.Information);
        CloseHandle(Directory);

        /* a buffer that does not hold one record */
        if (Class->Class != FileNamesInformation)
        {
            Directory = OpenDirectory(0);
            if (!Directory) return;
            Status = Query(Directory, NULL, &Iosb, Buffer, 16, Class->Class, FALSE, L"*", TRUE);
            ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
            ok(Iosb.Status == 0x55555555 && Iosb.Information == PatternValue(), "class %lu: IoStatusBlock (0x%lx, %Iu)\n", Class->Class, Iosb.Status, Iosb.Information);
            ok(Buffer[0] == PATTERN, "class %lu: buffer written\n", Class->Class);
            CloseHandle(Directory);
        }
    }
}

static
void
Test_Handles(void)
{
    HANDLE Directory, File, Event;
    UCHAR Buffer[BUFFER_SIZE + 16];
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    WCHAR Path[MAX_PATH];

    /* a file is not a directory */
    StringCbPrintfW(Path, sizeof(Path), L"%ls\\a.txt", TestDirectory);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "opening %ls failed, error %lu\n", Path, GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        Status = Query(File, NULL, &Iosb, Buffer, BUFFER_SIZE, FileBothDirectoryInformation, FALSE, NULL, TRUE);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
        ok(Iosb.Status == 0x55555555 && Iosb.Information == PatternValue(), "IoStatusBlock (0x%lx, %Iu)\n", Iosb.Status, Iosb.Information);
        CloseHandle(File);
    }

    /* an asynchronous handle completes at once, with or without an event; the event is signaled */
    Directory = OpenDirectory(FILE_FLAG_OVERLAPPED);
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Directory)
    {
        Status = Query(Directory, NULL, &Iosb, Buffer, BUFFER_SIZE, FileBothDirectoryInformation, FALSE, L"*", TRUE);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Iosb.Status == STATUS_SUCCESS && Iosb.Information != 0, "IoStatusBlock (0x%lx, %Iu)\n", Iosb.Status, Iosb.Information);
        Status = Query(Directory, Event, &Iosb, Buffer, BUFFER_SIZE, FileBothDirectoryInformation, FALSE, L"*", TRUE);
        if (Status == STATUS_PENDING)
        {
            ok(WaitForSingleObject(Event, 5000) == WAIT_OBJECT_0, "the request did not complete\n");
            Status = Iosb.Status;
        }
        ok_hex(Status, STATUS_SUCCESS);
        ok(Iosb.Status == STATUS_SUCCESS && Iosb.Information != 0, "IoStatusBlock (0x%lx, %Iu)\n", Iosb.Status, Iosb.Information);
        ok(WaitForSingleObject(Event, 0) == WAIT_OBJECT_0, "the event is not signaled\n");
        CloseHandle(Directory);
    }
    ResetEvent(Event);
    Directory = OpenDirectory(0);
    if (Directory)
    {
        Status = Query(Directory, Event, &Iosb, Buffer, BUFFER_SIZE, FileNamesInformation, FALSE, L"*", TRUE);
        ok_hex(Status, STATUS_SUCCESS);
        ok(WaitForSingleObject(Event, 0) == WAIT_OBJECT_0, "the event of a synchronous handle is not signaled\n");

        /* a missing IO_STATUS_BLOCK, a bad class */
        RtlFillMemory(Buffer, sizeof(Buffer), PATTERN);
        Status = NtQueryDirectoryFile(Directory, NULL, NULL, NULL, NULL, Buffer, BUFFER_SIZE, FileBothDirectoryInformation, FALSE, NULL, TRUE);
        ok_hex(Status, STATUS_ACCESS_VIOLATION);
        Status = Query(Directory, NULL, &Iosb, Buffer, BUFFER_SIZE, 0, FALSE, NULL, TRUE);
        ok(Status == STATUS_INVALID_INFO_CLASS || Status == STATUS_NOT_IMPLEMENTED, "class 0: 0x%lx\n", Status);
        Status = Query(Directory, NULL, &Iosb, Buffer, BUFFER_SIZE, 0x7fffffff, FALSE, NULL, TRUE);
        ok(Status == STATUS_INVALID_INFO_CLASS || Status == STATUS_NOT_IMPLEMENTED, "class 0x7fffffff: 0x%lx\n", Status);
        CloseHandle(Directory);
    }
    CloseHandle(Event);
}

static
void
CreateFiles(void)
{
    static const struct { PCWSTR Name; PCSTR Data; } Files[] =
    {
        { L"a.txt", "" }, { L"bb.txt", "xyz" }, { L"a_much_longer_file_name.dat", "" },
    };
    WCHAR Path[MAX_PATH];
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Files); i++)
    {
        HANDLE File;
        DWORD Written;

        StringCbPrintfW(Path, sizeof(Path), L"%ls\\%ls", TestDirectory, Files[i].Name);
        File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok(File != INVALID_HANDLE_VALUE, "creating %ls failed, error %lu\n", Path, GetLastError());
        if (File == INVALID_HANDLE_VALUE) continue;
        if (Files[i].Data[0]) WriteFile(File, Files[i].Data, (DWORD)strlen(Files[i].Data), &Written, NULL);
        CloseHandle(File);
    }
    StringCbPrintfW(Path, sizeof(Path), L"%ls\\sub", TestDirectory);
    ok(CreateDirectoryW(Path, NULL), "CreateDirectoryW failed, error %lu\n", GetLastError());
}

static
void
RemoveFiles(void)
{
    static const PCWSTR Names[] = { L"a.txt", L"bb.txt", L"a_much_longer_file_name.dat" };
    WCHAR Path[MAX_PATH];
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Names); i++)
    {
        StringCbPrintfW(Path, sizeof(Path), L"%ls\\%ls", TestDirectory, Names[i]);
        DeleteFileW(Path);
    }
    StringCbPrintfW(Path, sizeof(Path), L"%ls\\sub", TestDirectory);
    RemoveDirectoryW(Path);
    RemoveDirectoryW(TestDirectory);
}

START_TEST(NtQueryDirectoryFile)
{
    WCHAR TempPath[MAX_PATH];

    GetTempPathW(MAX_PATH, TempPath);
    StringCbPrintfW(TestDirectory, sizeof(TestDirectory), L"%lskqd_%lu", TempPath, GetCurrentProcessId());
    ok(CreateDirectoryW(TestDirectory, NULL), "CreateDirectoryW(%ls) failed, error %lu\n", TestDirectory, GetLastError());
    CreateFiles();
    Test_Classes();
    Test_Handles();
    RemoveFiles();
}
