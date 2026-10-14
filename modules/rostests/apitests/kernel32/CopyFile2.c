/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for CopyFile2, its progress callback and its cancellation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <winioctl.h>

#ifndef COPY_FILE_COPY_SYMLINK
#define COPY_FILE_COPY_SYMLINK          0x00000800
#endif
#ifndef COPY_FILE_RESUME_FROM_PAUSE
#define COPY_FILE_RESUME_FROM_PAUSE     0x00004000
#endif

typedef HRESULT (WINAPI *FN_CopyFile2)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS *);
static FN_CopyFile2 pCopyFile2;

static WCHAR SourceFile[MAX_PATH], DestFile[MAX_PATH];

typedef struct
{
    COPYFILE2_MESSAGE_TYPE Type;
    DWORD Stream;
    ULONGLONG Chunk, ChunkSize, StreamSize, StreamBytes, TotalSize, TotalBytes;
    HRESULT Failure;
    COPYFILE2_COPY_PHASE Phase;
    BOOL Handles;
    BOOL SameThread;
    DWORD Padding;
} EVENT;

static EVENT *Events;
static int EventCount, EventCapacity;
static int ActionAt;
static DWORD ActionToTake;
static int CancelAt;
static BOOL *CancelFlag;
static DWORD MainThread;

/* Whether an open handle is of the file with the given name (volume and file index) */
static BOOL HandleIsOf(HANDLE hFile, PCWSTR Name)
{
    BY_HANDLE_FILE_INFORMATION A, B;
    HANDLE hName;
    BOOL Same;

    if (!hFile || hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    hName = CreateFileW(Name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hName == INVALID_HANDLE_VALUE)
        return FALSE;
    Same = GetFileInformationByHandle(hFile, &A) && GetFileInformationByHandle(hName, &B) &&
           A.dwVolumeSerialNumber == B.dwVolumeSerialNumber && A.nFileIndexHigh == B.nFileIndexHigh &&
           A.nFileIndexLow == B.nFileIndexLow;
    CloseHandle(hName);
    return Same;
}

static COPYFILE2_MESSAGE_ACTION CALLBACK Progress(const COPYFILE2_MESSAGE *Message, PVOID Context)
{
    EVENT *Event;
    DWORD Action = COPYFILE2_PROGRESS_CONTINUE;

    if (EventCount == EventCapacity)
    {
        int NewCapacity = EventCapacity ? EventCapacity * 2 : 256;
        EVENT *NewEvents = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NewCapacity * sizeof(EVENT));

        ok(NewEvents != NULL, "Out of memory\n");
        if (!NewEvents)
            ExitProcess(1);
        if (Events)
        {
            memcpy(NewEvents, Events, EventCount * sizeof(EVENT));
            HeapFree(GetProcessHeap(), 0, Events);
        }
        Events = NewEvents;
        EventCapacity = NewCapacity;
    }
    Event = &Events[EventCount];

    ZeroMemory(Event, sizeof(*Event));
    Event->Type = Message->Type;
    Event->Padding = Message->dwPadding;
    Event->SameThread = GetCurrentThreadId() == MainThread;
    switch (Message->Type)
    {
        case COPYFILE2_CALLBACK_CHUNK_STARTED:
            Event->Stream = Message->Info.ChunkStarted.dwStreamNumber;
            Event->Chunk = Message->Info.ChunkStarted.uliChunkNumber.QuadPart;
            Event->ChunkSize = Message->Info.ChunkStarted.uliChunkSize.QuadPart;
            Event->StreamSize = Message->Info.ChunkStarted.uliStreamSize.QuadPart;
            Event->TotalSize = Message->Info.ChunkStarted.uliTotalFileSize.QuadPart;
            Event->Handles = HandleIsOf(Message->Info.ChunkStarted.hSourceFile, SourceFile) && HandleIsOf(Message->Info.ChunkStarted.hDestinationFile, DestFile);
            break;
        case COPYFILE2_CALLBACK_CHUNK_FINISHED:
            Event->Stream = Message->Info.ChunkFinished.dwStreamNumber;
            Event->Chunk = Message->Info.ChunkFinished.uliChunkNumber.QuadPart;
            Event->ChunkSize = Message->Info.ChunkFinished.uliChunkSize.QuadPart;
            Event->StreamSize = Message->Info.ChunkFinished.uliStreamSize.QuadPart;
            Event->StreamBytes = Message->Info.ChunkFinished.uliStreamBytesTransferred.QuadPart;
            Event->TotalSize = Message->Info.ChunkFinished.uliTotalFileSize.QuadPart;
            Event->TotalBytes = Message->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
            Event->Handles = HandleIsOf(Message->Info.ChunkFinished.hSourceFile, SourceFile) && HandleIsOf(Message->Info.ChunkFinished.hDestinationFile, DestFile);
            break;
        case COPYFILE2_CALLBACK_STREAM_STARTED:
            Event->Stream = Message->Info.StreamStarted.dwStreamNumber;
            Event->StreamSize = Message->Info.StreamStarted.uliStreamSize.QuadPart;
            Event->TotalSize = Message->Info.StreamStarted.uliTotalFileSize.QuadPart;
            Event->Handles = HandleIsOf(Message->Info.StreamStarted.hSourceFile, SourceFile) && HandleIsOf(Message->Info.StreamStarted.hDestinationFile, DestFile);
            break;
        case COPYFILE2_CALLBACK_STREAM_FINISHED:
            Event->Stream = Message->Info.StreamFinished.dwStreamNumber;
            Event->StreamSize = Message->Info.StreamFinished.uliStreamSize.QuadPart;
            Event->StreamBytes = Message->Info.StreamFinished.uliStreamBytesTransferred.QuadPart;
            Event->TotalSize = Message->Info.StreamFinished.uliTotalFileSize.QuadPart;
            Event->TotalBytes = Message->Info.StreamFinished.uliTotalBytesTransferred.QuadPart;
            Event->Handles = HandleIsOf(Message->Info.StreamFinished.hSourceFile, SourceFile) && HandleIsOf(Message->Info.StreamFinished.hDestinationFile, DestFile);
            break;
        case COPYFILE2_CALLBACK_ERROR:
            Event->Phase = Message->Info.Error.CopyPhase;
            Event->Stream = Message->Info.Error.dwStreamNumber;
            Event->Failure = Message->Info.Error.hrFailure;
            Event->Chunk = Message->Info.Error.uliChunkNumber.QuadPart;
            Event->StreamSize = Message->Info.Error.uliStreamSize.QuadPart;
            Event->StreamBytes = Message->Info.Error.uliStreamBytesTransferred.QuadPart;
            Event->TotalSize = Message->Info.Error.uliTotalFileSize.QuadPart;
            Event->TotalBytes = Message->Info.Error.uliTotalBytesTransferred.QuadPart;
            break;
        default:
            break;
    }

    if (EventCount == ActionAt)
        Action = ActionToTake;
    if (EventCount == CancelAt && CancelFlag)
        *CancelFlag = TRUE;
    EventCount++;
    return (COPYFILE2_MESSAGE_ACTION)Action;
}

/* The last message, or an empty one: a copy that sends none is a failure to report, not to crash on */
static EVENT *LastEvent(void)
{
    static EVENT Empty;

    return EventCount > 0 ? &Events[EventCount - 1] : &Empty;
}

