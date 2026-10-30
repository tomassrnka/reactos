/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     CopyFileW, CopyFileExW, CopyFile2 and MoveFileEx carry the named streams of a file
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Everything expected here was measured on Windows 10 22H2 (build 19045), and the test passes there.
 */

#include "precomp.h"

#ifndef COPY_FILE_NO_BUFFERING
#define COPY_FILE_NO_BUFFERING 0x00001000
#endif
#ifndef COPY_FILE_ALLOW_DECRYPTED_DESTINATION
#define COPY_FILE_ALLOW_DECRYPTED_DESTINATION 0x00000008
#endif

#ifndef FILE_NAMED_STREAMS
#define FILE_NAMED_STREAMS 0x00040000
#endif

typedef struct
{
    ULONG NextEntryOffset;
    UCHAR Flags;
    UCHAR EaNameLength;
    USHORT EaValueLength;
    CHAR EaName[1];
} TEST_EA;

typedef HRESULT (WINAPI *FN_CopyFile2)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS *);
typedef NTSTATUS (NTAPI *FN_SetEa)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG);
typedef NTSTATUS (NTAPI *FN_QueryEa)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, BOOLEAN, PVOID, ULONG, PULONG, BOOLEAN);

#define SIZE_S1     15
#define SIZE_S2     200000
static const char DefaultData[] = "hello";
static const char S1Data[] = "stream one data";
static char S2Data[SIZE_S2];

static WCHAR TestDir[MAX_PATH];

static void MakePath(PWCHAR Out, PCWSTR Name)
{
    StringCbPrintfW(Out, MAX_PATH * sizeof(WCHAR), L"%s\\%s", TestDir, Name);
}

static BOOL WriteTo(PCWSTR Path, const void *Data, DWORD Size, DWORD Disposition)
{
    DWORD Written;
    HANDLE File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              Disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    if (Size)
        WriteFile(File, Data, Size, &Written, NULL);
    CloseHandle(File);
    return TRUE;
}

static BOOL WriteStream(PCWSTR File, PCWSTR Stream, const void *Data, DWORD Size)
{
    WCHAR Path[MAX_PATH];

    StringCbPrintfW(Path, sizeof(Path), L"%s%s", File, Stream);
    return WriteTo(Path, Data, Size, CREATE_ALWAYS);
}

/* A file of the default stream "hello", ":s1" and ":s2" (200000 bytes) */
static void MakeSource(PCWSTR Path)
{
    ok(WriteTo(Path, DefaultData, 5, CREATE_ALWAYS), "create %ls: %lu\n", Path, GetLastError());
    ok(WriteStream(Path, L":s1", S1Data, SIZE_S1), "create :s1: %lu\n", GetLastError());
    ok(WriteStream(Path, L":s2", S2Data, SIZE_S2), "create :s2: %lu\n", GetLastError());
}

/* The size of a stream ("" for the default one), or -1 when there is none */
static LONGLONG SizeOfStream(PCWSTR File, PCWSTR Stream)
{
    WCHAR Path[MAX_PATH];
    LARGE_INTEGER Size;
    HANDLE Handle;

    StringCbPrintfW(Path, sizeof(Path), L"%s%s", File, Stream);
    Handle = CreateFileW(Path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetFileSizeEx(Handle, &Size))
        Size.QuadPart = -1;
    CloseHandle(Handle);
    return Size.QuadPart;
}

static BOOL StreamHolds(PCWSTR File, PCWSTR Stream, const void *Data, DWORD Size)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    BYTE *Buffer;
    DWORD Read = 0;
    BOOL Same = FALSE;

    StringCbPrintfW(Path, sizeof(Path), L"%s%s", File, Stream);
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    Buffer = HeapAlloc(GetProcessHeap(), 0, Size + 1);
    if (Buffer && ReadFile(Handle, Buffer, Size + 1, &Read, NULL))
        Same = (Read == Size) && !memcmp(Buffer, Data, Size);
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    CloseHandle(Handle);
    return Same;
}

/* The names of the named streams of a file, in a list ":a:$DATA|:b:$DATA|" (the default stream left out) */
static void NamedStreams(PCWSTR File, WCHAR *List, int Chars)
{
    WIN32_FIND_STREAM_DATA Data;
    HANDLE Find;

    List[0] = UNICODE_NULL;
    Find = FindFirstStreamW(File, FindStreamInfoStandard, &Data, 0);
    if (Find == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (wcscmp(Data.cStreamName, L"::$DATA"))
        {
            StringCchCatW(List, Chars, Data.cStreamName);
            StringCchCatW(List, Chars, L"|");
        }
    } while (FindNextStreamW(Find, &Data));
    FindClose(Find);
}

