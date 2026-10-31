/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQueryInformationFile
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 *              Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define ntv6(x) (LOBYTE(LOWORD(GetVersion())) >= 6 ? (x) : 0)

#define PATTERN 0x55

static
ULONG_PTR
PatternValue(void)
{
    ULONG_PTR Value;
    RtlFillMemory(&Value, sizeof(Value), PATTERN);
    return Value;
}

/* One class: a buffer one byte short is refused and nothing is written (not even the IO_STATUS_BLOCK),
 * the exact size succeeds with that Information and writes no further. */
static
VOID
CheckClass(
    HANDLE Handle,
    ULONG Class,
    ULONG Size,
    PUCHAR Buffer)
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    RtlFillMemory(Buffer, 512, PATTERN);
    RtlFillMemory(&Iosb, sizeof(Iosb), PATTERN);
    Status = NtQueryInformationFile(Handle, &Iosb, Buffer, Size - 1, Class);
    ok(Status == STATUS_INFO_LENGTH_MISMATCH, "class %lu: short buffer: 0x%lx\n", Class, Status);
    ok(Iosb.Status == 0x55555555 && Iosb.Information == PatternValue(),
       "class %lu: IoStatusBlock (0x%lx, %Iu) after a refused request\n", Class, Iosb.Status, Iosb.Information);
    ok(Buffer[0] == PATTERN, "class %lu: buffer written by a refused request\n", Class);

    RtlFillMemory(Buffer, 512, PATTERN);
    Status = NtQueryInformationFile(Handle, &Iosb, Buffer, Size, Class);
    ok(Status == STATUS_SUCCESS, "class %lu: 0x%lx\n", Class, Status);
    ok(Iosb.Status == STATUS_SUCCESS, "class %lu: IoStatusBlock.Status = 0x%lx\n", Class, Iosb.Status);
    ok(Iosb.Information == Size, "class %lu: Information = %Iu, expected %lu\n", Class, Iosb.Information, Size);
    ok(Buffer[Size] == PATTERN, "class %lu: wrote past %lu bytes\n", Class, Size);
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
VOID
Test_Layouts(void)
{
    static const struct { ULONG Class; ULONG Size; } Fixed[] =
    {
        { FileBasicInformation, 40 }, { FileStandardInformation, 24 }, { FileInternalInformation, 8 },
        { FileEaInformation, 4 }, { FileAccessInformation, 4 }, { FilePositionInformation, 8 },
        { FileModeInformation, 4 }, { FileAlignmentInformation, 4 }, { FileNetworkOpenInformation, 56 },
        { FileAttributeTagInformation, 8 },
    };
    WCHAR TempPath[MAX_PATH], TempFile[MAX_PATH], Pipe[64], *Base;
    static UCHAR Data[5000];
    UCHAR Buffer[512 + 16];
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    HANDLE File, Dir, Server;
    DWORD Written;
    ULONG i, Name;

    GetTempPathW(MAX_PATH, TempPath);
    GetTempFileNameW(TempPath, L"kqi", 0, TempFile);
    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;
    RtlFillMemory(Data, sizeof(Data), 'A');
    ok(WriteFile(File, Data, sizeof(Data), &Written, NULL), "WriteFile failed, error %lu\n", GetLastError());

    for (i = 0; i < RTL_NUMBER_OF(Fixed); i++)
        CheckClass(File, Fixed[i].Class, Fixed[i].Size, Buffer);

    /* the 64-bit fields sit at multiples of 8 in a record the 32-bit caller reads the same way */
    Status = NtQueryInformationFile(File, &Iosb, Buffer, 24, FileStandardInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Get64(Buffer, 0) >= 5000, "AllocationSize = %I64d\n", Get64(Buffer, 0));
    ok(Get64(Buffer, 8) == 5000, "EndOfFile = %I64d\n", Get64(Buffer, 8));
    ok(Get32(Buffer, 16) == 1, "NumberOfLinks = %lu\n", Get32(Buffer, 16));
    ok(Buffer[20] == 0 && Buffer[21] == 0, "DeletePending %u, Directory %u\n", Buffer[20], Buffer[21]);
    Status = NtQueryInformationFile(File, &Iosb, Buffer, 56, FileNetworkOpenInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Get64(Buffer, 0) != 0 && Get64(Buffer, 16) != 0, "times: 0x%I64x, 0x%I64x\n", Get64(Buffer, 0), Get64(Buffer, 16));
    ok(Get64(Buffer, 32) >= 5000, "AllocationSize = %I64d\n", Get64(Buffer, 32));
    ok(Get64(Buffer, 40) == 5000, "EndOfFile = %I64d\n", Get64(Buffer, 40));
    ok(Get32(Buffer, 48) & FILE_ATTRIBUTE_ARCHIVE, "FileAttributes = 0x%lx\n", Get32(Buffer, 48));
    Status = NtQueryInformationFile(File, &Iosb, Buffer, 4, FileModeInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Get32(Buffer, 0) == FILE_SYNCHRONOUS_IO_NONALERT, "Mode = 0x%lx\n", Get32(Buffer, 0));
    Status = NtQueryInformationFile(File, &Iosb, Buffer, 8, FilePositionInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Get64(Buffer, 0) == 5000, "CurrentByteOffset = %I64d\n", Get64(Buffer, 0));

    /* FileNameInformation: the length first, then the name; a short buffer gets the length and BUFFER_OVERFLOW */
    RtlFillMemory(Buffer, sizeof(Buffer), PATTERN);
    RtlFillMemory(&Iosb, sizeof(Iosb), PATTERN);
    Status = NtQueryInformationFile(File, &Iosb, Buffer, 8, FileNameInformation);
    ok_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok(Iosb.Information == 8, "Information = %Iu\n", Iosb.Information);
    Name = Get32(Buffer, 0);
    ok(Name > 8 && !(Name & 1) && Name < sizeof(Buffer) - 4, "FileNameLength = %lu\n", Name);
    RtlFillMemory(Buffer, sizeof(Buffer), PATTERN);
    Status = NtQueryInformationFile(File, &Iosb, Buffer, sizeof(Buffer) - 16, FileNameInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Iosb.Information == 4 + Get32(Buffer, 0), "Information = %Iu, FileNameLength = %lu\n", Iosb.Information, Get32(Buffer, 0));
    ok(Get32(Buffer, 0) == Name, "FileNameLength = %lu, expected %lu\n", Get32(Buffer, 0), Name);
    Base = wcsrchr(TempFile, L'\\');
    ok(Name >= wcslen(Base) * sizeof(WCHAR) &&
       !_wcsnicmp((WCHAR *)(Buffer + 4 + Name) - wcslen(Base), Base, wcslen(Base)),
       "the name does not end in %ls\n", Base);
    /* FileAllInformation is those classes in sequence, and the name last */
    Status = NtQueryInformationFile(File, &Iosb, Buffer, sizeof(Buffer) - 16, FileAllInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Get64(Buffer, 48) == 5000, "EndOfFile = %I64d\n", Get64(Buffer, 48));
    ok(Get64(Buffer, 80) == 5000, "CurrentByteOffset = %I64d\n", Get64(Buffer, 80));
    ok(Get32(Buffer, 88) == FILE_SYNCHRONOUS_IO_NONALERT, "Mode = 0x%lx\n", Get32(Buffer, 88));
    ok(Get32(Buffer, 96) == Name, "FileNameLength = %lu, expected %lu\n", Get32(Buffer, 96), Name);
    ok(Iosb.Information == 100 + Name, "Information = %Iu, expected %lu\n", Iosb.Information, 100 + Name);
    CloseHandle(File);

    /* a directory */
    Dir = CreateFileW(TempPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(Dir != INVALID_HANDLE_VALUE, "opening the temp directory failed, error %lu\n", GetLastError());
    if (Dir != INVALID_HANDLE_VALUE)
    {
        Status = NtQueryInformationFile(Dir, &Iosb, Buffer, 24, FileStandardInformation);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Buffer[21] == 1, "Directory = %u\n", Buffer[21]);
        CheckClass(Dir, FileBasicInformation, 40, Buffer);
        Status = NtQueryInformationFile(Dir, &Iosb, Buffer, 56, FileNetworkOpenInformation);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Get32(Buffer, 48) & FILE_ATTRIBUTE_DIRECTORY, "FileAttributes = 0x%lx\n", Get32(Buffer, 48));
        CloseHandle(Dir);
    }

    /* a pipe: the classes that are only for pipes */
    StringCbPrintfW(Pipe, sizeof(Pipe), L"\\\\.\\pipe\\kqi_%lu", GetCurrentProcessId());
    Server = CreateNamedPipeW(Pipe, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed, error %lu\n", GetLastError());
    if (Server != INVALID_HANDLE_VALUE)
    {
        CheckClass(Server, FilePipeInformation, 8, Buffer);
        CheckClass(Server, FilePipeLocalInformation, 40, Buffer);
        Status = NtQueryInformationFile(Server, &Iosb, Buffer, 40, FilePipeLocalInformation);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Get32(Buffer, 4) == FILE_PIPE_FULL_DUPLEX, "NamedPipeConfiguration = %lu\n", Get32(Buffer, 4));
        ok(Get32(Buffer, 8) == 1, "MaximumInstances = %lu\n", Get32(Buffer, 8));
        ok(Get32(Buffer, 36) == FILE_PIPE_SERVER_END, "NamedPipeEnd = %lu\n", Get32(Buffer, 36));
        /* not for a file */
        Status = NtQueryInformationFile(Server, &Iosb, Buffer, 40, FileStandardInformation);
        ok(NT_SUCCESS(Status) || Status == STATUS_INVALID_PARAMETER, "FileStandardInformation on a pipe: 0x%lx\n", Status);
        CloseHandle(Server);
    }
}