static void RemoveFiles(void)
{
    SetFileAttributesW(SourceFile, FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(DestFile, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(SourceFile);
    DeleteFileW(DestFile);
}

static BOOL Exists(PCWSTR Name)
{
    return GetFileAttributesW(Name) != INVALID_FILE_ATTRIBUTES;
}

static void WritePattern(PCWSTR Name, ULONGLONG Size, DWORD Attributes)
{
    HANDLE hFile = CreateFileW(Name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, Attributes, NULL);
    BYTE Buffer[4096];
    ULONGLONG Done = 0;
    DWORD Chunk, Written, i;

    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    while (Done < Size)
    {
        Chunk = (DWORD)min((ULONGLONG)sizeof(Buffer), Size - Done);
        for (i = 0; i < Chunk; i++)
            Buffer[i] = (BYTE)((Done + i) * 7 + (Done + i) / 251);
        WriteFile(hFile, Buffer, Chunk, &Written, NULL);
        Done += Chunk;
    }
    CloseHandle(hFile);
}

static ULONGLONG SizeOf(PCWSTR Name)
{
    WIN32_FILE_ATTRIBUTE_DATA Data;

    if (!GetFileAttributesExW(Name, GetFileExInfoStandard, &Data))
        return (ULONGLONG)-1;
    return ((ULONGLONG)Data.nFileSizeHigh << 32) | Data.nFileSizeLow;
}

static BOOL SameContent(PCWSTR A, PCWSTR B)
{
    HANDLE hA = CreateFileW(A, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hB = CreateFileW(B, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE BufA[4096], BufB[4096];
    DWORD ReadA, ReadB;
    BOOL Same = (hA != INVALID_HANDLE_VALUE && hB != INVALID_HANDLE_VALUE);

    while (Same)
    {
        Same = ReadFile(hA, BufA, sizeof(BufA), &ReadA, NULL) && ReadFile(hB, BufB, sizeof(BufB), &ReadB, NULL) &&
               ReadA == ReadB && !memcmp(BufA, BufB, ReadA);
        if (!Same || ReadA == 0)
            break;
    }
    if (hA != INVALID_HANDLE_VALUE)
        CloseHandle(hA);
    if (hB != INVALID_HANDLE_VALUE)
        CloseHandle(hB);
    return Same;
}

/* A named stream of Size bytes of a pattern */
static BOOL MakeBigStream(PCWSTR Name, PCWSTR Stream, ULONGLONG Size)
{
    WCHAR Path[MAX_PATH];
    HANDLE hFile;
    BYTE Buffer[4096];
    DWORD Written, i;
    ULONGLONG Done = 0;

    StringCchPrintfW(Path, _countof(Path), L"%s%s", Name, Stream);
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    while (Done < Size)
    {
        DWORD Chunk = (DWORD)min((ULONGLONG)sizeof(Buffer), Size - Done);

        for (i = 0; i < Chunk; i++)
            Buffer[i] = (BYTE)((Done + i) * 13 + (Done + i) / 239);
        WriteFile(hFile, Buffer, Chunk, &Written, NULL);
        Done += Chunk;
    }
    CloseHandle(hFile);
    return TRUE;
}

static BOOL IsHidden(PCWSTR Name)
{
    DWORD Attributes = GetFileAttributesW(Name);

    return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_HIDDEN);
}

/* Alternate data streams */
static BOOL MakeStream(PCWSTR Name, PCWSTR Stream, const char *Data)
{
    WCHAR Path[MAX_PATH];
    HANDLE hFile;
    DWORD Written;

    StringCchPrintfW(Path, _countof(Path), L"%s%s", Name, Stream);
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    WriteFile(hFile, Data, lstrlenA(Data), &Written, NULL);
    CloseHandle(hFile);
    return TRUE;
}

/* The size of a named stream, or -1 when the file has none of that name */
static LONGLONG StreamSizeOf(PCWSTR Name, PCWSTR Stream)
{
    WCHAR Path[MAX_PATH];
    LARGE_INTEGER Size;
    HANDLE hFile;

    StringCchPrintfW(Path, _countof(Path), L"%s%s", Name, Stream);
    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetFileSizeEx(hFile, &Size))
        Size.QuadPart = -1;
    CloseHandle(hFile);
    return Size.QuadPart;
}

static BOOL StreamHolds(PCWSTR Name, PCWSTR Stream, const char *Data)
{
    WCHAR Path[MAX_PATH];
    char Buffer[128] = { 0 };
    HANDLE hFile;
    DWORD Read = 0;

    StringCchPrintfW(Path, _countof(Path), L"%s%s", Name, Stream);
    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(hFile, Buffer, sizeof(Buffer), &Read, NULL);
    CloseHandle(hFile);
    return Read == (DWORD)lstrlenA(Data) && !memcmp(Buffer, Data, Read);
}

/* Whether the bytes of two files from Start up to Length are the same */
static BOOL SameRange(PCWSTR A, PCWSTR B, ULONGLONG Start, ULONGLONG Length)
{
    HANDLE hA = CreateFileW(A, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hB = CreateFileW(B, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE BufA[4096], BufB[4096];
    DWORD ReadA, ReadB;
    LARGE_INTEGER Position;
    BOOL Same = (hA != INVALID_HANDLE_VALUE && hB != INVALID_HANDLE_VALUE);

    Position.QuadPart = Start;
    Same = Same && SetFilePointerEx(hA, Position, NULL, FILE_BEGIN) && SetFilePointerEx(hB, Position, NULL, FILE_BEGIN);
    while (Same && Start < Length)
    {
        DWORD Chunk = (DWORD)min((ULONGLONG)sizeof(BufA), Length - Start);

        Same = ReadFile(hA, BufA, Chunk, &ReadA, NULL) && ReadFile(hB, BufB, Chunk, &ReadB, NULL) &&
               ReadA == Chunk && ReadB == Chunk && !memcmp(BufA, BufB, Chunk);
        Start += Chunk;
    }
    if (hA != INVALID_HANDLE_VALUE)
        CloseHandle(hA);
    if (hB != INVALID_HANDLE_VALUE)
        CloseHandle(hB);
    return Same;
}

static HRESULT RunCopy(DWORD Flags, BOOL UseCallback, int Action, DWORD ActionValue, BOOL *Cancel, int SetCancel, PDWORD Error)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    HRESULT hr;

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwCopyFlags = Flags;
    Params.pfCancel = Cancel;
    Params.pProgressRoutine = UseCallback ? Progress : NULL;
    EventCount = 0;
    ActionAt = Action;
    ActionToTake = ActionValue;
    CancelAt = SetCancel;
    CancelFlag = Cancel;
    MainThread = GetCurrentThreadId();
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    *Error = GetLastError();
    return hr;
}

/* The messages of a copy that ran to the end of a file of one stream */
static void CheckFullSequence(ULONGLONG Size, PCSTR Name)
{
    int i = 0, Chunk = 0;
    ULONGLONG Done = 0;

    ok(EventCount >= 2, "%s: only %d messages\n", Name, EventCount);
    if (EventCount < 2)
        return;

    ok(Events[0].Type == COPYFILE2_CALLBACK_STREAM_STARTED, "%s: first message is %d\n", Name, Events[0].Type);
    ok(Events[0].Stream == 1, "%s: stream %lu\n", Name, Events[0].Stream);
    ok(Events[0].StreamSize == Size && Events[0].TotalSize == Size,
       "%s: stream size %I64u, total %I64u\n", Name, Events[0].StreamSize, Events[0].TotalSize);
    ok(Events[0].Handles, "%s: the stream start has no handles\n", Name);

    for (i = 1; i + 1 < EventCount; i += 2, Chunk++)
    {
        ok(Events[i].Type == COPYFILE2_CALLBACK_CHUNK_STARTED, "%s: message %d is %d\n", Name, i, Events[i].Type);
        ok(i + 1 < EventCount - 1 && Events[i + 1].Type == COPYFILE2_CALLBACK_CHUNK_FINISHED,
           "%s: message %d is not a chunk end\n", Name, i + 1);
        if (Events[i].Type != COPYFILE2_CALLBACK_CHUNK_STARTED || Events[i + 1].Type != COPYFILE2_CALLBACK_CHUNK_FINISHED)
            break;
        ok(Events[i].Chunk == (ULONGLONG)Chunk && Events[i + 1].Chunk == (ULONGLONG)Chunk,
           "%s: chunk numbers %I64u and %I64u, expected %d\n", Name, Events[i].Chunk, Events[i + 1].Chunk, Chunk);
        ok(Events[i].ChunkSize == Events[i + 1].ChunkSize && Events[i].ChunkSize > 0,
           "%s: chunk sizes %I64u and %I64u\n", Name, Events[i].ChunkSize, Events[i + 1].ChunkSize);
        ok(Events[i].Handles && Events[i + 1].Handles, "%s: a chunk has no handles\n", Name);
        Done += Events[i + 1].ChunkSize;
        ok(Events[i + 1].StreamBytes == Done && Events[i + 1].TotalBytes == Done,
           "%s: chunk %d transferred %I64u and %I64u, expected %I64u\n",
           Name, Chunk, Events[i + 1].StreamBytes, Events[i + 1].TotalBytes, Done);
    }
    ok(Done == Size, "%s: the chunks add up to %I64u, the file has %I64u\n", Name, Done, Size);
    ok(Size == 0 ? Chunk == 0 : Chunk > 0, "%s: %d chunks for %I64u bytes\n", Name, Chunk, Size);

    ok(LastEvent()->Type == COPYFILE2_CALLBACK_STREAM_FINISHED, "%s: last message is %d\n", Name, LastEvent()->Type);
    ok(LastEvent()->StreamBytes == Size && LastEvent()->TotalBytes == Size,
       "%s: the end reports %I64u and %I64u bytes\n", Name, LastEvent()->StreamBytes, LastEvent()->TotalBytes);

    for (i = 0; i < EventCount; i++)
    {
        ok(Events[i].SameThread, "%s: message %d came from another thread\n", Name, i);
        ok(Events[i].Padding == 0, "%s: message %d has padding %lu\n", Name, i, Events[i].Padding);
    }
}

static void TestCallbackSequence(void)
{
    static const ULONGLONG Sizes[] = { 0, 1, 100, 65536, 5 << 20 };
    DWORD Error;
    HRESULT hr;
    int i;
    char Name[32];

    for (i = 0; i < _countof(Sizes); i++)
    {
        RemoveFiles();
        WritePattern(SourceFile, Sizes[i], FILE_ATTRIBUTE_NORMAL);
        hr = RunCopy(0, TRUE, -1, 0, NULL, -1, &Error);
        StringCchPrintfA(Name, _countof(Name), "size %I64u", Sizes[i]);
        ok(hr == S_OK, "%s: hr = %#lx\n", Name, hr);
        ok(Error == ERROR_SUCCESS, "%s: error %lu\n", Name, Error);
        ok(SizeOf(DestFile) == Sizes[i] && SameContent(SourceFile, DestFile), "%s: the copy differs\n", Name);
        CheckFullSequence(Sizes[i], Name);
    }

    /* Without a callback the same copy works */
    RemoveFiles();
    WritePattern(SourceFile, 12345, FILE_ATTRIBUTE_NORMAL);
    hr = RunCopy(0, FALSE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK && Error == ERROR_SUCCESS, "hr = %#lx, error %lu\n", hr, Error);
    ok(SizeOf(DestFile) == 12345 && SameContent(SourceFile, DestFile), "The copy differs\n");

    /* A copy with no parameters at all */
    DeleteFileW(DestFile);
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(SourceFile, DestFile, NULL);
    ok(hr == S_OK && GetLastError() == ERROR_SUCCESS, "hr = %#lx, error %lu\n", hr, GetLastError());
    ok(SizeOf(DestFile) == 12345 && SameContent(SourceFile, DestFile), "The copy differs\n");
}

static void TestActions(void)
{
    DWORD Error;
    HRESULT hr;
    ULONGLONG FirstChunk;
    BOOL Cancel;
    int i;

    /* Cancel at the start of the stream: the end is still reported, and the destination goes */
    RemoveFiles();
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);
    hr = RunCopy(0, TRUE, 0, COPYFILE2_PROGRESS_CANCEL, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(Error == ERROR_REQUEST_ABORTED, "error %lu\n", Error);
    ok(EventCount == 2 && Events[1].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[1].StreamBytes == 0,
       "%d messages, the last is %d with %I64u bytes\n", EventCount, LastEvent()->Type, LastEvent()->StreamBytes);
    ok(!Exists(DestFile), "The destination is still there\n");

    /* Cancel when a chunk starts */
    hr = RunCopy(0, TRUE, 1, COPYFILE2_PROGRESS_CANCEL, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(EventCount == 3 && Events[1].Type == COPYFILE2_CALLBACK_CHUNK_STARTED &&
       Events[2].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[2].StreamBytes == 0,
       "%d messages\n", EventCount);
    ok(!Exists(DestFile), "The destination is still there\n");

    /* Cancel when a chunk ends: the end carries what was copied */
    hr = RunCopy(0, TRUE, 2, COPYFILE2_PROGRESS_CANCEL, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(EventCount == 4 && Events[3].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[3].StreamBytes == 100,
       "%d messages, the last carries %I64u bytes\n", EventCount, LastEvent()->StreamBytes);
    ok(!Exists(DestFile), "The destination is still there\n");

    /* An answer to the last message counts, too */
    hr = RunCopy(0, TRUE, 3, COPYFILE2_PROGRESS_CANCEL, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(EventCount == 4, "%d messages\n", EventCount);
    ok(!Exists(DestFile), "The destination is still there\n");

    /* Stop ends the copy like a cancel when it comes in the middle of a stream */
    hr = RunCopy(0, TRUE, 2, COPYFILE2_PROGRESS_STOP, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(Error == ERROR_REQUEST_ABORTED, "error %lu\n", Error);
    ok(EventCount == 4 && Events[3].Type == COPYFILE2_CALLBACK_STREAM_FINISHED, "%d messages\n", EventCount);
    ok(!Exists(DestFile), "The destination is still there\n");
    hr = RunCopy(0, TRUE, 1, COPYFILE2_PROGRESS_STOP, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) && EventCount == 3, "hr = %#lx, %d messages\n", hr, EventCount);
    ok(!Exists(DestFile), "The destination is still there\n");

    /* but a stop at the start or at the end of a stream keeps the new file */
    hr = RunCopy(0, TRUE, 0, COPYFILE2_PROGRESS_STOP, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(EventCount == 2 && Events[1].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[1].StreamBytes == 0,
       "%d messages\n", EventCount);
    ok(Exists(DestFile), "A stop at the start of the stream deleted the destination\n");
    ok(SizeOf(DestFile) == 100, "A stop at the start of the stream left a destination of %I64u bytes, the source has 100\n", SizeOf(DestFile));
    hr = RunCopy(0, TRUE, 3, COPYFILE2_PROGRESS_STOP, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) && EventCount == 4, "hr = %#lx, %d messages\n", hr, EventCount);
    ok(Exists(DestFile) && SizeOf(DestFile) == 100 && SameContent(SourceFile, DestFile),
       "A stop at the end of the stream did not keep the copy\n");

    /* Pause before the data is copied is reported as a paused request, and the new file stays */
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 1, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED), "hr = %#lx\n", hr);
    ok(Error == ERROR_REQUEST_PAUSED, "error %lu\n", Error);
    ok(EventCount == 3 && Events[2].Type == COPYFILE2_CALLBACK_STREAM_FINISHED, "%d messages\n", EventCount);
    ok(Exists(DestFile), "A pause deleted the destination\n");
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 0, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED) && EventCount == 2, "hr = %#lx, %d messages\n", hr, EventCount);
    ok(Exists(DestFile), "A pause deleted the destination\n");
    SetFileAttributesW(DestFile, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(DestFile);

    /* and a pause after all data is copied has nothing to pause */
    hr = RunCopy(0, TRUE, 2, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == S_OK && Error == ERROR_SUCCESS && EventCount == 4, "hr = %#lx, error %lu, %d messages\n", hr, Error, EventCount);
    ok(SizeOf(DestFile) == 100 && SameContent(SourceFile, DestFile), "The copy differs\n");
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 3, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == S_OK && EventCount == 4, "hr = %#lx, %d messages\n", hr, EventCount);
    ok(SizeOf(DestFile) == 100 && SameContent(SourceFile, DestFile), "The copy differs\n");
    DeleteFileW(DestFile);

    /* The cancel flag counts wherever it is set, up to the last message */
    for (i = 0; i < 4; i++)
    {
        Cancel = FALSE;
        hr = RunCopy(0, TRUE, -1, 0, &Cancel, i, &Error);
        ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) && Error == ERROR_REQUEST_ABORTED,
           "Flag set in message %d: hr = %#lx, error %lu\n", i, hr, Error);
        ok(EventCount == (i < 2 ? 2 + i : 4), "Flag set in message %d: %d messages\n", i, EventCount);
        ok(!Exists(DestFile), "Flag set in message %d: the destination is still there\n", i);
    }
    /* it wins over a stop that is given in the same message, even where a stop keeps the new file, and a pause is
       reported as one but deletes the new file */
    Cancel = FALSE;
    hr = RunCopy(0, TRUE, 1, COPYFILE2_PROGRESS_PAUSE, &Cancel, 1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED) && Error == ERROR_REQUEST_PAUSED, "Pause and cancel flag: hr = %#lx, error %lu\n", hr, Error);
    ok(!Exists(DestFile), "Pause and cancel flag: the destination is still there\n");
    Cancel = FALSE;
    hr = RunCopy(0, TRUE, 0, COPYFILE2_PROGRESS_STOP, &Cancel, 0, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "Stop and cancel flag: hr = %#lx\n", hr);
    ok(!Exists(DestFile), "Stop and cancel flag: the destination is still there\n");
    WritePattern(SourceFile, 0, FILE_ATTRIBUTE_NORMAL);
    for (i = 0; i < 2; i++)
    {
        Cancel = FALSE;
        hr = RunCopy(0, TRUE, -1, 0, &Cancel, i, &Error);
        ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "Empty file, flag set in message %d: hr = %#lx\n", i, hr);
        ok(EventCount == 2 && !Exists(DestFile), "Empty file, flag set in message %d: %d messages\n", i, EventCount);
    }
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);

    /* Quiet ends the messages, the copy goes on */
    RemoveFiles();
    WritePattern(SourceFile, 5 << 20, FILE_ATTRIBUTE_NORMAL);
    hr = RunCopy(0, TRUE, 0, COPYFILE2_PROGRESS_QUIET, NULL, -1, &Error);
    ok(hr == S_OK && Error == ERROR_SUCCESS, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount == 1, "%d messages after quiet\n", EventCount);
    ok(SizeOf(DestFile) == (5 << 20) && SameContent(SourceFile, DestFile), "The copy differs\n");

    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 2, COPYFILE2_PROGRESS_QUIET, NULL, -1, &Error);
    ok(hr == S_OK && EventCount == 3, "hr = %#lx, %d messages\n", hr, EventCount);
    ok(SizeOf(DestFile) == (5 << 20) && SameContent(SourceFile, DestFile), "The copy differs\n");

    /* An answer that is not defined is a continue */
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 1, 77, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    CheckFullSequence(5 << 20, "undefined answer");
    ok(SameContent(SourceFile, DestFile), "The copy differs\n");

    /* The cancel flag, set before the call */
    DeleteFileW(DestFile);
    Cancel = TRUE;
    hr = RunCopy(0, TRUE, -1, 0, &Cancel, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) && Error == ERROR_REQUEST_ABORTED, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount == 2 && Events[0].Type == COPYFILE2_CALLBACK_STREAM_STARTED &&
       Events[1].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[1].StreamBytes == 0,
       "%d messages\n", EventCount);
    ok(!Exists(DestFile), "The destination is still there\n");

    Cancel = TRUE;
    hr = RunCopy(0, FALSE, -1, 0, &Cancel, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(!Exists(DestFile), "The destination is still there\n");

    /* and set while the copy runs: the copy stops before the next chunk */
    Cancel = FALSE;
    hr = RunCopy(0, TRUE, -1, 0, &Cancel, 1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) && Error == ERROR_REQUEST_ABORTED, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount == 3 && Events[1].Type == COPYFILE2_CALLBACK_CHUNK_STARTED &&
       Events[2].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[2].StreamBytes == 0,
       "%d messages\n", EventCount);
    ok(!Exists(DestFile), "The destination is still there\n");

    Cancel = FALSE;
    hr = RunCopy(0, TRUE, -1, 0, &Cancel, 3, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    ok(EventCount == 5 && Events[3].Type == COPYFILE2_CALLBACK_CHUNK_STARTED && Events[3].Chunk == 1 &&
       Events[4].Type == COPYFILE2_CALLBACK_STREAM_FINISHED, "%d messages\n", EventCount);
    if (EventCount == 5)
    {
        FirstChunk = Events[2].ChunkSize;
        ok(Events[4].StreamBytes == FirstChunk && FirstChunk > 0 && FirstChunk < (5 << 20),
           "The end carries %I64u bytes, the first chunk had %I64u\n", Events[4].StreamBytes, FirstChunk);
    }
    ok(!Exists(DestFile), "The destination is still there\n");

    RemoveFiles();
}