static BOOL Exists(PCWSTR Path)
{
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

static void Remove(PCWSTR Path)
{
    SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(Path);
}

/* The whole result of a copy of the source made by MakeSource: the data of every stream and nothing more */
static void CheckFullCopy(const char *Label, PCWSTR Dest)
{
    WCHAR List[200];

    ok(StreamHolds(Dest, L"", DefaultData, 5), "%s: the default stream is wrong\n", Label);
    ok(StreamHolds(Dest, L":s1", S1Data, SIZE_S1), "%s: :s1 is wrong (size %I64d)\n", Label, SizeOfStream(Dest, L":s1"));
    ok(StreamHolds(Dest, L":s2", S2Data, SIZE_S2), "%s: :s2 is wrong (size %I64d)\n", Label, SizeOfStream(Dest, L":s2"));
    NamedStreams(Dest, List, RTL_NUMBER_OF(List));
    ok(!wcscmp(List, L":s1:$DATA|:s2:$DATA|"), "%s: the named streams are '%ls'\n", Label, List);
}

/* THE PROGRESS ROUTINE ******************************************************/

typedef struct
{
    DWORD Reason, Number;
    LONGLONG Total, TotalDone, StreamSize, StreamDone;
    BOOL Handles;
} CALL;

static CALL Calls[256];
static int CallCount;
static int ActionAt;            /* the call (1 based) that gets ActionToTake */
static DWORD ActionToTake;

static DWORD CALLBACK Progress(LARGE_INTEGER Total, LARGE_INTEGER TotalDone, LARGE_INTEGER StreamSize,
                               LARGE_INTEGER StreamDone, DWORD Number, DWORD Reason, HANDLE Source, HANDLE Dest,
                               LPVOID Data)
{
    if (CallCount < RTL_NUMBER_OF(Calls))
    {
        CALL *Call = &Calls[CallCount];

        Call->Reason = Reason;
        Call->Number = Number;
        Call->Total = Total.QuadPart;
        Call->TotalDone = TotalDone.QuadPart;
        Call->StreamSize = StreamSize.QuadPart;
        Call->StreamDone = StreamDone.QuadPart;
        Call->Handles = Source != NULL && Source != INVALID_HANDLE_VALUE && Dest != NULL && Dest != INVALID_HANDLE_VALUE;
    }
    CallCount++;
    if (ActionAt && CallCount == ActionAt)
        return ActionToTake;
    return PROGRESS_CONTINUE;
}

static void ResetCalls(int At, DWORD Action)
{
    CallCount = 0;
    ActionAt = At;
    ActionToTake = Action;
    memset(Calls, 0, sizeof(Calls));
}

/*
 * The calls of a copy of the streams of sizes Sizes[0..Count): the stream numbers count from 1 in the order the
 * file system lists them; every stream has one call for the switch to it, and one after each chunk (an empty
 * stream has none); the total is the size of all streams. The first chunk of a stream is Chunk bytes
 * (0: the stream in one piece).
 */
static void CheckCalls(const char *Label, const LONGLONG *Sizes, int Count, const LONGLONG *Chunks)
{
    LONGLONG Total = 0, Done = 0;
    int i, c = 0, n;

    for (i = 0; i < Count; i++)
        Total += Sizes[i];

    for (n = 1; n <= Count; n++)
    {
        LONGLONG StreamDone = 0, Size;
        int Index = -1;

        ok(c < CallCount && c < (int)RTL_NUMBER_OF(Calls), "%s: no call for the stream %d (%d calls)\n", Label, n, CallCount);
        if (c >= CallCount || c >= (int)RTL_NUMBER_OF(Calls))
            return;

        ok(Calls[c].Reason == CALLBACK_STREAM_SWITCH, "%s: call %d is not the switch to stream %d\n", Label, c + 1, n);
        ok(Calls[c].Number == (DWORD)n, "%s: call %d is for stream %lu, expected %d\n", Label, c + 1, Calls[c].Number, n);
        Size = Calls[c].StreamSize;
        for (i = 0; i < Count; i++)
        {
            if (Sizes[i] == Size)
                Index = i;
        }
        ok(Index >= 0, "%s: stream %d has size %I64d, not one of the streams\n", Label, n, Size);
        ok(Calls[c].Total == Total, "%s: call %d reports a total of %I64d, expected %I64d\n", Label, c + 1, Calls[c].Total, Total);
        ok(Calls[c].TotalDone == Done && Calls[c].StreamDone == 0, "%s: switch to stream %d at %I64d/%I64d\n",
           Label, n, Calls[c].TotalDone, Calls[c].StreamDone);
        ok(Calls[c].Handles, "%s: call %d has no handles\n", Label, c + 1);
        c++;

        while (StreamDone < Size && c < CallCount && c < (int)RTL_NUMBER_OF(Calls))
        {
            LONGLONG Step = Calls[c].StreamDone - StreamDone;

            ok(Calls[c].Reason == CALLBACK_CHUNK_FINISHED && Calls[c].Number == (DWORD)n,
               "%s: call %d is reason %lu of stream %lu, expected a chunk of stream %d\n", Label, c + 1, Calls[c].Reason, Calls[c].Number, n);
            ok(Calls[c].StreamSize == Size && Calls[c].Total == Total, "%s: call %d reports %I64d of %I64d\n", Label, c + 1, Calls[c].StreamSize, Calls[c].Total);
            ok(Step > 0 && Calls[c].TotalDone == Done + Calls[c].StreamDone, "%s: call %d is at %I64d/%I64d\n", Label, c + 1, Calls[c].TotalDone, Calls[c].StreamDone);
            if (Chunks && StreamDone == 0 && Index >= 0 && Chunks[Index])
            {
                ok(Step == Chunks[Index], "%s: the first chunk of stream %d is %I64d bytes, expected %I64d\n", Label, n, Step, Chunks[Index]);
            }
            StreamDone = Calls[c].StreamDone;
            c++;
        }
        ok(StreamDone == Size, "%s: stream %d ends at %I64d of %I64d\n", Label, n, StreamDone, Size);
        Done += Size;
    }
    ok(CallCount == c, "%s: %d calls, expected %d\n", Label, CallCount, c);
}

/* COPYFILE2 */
static FN_CopyFile2 pCopyFile2;
static int Copy2Streams[16];
static int Copy2StartCount, Copy2FinishCount;
static COPYFILE2_MESSAGE_ACTION CALLBACK Progress2(const COPYFILE2_MESSAGE *Message, PVOID Context)
{
    if (Message->Type == COPYFILE2_CALLBACK_STREAM_STARTED)
    {
        if (Copy2StartCount < (int)RTL_NUMBER_OF(Copy2Streams))
            Copy2Streams[Copy2StartCount] = Message->Info.StreamStarted.dwStreamNumber;
        Copy2StartCount++;
    }
    else if (Message->Type == COPYFILE2_CALLBACK_STREAM_FINISHED)
    {
        Copy2FinishCount++;
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

/* THE TESTS *****************************************************************/

static void TestCopyFile(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA SrcData, DstData;
    BOOL Ret;

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakeSource(Src);
    Remove(Dst);

    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Src, Dst, FALSE);
    ok(Ret, "CopyFileW failed: %lu\n", GetLastError());
    CheckFullCopy("CopyFileW", Dst);

    /* the copy has the write time of the source */
    ok(GetFileAttributesExW(Src, GetFileExInfoStandard, &SrcData) && GetFileAttributesExW(Dst, GetFileExInfoStandard, &DstData),
       "no attributes\n");
    ok(!memcmp(&SrcData.ftLastWriteTime, &DstData.ftLastWriteTime, sizeof(FILETIME)), "the write time is not that of the source\n");
    ok(SrcData.dwFileAttributes == DstData.dwFileAttributes, "attributes %#lx and %#lx\n", SrcData.dwFileAttributes, DstData.dwFileAttributes);
    ok(DstData.nFileSizeLow == 5, "the size of the copy is %lu\n", DstData.nFileSizeLow);

    /* the source is not changed */
    CheckFullCopy("the source", Src);
    Remove(Dst);
}

static void TestProgress(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH], Plain[MAX_PATH];
    LONGLONG Sizes[3] = { 5, SIZE_S1, SIZE_S2 };
    LONGLONG Chunks[3] = { 0, 0, 131072 };
    BOOL Ret;

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakePath(Plain, L"plain");
    MakeSource(Src);
    Remove(Dst);

    /* the streams are numbered from 1; the total is the size of all of them */
    ResetCalls(0, 0);
    SetLastError(0xdeadbeef);
    Ret = CopyFileExW(Src, Dst, Progress, NULL, NULL, 0);
    ok(Ret, "CopyFileExW failed: %lu\n", GetLastError());
    CheckFullCopy("CopyFileExW", Dst);
    CheckCalls("CopyFileExW", Sizes, 3, Chunks);
    if (CallCount == 7)
    {
        ok(Calls[0].Number == 1 && Calls[0].StreamSize == 5, "the default stream is not the first\n");
        ok(Calls[6].TotalDone == 5 + SIZE_S1 + SIZE_S2, "the end is at %I64d\n", Calls[6].TotalDone);
    }
    Remove(Dst);

    /* a file of one stream: the default stream is number 1, as always */
    ok(WriteTo(Plain, DefaultData, 5, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    ResetCalls(0, 0);
    Ret = CopyFileExW(Plain, Dst, Progress, NULL, NULL, 0);
    ok(Ret, "CopyFileExW failed: %lu\n", GetLastError());
    ok(CallCount == 2 && Calls[0].Reason == CALLBACK_STREAM_SWITCH && Calls[0].Number == 1 &&
       Calls[1].Reason == CALLBACK_CHUNK_FINISHED && Calls[1].TotalDone == 5 && Calls[1].Total == 5,
       "%d calls for a file of one stream\n", CallCount);
    Remove(Dst);
    Remove(Plain);

    /* cancel at each of the first calls: the copy is gone, and the error is ERROR_REQUEST_ABORTED */
    {
        int At;

        for (At = 1; At <= 5; At++)
        {
            ResetCalls(At, PROGRESS_CANCEL);
            SetLastError(0xdeadbeef);
            Ret = CopyFileExW(Src, Dst, Progress, NULL, NULL, 0);
            ok(!Ret && GetLastError() == ERROR_REQUEST_ABORTED, "cancel at call %d: ret %d, error %lu\n", At, Ret, GetLastError());
            ok(CallCount == At, "cancel at call %d: %d calls\n", At, CallCount);
            ok(!Exists(Dst), "cancel at call %d: the copy was left\n", At);
        }
    }

    /* "quiet": no more calls, the copy goes on to the end with all its streams */
    ResetCalls(2, PROGRESS_QUIET);
    Ret = CopyFileExW(Src, Dst, Progress, NULL, NULL, 0);
    ok(Ret, "quiet: %lu\n", GetLastError());
    ok(CallCount == 2, "quiet: %d calls\n", CallCount);
    CheckFullCopy("quiet", Dst);
    Remove(Dst);
    Remove(Src);
}

/* A file whose default stream and a named stream are empty, and one that is not */
static void TestEmptyStreams(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    LONGLONG Sizes[3] = { 0, 0, 3 };
    BOOL Ret;

    MakePath(Src, L"empty");
    MakePath(Dst, L"emptycopy");
    Remove(Dst);
    ok(WriteTo(Src, "", 0, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    ok(WriteStream(Src, L":s1", "abc", 3), "create :s1: %lu\n", GetLastError());
    ok(WriteStream(Src, L":e", "", 0), "create :e: %lu\n", GetLastError());

    ResetCalls(0, 0);
    Ret = CopyFileExW(Src, Dst, Progress, NULL, NULL, 0);
    ok(Ret, "CopyFileExW failed: %lu\n", GetLastError());
    ok(SizeOfStream(Dst, L"") == 0 && SizeOfStream(Dst, L":e") == 0 && StreamHolds(Dst, L":s1", "abc", 3),
       "the empty streams were not copied (%I64d, %I64d, %I64d)\n", SizeOfStream(Dst, L""), SizeOfStream(Dst, L":e"), SizeOfStream(Dst, L":s1"));
    /* an empty stream has the call for the switch to it and none for a chunk: three switches and one chunk */
    ok(CallCount == 4, "%d calls\n", CallCount);
    CheckCalls("empty streams", Sizes, 3, NULL);
    Remove(Dst);
    Remove(Src);
}

/* The size of the pieces: 128 KB up to 512 KB, 256 KB up to 2 MB, 512 KB up to 8 MB, 1 MB beyond */
static void TestChunkSizes(void)
{
    static const struct { DWORD Size; DWORD Chunk; } Cases[] =
    {
        { 1000, 1000 }, { 131072, 131072 }, { 131073, 131072 }, { 524288, 131072 }, { 524289, 262144 },
        { 2097152, 262144 }, { 2097153, 524288 }, { 8388608, 524288 }, { 8388609, 1048576 }
    };
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    BYTE *Data = HeapAlloc(GetProcessHeap(), 0, 8388609);
    int i;

    MakePath(Src, L"chunk");
    MakePath(Dst, L"chunkcopy");
    if (!Data)
    {
        skip("no memory\n");
        return;
    }
    memset(Data, 'C', 8388609);
    for (i = 0; i < (int)RTL_NUMBER_OF(Cases); i++)
    {
        BOOL Ret;

        Remove(Dst);
        ok(WriteTo(Src, Data, Cases[i].Size, CREATE_ALWAYS), "create: %lu\n", GetLastError());
        ResetCalls(0, 0);
        Ret = CopyFileExW(Src, Dst, Progress, NULL, NULL, 0);
        ok(Ret, "CopyFileExW failed for %lu bytes: %lu\n", Cases[i].Size, GetLastError());
        ok(CallCount >= 2 && Calls[1].StreamDone == Cases[i].Chunk, "%lu bytes: the first piece is %I64d, expected %lu\n",
           Cases[i].Size, CallCount >= 2 ? Calls[1].StreamDone : -1, Cases[i].Chunk);
        ok(SizeOfStream(Dst, L"") == Cases[i].Size, "%lu bytes: the copy has %I64d\n", Cases[i].Size, SizeOfStream(Dst, L""));
    }
    HeapFree(GetProcessHeap(), 0, Data);
    Remove(Dst);
    Remove(Src);
}

static void TestFlags(void)
{
    static const DWORD Flags[] = { COPY_FILE_ALLOW_DECRYPTED_DESTINATION, COPY_FILE_NO_BUFFERING, COPY_FILE_RESTARTABLE };
    static const char *Names[] = { "ALLOW_DECRYPTED_DESTINATION", "NO_BUFFERING", "RESTARTABLE" };
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    BOOL Ret;
    int i;

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakeSource(Src);
    Remove(Dst);

    for (i = 0; i < (int)RTL_NUMBER_OF(Flags); i++)
    {
        SetLastError(0xdeadbeef);
        Ret = CopyFileExW(Src, Dst, NULL, NULL, NULL, Flags[i]);
        ok(Ret, "%s: CopyFileExW failed: %lu\n", Names[i], GetLastError());
        CheckFullCopy(Names[i], Dst);
        Remove(Dst);
    }

    /* FAIL_IF_EXISTS: a new file is made with its streams; one that is there stays as it is */
    Ret = CopyFileExW(Src, Dst, NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS);
    ok(Ret, "FAIL_IF_EXISTS on a new file: %lu\n", GetLastError());
    CheckFullCopy("FAIL_IF_EXISTS", Dst);
    SetLastError(0xdeadbeef);
    Ret = CopyFileExW(Src, Dst, NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS);
    ok(!Ret && GetLastError() == ERROR_FILE_EXISTS, "FAIL_IF_EXISTS on a file: %d, %lu\n", Ret, GetLastError());
    CheckFullCopy("FAIL_IF_EXISTS, again", Dst);
    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Src, Dst, TRUE);
    ok(!Ret && GetLastError() == ERROR_FILE_EXISTS, "CopyFileW(TRUE) on a file: %d, %lu\n", Ret, GetLastError());
    Remove(Dst);
    Remove(Src);
}

/* A file that is overwritten has the streams of the source and no others */
static void TestOverwrite(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH], Plain[MAX_PATH], List[100];
    BOOL Ret;

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakePath(Plain, L"plain");
    MakeSource(Src);

    ok(WriteTo(Dst, "old default content that is longer", 34, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    ok(WriteStream(Dst, L":old", "old stream", 10), "create :old: %lu\n", GetLastError());
    ok(WriteStream(Dst, L":s1", "OLD S1 LONGER THAN THE NEW ONE ABCDEFGHIJ", 41), "create :s1: %lu\n", GetLastError());
    Ret = CopyFileW(Src, Dst, FALSE);
    ok(Ret, "CopyFileW over a file with streams: %lu\n", GetLastError());
    CheckFullCopy("overwrite", Dst);
    ok(SizeOfStream(Dst, L":old") == -1, "the old stream is still there\n");

    /* a source of one stream gives a file of one stream */
    ok(WriteTo(Plain, DefaultData, 5, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    Ret = CopyFileW(Plain, Dst, FALSE);
    ok(Ret, "CopyFileW of a plain file over a file with streams: %lu\n", GetLastError());
    NamedStreams(Dst, List, RTL_NUMBER_OF(List));
    ok(!List[0], "the streams '%ls' are left\n", List);
    ok(StreamHolds(Dst, L"", DefaultData, 5), "the default stream is wrong\n");
    Remove(Dst);
    Remove(Plain);
    Remove(Src);
}

static void TestAttributes(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    DWORD Attributes;
    BOOL Ret;

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakeSource(Src);
    Remove(Dst);
    ok(SetFileAttributesW(Src, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE),
       "SetFileAttributes: %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Src, Dst, FALSE);
    ok(Ret, "CopyFileW of a read-only file with streams: %lu\n", GetLastError());
    Attributes = GetFileAttributesW(Dst);
    ok(Attributes == (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE), "attributes %#lx\n", Attributes);
    CheckFullCopy("read-only", Dst);

    /* a read-only file is not overwritten */
    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Src, Dst, FALSE);
    ok(!Ret && GetLastError() == ERROR_ACCESS_DENIED, "CopyFileW over a read-only file: %d, %lu\n", Ret, GetLastError());
    CheckFullCopy("read-only, again", Dst);

    Remove(Dst);
    Remove(Src);
}

static void TestStreamNames(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH], Path[MAX_PATH], DstStream[MAX_PATH], List[100];
    BOOL Ret;

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakeSource(Src);
    Remove(Dst);

    /* a stream is not a source */
    StringCbPrintfW(Path, sizeof(Path), L"%s:s1", Src);
    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Path, Dst, FALSE);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "CopyFileW from a stream: %d, %lu\n", Ret, GetLastError());
    ok(!Exists(Dst), "a copy of a stream was made\n");
    StringCbPrintfW(Path, sizeof(Path), L"%s:s1:$DATA", Src);
    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Path, Dst, FALSE);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "CopyFileW from :s1:$DATA: %d, %lu\n", Ret, GetLastError());
    ok(!Exists(Dst), "a copy of a stream was made\n");

    /* the name of the default stream is the file */
    StringCbPrintfW(Path, sizeof(Path), L"%s::$DATA", Src);
    Ret = CopyFileW(Path, Dst, FALSE);
    ok(Ret, "CopyFileW from ::$DATA: %lu\n", GetLastError());
    CheckFullCopy("::$DATA", Dst);
    Remove(Dst);

    /* a file of one stream can go to a stream */
    MakePath(Path, L"plain");
    ok(WriteTo(Path, "plain data", 10, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    StringCbPrintfW(DstStream, sizeof(DstStream), L"%s:x", Dst);
    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Path, DstStream, FALSE);
    ok(Ret, "CopyFileW to a stream: %lu\n", GetLastError());
    ok(StreamHolds(Dst, L":x", "plain data", 10), "the stream has not the data\n");
    NamedStreams(Dst, List, RTL_NUMBER_OF(List));
    ok(!wcscmp(List, L":x:$DATA|"), "the streams are '%ls'\n", List);
    Remove(Dst);
    Remove(Path);

    /* but not a file of several */
    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Src, DstStream, FALSE);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "CopyFileW of a file with streams to a stream: %d, %lu\n", Ret, GetLastError());
    Remove(Dst);
    Remove(Src);
}

/* A stream that cannot be read fails the copy, and the copy is not left */
static void TestLockedStream(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH], Path[MAX_PATH];
    HANDLE Lock;
    BOOL Ret;

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakeSource(Src);
    Remove(Dst);

    StringCbPrintfW(Path, sizeof(Path), L"%s:s1", Src);
    Lock = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Lock != INVALID_HANDLE_VALUE, "lock: %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Ret = CopyFileW(Src, Dst, FALSE);
    ok(!Ret && GetLastError() == ERROR_SHARING_VIOLATION, "CopyFileW with :s1 locked: %d, %lu\n", Ret, GetLastError());
    ok(!Exists(Dst), "the copy was left\n");
    if (Lock != INVALID_HANDLE_VALUE)
        CloseHandle(Lock);

    Ret = CopyFileW(Src, Dst, FALSE);
    ok(Ret, "CopyFileW after the unlock: %lu\n", GetLastError());
    CheckFullCopy("after the unlock", Dst);
    Remove(Dst);
    Remove(Src);
}