START_TEST(NtQueryInformationFile)
{
    NTSTATUS Status;

    Status = NtQueryInformationFile(NULL, NULL, NULL, 0, 0);
    ok(Status == STATUS_INVALID_INFO_CLASS ||
       ntv6(Status == STATUS_NOT_IMPLEMENTED), "Status = %lx\n", Status);

    Status = NtQueryInformationFile(NULL, NULL, NULL, 0, 0x80000000);
    ok(Status == STATUS_INVALID_INFO_CLASS ||
       ntv6(Status == STATUS_NOT_IMPLEMENTED), "Status = %lx\n", Status);

    /* Get the full path of the current executable */
    CHAR Path[MAX_PATH];
    DWORD Length = GetModuleFileNameA(NULL, Path, _countof(Path));
    ok(Length != 0, "GetModuleFileNameA failed\n");
    if (Length == 0)
        return;

    /* Open the file */
    HANDLE hFile = CreateFileA(Path,
                               GENERIC_READ,
                               FILE_SHARE_READ,
                               NULL,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileA failed\n");
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    /* Query FileEndOfFileInformation */
    FILE_END_OF_FILE_INFORMATION EndOfFileInformation;
    EndOfFileInformation.EndOfFile.QuadPart = 0xdeaddead;
    Status = NtQueryInformationFile(hFile,
                                    NULL,
                                    &EndOfFileInformation,
                                    sizeof(EndOfFileInformation),
                                    FileEndOfFileInformation);
    ok(Status == STATUS_INVALID_INFO_CLASS ||
       ntv6(Status == STATUS_NOT_IMPLEMENTED), "Status = %lx\n", Status);
    ok(EndOfFileInformation.EndOfFile.QuadPart == 0xdeaddead, "EndOfFile is modified\n");

    CloseHandle(hFile);

    Test_Layouts();
}