static void TestFailures(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Other[MAX_PATH];
    HANDLE hLock;
    DWORD Error;
    HRESULT hr;

    RemoveFiles();
    WritePattern(SourceFile, 50, FILE_ATTRIBUTE_NORMAL);

    /* The destination is there */
    WritePattern(DestFile, 7, FILE_ATTRIBUTE_NORMAL);
    hr = RunCopy(COPY_FILE_FAIL_IF_EXISTS, FALSE, -1, 0, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) && Error == ERROR_FILE_EXISTS, "hr = %#lx, error %lu\n", hr, Error);
    ok(SizeOf(DestFile) == 7, "The destination changed\n");

    /* the callback hears about it, once, and it is the only message */
    hr = RunCopy(COPY_FILE_FAIL_IF_EXISTS, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), "hr = %#lx\n", hr);
    ok(EventCount == 1 && Events[0].Type == COPYFILE2_CALLBACK_ERROR, "%d messages\n", EventCount);
    if (EventCount >= 1)
    {
        ok(Events[0].Phase == COPYFILE2_PHASE_PREPARE_DEST, "phase %d\n", Events[0].Phase);
        ok(Events[0].Stream == 1, "stream %lu\n", Events[0].Stream);
        ok(Events[0].Failure == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), "failure %#lx\n", Events[0].Failure);
        ok(Events[0].StreamSize == 50 && Events[0].TotalSize == 50, "sizes %I64u, %I64u\n", Events[0].StreamSize, Events[0].TotalSize);
        ok(Events[0].StreamBytes == 0 && Events[0].TotalBytes == 0, "bytes %I64u, %I64u\n", Events[0].StreamBytes, Events[0].TotalBytes);
    }
    ok(SizeOf(DestFile) == 7, "The destination changed\n");

    /* it is replaced without the flag */
    hr = RunCopy(0, FALSE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK && Error == ERROR_SUCCESS, "hr = %#lx, error %lu\n", hr, Error);
    ok(SizeOf(DestFile) == 50 && SameContent(SourceFile, DestFile), "The copy differs\n");

    /* unless it is read-only */
    SetFileAttributesW(DestFile, FILE_ATTRIBUTE_READONLY);
    WritePattern(SourceFile, 20, FILE_ATTRIBUTE_NORMAL);
    hr = RunCopy(0, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && Error == ERROR_ACCESS_DENIED, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount == 1 && Events[0].Type == COPYFILE2_CALLBACK_ERROR && Events[0].Phase == COPYFILE2_PHASE_PREPARE_DEST,
       "%d messages\n", EventCount);
    ok(SizeOf(DestFile) == 50, "The destination changed\n");
    SetFileAttributesW(DestFile, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(DestFile);

    /* The source is not there */
    DeleteFileW(SourceFile);
    hr = RunCopy(0, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && Error == ERROR_FILE_NOT_FOUND, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount == 1 && Events[0].Type == COPYFILE2_CALLBACK_ERROR && Events[0].Phase == COPYFILE2_PHASE_PREPARE_SOURCE &&
       Events[0].Stream == 0 && Events[0].Failure == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
       Events[0].StreamSize == 0 && Events[0].TotalSize == 0, "%d messages\n", EventCount);
    ok(!Exists(DestFile), "The destination was created\n");

    /* The directory of the destination is not there */
    WritePattern(SourceFile, 50, FILE_ATTRIBUTE_NORMAL);
    StringCchCopyW(Other, _countof(Other), DestFile);
    StringCchCatW(Other, _countof(Other), L".missing\\file.bin");
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(SourceFile, Other, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) && GetLastError() == ERROR_PATH_NOT_FOUND,
       "hr = %#lx, error %lu\n", hr, GetLastError());

    /* A source that another handle holds for writing without sharing */
    hLock = CreateFileW(SourceFile, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(hLock != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    hr = RunCopy(0, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) && Error == ERROR_SHARING_VIOLATION, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount == 1 && Events[0].Type == COPYFILE2_CALLBACK_ERROR && Events[0].Phase == COPYFILE2_PHASE_PREPARE_SOURCE,
       "%d messages\n", EventCount);
    ok(!Exists(DestFile), "The destination was created\n");
    CloseHandle(hLock);

    /* one that is open with sharing is copied */
    hLock = CreateFileW(SourceFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    hr = RunCopy(0, FALSE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(SameContent(SourceFile, DestFile), "The copy differs\n");
    CloseHandle(hLock);

    /* a file onto itself */
    hr = RunCopy(0, FALSE, -1, 0, NULL, -1, &Error);
    StringCchCopyW(Other, _countof(Other), SourceFile);
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(SourceFile, Other, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "hr = %#lx\n", hr);
    ok(SizeOf(SourceFile) == 50, "The file changed\n");

    RemoveFiles();
}

static void TestParameters(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    DWORD Sizes[3], Flags[] = { 0x80000000 };
    HRESULT hr;
    int i;

    RemoveFiles();
    WritePattern(SourceFile, 20, FILE_ATTRIBUTE_NORMAL);

    Sizes[0] = 0;
    Sizes[1] = sizeof(Params) - 1;
    Sizes[2] = sizeof(Params) + 8;
    for (i = 0; i < _countof(Sizes); i++)
    {
        ZeroMemory(&Params, sizeof(Params));
        Params.dwSize = Sizes[i];
        hr = pCopyFile2(SourceFile, DestFile, &Params);
        ok(hr == E_INVALIDARG, "dwSize %lu: hr = %#lx\n", Sizes[i], hr);
        ok(!Exists(DestFile), "dwSize %lu: the destination was created\n", Sizes[i]);
    }

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwCopyFlags = Flags[0];
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == E_INVALIDARG, "Flags %#lx: hr = %#lx\n", Flags[0], hr);
    ok(!Exists(DestFile), "The destination was created\n");

    /* flags that only matter for a restartable or a protected copy are accepted */
    Params.dwCopyFlags = 0x00000008 /* allow decrypted destination */ | 0x00000800 /* copy symlink */;
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(SameContent(SourceFile, DestFile), "The copy differs\n");

    /* a source that is not named */
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(NULL, DestFile, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) && GetLastError() == ERROR_PATH_NOT_FOUND,
       "hr = %#lx, error %lu\n", hr, GetLastError());
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(L"", DestFile, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) && GetLastError() == ERROR_PATH_NOT_FOUND,
       "hr = %#lx, error %lu\n", hr, GetLastError());
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(SourceFile, L"", &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) && GetLastError() == ERROR_PATH_NOT_FOUND,
       "hr = %#lx, error %lu\n", hr, GetLastError());

    RemoveFiles();
}

