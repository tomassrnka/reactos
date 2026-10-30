/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Named Pipe File System (NPFS): listing the pipes of \Device\NamedPipe\
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <ndk/ntndk.h>

#define PREFIX L"NpfsDirQueryTest_"

typedef struct _TEST_NAMES_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    ULONG FileNameLength;
    WCHAR FileName[1];
} TEST_NAMES_INFORMATION;

static HANDLE
OpenPipeDirectory(void)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE Handle = NULL;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, L"\\Device\\NamedPipe\\");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtCreateFile(&Handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &Attributes, &Iosb, NULL, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE, NULL, 0);
    ok_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? Handle : NULL;
}

static NTSTATUS
Query(HANDLE Directory, ULONG Class, PCWSTR Pattern, BOOLEAN Single, BOOLEAN Restart, PVOID Buffer,
      ULONG Length, ULONG_PTR *Information)
{
    UNICODE_STRING Name;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    Iosb.Information = 0xdeadbeef;
    if (Pattern)
        RtlInitUnicodeString(&Name, Pattern);
    Status = NtQueryDirectoryFile(Directory, NULL, NULL, NULL, &Iosb, Buffer, Length, Class, Single,
                                  Pattern ? &Name : NULL, Restart);
    if (Information)
        *Information = Iosb.Information;
    return Status;
}

static int
NameIs(const WCHAR *Name, ULONG Bytes, PCWSTR Expected)
{
    return Bytes == wcslen(Expected) * sizeof(WCHAR) && !memcmp(Name, Expected, Bytes);
}

