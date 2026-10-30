/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Named Pipe File System (NPFS): attribute queries of a pipe and of the pipe directory
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <ndk/ntndk.h>

#define PIPE_NAME L"NpfsAttrTest_pipe"

static NTSTATUS
QueryBasic(PCWSTR Path, PFILE_BASIC_INFORMATION Info)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    RtlFillMemory(Info, sizeof(*Info), 0x55);
    return NtQueryAttributesFile(&Attributes, Info);
}

static NTSTATUS
QueryFull(PCWSTR Path, PFILE_NETWORK_OPEN_INFORMATION Info)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    RtlFillMemory(Info, sizeof(*Info), 0x55);
    return NtQueryFullAttributesFile(&Attributes, Info);
}

/* A query by name opens the pipe, which uses up a listening instance: each one gets a fresh pipe */
static HANDLE
CreateTestPipe(void)
{
    HANDLE Pipe = CreateNamedPipeW(L"\\\\.\\pipe\\" PIPE_NAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 3, 4096, 4096, 0, NULL);
    ok(Pipe != INVALID_HANDLE_VALUE, "CreateNamedPipe %lu\n", GetLastError());
    return Pipe;
}

static void
TestPath(PCWSTR Path, BOOLEAN NeedPipe, NTSTATUS ExpectedStatus, ULONG ExpectedAttributes, LONGLONG ExpectedAllocation)
{
    FILE_BASIC_INFORMATION Basic;
    FILE_NETWORK_OPEN_INFORMATION Full;
    NTSTATUS Status;
    HANDLE Pipe;

    Pipe = NeedPipe ? CreateTestPipe() : NULL;
    Status = QueryBasic(Path, &Basic);
    ok(Status == ExpectedStatus, "NtQueryAttributesFile(%ls) = %lx, expected %lx\n", Path, Status, ExpectedStatus);
    if (NT_SUCCESS(Status))
    {
        ok(Basic.FileAttributes == ExpectedAttributes, "%ls basic attributes %lx\n", Path, Basic.FileAttributes);
        ok(Basic.CreationTime.QuadPart == 0 && Basic.LastWriteTime.QuadPart == 0, "%ls basic times\n", Path);
    }
    if (Pipe)
        CloseHandle(Pipe);

    Pipe = NeedPipe ? CreateTestPipe() : NULL;
    Status = QueryFull(Path, &Full);
    ok(Status == ExpectedStatus, "NtQueryFullAttributesFile(%ls) = %lx, expected %lx\n", Path, Status, ExpectedStatus);
    if (NT_SUCCESS(Status))
    {
        ok(Full.FileAttributes == ExpectedAttributes, "%ls full attributes %lx\n", Path, Full.FileAttributes);
        ok(Full.EndOfFile.QuadPart == 0 && Full.AllocationSize.QuadPart == ExpectedAllocation, "%ls sizes %I64d %I64d\n", Path,
           Full.EndOfFile.QuadPart, Full.AllocationSize.QuadPart);
        ok(Full.CreationTime.QuadPart == 0 && Full.LastAccessTime.QuadPart == 0 &&
           Full.LastWriteTime.QuadPart == 0 && Full.ChangeTime.QuadPart == 0, "%ls times not zero\n", Path);
    }
    if (Pipe)
        CloseHandle(Pipe);
}

static void
TestWin32(void)
{
    WIN32_FILE_ATTRIBUTE_DATA Data;
    HANDLE Pipe;
    DWORD Attr, Error;
    BOOL Ret;

    /* The first query of a listening pipe works and uses the instance up: the next one finds it busy */
    Pipe = CreateTestPipe();
    RtlFillMemory(&Data, sizeof(Data), 0x55);
    SetLastError(0xdeadbeef);
    Ret = GetFileAttributesExW(L"\\\\.\\pipe\\" PIPE_NAME, GetFileExInfoStandard, &Data);
    Error = GetLastError();
    ok(Ret, "GetFileAttributesEx(pipe) failed, error %lu\n", Error);
    ok(Data.dwFileAttributes == FILE_ATTRIBUTE_NORMAL, "GetFileAttributesEx(pipe) attributes %lx\n", Data.dwFileAttributes);
    ok(Data.nFileSizeLow == 0 && Data.nFileSizeHigh == 0, "GetFileAttributesEx(pipe) size\n");
    SetLastError(0xdeadbeef);
    Ret = GetFileAttributesExW(L"\\\\.\\pipe\\" PIPE_NAME, GetFileExInfoStandard, &Data);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_PIPE_BUSY, "second GetFileAttributesEx(pipe) %d, error %lu\n", Ret, Error);
    CloseHandle(Pipe);

    Pipe = CreateTestPipe();
    SetLastError(0xdeadbeef);
    Attr = GetFileAttributesW(L"\\\\.\\pipe\\" PIPE_NAME);
    ok(Attr == FILE_ATTRIBUTE_NORMAL, "GetFileAttributes(pipe) %lx, error %lu\n", Attr, GetLastError());
    CloseHandle(Pipe);

    Pipe = CreateTestPipe();
    Attr = GetFileAttributesW(L"\\\\.\\pipe\\");
    ok(Attr == FILE_ATTRIBUTE_DIRECTORY, "GetFileAttributes(pipe dir) %lx, error %lu\n", Attr, GetLastError());
    RtlFillMemory(&Data, sizeof(Data), 0x55);
    SetLastError(0xdeadbeef);
    Ret = GetFileAttributesExW(L"\\\\.\\pipe\\", GetFileExInfoStandard, &Data);
    Error = GetLastError();
    ok(Ret, "GetFileAttributesEx(pipe dir) failed, error %lu\n", Error);
    ok(Data.dwFileAttributes == FILE_ATTRIBUTE_DIRECTORY, "GetFileAttributesEx(pipe dir) attributes %lx\n", Data.dwFileAttributes);
    CloseHandle(Pipe);

    SetLastError(0xdeadbeef);
    Attr = GetFileAttributesW(L"\\\\.\\pipe\\" PIPE_NAME L"_missing");
    ok(Attr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
       "GetFileAttributes(missing) %lx, error %lu\n", Attr, GetLastError());
}