static void TestMetadata(void)
{
    WIN32_FILE_ATTRIBUTE_DATA Source, Dest;
    COPYFILE2_EXTENDED_PARAMETERS Params;
    FILETIME Time;
    HANDLE hFile;
    HRESULT hr;

    RemoveFiles();
    WritePattern(SourceFile, 30, FILE_ATTRIBUTE_NORMAL);

    /* old times, so that a copy made now cannot look the same by chance */
    Time.dwLowDateTime = 0x11223344;
    Time.dwHighDateTime = 0x01d00000;
    hFile = CreateFileW(SourceFile, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        SetFileTime(hFile, &Time, &Time, &Time);
        CloseHandle(hFile);
    }
    SetFileAttributesW(SourceFile, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE);

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);

    ZeroMemory(&Source, sizeof(Source));
    ZeroMemory(&Dest, sizeof(Dest));
    GetFileAttributesExW(SourceFile, GetFileExInfoStandard, &Source);
    GetFileAttributesExW(DestFile, GetFileExInfoStandard, &Dest);
    ok(Dest.dwFileAttributes == Source.dwFileAttributes, "Attributes %#lx and %#lx\n", Source.dwFileAttributes, Dest.dwFileAttributes);
    ok(!memcmp(&Source.ftLastWriteTime, &Dest.ftLastWriteTime, sizeof(FILETIME)),
       "The write times differ: %08lx%08lx and %08lx%08lx\n", Source.ftLastWriteTime.dwHighDateTime, Source.ftLastWriteTime.dwLowDateTime,
       Dest.ftLastWriteTime.dwHighDateTime, Dest.ftLastWriteTime.dwLowDateTime);

    RemoveFiles();
}