static void TestEas(void)
{
    FN_SetEa SetEa = (FN_SetEa)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetEaFile");
    FN_QueryEa QueryEa = (FN_QueryEa)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryEaFile");
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    BYTE Buffer[128];
    TEST_EA *Ea = (TEST_EA *)Buffer;
    IO_STATUS_BLOCK Io;
    HANDLE File;
    NTSTATUS Status;
    BOOL Ret;

    MakePath(Src, L"eas");
    MakePath(Dst, L"eascopy");
    Remove(Dst);
    ok(WriteTo(Src, "data", 4, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    ok(WriteStream(Src, L":s1", "stream", 6), "create :s1: %lu\n", GetLastError());

    RtlZeroMemory(Buffer, sizeof(Buffer));
    Ea->EaNameLength = 4;
    Ea->EaValueLength = 5;
    memcpy(Ea->EaName, "MYEA\0value", 10);
    File = CreateFileW(Src, FILE_WRITE_EA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    Status = (File == INVALID_HANDLE_VALUE) ? STATUS_ACCESS_DENIED :
             SetEa(File, &Io, Buffer, FIELD_OFFSET(TEST_EA, EaName) + 10);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    if (!NT_SUCCESS(Status))
    {
        skip("The file system keeps no extended attributes (%08lx)\n", (ULONG)Status);
        Remove(Src);
        return;
    }

    Ret = CopyFileW(Src, Dst, FALSE);
    ok(Ret, "CopyFileW failed: %lu\n", GetLastError());
    File = CreateFileW(Dst, FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "open: %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        RtlZeroMemory(Buffer, sizeof(Buffer));
        Status = QueryEa(File, &Io, Buffer, sizeof(Buffer), FALSE, NULL, 0, NULL, TRUE);
        ok(NT_SUCCESS(Status), "NtQueryEaFile of the copy: %08lx\n", (ULONG)Status);
        ok(NT_SUCCESS(Status) && Ea->EaNameLength == 4 && !memcmp(Ea->EaName, "MYEA", 4) && Ea->EaValueLength == 5 &&
           !memcmp(Ea->EaName + 5, "value", 5), "the extended attribute of the copy is wrong\n");
        CloseHandle(File);
    }
    ok(StreamHolds(Dst, L":s1", "stream", 6), "the stream was not copied\n");
    Remove(Dst);
    Remove(Src);
}

static void TestCopyFile2(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params = { sizeof(Params) };
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    HRESULT hr;
    int i;

    if (!pCopyFile2)
    {
        skip("CopyFile2 is not there\n");
        return;
    }

    MakePath(Src, L"src");
    MakePath(Dst, L"dst");
    MakeSource(Src);
    Remove(Dst);

    Params.pProgressRoutine = Progress2;
    hr = pCopyFile2(Src, Dst, &Params);
    ok(hr == S_OK, "CopyFile2: %#lx\n", hr);
    CheckFullCopy("CopyFile2", Dst);
    ok(Copy2StartCount == 3 && Copy2FinishCount == 3, "%d streams started, %d finished\n", Copy2StartCount, Copy2FinishCount);
    for (i = 0; i < 3 && i < Copy2StartCount; i++)
        ok(Copy2Streams[i] == i + 1, "the stream %d is numbered %d\n", i, Copy2Streams[i]);
    Remove(Dst);

    hr = pCopyFile2(Src, Dst, NULL);
    ok(hr == S_OK, "CopyFile2 without a routine: %#lx\n", hr);
    CheckFullCopy("CopyFile2 without a routine", Dst);
    Remove(Dst);
    Remove(Src);
}

static void TestMove(void)
{
    WCHAR Src[MAX_PATH], Dst[MAX_PATH];
    BOOL Ret;

    MakePath(Src, L"mvsrc");
    MakePath(Dst, L"mvdst");
    MakeSource(Src);
    Remove(Dst);

    ResetCalls(0, 0);
    Ret = MoveFileWithProgressW(Src, Dst, Progress, NULL, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
    ok(Ret, "MoveFileWithProgressW: %lu\n", GetLastError());
    ok(!Exists(Src), "the source is still there\n");
    CheckFullCopy("MoveFileWithProgressW", Dst);
    Remove(Dst);
}

/* What is made by copying to another volume, if there is one to copy to (CPS_OTHER_DIR): the streams go with a
   move that has to copy. When the volume keeps no streams (FAT) they are not copied, and the copy still is made
   (not measured on Windows: this is what ReactOS does). */
static void TestOtherVolume(void)
{
    WCHAR Other[MAX_PATH], Src[MAX_PATH], Dst[MAX_PATH], Root[MAX_PATH], List[100];
    DWORD Flags = 0, Length;
    BOOL Ret;

    Length = GetEnvironmentVariableW(L"CPS_OTHER_DIR", Other, RTL_NUMBER_OF(Other));
    if (!Length || Length >= RTL_NUMBER_OF(Other))
    {
        skip("CPS_OTHER_DIR names no directory on another volume\n");
        return;
    }
    if (!GetVolumePathNameW(Other, Root, RTL_NUMBER_OF(Root)) ||
        !GetVolumeInformationW(Root, NULL, 0, NULL, NULL, &Flags, NULL, 0))
    {
        skip("No volume information for %ls\n", Other);
        return;
    }

    MakePath(Src, L"ovsrc");
    StringCbPrintfW(Dst, sizeof(Dst), L"%s\\ovdst", Other);
    MakeSource(Src);
    Remove(Dst);

    ResetCalls(0, 0);
    Ret = CopyFileExW(Src, Dst, Progress, NULL, NULL, 0);
    ok(Ret, "CopyFileExW to %ls: %lu\n", Other, GetLastError());
    if (Flags & FILE_NAMED_STREAMS)
    {
        CheckFullCopy("another volume", Dst);
    }
    else
    {
        trace("The volume %ls keeps no streams\n", Root);
        ok(StreamHolds(Dst, L"", DefaultData, 5), "the default stream is wrong\n");
        NamedStreams(Dst, List, RTL_NUMBER_OF(List));
        ok(!List[0], "streams '%ls' on a volume that keeps none\n", List);
        ok(CallCount >= 2 && Calls[0].Number == 1, "%d calls\n", CallCount);
    }
    Remove(Dst);

    ResetCalls(0, 0);
    Ret = MoveFileWithProgressW(Src, Dst, Progress, NULL, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
    ok(Ret, "MoveFileWithProgressW to %ls: %lu\n", Other, GetLastError());
    ok(!Exists(Src), "the source is still there\n");
    if (Flags & FILE_NAMED_STREAMS)
        CheckFullCopy("move to another volume", Dst);
    else
        ok(StreamHolds(Dst, L"", DefaultData, 5), "the default stream is wrong\n");
    Remove(Dst);
}

static void TestDirectoryTemplate(void)
{
    WCHAR Template[MAX_PATH], New[MAX_PATH], Path[MAX_PATH];
    BOOL Ret;

    MakePath(Template, L"tpl");
    MakePath(New, L"tplcopy");
    CreateDirectoryW(Template, NULL);
    StringCbPrintfW(Path, sizeof(Path), L"%s:ds", Template);
    ok(WriteTo(Path, "dirstream", 9, CREATE_ALWAYS), "create a stream of a directory: %lu\n", GetLastError());
    RemoveDirectoryW(New);

    Ret = CreateDirectoryExW(Template, New, NULL);
    ok(Ret, "CreateDirectoryExW: %lu\n", GetLastError());
    ok(StreamHolds(New, L":ds", "dirstream", 9), "the new directory has not the stream of the template (%I64d)\n", SizeOfStream(New, L":ds"));
    DeleteFileW(Path);
    StringCbPrintfW(Path, sizeof(Path), L"%s:ds", New);
    DeleteFileW(Path);
    RemoveDirectoryW(New);
    RemoveDirectoryW(Template);
}

/* The commands: copy and xcopy, in the shell of the system. The shell has the null device for its handles. */
static DWORD RunCmd(PCWSTR Arguments)
{
    WCHAR Cmd[MAX_PATH * 3], System[MAX_PATH];
    STARTUPINFOW Startup = { sizeof(Startup) };
    SECURITY_ATTRIBUTES Inherit = { sizeof(Inherit), NULL, TRUE };
    PROCESS_INFORMATION Info;
    DWORD Exit = 0xdeadbeef;
    HANDLE Null;

    GetSystemDirectoryW(System, RTL_NUMBER_OF(System));
    StringCbPrintfW(Cmd, sizeof(Cmd), L"\"%s\\cmd.exe\" /c %s", System, Arguments);
    Null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inherit,
                       OPEN_EXISTING, 0, NULL);
    ok(Null != INVALID_HANDLE_VALUE, "open NUL: %lu\n", GetLastError());
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdInput = Startup.hStdOutput = Startup.hStdError = Null;
    if (!CreateProcessW(NULL, Cmd, NULL, NULL, TRUE, 0, NULL, TestDir, &Startup, &Info))
    {
        ok(0, "CreateProcess of %ls: %lu\n", Cmd, GetLastError());
        CloseHandle(Null);
        return Exit;
    }
    CloseHandle(Null);
    WaitForSingleObject(Info.hProcess, 60000);
    GetExitCodeProcess(Info.hProcess, &Exit);
    CloseHandle(Info.hThread);
    CloseHandle(Info.hProcess);
    return Exit;
}

static void TestCommands(void)
{
    WCHAR Path[MAX_PATH], Dst[MAX_PATH], Sub[MAX_PATH], Sub2[MAX_PATH], Inner[MAX_PATH];
    static const char Zz[] = "zz data", Ds[] = "dir stream", Ids[] = "inner dir stream";

    /* copy f g */
    MakePath(Path, L"f.txt");
    MakePath(Dst, L"g.txt");
    MakeSource(Path);
    Remove(Dst);
    RunCmd(L"copy f.txt g.txt");
    CheckFullCopy("cmd copy", Dst);
    Remove(Dst);
    RunCmd(L"copy /b f.txt g.txt");
    CheckFullCopy("cmd copy /b", Dst);
    Remove(Dst);
    RunCmd(L"copy /y f.txt g.txt");
    RunCmd(L"copy /y f.txt g.txt");
    CheckFullCopy("cmd copy over a file", Dst);
    Remove(Dst);

    /* a concatenation has the data (and the end-of-file mark of a text file) and none of the streams */
    RunCmd(L"copy f.txt+f.txt g.txt");
    ok((SizeOfStream(Dst, L"") == 10 || SizeOfStream(Dst, L"") == 11) && SizeOfStream(Dst, L":s1") == -1,
       "cmd copy a+b: %I64d bytes, :s1 %I64d\n", SizeOfStream(Dst, L""), SizeOfStream(Dst, L":s1"));
    Remove(Dst);

    /* xcopy of a file */
    RunCmd(L"xcopy /y f.txt dstx\\");
    StringCbPrintfW(Dst, sizeof(Dst), L"%s\\dstx\\f.txt", TestDir);
    CheckFullCopy("xcopy a file", Dst);
    RunCmd(L"rd /s /q dstx");
    Remove(Path);

    /* xcopy /e: the files and the directories, with their streams */
    MakePath(Sub, L"sub");
    MakePath(Sub2, L"sub2");
    MakePath(Inner, L"sub\\inner");
    CreateDirectoryW(Sub, NULL);
    CreateDirectoryW(Inner, NULL);
    StringCbPrintfW(Path, sizeof(Path), L"%s\\a.txt", Sub);
    ok(WriteTo(Path, "a", 1, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    ok(WriteStream(Path, L":zz", Zz, 7), "create :zz: %lu\n", GetLastError());
    StringCbPrintfW(Path, sizeof(Path), L"%s\\i.txt", Inner);
    ok(WriteTo(Path, "i", 1, CREATE_ALWAYS), "create: %lu\n", GetLastError());
    ok(WriteStream(Path, L":iz", "inner data", 10), "create :iz: %lu\n", GetLastError());
    ok(WriteStream(Sub, L":ds", Ds, 10), "create a stream of a directory: %lu\n", GetLastError());
    ok(WriteStream(Inner, L":ids", Ids, 16), "create a stream of a directory: %lu\n", GetLastError());

    RunCmd(L"xcopy /e /i /q sub sub2");
    StringCbPrintfW(Path, sizeof(Path), L"%s\\a.txt", Sub2);
    ok(StreamHolds(Path, L":zz", Zz, 7), "xcopy /e: sub2\\a.txt:zz is wrong (%I64d)\n", SizeOfStream(Path, L":zz"));
    StringCbPrintfW(Path, sizeof(Path), L"%s\\inner\\i.txt", Sub2);
    ok(StreamHolds(Path, L":iz", "inner data", 10), "xcopy /e: sub2\\inner\\i.txt:iz is wrong\n");
    ok(StreamHolds(Sub2, L":ds", Ds, 10), "xcopy /e: sub2:ds is wrong (%I64d)\n", SizeOfStream(Sub2, L":ds"));
    StringCbPrintfW(Path, sizeof(Path), L"%s\\inner", Sub2);
    ok(StreamHolds(Path, L":ids", Ids, 16), "xcopy /e: sub2\\inner:ids is wrong (%I64d)\n", SizeOfStream(Path, L":ids"));

    /* clean up the tree */
    RunCmd(L"rd /s /q sub");
    RunCmd(L"rd /s /q sub2");
    if (Exists(Sub2) || Exists(Sub))
    {
        /* a directory with streams does not go with every rd */
        StringCbPrintfW(Path, sizeof(Path), L"%s:ds", Sub2);
        DeleteFileW(Path);
        StringCbPrintfW(Path, sizeof(Path), L"%s:ds", Sub);
        DeleteFileW(Path);
    }
}

START_TEST(CopyFileStreams)
{
    WCHAR Root[4] = L"C:\\";
    WCHAR Temp[MAX_PATH];
    DWORD Flags = 0, i;

    GetTempPathW(RTL_NUMBER_OF(Temp), Temp);
    Root[0] = Temp[0];
    if (!GetVolumeInformationW(Root, NULL, 0, NULL, NULL, &Flags, NULL, 0) || !(Flags & FILE_NAMED_STREAMS))
    {
        skip("The volume %ls has no named streams\n", Root);
        return;
    }

    for (i = 0; i < SIZE_S2; i++)
        S2Data[i] = (char)('A' + (i * 7 + i / 251) % 26);
    pCopyFile2 = (FN_CopyFile2)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CopyFile2");

    StringCbPrintfW(TestDir, sizeof(TestDir), L"%scpstreams%lu_%lu", Temp, GetCurrentProcessId(), GetTickCount());
    ok(CreateDirectoryW(TestDir, NULL), "create test dir: %lu\n", GetLastError());

    TestCopyFile();
    TestProgress();
    TestEmptyStreams();
    TestChunkSizes();
    TestFlags();
    TestOverwrite();
    TestAttributes();
    TestStreamNames();
    TestLockedStream();
    TestEas();
    TestCopyFile2();
    TestMove();
    TestOtherVolume();
    TestDirectoryTemplate();
    TestCommands();

    RemoveDirectoryW(TestDir);
}