static void
TestDirectoryInformation(HANDLE Directory)
{
    static BYTE Buffer[4096];
    FILE_DIRECTORY_INFORMATION *Info;
    ULONG_PTR Information;
    NTSTATUS Status;

    /* Two entries, in creation order; no times, attributes NORMAL, EndOfFile the open server
     * instances and AllocationSize the maximum */
    Status = Query(Directory, FileDirectoryInformation, PREFIX L"*", FALSE, TRUE, Buffer, sizeof(Buffer), &Information);
    ok_hex(Status, STATUS_SUCCESS);
    Info = (PVOID)Buffer;
    ok(NameIs(Info->FileName, Info->FileNameLength, PREFIX L"alpha"), "first name\n");
    ok(Info->NextEntryOffset == ((FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + Info->FileNameLength + 7) & ~7),
       "NextEntryOffset %lu\n", Info->NextEntryOffset);
    ok(Info->FileAttributes == FILE_ATTRIBUTE_NORMAL, "attributes %lx\n", Info->FileAttributes);
    ok(Info->EndOfFile.QuadPart == 2, "instances %I64d\n", Info->EndOfFile.QuadPart);
    ok(Info->AllocationSize.QuadPart == 4, "maximum %I64d\n", Info->AllocationSize.QuadPart);
    ok(Info->CreationTime.QuadPart == 0 && Info->LastWriteTime.QuadPart == 0, "times\n");
    if (Info->NextEntryOffset)
    {
        FILE_DIRECTORY_INFORMATION *Second = (PVOID)(Buffer + Info->NextEntryOffset);
        ok(NameIs(Second->FileName, Second->FileNameLength, PREFIX L"beta"), "second name\n");
        ok(Second->NextEntryOffset == 0, "last entry has NextEntryOffset %lu\n", Second->NextEntryOffset);
        ok(Second->EndOfFile.QuadPart == 1 && Second->AllocationSize.QuadPart == 1, "beta counts\n");
        ok(Information == Info->NextEntryOffset + FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + Second->FileNameLength,
           "information %lu\n", (ULONG)Information);
    }
    else
        ok(0, "only one entry\n");

    /* the scan is over until a restart */
    Status = Query(Directory, FileDirectoryInformation, NULL, FALSE, FALSE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_NO_MORE_FILES);

    /* one entry at a time */
    Status = Query(Directory, FileDirectoryInformation, NULL, TRUE, TRUE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(NameIs(((FILE_DIRECTORY_INFORMATION *)Buffer)->FileName, ((FILE_DIRECTORY_INFORMATION *)Buffer)->FileNameLength, PREFIX L"alpha"), "single 1\n");
    ok(((FILE_DIRECTORY_INFORMATION *)Buffer)->NextEntryOffset == 0, "single entry NextEntryOffset\n");
    Status = Query(Directory, FileDirectoryInformation, NULL, TRUE, FALSE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(NameIs(((FILE_DIRECTORY_INFORMATION *)Buffer)->FileName, ((FILE_DIRECTORY_INFORMATION *)Buffer)->FileNameLength, PREFIX L"beta"), "single 2\n");
    Status = Query(Directory, FileDirectoryInformation, NULL, TRUE, FALSE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_NO_MORE_FILES);

    /* the pattern of the first query on a handle is the one that stays, a later FileName is ignored */
    Status = Query(Directory, FileDirectoryInformation, L"nothing_like_it*", FALSE, TRUE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(NameIs(((FILE_DIRECTORY_INFORMATION *)Buffer)->FileName, ((FILE_DIRECTORY_INFORMATION *)Buffer)->FileNameLength, PREFIX L"alpha"), "pattern kept\n");

    /* a buffer that holds the fixed part but not the name: what fits, and the warning */
    Status = Query(Directory, FileDirectoryInformation, NULL, FALSE, TRUE, Buffer, FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + 8, &Information);
    ok_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok(Information == FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + 8, "information %lu\n", (ULONG)Information);
    ok(!memcmp(((FILE_DIRECTORY_INFORMATION *)Buffer)->FileName, PREFIX, 8), "partial name\n");
    /* the entry was not consumed */
    Status = Query(Directory, FileDirectoryInformation, NULL, TRUE, TRUE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_SUCCESS);
}

static void
TestOtherClasses(HANDLE Directory)
{
    static BYTE Buffer[4096];
    ULONG_PTR Information;
    NTSTATUS Status;

    Status = Query(Directory, FileFullDirectoryInformation, PREFIX L"*", FALSE, TRUE, Buffer, sizeof(Buffer), &Information);
    ok_hex(Status, STATUS_SUCCESS);
    ok(NameIs(((FILE_FULL_DIR_INFORMATION *)Buffer)->FileName, ((FILE_FULL_DIR_INFORMATION *)Buffer)->FileNameLength, PREFIX L"alpha"), "full\n");
    ok(((FILE_FULL_DIR_INFORMATION *)Buffer)->EndOfFile.QuadPart == 2, "full count\n");
    ok(((FILE_FULL_DIR_INFORMATION *)Buffer)->EaSize == 0, "full EaSize\n");

    Status = Query(Directory, FileBothDirectoryInformation, NULL, FALSE, TRUE, Buffer, sizeof(Buffer), &Information);
    ok_hex(Status, STATUS_SUCCESS);
    ok(NameIs(((FILE_BOTH_DIR_INFORMATION *)Buffer)->FileName, ((FILE_BOTH_DIR_INFORMATION *)Buffer)->FileNameLength, PREFIX L"alpha"), "both\n");
    ok(((FILE_BOTH_DIR_INFORMATION *)Buffer)->ShortNameLength == 0, "both short name\n");

    Status = Query(Directory, FileNamesInformation, NULL, FALSE, TRUE, Buffer, sizeof(Buffer), &Information);
    ok_hex(Status, STATUS_SUCCESS);
    ok(NameIs(((TEST_NAMES_INFORMATION *)Buffer)->FileName, ((TEST_NAMES_INFORMATION *)Buffer)->FileNameLength, PREFIX L"alpha"), "names\n");

    /* classes a directory query does not know */
    Status = Query(Directory, FileBasicInformation, NULL, FALSE, TRUE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);
    Status = Query(Directory, 37, NULL, FALSE, TRUE, Buffer, sizeof(Buffer), NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);
}

static void
TestFindFile(void)
{
    WIN32_FIND_DATAW Data;
    HANDLE Find;
    int Count = 0;
    DWORD Attributes;

    Find = FindFirstFileW(L"\\\\.\\pipe\\" PREFIX L"alpha", &Data);
    ok(Find != INVALID_HANDLE_VALUE, "FindFirstFile: %lu\n", GetLastError());
    if (Find != INVALID_HANDLE_VALUE)
    {
        ok(!wcscmp(Data.cFileName, PREFIX L"alpha"), "name %ls\n", Data.cFileName);
        ok(Data.dwFileAttributes == FILE_ATTRIBUTE_NORMAL, "attributes %lx\n", Data.dwFileAttributes);
        ok(Data.nFileSizeLow == 2, "size %lu\n", Data.nFileSizeLow);
        ok(!FindNextFileW(Find, &Data) && GetLastError() == ERROR_NO_MORE_FILES, "FindNextFile\n");
        FindClose(Find);
    }

    Find = FindFirstFileW(L"\\\\.\\pipe\\" PREFIX L"*", &Data);
    ok(Find != INVALID_HANDLE_VALUE, "FindFirstFile pattern: %lu\n", GetLastError());
    if (Find != INVALID_HANDLE_VALUE)
    {
        do
            Count++;
        while (FindNextFileW(Find, &Data));
        FindClose(Find);
        ok(Count == 2, "count %d\n", Count);
    }

    Find = FindFirstFileW(L"\\\\.\\pipe\\" PREFIX L"nonexistent", &Data);
    ok(Find == INVALID_HANDLE_VALUE, "nonexistent pipe found\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_NO_MORE_FILES, "nonexistent: %lu\n", GetLastError());

    /* the pipe directory is a directory */
    Attributes = GetFileAttributesW(L"\\\\.\\pipe\\");
    ok(Attributes == FILE_ATTRIBUTE_DIRECTORY, "attributes of the pipe directory: %lx\n", Attributes);
    Attributes = GetFileAttributesW(L"\\\\.\\pipe\\" PREFIX L"alpha");
    ok(Attributes == FILE_ATTRIBUTE_NORMAL, "attributes of a pipe: %lx\n", Attributes);
}

static void
TestRootInformation(void)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE Handle = NULL;
    FILE_BASIC_INFORMATION Basic;
    FILE_STANDARD_INFORMATION Standard;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, L"\\Device\\NamedPipe\\");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtCreateFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &Attributes, &Iosb, NULL, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    memset(&Basic, 0xcc, sizeof(Basic));
    Status = NtQueryInformationFile(Handle, &Iosb, &Basic, sizeof(Basic), FileBasicInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Basic.FileAttributes == FILE_ATTRIBUTE_DIRECTORY, "basic attributes %lx\n", Basic.FileAttributes);
    ok(Basic.CreationTime.QuadPart == 0, "basic time\n");

    memset(&Standard, 0xcc, sizeof(Standard));
    Status = NtQueryInformationFile(Handle, &Iosb, &Standard, sizeof(Standard), FileStandardInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Standard.Directory == TRUE, "standard Directory %d\n", Standard.Directory);
    ok(Standard.DeletePending == FALSE, "standard DeletePending\n");
    ok(Standard.NumberOfLinks == 1, "standard links %lu\n", Standard.NumberOfLinks);
    ok(Standard.EndOfFile.QuadPart == 0 && Standard.AllocationSize.QuadPart == 0, "standard sizes\n");
    CloseHandle(Handle);
}

START_TEST(NpfsDirectoryQuery)
{
    HANDLE Alpha[2], Beta, Directory;
    int i;

    for (i = 0; i < 2; i++)
        Alpha[i] = CreateNamedPipeW(L"\\\\.\\pipe\\" PREFIX L"alpha", PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_WAIT, 4, 512, 512, 0, NULL);
    Beta = CreateNamedPipeW(L"\\\\.\\pipe\\" PREFIX L"beta", PIPE_ACCESS_INBOUND,
                            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512, 512, 0, NULL);
    if (Alpha[0] == INVALID_HANDLE_VALUE || Alpha[1] == INVALID_HANDLE_VALUE || Beta == INVALID_HANDLE_VALUE)
    {
        skip("Failed to create the pipes: %lu\n", GetLastError());
        return;
    }

    Directory = OpenPipeDirectory();
    if (Directory)
    {
        TestDirectoryInformation(Directory);
        CloseHandle(Directory);

        /* a handle of its own starts with its own pattern; a handle to a pipe instance is no directory (STATUS_INVALID_PARAMETER) */
        Directory = OpenPipeDirectory();
        if (Directory)
        {
            static BYTE Buffer[1024];
            NTSTATUS Status;

            Status = Query(Directory, FileDirectoryInformation, PREFIX L"nothing", FALSE, TRUE, Buffer, sizeof(Buffer), NULL);
            ok_hex(Status, STATUS_NO_MORE_FILES);
            CloseHandle(Directory);
        }
        Directory = OpenPipeDirectory();
        if (Directory)
        {
            TestOtherClasses(Directory);
            CloseHandle(Directory);
        }
    }

    {
        static BYTE Buffer[1024];
        NTSTATUS Status = Query(Alpha[0], FileDirectoryInformation, NULL, FALSE, TRUE, Buffer, sizeof(Buffer), NULL);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
    }

    TestFindFile();
    TestRootInformation();

    for (i = 0; i < 2; i++)
        CloseHandle(Alpha[i]);
    CloseHandle(Beta);
}