/* Alternate data streams exist on NTFS only */
static void TestStreams(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Stream[MAX_PATH];
    HANDLE hFile;
    DWORD Written;
    HRESULT hr;
    DWORD Error;
    int i;

    RemoveFiles();
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);
    StringCchPrintfW(Stream, _countof(Stream), L"%s:ads1", SourceFile);
    hFile = CreateFileW(Stream, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        skip("The volume has no alternate data streams\n");
        RemoveFiles();
        return;
    }
    WriteFile(hFile, "abcdefghijklmnopqrstuvwxyz", 26, &Written, NULL);
    CloseHandle(hFile);

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.pProgressRoutine = Progress;
    EventCount = 0;
    ActionAt = -1;
    CancelAt = -1;
    MainThread = GetCurrentThreadId();
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);

    /* the unnamed stream first, then the named one, with totals over both */
    ok(EventCount == 8, "%d messages\n", EventCount);
    if (EventCount == 8)
    {
        ok(Events[0].Type == COPYFILE2_CALLBACK_STREAM_STARTED && Events[0].Stream == 1 &&
           Events[0].StreamSize == 100 && Events[0].TotalSize == 126, "message 0\n");
        ok(Events[3].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[3].Stream == 1 &&
           Events[3].StreamBytes == 100 && Events[3].TotalBytes == 100, "message 3\n");
        ok(Events[4].Type == COPYFILE2_CALLBACK_STREAM_STARTED && Events[4].Stream == 2 &&
           Events[4].StreamSize == 26 && Events[4].TotalSize == 126, "message 4\n");
        ok(Events[6].Type == COPYFILE2_CALLBACK_CHUNK_FINISHED && Events[6].Stream == 2 &&
           Events[6].StreamBytes == 26 && Events[6].TotalBytes == 126, "message 6\n");
        ok(Events[7].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[7].Stream == 2 &&
           Events[7].StreamBytes == 26 && Events[7].TotalBytes == 126, "message 7\n");
    }

    StringCchPrintfW(Stream, _countof(Stream), L"%s:ads1", DestFile);
    hFile = CreateFileW(Stream, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "The stream is not in the copy: %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        char Data[32] = { 0 };

        ReadFile(hFile, Data, sizeof(Data), &Written, NULL);
        ok(Written == 26 && !memcmp(Data, "abcdefghijklmnopqrstuvwxyz", 26), "The stream has %lu bytes\n", Written);
        CloseHandle(hFile);
    }

    /* a read-only source with a stream: the stream is copied and the new file is read-only in the end */
    SetFileAttributesW(DestFile, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(DestFile);
    SetFileAttributesW(SourceFile, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE);
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(GetFileAttributesW(DestFile) == (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE),
       "The attributes of the copy are %#lx\n", GetFileAttributesW(DestFile));
    StringCchPrintfW(Stream, _countof(Stream), L"%s:ads1", DestFile);
    hFile = CreateFileW(Stream, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "The stream of the read-only file is not in the copy: %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    UNREFERENCED_PARAMETER(Error);
    UNREFERENCED_PARAMETER(i);
    RemoveFiles();
}


/* An extended name is used as it is: "name." and "name" are two files */
static void TestExtendedNames(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Plain[MAX_PATH], Dotted[MAX_PATH], Copy[MAX_PATH];
    HANDLE hFile;
    DWORD Written;
    HRESULT hr;

    StringCchPrintfW(Plain, _countof(Plain), L"\\\\?\\%s.ext", SourceFile);
    StringCchPrintfW(Dotted, _countof(Dotted), L"\\\\?\\%s.ext.", SourceFile);
    StringCchPrintfW(Copy, _countof(Copy), L"\\\\?\\%s", DestFile);
    RemoveFiles();

    hFile = CreateFileW(Dotted, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        skip("The volume has no file names that end in a dot\n");
        return;
    }
    WriteFile(hFile, "dotted", 6, &Written, NULL);
    CloseHandle(hFile);
    hFile = CreateFileW(Plain, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile(hFile, "plain-name", 10, &Written, NULL);
        CloseHandle(hFile);
    }

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    hr = pCopyFile2(Dotted, Copy, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(SizeOf(DestFile) == 6, "The copy of the name with the dot has %I64u bytes\n", SizeOf(DestFile));

    DeleteFileW(Dotted);
    DeleteFileW(Plain);
    RemoveFiles();
}

static WCHAR OtherDirectory[MAX_PATH];

static COPYFILE2_MESSAGE_ACTION CALLBACK ChangeDirectory(const COPYFILE2_MESSAGE *Message, PVOID Context)
{
    if (Message->Type == COPYFILE2_CALLBACK_CHUNK_STARTED)
    {
        SetCurrentDirectoryW(OtherDirectory);
        return COPYFILE2_PROGRESS_CANCEL;
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

static void TestRelativeNames(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Directory[MAX_PATH], Name[MAX_PATH], Relative[MAX_PATH];
    HRESULT hr;
    PWSTR Slash;

    /* The names are relative to the directory the call was made in, also when it is left in a callback */
    GetCurrentDirectoryW(_countof(Directory), Directory);
    StringCchCopyW(Name, _countof(Name), SourceFile);
    Slash = wcsrchr(Name, L'\\');
    ok(Slash != NULL, "No directory in %ls\n", SourceFile);
    if (!Slash)
        return;

    StringCchCopyW(OtherDirectory, _countof(OtherDirectory), SourceFile);
    StringCchCatW(OtherDirectory, _countof(OtherDirectory), L".dir");
    CreateDirectoryW(OtherDirectory, NULL);

    RemoveFiles();
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);
    *Slash = UNICODE_NULL;
    ok(SetCurrentDirectoryW(Name), "SetCurrentDirectoryW failed with %lu\n", GetLastError());

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    StringCchCopyW(Relative, _countof(Relative), Slash + 1);
    hr = pCopyFile2(Relative, wcsrchr(DestFile, L'\\') + 1, &Params);
    ok(hr == S_OK, "A copy between relative names: hr = %#lx\n", hr);
    ok(SizeOf(DestFile) == 100 && SameContent(SourceFile, DestFile), "The copy differs\n");
    DeleteFileW(DestFile);

    Params.pProgressRoutine = ChangeDirectory;
    hr = pCopyFile2(Relative, wcsrchr(DestFile, L'\\') + 1, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "hr = %#lx\n", hr);
    SetCurrentDirectoryW(Directory);
    ok(!Exists(DestFile), "The new file is still in the first directory\n");
    StringCchCatW(OtherDirectory, _countof(OtherDirectory), L"\\");
    StringCchCatW(OtherDirectory, _countof(OtherDirectory), wcsrchr(DestFile, L'\\') + 1);
    ok(!Exists(OtherDirectory), "The new file was made in the second directory\n");

    SetCurrentDirectoryW(Directory);
    RemoveFiles();
    *wcsrchr(OtherDirectory, L'\\') = UNICODE_NULL;
    RemoveDirectoryW(OtherDirectory);
}

/* A file is never copied onto itself, however its name is written (an extended name, the short name) */
static void TestSameFile(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Short[MAX_PATH], Extended[MAX_PATH];
    HRESULT hr;
    DWORD ShortLength;

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    StringCchPrintfW(Extended, _countof(Extended), L"\\\\?\\%s", SourceFile);

    /* an empty file has no file index on some file systems: it is the same file all the same */
    RemoveFiles();
    WritePattern(SourceFile, 0, FILE_ATTRIBUTE_NORMAL);
    hr = pCopyFile2(SourceFile, Extended, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "empty file onto its extended name: hr = %#lx\n", hr);
    ok(Exists(SourceFile), "empty file onto its extended name: the file is gone\n");
    hr = pCopyFile2(Extended, SourceFile, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "empty file from its extended name: hr = %#lx\n", hr);
    ok(Exists(SourceFile), "empty file from its extended name: the file is gone\n");
    hr = pCopyFile2(Extended, Extended, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "empty file onto itself, extended names: hr = %#lx\n", hr);
    ok(Exists(SourceFile), "empty file onto itself, extended names: the file is gone\n");

    ShortLength = GetShortPathNameW(SourceFile, Short, _countof(Short));
    if (ShortLength && ShortLength < _countof(Short) && _wcsicmp(Short, SourceFile))
    {
        hr = pCopyFile2(SourceFile, Short, &Params);
        ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "empty file onto its short name: hr = %#lx\n", hr);
        ok(Exists(SourceFile), "empty file onto its short name: the file is gone\n");
        hr = pCopyFile2(Short, SourceFile, &Params);
        ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "empty file from its short name: hr = %#lx\n", hr);
        ok(Exists(SourceFile), "empty file from its short name: the file is gone\n");

        /* a cancel after the check must not take the source with it */
        {
            BOOL Cancel = TRUE;

            Params.pfCancel = &Cancel;
            hr = pCopyFile2(SourceFile, Short, &Params);
            Params.pfCancel = NULL;
            ok(FAILED(hr), "a cancelled copy onto the short name: hr = %#lx\n", hr);
            ok(Exists(SourceFile), "a cancelled copy onto the short name took the source\n");
        }
    }
    else
    {
        skip("The volume has no short names\n");
    }

    /* a file with data keeps it */
    WritePattern(SourceFile, 1000, FILE_ATTRIBUTE_NORMAL);
    hr = pCopyFile2(SourceFile, Extended, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "file onto its extended name: hr = %#lx\n", hr);
    ok(SizeOf(SourceFile) == 1000, "file onto its extended name: %I64u bytes are left of 1000\n", SizeOf(SourceFile));
    RemoveFiles();
}

/* The flags that change what is opened or copied */
static void TestFlags(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    HRESULT hr;

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);

    /* COPY_FILE_OPEN_SOURCE_FOR_WRITE changes nothing that can be seen: a read only source is copied with it as without it (measured on Windows 10) */
    RemoveFiles();
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_READONLY);
    Params.dwCopyFlags = 0;
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "read-only source, no flags: hr = %#lx\n", hr);
    SetFileAttributesW(DestFile, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(DestFile);
    Params.dwCopyFlags = COPY_FILE_OPEN_SOURCE_FOR_WRITE;
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "read-only source, OPEN_SOURCE_FOR_WRITE: hr = %#lx\n", hr);
    ok(Exists(DestFile) && SizeOf(DestFile) == 100, "read-only source, OPEN_SOURCE_FOR_WRITE: the destination is not a copy\n");
    SetFileAttributesW(DestFile, FILE_ATTRIBUTE_NORMAL);
    RemoveFiles();
}

