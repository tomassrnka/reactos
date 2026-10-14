/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     CopyFile2, the Windows 8 file copy with a progress callback
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *****************************************************************/

#include <k32.h>
#define NDEBUG
#include <debug.h>

#if DBG
DEBUG_CHANNEL(kernel32file);
#endif

/* DEFINES ******************************************************************/

#define COPY2_CHUNK_SIZE        0x80000
#define COPY2_INVALID_FLAGS     0x80000000

/* FUNCTIONS ****************************************************************/

typedef struct _COPY2_STREAM
{
    PWSTR Name;                 /* NULL for the unnamed data stream, else ":name" */
    ULARGE_INTEGER Size;
} COPY2_STREAM;

typedef struct _COPY2_STATE
{
    PCOPYFILE2_PROGRESS_ROUTINE Progress;
    PVOID Context;
    BOOL *Cancel;
    HANDLE Source;
    HANDLE Dest;
    ULARGE_INTEGER TotalSize;
    ULARGE_INTEGER TotalTransferred;
    BOOL Quiet;
    BOOL KeepDest;
} COPY2_STATE;

static BOOL
Copy2Cancelled(
    _In_ COPY2_STATE *State)
{
    return State->Cancel && *State->Cancel;
}

/* Sends a message and returns S_OK, or the result that ends the copy; the cancel flag is looked at after every
   message. Final is for a message sent after the copy has failed: its answer changes nothing. */
static HRESULT
Copy2Notify(
    _Inout_ COPY2_STATE *State,
    _In_ const COPYFILE2_MESSAGE *Message,
    _In_ BOOL Final)
{
    COPYFILE2_MESSAGE_ACTION Action = COPYFILE2_PROGRESS_CONTINUE;
    HRESULT Result = S_OK;

    if (State->Progress && !State->Quiet)
        Action = State->Progress(Message, State->Context);

    if (Final)
    {
        /* A cancel flag set now still takes the new file */
        if (Copy2Cancelled(State))
            State->KeepDest = FALSE;
        return S_OK;
    }

    switch (Action)
    {
        case COPYFILE2_PROGRESS_QUIET:
            State->Quiet = TRUE;
            break;
        case COPYFILE2_PROGRESS_CANCEL:
            Result = HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
            break;
        case COPYFILE2_PROGRESS_STOP:
            /* A stop keeps the new file only where the copy is between two streams */
            Result = HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
            State->KeepDest = (Message->Type == COPYFILE2_CALLBACK_STREAM_STARTED ||
                               Message->Type == COPYFILE2_CALLBACK_STREAM_FINISHED);
            break;
        case COPYFILE2_PROGRESS_PAUSE:
            /* Once all data is copied there is nothing left to pause */
            if (State->TotalTransferred.QuadPart < State->TotalSize.QuadPart)
            {
                Result = HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED);
                State->KeepDest = TRUE;
            }
            break;
        default:
            break;
    }

    /* The cancel flag deletes the new file whatever the answer was, and ends the copy as a cancel; a pause still
       reports itself, as on Windows 10 */
    if (Copy2Cancelled(State))
    {
        if (Result != HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED))
            Result = HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
        State->KeepDest = FALSE;
    }

    return Result;
}

static VOID
Copy2NotifyError(
    _Inout_ COPY2_STATE *State,
    _In_ COPYFILE2_COPY_PHASE Phase,
    _In_ DWORD StreamNumber,
    _In_ HRESULT Failure,
    _In_ ULARGE_INTEGER ChunkNumber,
    _In_ ULARGE_INTEGER StreamSize,
    _In_ ULARGE_INTEGER StreamTransferred)
{
    COPYFILE2_MESSAGE Message;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Type = COPYFILE2_CALLBACK_ERROR;
    Message.Info.Error.CopyPhase = Phase;
    Message.Info.Error.dwStreamNumber = StreamNumber;
    Message.Info.Error.hrFailure = Failure;
    Message.Info.Error.uliChunkNumber = ChunkNumber;
    Message.Info.Error.uliStreamSize = StreamSize;
    Message.Info.Error.uliStreamBytesTransferred = StreamTransferred;
    Message.Info.Error.uliTotalFileSize = State->TotalSize;
    Message.Info.Error.uliTotalBytesTransferred = State->TotalTransferred;

    /* The callback cannot undo a failure */
    Copy2Notify(State, &Message, TRUE);
}

