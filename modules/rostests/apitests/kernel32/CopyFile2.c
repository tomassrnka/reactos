/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for CopyFile2, its progress callback and its cancellation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

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
            Event->Handles = Message->Info.ChunkStarted.hSourceFile && Message->Info.ChunkStarted.hDestinationFile;
            break;
        case COPYFILE2_CALLBACK_CHUNK_FINISHED:
            Event->Stream = Message->Info.ChunkFinished.dwStreamNumber;
            Event->Chunk = Message->Info.ChunkFinished.uliChunkNumber.QuadPart;
            Event->ChunkSize = Message->Info.ChunkFinished.uliChunkSize.QuadPart;
            Event->StreamSize = Message->Info.ChunkFinished.uliStreamSize.QuadPart;
            Event->StreamBytes = Message->Info.ChunkFinished.uliStreamBytesTransferred.QuadPart;
            Event->TotalSize = Message->Info.ChunkFinished.uliTotalFileSize.QuadPart;
            Event->TotalBytes = Message->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
            Event->Handles = Message->Info.ChunkFinished.hSourceFile && Message->Info.ChunkFinished.hDestinationFile;
            break;
        case COPYFILE2_CALLBACK_STREAM_STARTED:
            Event->Stream = Message->Info.StreamStarted.dwStreamNumber;
            Event->StreamSize = Message->Info.StreamStarted.uliStreamSize.QuadPart;
            Event->TotalSize = Message->Info.StreamStarted.uliTotalFileSize.QuadPart;
            Event->Handles = Message->Info.StreamStarted.hSourceFile && Message->Info.StreamStarted.hDestinationFile;
            break;
        case COPYFILE2_CALLBACK_STREAM_FINISHED:
            Event->Stream = Message->Info.StreamFinished.dwStreamNumber;
            Event->StreamSize = Message->Info.StreamFinished.uliStreamSize.QuadPart;
            Event->StreamBytes = Message->Info.StreamFinished.uliStreamBytesTransferred.QuadPart;
            Event->TotalSize = Message->Info.StreamFinished.uliTotalFileSize.QuadPart;
            Event->TotalBytes = Message->Info.StreamFinished.uliTotalBytesTransferred.QuadPart;
            Event->Handles = Message->Info.StreamFinished.hSourceFile && Message->Info.StreamFinished.hDestinationFile;
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

    RemoveFiles();
}