/* What a pause leaves behind and what the next call does with it (COPY_FILE_RESUME_FROM_PAUSE) */
static void TestResume(void)
{
    ULONGLONG First;
    DWORD Error;
    HRESULT hr;

    RemoveFiles();
    WritePattern(SourceFile, 5 << 20, FILE_ATTRIBUTE_NORMAL);

    /* After one chunk the new file holds that chunk and nothing else */
    hr = RunCopy(0, TRUE, 2, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED) && Error == ERROR_REQUEST_PAUSED, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount == 4 && Events[2].Type == COPYFILE2_CALLBACK_CHUNK_FINISHED, "%d messages\n", EventCount);
    First = Events[2].ChunkSize;
    /* (Windows 10 keeps a record of 56 bytes at the start of a paused file, so the first block is not compared) */
    ok(SizeOf(DestFile) == First && SameRange(SourceFile, DestFile, 4096, First),
       "The paused file has %I64u bytes, the first chunk had %I64u\n", SizeOf(DestFile), First);

    /* The next call goes on after it: the chunk numbers and the counters continue */
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK && Error == ERROR_SUCCESS, "hr = %#lx, error %lu\n", hr, Error);
    ok(EventCount > 4 && Events[0].Type == COPYFILE2_CALLBACK_STREAM_STARTED && Events[0].StreamSize == (5 << 20) &&
       Events[0].TotalSize == (5 << 20), "%d messages\n", EventCount);
    if (EventCount > 4)
    {
        ok(Events[1].Type == COPYFILE2_CALLBACK_CHUNK_STARTED && Events[1].Chunk == 1, "The first chunk of the second call is %I64u\n", Events[1].Chunk);
        ok(Events[2].Type == COPYFILE2_CALLBACK_CHUNK_FINISHED && Events[2].StreamBytes == 2 * First && Events[2].TotalBytes == 2 * First,
           "The first chunk of the second call ends at %I64u (%I64u in all)\n", Events[2].StreamBytes, Events[2].TotalBytes);
        ok(LastEvent()->Type == COPYFILE2_CALLBACK_STREAM_FINISHED && LastEvent()->StreamBytes == (5 << 20) &&
           LastEvent()->TotalBytes == (5 << 20), "The end reports %I64u bytes\n", LastEvent()->StreamBytes);
    }
    ok(SizeOf(DestFile) == (5 << 20) && SameContent(SourceFile, DestFile), "The resumed copy differs\n");

    /* A pause before any data was copied is resumed from the start; the flag lets an existing file be used even
       with the flag that forbids one */
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 0, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED) && Exists(DestFile), "hr = %#lx\n", hr);
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE | COPY_FILE_FAIL_IF_EXISTS, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(EventCount > 2 && Events[1].Chunk == 0, "The copy did not start at the first chunk\n");
    ok(SizeOf(DestFile) == (5 << 20) && SameContent(SourceFile, DestFile), "The resumed copy differs\n");

    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 4, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED), "hr = %#lx\n", hr);
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE | COPY_FILE_FAIL_IF_EXISTS, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(EventCount > 2 && Events[1].Chunk == 2, "The copy went on at chunk %I64u\n", EventCount > 2 ? Events[1].Chunk : 0);
    ok(SizeOf(DestFile) == (5 << 20) && SameContent(SourceFile, DestFile), "The resumed copy differs\n");

    /* A file that is not a paused copy: without a conflict flag it is replaced from the start, with one it is a
       conflict */
    DeleteFileW(DestFile);
    WritePattern(DestFile, 7, FILE_ATTRIBUTE_NORMAL);
    {
        WCHAR Other[MAX_PATH];
        COPYFILE2_EXTENDED_PARAMETERS Params;

        StringCchCopyW(Other, _countof(Other), DestFile);
        ZeroMemory(&Params, sizeof(Params));
        Params.dwSize = sizeof(Params);
        Params.dwCopyFlags = COPY_FILE_RESUME_FROM_PAUSE | COPY_FILE_FAIL_IF_EXISTS;
        SetLastError(0xdeadbeef);
        /* the 7 bytes happen to be the start of the pattern, so make them different */
        {
            HANDLE hFile = CreateFileW(DestFile, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            DWORD Written;

            WriteFile(hFile, "ZZZZZZZ", 7, &Written, NULL);
            CloseHandle(hFile);
        }
        hr = pCopyFile2(SourceFile, Other, &Params);
        ok(hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) && GetLastError() == ERROR_ALREADY_EXISTS, "hr = %#lx, error %lu\n", hr, GetLastError());
        ok(SizeOf(DestFile) == 7, "The file changed\n");
    }
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(EventCount > 2 && Events[1].Chunk == 0, "The copy did not start at the first chunk\n");
    ok(SizeOf(DestFile) == (5 << 20) && SameContent(SourceFile, DestFile), "The copy differs\n");

    /* The two flags do not go together */
    DeleteFileW(DestFile);
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE | COPY_FILE_RESTARTABLE, FALSE, -1, 0, NULL, -1, &Error);
    ok(hr == E_INVALIDARG && Error == ERROR_INVALID_PARAMETER, "hr = %#lx, error %lu\n", hr, Error);
    ok(!Exists(DestFile), "The destination was created\n");

    RemoveFiles();
}