/* Copies one stream; both handles are positioned at its start */
static HRESULT
Copy2Stream(
    _Inout_ COPY2_STATE *State,
    _In_ DWORD StreamNumber,
    _In_ HANDLE Source,
    _In_ HANDLE Dest,
    _In_ ULARGE_INTEGER StreamSize,
    _In_ PVOID Buffer)
{
    COPYFILE2_MESSAGE Message;
    ULARGE_INTEGER StreamTransferred, ChunkNumber;
    HRESULT Result;

    StreamTransferred.QuadPart = 0;
    ChunkNumber.QuadPart = 0;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Type = COPYFILE2_CALLBACK_STREAM_STARTED;
    Message.Info.StreamStarted.dwStreamNumber = StreamNumber;
    Message.Info.StreamStarted.hSourceFile = Source;
    Message.Info.StreamStarted.hDestinationFile = Dest;
    Message.Info.StreamStarted.uliStreamSize = StreamSize;
    Message.Info.StreamStarted.uliTotalFileSize = State->TotalSize;
    Result = Copy2Notify(State, &Message, FALSE);

    while (SUCCEEDED(Result) && StreamTransferred.QuadPart < StreamSize.QuadPart)
    {
        ULARGE_INTEGER ChunkSize;
        IO_STATUS_BLOCK IoStatusBlock;
        NTSTATUS Status;

        if (Copy2Cancelled(State))
        {
            Result = HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);
            break;
        }

        ChunkSize.QuadPart = StreamSize.QuadPart - StreamTransferred.QuadPart;
        if (ChunkSize.QuadPart > COPY2_CHUNK_SIZE)
            ChunkSize.QuadPart = COPY2_CHUNK_SIZE;

        RtlZeroMemory(&Message, sizeof(Message));
        Message.Type = COPYFILE2_CALLBACK_CHUNK_STARTED;
        Message.Info.ChunkStarted.dwStreamNumber = StreamNumber;
        Message.Info.ChunkStarted.hSourceFile = Source;
        Message.Info.ChunkStarted.hDestinationFile = Dest;
        Message.Info.ChunkStarted.uliChunkNumber = ChunkNumber;
        Message.Info.ChunkStarted.uliChunkSize = ChunkSize;
        Message.Info.ChunkStarted.uliStreamSize = StreamSize;
        Message.Info.ChunkStarted.uliTotalFileSize = State->TotalSize;
        Result = Copy2Notify(State, &Message, FALSE);
        if (FAILED(Result))
            break;

        Status = NtReadFile(Source, NULL, NULL, NULL, &IoStatusBlock, Buffer, (ULONG)ChunkSize.QuadPart, NULL, NULL);
        if (Status == STATUS_END_OF_FILE || (NT_SUCCESS(Status) && IoStatusBlock.Information == 0))
        {
            /* The source shrank while it was copied */
            break;
        }
        if (!NT_SUCCESS(Status))
        {
            Result = HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
            Copy2NotifyError(State, COPYFILE2_PHASE_READ_SOURCE, StreamNumber, Result,
                             ChunkNumber, StreamSize, StreamTransferred);
            break;
        }

        ChunkSize.QuadPart = IoStatusBlock.Information;
        Status = NtWriteFile(Dest, NULL, NULL, NULL, &IoStatusBlock, Buffer, (ULONG)ChunkSize.QuadPart, NULL, NULL);
        if (!NT_SUCCESS(Status))
        {
            Result = HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
            Copy2NotifyError(State, COPYFILE2_PHASE_WRITE_DESTINATION, StreamNumber, Result,
                             ChunkNumber, StreamSize, StreamTransferred);
            break;
        }

        StreamTransferred.QuadPart += ChunkSize.QuadPart;
        State->TotalTransferred.QuadPart += ChunkSize.QuadPart;

        RtlZeroMemory(&Message, sizeof(Message));
        Message.Type = COPYFILE2_CALLBACK_CHUNK_FINISHED;
        Message.Info.ChunkFinished.dwStreamNumber = StreamNumber;
        Message.Info.ChunkFinished.hSourceFile = Source;
        Message.Info.ChunkFinished.hDestinationFile = Dest;
        Message.Info.ChunkFinished.uliChunkNumber = ChunkNumber;
        Message.Info.ChunkFinished.uliChunkSize = ChunkSize;
        Message.Info.ChunkFinished.uliStreamSize = StreamSize;
        Message.Info.ChunkFinished.uliStreamBytesTransferred = StreamTransferred;
        Message.Info.ChunkFinished.uliTotalFileSize = State->TotalSize;
        Message.Info.ChunkFinished.uliTotalBytesTransferred = State->TotalTransferred;
        Result = Copy2Notify(State, &Message, FALSE);
        ChunkNumber.QuadPart++;
    }

    /* The end of a stream is reported whether it went well or not; after a failure its answer is not looked at */
    RtlZeroMemory(&Message, sizeof(Message));
    Message.Type = COPYFILE2_CALLBACK_STREAM_FINISHED;
    Message.Info.StreamFinished.dwStreamNumber = StreamNumber;
    Message.Info.StreamFinished.hSourceFile = Source;
    Message.Info.StreamFinished.hDestinationFile = Dest;
    Message.Info.StreamFinished.uliStreamSize = StreamSize;
    Message.Info.StreamFinished.uliStreamBytesTransferred = StreamTransferred;
    Message.Info.StreamFinished.uliTotalFileSize = State->TotalSize;
    Message.Info.StreamFinished.uliTotalBytesTransferred = State->TotalTransferred;
    if (SUCCEEDED(Result))
        Result = Copy2Notify(State, &Message, FALSE);
    else
        Copy2Notify(State, &Message, TRUE);

    return Result;
}