static void
TestHandleQuery(HANDLE Handle, PCSTR What, NTSTATUS ExpectedStatus, ULONG ExpectedAttributes)
{
    IO_STATUS_BLOCK Iosb;
    FILE_NETWORK_OPEN_INFORMATION Info;
    FILE_ATTRIBUTE_TAG_INFORMATION Tag;
    NTSTATUS Status;

    RtlFillMemory(&Info, sizeof(Info), 0x55);
    Status = NtQueryInformationFile(Handle, &Iosb, &Info, sizeof(Info), FileNetworkOpenInformation);
    ok(Status == ExpectedStatus, "%s: FileNetworkOpenInformation %lx, expected %lx\n", What, Status, ExpectedStatus);
    if (NT_SUCCESS(Status))
    {
        ok(Iosb.Information == sizeof(Info), "%s: Information %Iu\n", What, Iosb.Information);
        ok(Info.FileAttributes == ExpectedAttributes, "%s: attributes %lx\n", What, Info.FileAttributes);
    }

    Status = NtQueryInformationFile(Handle, &Iosb, &Info, sizeof(Info) - 1, FileNetworkOpenInformation);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    RtlFillMemory(&Tag, sizeof(Tag), 0x55);
    Status = NtQueryInformationFile(Handle, &Iosb, &Tag, sizeof(Tag), FileAttributeTagInformation);
    ok(Status == ExpectedStatus, "%s: FileAttributeTagInformation %lx, expected %lx\n", What, Status, ExpectedStatus);
}

START_TEST(NpfsQueryAttributes)
{
    HANDLE Server, Client, Directory;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    /* By name: a pipe is a normal file, the directory a directory */
    TestPath(L"\\??\\pipe\\" PIPE_NAME, TRUE, STATUS_SUCCESS, FILE_ATTRIBUTE_NORMAL, 8192);
    TestPath(L"\\Device\\NamedPipe\\" PIPE_NAME, TRUE, STATUS_SUCCESS, FILE_ATTRIBUTE_NORMAL, 8192);
    TestPath(L"\\??\\pipe\\", FALSE, STATUS_SUCCESS, FILE_ATTRIBUTE_DIRECTORY, 0);
    TestPath(L"\\Device\\NamedPipe\\", FALSE, STATUS_SUCCESS, FILE_ATTRIBUTE_DIRECTORY, 0);
    TestPath(L"\\??\\pipe\\" PIPE_NAME L"_missing", FALSE, STATUS_OBJECT_NAME_NOT_FOUND, 0, 0);

    TestWin32();

    /* By handle: not supported for a pipe, but for the directory */
    Server = CreateTestPipe();
    TestHandleQuery(Server, "server", STATUS_INVALID_PARAMETER, 0);

    RtlInitUnicodeString(&Name, L"\\Device\\NamedPipe\\");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Directory = NULL;
    Status = NtCreateFile(&Directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &Attributes, &Iosb, NULL, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE, NULL, 0);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        TestHandleQuery(Directory, "directory", STATUS_INVALID_PARAMETER, 0);
        CloseHandle(Directory);
    }

    Client = CreateFileW(L"\\\\.\\pipe\\" PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Client != INVALID_HANDLE_VALUE, "CreateFile %lu\n", GetLastError());
    if (Client != INVALID_HANDLE_VALUE)
    {
        TestHandleQuery(Client, "client", STATUS_INVALID_PARAMETER, 0);
        CloseHandle(Client);
    }
    CloseHandle(Server);
}
