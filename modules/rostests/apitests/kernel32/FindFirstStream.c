/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for FindFirstStreamW/FindNextStreamW and FileStreamInformation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef FILE_NAMED_STREAMS
#define FILE_NAMED_STREAMS 0x00040000
#endif

#define MANY_STREAMS 200
#define NAMELEN 64

static WCHAR TestDir[MAX_PATH];

static void
MakePath(PWCHAR Out, PCWSTR Name)
{
    StringCbPrintfW(Out, MAX_PATH * sizeof(WCHAR), L"%s\\%s", TestDir, Name);
}

static BOOL
WriteTo(PCWSTR Path, const char *Data, DWORD Disposition)
{
    DWORD Written;
    HANDLE File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, Disposition, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    if (Data)
        WriteFile(File, Data, (DWORD)strlen(Data), &Written, NULL);
    CloseHandle(File);
    return TRUE;
}

/* Collects every stream into Names/Sizes; returns the count, or -1 with *Error set. */
static int
Enumerate(PCWSTR Path, WCHAR Names[][NAMELEN], LONGLONG *Sizes, int Max, DWORD *Error)
{
    WIN32_FIND_STREAM_DATA Data;
    HANDLE Find;
    int Count = 0;
    DWORD Last;

    SetLastError(0xdeadbeef);
    Find = FindFirstStreamW(Path, FindStreamInfoStandard, &Data, 0);
    if (Find == INVALID_HANDLE_VALUE)
    {
        *Error = GetLastError();
        return -1;
    }
    do
    {
        if (Count < Max)
        {
            StringCbCopyW(Names[Count], sizeof(Names[Count]), Data.cStreamName);
            Sizes[Count] = Data.StreamSize.QuadPart;
        }
        Count++;
        SetLastError(0xdeadbeef);
    } while (FindNextStreamW(Find, &Data));
    Last = GetLastError();
    ok(Last == ERROR_HANDLE_EOF, "FindNextStreamW ended with error %lu\n", Last);
    FindClose(Find);
    *Error = 0;
    return Count;
}

static int
IndexOf(WCHAR Names[][NAMELEN], int Count, PCWSTR Name)
{
    int i;
    for (i = 0; i < Count && i < 8; i++)
        if (!wcscmp(Names[i], Name))
            return i;
    return -1;
}

static void
TestFileStreams(void)
{
    static WCHAR Names[MANY_STREAMS + 8][NAMELEN];
    static LONGLONG Sizes[MANY_STREAMS + 8];
    WCHAR File[MAX_PATH], Path[MAX_PATH];
    DWORD Error;
    int Count, Index;

    MakePath(File, L"f.txt");
    ok(WriteTo(File, "hello", CREATE_ALWAYS), "create file: %lu\n", GetLastError());
    StringCbPrintfW(Path, sizeof(Path), L"%s:s1", File);
    ok(WriteTo(Path, "alt data", CREATE_ALWAYS), "create s1: %lu\n", GetLastError());
    StringCbPrintfW(Path, sizeof(Path), L"%s:s2", File);
    ok(WriteTo(Path, NULL, CREATE_ALWAYS), "create s2: %lu\n", GetLastError());

    Count = Enumerate(File, Names, Sizes, 8, &Error);
    ok_int(Count, 3);
    ok_wstr(Names[0], L"::$DATA");
    ok(Sizes[0] == 5, "unnamed size %I64d\n", Sizes[0]);
    Index = IndexOf(Names, Count, L":s1:$DATA");
    ok(Index > 0, "no :s1:$DATA\n");
    if (Index > 0)
        ok(Sizes[Index] == 8, "s1 size %I64d\n", Sizes[Index]);
    Index = IndexOf(Names, Count, L":s2:$DATA");
    ok(Index > 0, "no :s2:$DATA\n");
    if (Index > 0)
        ok(Sizes[Index] == 0, "s2 size %I64d\n", Sizes[Index]);

    /* a stream name in the path still lists every stream of the file */
    StringCbPrintfW(Path, sizeof(Path), L"%s:s1", File);
    Count = Enumerate(Path, Names, Sizes, 8, &Error);
    ok_int(Count, 3);
    ok_wstr(Names[0], L"::$DATA");

    /* a stream that does not exist */
    StringCbPrintfW(Path, sizeof(Path), L"%s:nope", File);
    Count = Enumerate(Path, Names, Sizes, 8, &Error);
    ok_int(Count, -1);
    ok_int(Error, ERROR_FILE_NOT_FOUND);

    /* truncating a stream keeps it listed with size 0 */
    StringCbPrintfW(Path, sizeof(Path), L"%s:s1", File);
    ok(WriteTo(Path, NULL, TRUNCATE_EXISTING), "truncate s1: %lu\n", GetLastError());
    Count = Enumerate(File, Names, Sizes, 8, &Error);
    ok_int(Count, 3);
    Index = IndexOf(Names, Count, L":s1:$DATA");
    if (Index > 0)
        ok(Sizes[Index] == 0, "truncated s1 size %I64d\n", Sizes[Index]);

    /* deleting a stream removes it from the list, and only that stream */
    ok(DeleteFileW(Path), "delete s1: %lu\n", GetLastError());
    Count = Enumerate(File, Names, Sizes, 8, &Error);
    ok_int(Count, 2);
    ok(IndexOf(Names, Count, L":s1:$DATA") < 0, "s1 still listed\n");
    ok(IndexOf(Names, Count, L":s2:$DATA") > 0, "s2 lost\n");

    DeleteFileW(File);
}