/* Lists the named data streams of the source; the unnamed stream is not part of the list. A file system that
   has no streams gives no list and success. */
static NTSTATUS
Copy2QueryNamedStreams(
    _In_ HANDLE Source,
    _Out_ COPY2_STREAM **Streams,
    _Out_ PULONG Count,
    _Out_ PVOID *BufferToFree)
{
    FILE_STREAM_INFORMATION *Info;
    IO_STATUS_BLOCK IoStatusBlock;
    ULONG Size = 0x1000, Entries = 0, Index = 0;
    PUCHAR Buffer, End;
    NTSTATUS Status;

    *Streams = NULL;
    *Count = 0;
    *BufferToFree = NULL;

    for (;;)
    {
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
        if (!Buffer)
            return STATUS_NO_MEMORY;

        Status = NtQueryInformationFile(Source, &IoStatusBlock, Buffer, Size, FileStreamInformation);
        if (Status != STATUS_BUFFER_OVERFLOW && Status != STATUS_BUFFER_TOO_SMALL)
            break;

        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        Size *= 2;
        if (Size > 0x100000)
            return Status;
    }

    if (!NT_SUCCESS(Status) || IoStatusBlock.Information < sizeof(FILE_STREAM_INFORMATION))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        switch (Status)
        {
            /* The file system does not know the information class */
            case STATUS_INVALID_PARAMETER:
            case STATUS_INVALID_INFO_CLASS:
            case STATUS_NOT_IMPLEMENTED:
            case STATUS_NOT_SUPPORTED:
            case STATUS_INVALID_DEVICE_REQUEST:
                return STATUS_SUCCESS;
            default:
                return NT_SUCCESS(Status) ? STATUS_SUCCESS : Status;
        }
    }

    End = Buffer + IoStatusBlock.Information;
    for (Info = (FILE_STREAM_INFORMATION *)Buffer;;)
    {
        Entries++;
        if (!Info->NextEntryOffset)
            break;
        Info = (FILE_STREAM_INFORMATION *)((PUCHAR)Info + Info->NextEntryOffset);
        if ((PUCHAR)Info + sizeof(*Info) > End)
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
            return STATUS_INFO_LENGTH_MISMATCH;
        }
    }

    *Streams = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Entries * sizeof(COPY2_STREAM));
    if (!*Streams)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        return STATUS_NO_MEMORY;
    }

    for (Info = (FILE_STREAM_INFORMATION *)Buffer;;)
    {
        ULONG Chars = Info->StreamNameLength / sizeof(WCHAR);
        PWCHAR Name = Info->StreamName;

        if ((PUCHAR)Name + Info->StreamNameLength > End)
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, *Streams);
            RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
            *Streams = NULL;
            return STATUS_INFO_LENGTH_MISMATCH;
        }

        /* Names look like ":name:$DATA", and "::$DATA" is the unnamed stream */
        if (Chars > 7 && Name[0] == L':' && !_wcsnicmp(&Name[Chars - 6], L":$DATA", 6))
        {
            Name[Chars - 6] = UNICODE_NULL;
            (*Streams)[Index].Name = Name;
            (*Streams)[Index].Size.QuadPart = Info->StreamSize.QuadPart;
            Index++;
        }

        if (!Info->NextEntryOffset)
            break;
        Info = (FILE_STREAM_INFORMATION *)((PUCHAR)Info + Info->NextEntryOffset);
    }

    *Count = Index;
    *BufferToFree = Buffer;
    return STATUS_SUCCESS;
}