/* Streams: what stop and pause keep, what an overwritten file loses, what a resume skips */
static void TestStreamActions(void)
{
    static const char A1[] = "abcdefghijklmnopqrstuvwxyz";
    static const char B2[] = "12345";
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Other[MAX_PATH];
    DWORD Error;
    HRESULT hr;
    int i;

    RemoveFiles();
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);
    if (!MakeStream(SourceFile, L":a1", A1))
    {
        skip("The volume has no alternate data streams\n");
        RemoveFiles();
        return;
    }
    MakeStream(SourceFile, L":b2", B2);

    /* Three streams, four messages each. A stop keeps the new file only at the start or the end of the first
       stream (Windows 10), and then there is no named stream in it yet. */
    for (i = 0; i < 12; i++)
    {
        DeleteFileW(DestFile);
        hr = RunCopy(0, TRUE, i, COPYFILE2_PROGRESS_STOP, NULL, -1, &Error);
        ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), "Stop at %d: hr = %#lx\n", i, hr);
        if (i == 0 || i == 3)
        {
            ok(Exists(DestFile) && SizeOf(DestFile) == 100, "Stop at %d: the new file is %I64u bytes\n", i, SizeOf(DestFile));
            ok(StreamSizeOf(DestFile, L":a1") == -1, "Stop at %d: the stream is there\n", i);
        }
        else
        {
            ok(!Exists(DestFile), "Stop at %d: the new file is still there\n", i);
        }
    }

    /* A pause keeps it everywhere, with the streams that were started */
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 4, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED), "hr = %#lx\n", hr);
    ok(SizeOf(DestFile) == 100 && StreamSizeOf(DestFile, L":a1") == 0 && StreamSizeOf(DestFile, L":b2") == -1,
       "Pause at the start of the second stream: %I64u, %I64d, %I64d\n", SizeOf(DestFile),
       StreamSizeOf(DestFile, L":a1"), StreamSizeOf(DestFile, L":b2"));
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 7, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED), "hr = %#lx\n", hr);
    ok(StreamSizeOf(DestFile, L":a1") == 26 && StreamSizeOf(DestFile, L":b2") == -1, "Pause at the end of the second stream: %I64d, %I64d\n",
       StreamSizeOf(DestFile, L":a1"), StreamSizeOf(DestFile, L":b2"));

    /* and a resume skips the streams that are done */
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 5, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED), "hr = %#lx\n", hr);
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(EventCount == 8 && Events[0].Type == COPYFILE2_CALLBACK_STREAM_STARTED && Events[0].Stream == 2 &&
       Events[0].TotalSize == 131, "%d messages, the first is for stream %lu\n", EventCount, Events[0].Stream);
    if (EventCount == 8)
    {
        ok(Events[2].TotalBytes == 126, "The counters restarted: %I64u\n", Events[2].TotalBytes);
        ok(Events[7].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[7].Stream == 3 && Events[7].TotalBytes == 131, "The last message\n");
    }
    ok(SameContent(SourceFile, DestFile) && StreamHolds(DestFile, L":a1", A1) && StreamHolds(DestFile, L":b2", B2),
       "The resumed copy differs\n");

    /* An overwritten file loses the streams it had, also when the copy is stopped right away */
    DeleteFileW(DestFile);
    WritePattern(DestFile, 7, FILE_ATTRIBUTE_NORMAL);
    MakeStream(DestFile, L":old", "old data");
    MakeStream(DestFile, L":a1", "this is longer than the new stream, and goes");
    hr = RunCopy(0, FALSE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(StreamSizeOf(DestFile, L":old") == -1, "The old stream is still in the new file\n");
    ok(StreamHolds(DestFile, L":a1", A1), "The stream of the same name was not replaced\n");

    DeleteFileW(DestFile);
    WritePattern(DestFile, 7, FILE_ATTRIBUTE_NORMAL);
    MakeStream(DestFile, L":old", "old data");
    hr = RunCopy(0, TRUE, 0, COPYFILE2_PROGRESS_STOP, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) && Exists(DestFile), "hr = %#lx\n", hr);
    ok(StreamSizeOf(DestFile, L":old") == -1, "The old stream survived a stop\n");

    /* A source without streams over a destination with some */
    DeleteFileW(DestFile);
    WritePattern(DestFile, 7, FILE_ATTRIBUTE_NORMAL);
    MakeStream(DestFile, L":old", "old data");
    StringCchPrintfW(Other, _countof(Other), L"%s.plain", SourceFile);
    DeleteFileW(Other);
    WritePattern(Other, 30, FILE_ATTRIBUTE_NORMAL);
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    hr = pCopyFile2(Other, DestFile, &Params);
    ok(hr == S_OK && StreamSizeOf(DestFile, L":old") == -1 && SizeOf(DestFile) == 30, "hr = %#lx\n", hr);
    DeleteFileW(Other);

    /* A resume with nothing to resume is a plain copy: an empty named stream still sends its messages */
    RemoveFiles();
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);
    ok(MakeStream(SourceFile, L":empty", ""), "The empty stream was not made\n");
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE, TRUE, -1, 0, NULL, -1, &Error);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(EventCount == 6 && Events[4].Type == COPYFILE2_CALLBACK_STREAM_STARTED && Events[4].Stream == 2 &&
       Events[5].Type == COPYFILE2_CALLBACK_STREAM_FINISHED && Events[5].Stream == 2, "%d messages\n", EventCount);
    ok(StreamSizeOf(DestFile, L":empty") == 0, "The empty stream is not in the copy\n");

    /* An empty first stream and a long second one: a copy paused in the second goes on there */
    RemoveFiles();
    WritePattern(SourceFile, 0, FILE_ATTRIBUTE_NORMAL);
    MakeBigStream(SourceFile, L":big", 3 << 20);
    hr = RunCopy(0, TRUE, 4, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED), "hr = %#lx\n", hr);
    ok(EventCount == 6 && Events[4].Type == COPYFILE2_CALLBACK_CHUNK_FINISHED && Events[4].Stream == 2, "%d messages\n", EventCount);
    if (EventCount == 6)
    {
        ULONGLONG First = Events[4].ChunkSize;

        ok(StreamSizeOf(DestFile, L":big") == (LONGLONG)First, "The paused stream has %I64d bytes, the chunk had %I64u\n",
           StreamSizeOf(DestFile, L":big"), First);
        hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE, TRUE, -1, 0, NULL, -1, &Error);
        ok(hr == S_OK, "hr = %#lx\n", hr);
        ok(Events[0].Type == COPYFILE2_CALLBACK_STREAM_STARTED && Events[0].Stream == 2 && EventCount > 4 &&
           Events[1].Type == COPYFILE2_CALLBACK_CHUNK_STARTED && Events[1].Chunk == 1,
           "The resumed copy starts with stream %lu, chunk %I64u\n", Events[0].Stream, Events[1].Chunk);
    }
    ok(SizeOf(DestFile) == 0 && StreamSizeOf(DestFile, L":big") == (3 << 20), "The resumed copy is not complete\n");

    RemoveFiles();
}

/* A resume that goes on in the file drops the streams the source does not have, and does not skip the cancel flag */
static void TestResumeDetails(void)
{
    BOOL Cancel;
    DWORD Error;
    HRESULT hr;

    RemoveFiles();
    WritePattern(SourceFile, 5 << 20, FILE_ATTRIBUTE_NORMAL);

    /* an identical copy of the source that is already there is not resumed past a cancel flag */
    CopyFileW(SourceFile, DestFile, FALSE);
    Cancel = TRUE;
    hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE, TRUE, -1, 0, &Cancel, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED) && Error == ERROR_REQUEST_ABORTED, "hr = %#lx, error %lu\n", hr, Error);
    ok(!Exists(DestFile), "The cancelled copy left the destination\n");

    /* a stream that the source has not is not kept by a resumed copy */
    DeleteFileW(DestFile);
    hr = RunCopy(0, TRUE, 2, COPYFILE2_PROGRESS_PAUSE, NULL, -1, &Error);
    ok(hr == HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED), "hr = %#lx\n", hr);
    if (MakeStream(DestFile, L":old", "old data"))
    {
        hr = RunCopy(COPY_FILE_RESUME_FROM_PAUSE, TRUE, -1, 0, NULL, -1, &Error);
        ok(hr == S_OK, "hr = %#lx\n", hr);
        /* (Windows 10 leaves it: it resumes a file it paused as it is. ReactOS decides by the bytes and makes the
           result a copy of the source, so the stream goes.) */
        if (is_reactos())
            ok(StreamSizeOf(DestFile, L":old") == -1, "The old stream is still in the resumed copy\n");
        ok(SizeOf(DestFile) == (5 << 20) && SameContent(SourceFile, DestFile), "The resumed copy differs\n");
    }
    else
    {
        skip("The volume has no alternate data streams\n");
    }

    RemoveFiles();
}

static const WCHAR *ReplacedName, *MovedName;
static BOOL Replaced, Moved;

static COPYFILE2_MESSAGE_ACTION CALLBACK ReplaceSource(const COPYFILE2_MESSAGE *Message, PVOID Context)
{
    if (Message->Type == COPYFILE2_CALLBACK_STREAM_STARTED && !Replaced)
    {
        Replaced = TRUE;
        Moved = MoveFileW(ReplacedName, MovedName);
        if (Moved)
        {
            HANDLE hFile = CreateFileW(ReplacedName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            char Data[100];
            DWORD Written;

            memset(Data, 'R', sizeof(Data));
            if (hFile != INVALID_HANDLE_VALUE)
            {
                WriteFile(hFile, Data, sizeof(Data), &Written, NULL);
                CloseHandle(hFile);
            }
            MakeStream(ReplacedName, L":a1", "RRRRRRRRRRRRRRRRRRRRRRRRRR");
        }
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

/* A callback that renames the source and puts another file under its name changes nothing for the copy: the
   original file and its streams are what is copied (Windows 10) */
static void TestSourceReplaced(void)
{
    static const char A1[] = "abcdefghijklmnopqrstuvwxyz";
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Moved2[MAX_PATH];
    BOOL Streams;
    HRESULT hr;

    RemoveFiles();
    StringCchPrintfW(Moved2, _countof(Moved2), L"%s.moved", SourceFile);
    DeleteFileW(Moved2);
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);
    Streams = MakeStream(SourceFile, L":a1", A1);

    ReplacedName = SourceFile;
    MovedName = Moved2;
    Replaced = FALSE;
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.pProgressRoutine = ReplaceSource;
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(Replaced, "The callback did not run\n");
    if (!Moved)
    {
        skip("The source cannot be renamed while it is copied\n");
    }
    else
    {
        ok(hr == S_OK, "hr = %#lx\n", hr);
        ok(SizeOf(DestFile) == 100 && SameContent(Moved2, DestFile), "The copy is not the original file\n");
        if (Streams)
            ok(StreamHolds(DestFile, L":a1", A1), "The copy has not the original stream\n");
    }

    DeleteFileW(Moved2);
    RemoveFiles();
}

static void TestSharing(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    HANDLE hFile;
    HRESULT hr;

    RemoveFiles();
    WritePattern(SourceFile, 100, FILE_ATTRIBUTE_NORMAL);
    WritePattern(DestFile, 5, FILE_ATTRIBUTE_NORMAL);
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);

    /* a destination that another handle reads, and one it reads and writes while sharing writing, are used */
    hFile = CreateFileW(DestFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "reader on the destination: hr = %#lx\n", hr);
    CloseHandle(hFile);

    hFile = CreateFileW(DestFile, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "reader and writer sharing writing on the destination: hr = %#lx\n", hr);
    CloseHandle(hFile);

    /* a handle that can delete the destination stops the copy, even when it shares deleting */
    hFile = CreateFileW(DestFile, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "delete handle on the destination: hr = %#lx\n", hr);
    ok(SizeOf(DestFile) == 100, "The destination changed\n");
    CloseHandle(hFile);

    /* a source that can be deleted while it is read is copied */
    DeleteFileW(DestFile);
    hFile = CreateFileW(SourceFile, DELETE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK, "delete handle on the source: hr = %#lx\n", hr);
    CloseHandle(hFile);

    RemoveFiles();
}

static void TestHiddenDestination(void)
{
    COPYFILE2_EXTENDED_PARAMETERS Params;
    HRESULT hr;

    RemoveFiles();
    WritePattern(SourceFile, 20, FILE_ATTRIBUTE_NORMAL);
    WritePattern(DestFile, 5, FILE_ATTRIBUTE_HIDDEN);
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);

    /* a hidden destination is replaced only by a copy that is hidden itself */
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), "hr = %#lx\n", hr);
    ok(SizeOf(DestFile) == 5 && IsHidden(DestFile), "The destination changed\n");

    SetFileAttributesW(SourceFile, FILE_ATTRIBUTE_HIDDEN);
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK && SizeOf(DestFile) == 20 && IsHidden(DestFile), "hr = %#lx\n", hr);

    RemoveFiles();
}