static void
TestDirectoryStreams(void)
{
    static WCHAR Names[8][NAMELEN];
    static LONGLONG Sizes[8];
    WCHAR Dir[MAX_PATH], Path[MAX_PATH];
    DWORD Error;
    int Count;

    MakePath(Dir, L"d");
    RemoveDirectoryW(Dir);
    ok(CreateDirectoryW(Dir, NULL), "create dir: %lu\n", GetLastError());

    /* no streams at all: the end of the file, not an access failure */
    Count = Enumerate(Dir, Names, Sizes, 8, &Error);
    ok_int(Count, -1);
    ok_int(Error, ERROR_HANDLE_EOF);

    StringCbPrintfW(Path, sizeof(Path), L"%s:s1", Dir);
    ok(WriteTo(Path, "dir alt", CREATE_ALWAYS), "create dir:s1: %lu\n", GetLastError());
    Count = Enumerate(Dir, Names, Sizes, 8, &Error);
    ok_int(Count, 1);
    ok_wstr(Names[0], L":s1:$DATA");
    ok(Sizes[0] == 7, "dir s1 size %I64d\n", Sizes[0]);

    /* through the stream's own path */
    Count = Enumerate(Path, Names, Sizes, 8, &Error);
    ok_int(Count, 1);
    ok_wstr(Names[0], L":s1:$DATA");

    ok(DeleteFileW(Path), "delete dir:s1: %lu\n", GetLastError());
    Count = Enumerate(Dir, Names, Sizes, 8, &Error);
    ok_int(Count, -1);
    ok_int(Error, ERROR_HANDLE_EOF);

    RemoveDirectoryW(Dir);
}

static void
TestManyStreams(void)
{
    static WCHAR Names[MANY_STREAMS + 8][NAMELEN];
    static LONGLONG Sizes[MANY_STREAMS + 8];
    WCHAR File[MAX_PATH], Path[MAX_PATH];
    DWORD Error;
    int i, Count;

    /* more than the first buffer of the enumeration holds */
    MakePath(File, L"many.txt");
    ok(WriteTo(File, "x", CREATE_ALWAYS), "create file: %lu\n", GetLastError());
    for (i = 0; i < MANY_STREAMS; i++)
    {
        StringCbPrintfW(Path, sizeof(Path), L"%s:stream_with_a_long_name_%03d", File, i);
        if (!WriteTo(Path, "y", CREATE_ALWAYS))
        {
            ok(0, "create stream %d: %lu\n", i, GetLastError());
            break;
        }
    }
    Count = Enumerate(File, Names, Sizes, MANY_STREAMS + 8, &Error);
    ok_int(Count, MANY_STREAMS + 1);
    DeleteFileW(File);
}