static PWSTR
Copy2StreamPath(
    _In_ PCWSTR FileName,
    _In_ PCWSTR StreamName)
{
    SIZE_T Length = wcslen(FileName) + wcslen(StreamName) + 1;
    PWSTR Path = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length * sizeof(WCHAR));

    if (Path)
    {
        wcscpy(Path, FileName);
        wcscat(Path, StreamName);
    }
    return Path;
}

/* A name made absolute at the start, so that a callback that changes the current directory cannot move the copy.
   An extended name is already complete and used as it is, and so is a name that cannot be made absolute: it fails
   where it is opened. *Path stays NULL for those. */
static NTSTATUS
Copy2FullPath(
    _In_ PCWSTR Name,
    _Out_ PWSTR *Path)
{
    ULONG Length, Result;
    PWSTR Buffer;
    int Tries;

    *Path = NULL;

    if (Name[0] == L'\\' && Name[1] == L'\\' && (Name[2] == L'?' || Name[2] == L'.') && Name[3] == L'\\')
        return STATUS_SUCCESS;

    Length = GetFullPathNameW(Name, 0, NULL, NULL);

    /* The current directory can change between the calls, so the length is checked on each */
    for (Tries = 0; Length && Tries < 4; Tries++)
    {
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length * sizeof(WCHAR));
        if (!Buffer)
            return STATUS_NO_MEMORY;

        Result = GetFullPathNameW(Name, Length, Buffer, NULL);
        if (Result && Result < Length)
        {
            *Path = Buffer;
            return STATUS_SUCCESS;
        }

        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        Length = Result ? Result + 1 : 0;
    }

    return STATUS_SUCCESS;
}

/* The same file, by volume and file index; a file system that gives no index for a file (an empty one on FAT) is
   never the same here */
static BOOL
Copy2SameFile(
    _In_ HANDLE First,
    _In_ HANDLE Second)
{
    BY_HANDLE_FILE_INFORMATION InfoFirst, InfoSecond;

    if (!GetFileInformationByHandle(First, &InfoFirst) || !GetFileInformationByHandle(Second, &InfoSecond))
        return FALSE;

    return InfoFirst.dwVolumeSerialNumber == InfoSecond.dwVolumeSerialNumber &&
           InfoFirst.nFileIndexHigh == InfoSecond.nFileIndexHigh &&
           InfoFirst.nFileIndexLow == InfoSecond.nFileIndexLow &&
           (InfoFirst.nFileIndexHigh || InfoFirst.nFileIndexLow);
}