/* The reparse data of a file (the link itself, not its target), or FALSE */
static BOOL ReparseDataOf(PCWSTR Name, BYTE *Data, DWORD Size, PDWORD Length)
{
    HANDLE hFile = CreateFileW(Name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BOOL Ret;

    *Length = 0;
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = DeviceIoControl(hFile, FSCTL_GET_REPARSE_POINT, NULL, 0, Data, Size, Length, NULL);
    CloseHandle(hFile);
    return Ret;
}

static BOOL SameLink(PCWSTR A, PCWSTR B)
{
    BYTE DataA[1024], DataB[1024];
    DWORD LengthA, LengthB;

    return ReparseDataOf(A, DataA, sizeof(DataA), &LengthA) && ReparseDataOf(B, DataB, sizeof(DataB), &LengthB) &&
           LengthA == LengthB && !memcmp(DataA, DataB, LengthA);
}

static BOOL IsLink(PCWSTR Name)
{
    DWORD Attributes = GetFileAttributesW(Name);

    return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static void TestLinks(void)
{
    BOOLEAN (WINAPI *pCreateSymbolicLinkW)(LPCWSTR, LPCWSTR, DWORD);
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Link[MAX_PATH];
    BYTE Original[1024];
    DWORD OriginalLength;
    BOOL Cancel, Follows;
    HRESULT hr;

    pCreateSymbolicLinkW = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateSymbolicLinkW");
    RemoveFiles();
    StringCchPrintfW(Link, _countof(Link), L"%s.link", SourceFile);
    SetFileAttributesW(Link, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(Link);
    WritePattern(SourceFile, 40, FILE_ATTRIBUTE_NORMAL);
    if (!pCreateSymbolicLinkW || !pCreateSymbolicLinkW(Link, SourceFile, 0))
    {
        skip("The volume has no symbolic links\n");
        RemoveFiles();
        return;
    }
    ok(ReparseDataOf(Link, Original, sizeof(Original), &OriginalLength), "The link has no reparse data\n");

    /* A volume whose links are not followed when a file is opened (ReactOS NTFS) has nothing to copy by target */
    {
        HANDLE hLink = CreateFileW(Link, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        LARGE_INTEGER LinkSize;

        Follows = FALSE;
        if (hLink != INVALID_HANDLE_VALUE)
        {
            Follows = GetFileSizeEx(hLink, &LinkSize) && LinkSize.QuadPart == 40;
            CloseHandle(hLink);
        }
    }
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);

    /* without the flag the target is what is copied */
    hr = pCopyFile2(Link, DestFile, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    if (Follows)
    {
        ok(SizeOf(DestFile) == 40 && SameContent(SourceFile, DestFile) && !IsLink(DestFile),
           "The copy of the target is not a plain file\n");
    }
    else
    {
        skip("The link is not followed when it is opened\n");
    }
    DeleteFileW(DestFile);

    /* with the flag the link is copied, without any message, and the cancel flag does not stop it (Windows 10) */
    Params.dwCopyFlags = COPY_FILE_COPY_SYMLINK;
    Params.pProgressRoutine = Progress;
    EventCount = 0;
    ActionAt = -1;
    CancelAt = -1;
    MainThread = GetCurrentThreadId();
    hr = pCopyFile2(Link, DestFile, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(EventCount == 0, "%d messages for a link\n", EventCount);
    ok(IsLink(DestFile) && SameLink(Link, DestFile), "The copy is not a link with the same data\n");
    if (Follows)
        ok(SameContent(SourceFile, DestFile), "The link does not lead to the target\n");

    /* it replaces a plain file and a link, and a link onto itself is left alone */
    DeleteFileW(DestFile);
    WritePattern(DestFile, 100, FILE_ATTRIBUTE_NORMAL);
    Params.pProgressRoutine = NULL;
    hr = pCopyFile2(Link, DestFile, &Params);
    ok(hr == S_OK && IsLink(DestFile) && SameLink(Link, DestFile), "onto a plain file: hr = %#lx\n", hr);
    hr = pCopyFile2(Link, DestFile, &Params);
    ok(hr == S_OK && IsLink(DestFile) && SameLink(Link, DestFile), "onto a link: hr = %#lx\n", hr);
    hr = pCopyFile2(Link, Link, &Params);
    ok(hr == S_OK && IsLink(Link) && SameLink(Link, DestFile) && SizeOf(SourceFile) == 40 && !IsLink(SourceFile),
       "a link onto itself: hr = %#lx\n", hr);

    Cancel = TRUE;
    Params.pfCancel = &Cancel;
    DeleteFileW(DestFile);
    SetLastError(0xdeadbeef);
    hr = pCopyFile2(Link, DestFile, &Params);
    ok(hr == S_OK && GetLastError() == ERROR_SUCCESS && IsLink(DestFile),
       "a link with the cancel flag set: hr = %#lx, error %lu\n", hr, GetLastError());

    /* a source that is no link is copied as usual */
    Params.pfCancel = NULL;
    DeleteFileW(DestFile);
    hr = pCopyFile2(SourceFile, DestFile, &Params);
    ok(hr == S_OK && SizeOf(DestFile) == 40 && !IsLink(DestFile), "hr = %#lx\n", hr);

    SetFileAttributesW(Link, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(Link);
    RemoveFiles();
}

/* A file is not copied onto its hard link either */
static void TestHardLink(void)
{
    BOOL (WINAPI *pCreateHardLinkW)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);
    COPYFILE2_EXTENDED_PARAMETERS Params;
    WCHAR Link[MAX_PATH];
    HRESULT hr;

    pCreateHardLinkW = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateHardLinkW");
    RemoveFiles();
    StringCchPrintfW(Link, _countof(Link), L"%s.hard", SourceFile);
    DeleteFileW(Link);
    WritePattern(SourceFile, 300, FILE_ATTRIBUTE_NORMAL);
    if (!pCreateHardLinkW || !pCreateHardLinkW(Link, SourceFile, NULL))
    {
        skip("The volume has no hard links\n");
        RemoveFiles();
        return;
    }

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    hr = pCopyFile2(SourceFile, Link, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "onto the hard link: hr = %#lx\n", hr);
    hr = pCopyFile2(Link, SourceFile, &Params);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "from the hard link: hr = %#lx\n", hr);
    ok(SizeOf(SourceFile) == 300 && SizeOf(Link) == 300, "The file changed\n");

    DeleteFileW(Link);
    RemoveFiles();
}

START_TEST(CopyFile2)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    WCHAR TempDir[MAX_PATH];

    pCopyFile2 = (FN_CopyFile2)GetProcAddress(hKernel32, "CopyFile2");
    if (!pCopyFile2)
    {
        ok(!is_reactos(), "CopyFile2 is missing\n");
        skip("CopyFile2 is not available\n");
        return;
    }

    GetTempPathW(_countof(TempDir), TempDir);
    StringCchPrintfW(SourceFile, _countof(SourceFile), L"%sCopyFile2-%lu.src", TempDir, GetCurrentProcessId());
    StringCchPrintfW(DestFile, _countof(DestFile), L"%sCopyFile2-%lu.dst", TempDir, GetCurrentProcessId());
    RemoveFiles();

    TestCallbackSequence();
    TestActions();
    TestFailures();
    TestParameters();
    TestMetadata();
    TestStreams();
    TestRelativeNames();
    TestExtendedNames();
    TestSameFile();
    TestFlags();
    TestResume();
    TestResumeDetails();
    TestStreamActions();
    TestSourceReplaced();
    TestSharing();
    TestHiddenDestination();
    TestLinks();
    TestHardLink();

    RemoveFiles();
}