static void
TestParameters(void)
{
    WIN32_FIND_STREAM_DATA Data;
    WCHAR File[MAX_PATH];
    HANDLE Find;

    MakePath(File, L"p.txt");
    ok(WriteTo(File, "x", CREATE_ALWAYS), "create file: %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    Find = FindFirstStreamW(File, FindStreamInfoStandard, &Data, 1);
    ok(Find == INVALID_HANDLE_VALUE, "flags 1: %p\n", Find);
    ok_int(GetLastError(), ERROR_INVALID_PARAMETER);

    SetLastError(0xdeadbeef);
    Find = FindFirstStreamW(File, (STREAM_INFO_LEVELS)1, &Data, 0);
    ok(Find == INVALID_HANDLE_VALUE, "level 1: %p\n", Find);
    ok_int(GetLastError(), ERROR_INVALID_PARAMETER);

    DeleteFileW(File);
}

/* The native query: a directory without streams is a success with nothing returned, a small buffer a warning. */
static void
TestNativeQuery(void)
{
    typedef NTSTATUS (NTAPI *PFN_QUERY)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    PFN_QUERY Query = (PFN_QUERY)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
    WCHAR Dir[MAX_PATH], File[MAX_PATH], Path[MAX_PATH];
    IO_STATUS_BLOCK Io;
    BYTE Buffer[512];
    NTSTATUS Status;
    HANDLE Handle;

    MakePath(Dir, L"nd");
    RemoveDirectoryW(Dir);
    CreateDirectoryW(Dir, NULL);
    Handle = CreateFileW(Dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "open dir: %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
    {
        Io.Information = 0xdeadbeef;
        Status = Query(Handle, &Io, Buffer, sizeof(Buffer), (FILE_INFORMATION_CLASS)22);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Io.Information == 0, "information %lu\n", (ULONG)Io.Information);
        CloseHandle(Handle);
    }
    RemoveDirectoryW(Dir);

    MakePath(File, L"nq.txt");
    ok(WriteTo(File, "hello", CREATE_ALWAYS), "create file: %lu\n", GetLastError());
    StringCbPrintfW(Path, sizeof(Path), L"%s:longer_stream_name", File);
    ok(WriteTo(Path, "x", CREATE_ALWAYS), "create stream: %lu\n", GetLastError());

    /* a handle to the unnamed stream, one to a named stream, and no access mask but attributes */
    Handle = CreateFileW(File, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "open file: %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
    {
        Status = Query(Handle, &Io, Buffer, 8, (FILE_INFORMATION_CLASS)22);
        ok(Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_INFO_LENGTH_MISMATCH,
           "tiny buffer: %08lx\n", (ULONG)Status);
        Status = Query(Handle, &Io, Buffer, sizeof(Buffer), (FILE_INFORMATION_CLASS)22);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Io.Information > 0, "information %lu\n", (ULONG)Io.Information);
        CloseHandle(Handle);
    }
    Handle = CreateFileW(Path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "open stream: %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
    {
        Status = Query(Handle, &Io, Buffer, sizeof(Buffer), (FILE_INFORMATION_CLASS)22);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Io.Information > 0, "stream handle information %lu\n", (ULONG)Io.Information);
        CloseHandle(Handle);
    }
    DeleteFileW(File);
}

START_TEST(FindFirstStream)
{
    WCHAR Root[4] = L"C:\\";
    WCHAR Temp[MAX_PATH];
    DWORD Flags = 0;

    GetTempPathW(RTL_NUMBER_OF(Temp), Temp);
    Root[0] = Temp[0];
    if (!GetVolumeInformationW(Root, NULL, 0, NULL, NULL, &Flags, NULL, 0) || !(Flags & FILE_NAMED_STREAMS))
    {
        skip("The volume %ls has no named streams\n", Root);
        return;
    }

    StringCbPrintfW(TestDir, sizeof(TestDir), L"%sfstream%lu", Temp, GetCurrentProcessId());
    ok(CreateDirectoryW(TestDir, NULL), "create test dir: %lu\n", GetLastError());

    TestFileStreams();
    TestDirectoryStreams();
    TestManyStreams();
    TestParameters();
    TestNativeQuery();

    RemoveDirectoryW(TestDir);
}