HRESULT
WINAPI
CopyFile2(
    _In_ PCWSTR pwszExistingFileName,
    _In_ PCWSTR pwszNewFileName,
    _In_opt_ COPYFILE2_EXTENDED_PARAMETERS *pExtendedParameters)
{
    COPY2_STATE State = { 0 };
    COPY2_STREAM *NamedStreams = NULL;
    FILE_STANDARD_INFORMATION Standard;
    FILE_BASIC_INFORMATION Basic;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    ULARGE_INTEGER Zero, SourceSize;
    DWORD CopyFlags = 0;
    HRESULT Result;
    BOOL Created = FALSE;
    PVOID Buffer = NULL, StreamBuffer = NULL;
    SIZE_T BufferSize = COPY2_CHUNK_SIZE;
    PWSTR SourceName = NULL, DestName = NULL;
    ULONG NamedCount = 0, i;

    Zero.QuadPart = 0;

    if (!pwszExistingFileName || !pwszNewFileName)
    {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    }

    if (pExtendedParameters)
    {
        if (pExtendedParameters->dwSize != sizeof(*pExtendedParameters) ||
            (pExtendedParameters->dwCopyFlags & COPY2_INVALID_FLAGS))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return E_INVALIDARG;
        }

        CopyFlags = pExtendedParameters->dwCopyFlags;
        State.Progress = pExtendedParameters->pProgressRoutine;
        State.Context = pExtendedParameters->pvCallbackContext;
        State.Cancel = pExtendedParameters->pfCancel;
    }

    State.Dest = INVALID_HANDLE_VALUE;
    State.Source = INVALID_HANDLE_VALUE;
    Status = Copy2FullPath(pwszExistingFileName, &SourceName);
    if (NT_SUCCESS(Status))
        Status = Copy2FullPath(pwszNewFileName, &DestName);
    if (!NT_SUCCESS(Status))
    {
        Result = HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
        Copy2NotifyError(&State, COPYFILE2_PHASE_PREPARE_SOURCE, 0, Result, Zero, Zero, Zero);
        goto Done;
    }
    if (SourceName)
        pwszExistingFileName = SourceName;
    if (DestName)
        pwszNewFileName = DestName;

    State.Source = CreateFileW(pwszExistingFileName,
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                               NULL);
    if (State.Source == INVALID_HANDLE_VALUE)
    {
        Result = HRESULT_FROM_WIN32(GetLastError());
        Copy2NotifyError(&State, COPYFILE2_PHASE_PREPARE_SOURCE, 0, Result, Zero, Zero, Zero);
        goto Done;
    }

    Status = NtQueryInformationFile(State.Source, &IoStatusBlock, &Standard, sizeof(Standard), FileStandardInformation);
    if (NT_SUCCESS(Status))
        Status = NtQueryInformationFile(State.Source, &IoStatusBlock, &Basic, sizeof(Basic), FileBasicInformation);
    if (NT_SUCCESS(Status))
        Status = Copy2QueryNamedStreams(State.Source, &NamedStreams, &NamedCount, &StreamBuffer);
    if (!NT_SUCCESS(Status))
    {
        Result = HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
        Copy2NotifyError(&State, COPYFILE2_PHASE_PREPARE_SOURCE, 0, Result, Zero, Zero, Zero);
        goto Done;
    }

    SourceSize.QuadPart = Standard.EndOfFile.QuadPart;
    State.TotalSize = SourceSize;
    for (i = 0; i < NamedCount; i++)
        State.TotalSize.QuadPart += NamedStreams[i].Size.QuadPart;

    /* Opening the new file would cut the source down if both are one file */
    if (!(CopyFlags & COPY_FILE_FAIL_IF_EXISTS))
    {
        HANDLE Existing = CreateFileW(pwszNewFileName,
                                      0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      NULL,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      NULL);

        if (Existing != INVALID_HANDLE_VALUE)
        {
            /* A file with no index (an empty one on some file systems) is found by its name */
            BOOL Same = Copy2SameFile(State.Source, Existing) ||
                        (SourceName && DestName && !_wcsicmp(SourceName, DestName));

            CloseHandle(Existing);
            if (Same)
            {
                Result = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
                Copy2NotifyError(&State, COPYFILE2_PHASE_PREPARE_DEST, 1, Result, Zero, SourceSize, Zero);
                goto Done;
            }
        }
    }

    /* A read-only file takes no new stream, so the attribute is set when the copy is done */
    State.Dest = CreateFileW(pwszNewFileName,
                             GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL,
                             (CopyFlags & COPY_FILE_FAIL_IF_EXISTS) ? CREATE_NEW : CREATE_ALWAYS,
                             Basic.FileAttributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY),
                             NULL);
    if (State.Dest == INVALID_HANDLE_VALUE)
    {
        Result = HRESULT_FROM_WIN32(GetLastError());
        Copy2NotifyError(&State, COPYFILE2_PHASE_PREPARE_DEST, 1, Result, Zero, SourceSize, Zero);
        goto Done;
    }
    Created = TRUE;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Buffer, 0, &BufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        Buffer = NULL;
        Result = HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
        Copy2NotifyError(&State, COPYFILE2_PHASE_PREPARE_DEST, 1, Result, Zero, SourceSize, Zero);
        goto Done;
    }

    Result = Copy2Stream(&State, 1, State.Source, State.Dest, SourceSize, Buffer);

    for (i = 0; SUCCEEDED(Result) && i < NamedCount; i++)
    {
        PWSTR SourcePath = Copy2StreamPath(pwszExistingFileName, NamedStreams[i].Name);
        PWSTR DestPath = Copy2StreamPath(pwszNewFileName, NamedStreams[i].Name);
        HANDLE SourceStream = INVALID_HANDLE_VALUE, DestStream = INVALID_HANDLE_VALUE;

        if (SourcePath && DestPath)
        {
            SourceStream = CreateFileW(SourcePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (SourceStream != INVALID_HANDLE_VALUE)
            {
                DestStream = CreateFileW(DestPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            }
        }

        if (DestStream == INVALID_HANDLE_VALUE)
        {
            COPYFILE2_COPY_PHASE Phase = (SourceStream == INVALID_HANDLE_VALUE) ? COPYFILE2_PHASE_PREPARE_SOURCE
                                                                                : COPYFILE2_PHASE_PREPARE_DEST;

            Result = HRESULT_FROM_WIN32((SourcePath && DestPath) ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY);
            Copy2NotifyError(&State, Phase, i + 2, Result, Zero, NamedStreams[i].Size, Zero);
        }
        else
        {
            Result = Copy2Stream(&State, i + 2, SourceStream, DestStream, NamedStreams[i].Size, Buffer);
        }

        if (SourceStream != INVALID_HANDLE_VALUE)
            CloseHandle(SourceStream);
        if (DestStream != INVALID_HANDLE_VALUE)
            CloseHandle(DestStream);
        if (SourcePath)
            RtlFreeHeap(RtlGetProcessHeap(), 0, SourcePath);
        if (DestPath)
            RtlFreeHeap(RtlGetProcessHeap(), 0, DestPath);
    }

    if (SUCCEEDED(Result))
    {
        /* The copy keeps the write time and the attributes of the source, and has its own creation time */
        Basic.CreationTime.QuadPart = 0;
        Basic.LastAccessTime.QuadPart = 0;
        Basic.ChangeTime.QuadPart = 0;
        Basic.FileAttributes &= FILE_ATTRIBUTE_VALID_SET_FLAGS;
        Status = NtSetInformationFile(State.Dest, &IoStatusBlock, &Basic, sizeof(Basic), FileBasicInformation);
        if (!NT_SUCCESS(Status))
        {
            Result = HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
            Copy2NotifyError(&State, COPYFILE2_PHASE_WRITE_DESTINATION, 1, Result, Zero, SourceSize, SourceSize);
        }
    }

Done:
    if (Buffer)
    {
        BufferSize = 0;
        NtFreeVirtualMemory(NtCurrentProcess(), &Buffer, &BufferSize, MEM_RELEASE);
    }
    if (NamedStreams)
        RtlFreeHeap(RtlGetProcessHeap(), 0, NamedStreams);
    if (StreamBuffer)
        RtlFreeHeap(RtlGetProcessHeap(), 0, StreamBuffer);
    if (State.Dest != INVALID_HANDLE_VALUE)
        CloseHandle(State.Dest);
    if (Created && FAILED(Result) && !State.KeepDest)
    {
        SetFileAttributesW(pwszNewFileName, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(pwszNewFileName);
    }
    if (State.Source != INVALID_HANDLE_VALUE)
        CloseHandle(State.Source);
    if (SourceName)
        RtlFreeHeap(RtlGetProcessHeap(), 0, SourceName);
    if (DestName)
        RtlFreeHeap(RtlGetProcessHeap(), 0, DestName);

    SetLastError(SUCCEEDED(Result) ? ERROR_SUCCESS : HRESULT_CODE(Result));
    return Result;
}
