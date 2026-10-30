/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     What CopyFileExW and CopyFile2 share to copy the named streams of a file
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _COPYSTREAMS_H_
#define _COPYSTREAMS_H_

typedef struct _COPY2_STREAM
{
    PWSTR Name;                 /* ":name", the unnamed stream is not in the list */
    ULONGLONG Size;
    HANDLE Source;              /* opened before the first callback, so that a callback cannot swap the file */
} COPY2_STREAM;

NTSTATUS
Copy2SetLength(
    _In_ HANDLE File,
    _In_ ULONGLONG Length);

NTSTATUS
Copy2DeleteByHandle(
    _In_ HANDLE File);

DWORD
Copy2FullPath(
    _In_ PCWSTR Name,
    _Out_ PWSTR *Path);

NTSTATUS
Copy2QueryNamedStreams(
    _In_ HANDLE File,
    _Out_ COPY2_STREAM **Streams,
    _Out_ PULONG Count,
    _Out_ PVOID *BufferToFree);

HANDLE
Copy2OpenStream(
    _In_ PCWSTR FileName,
    _In_ PCWSTR StreamName,
    _In_ DWORD Access,
    _In_ DWORD Share,
    _In_ DWORD Disposition,
    _In_ HANDLE Owner,
    _In_ DWORD Flags,
    _Out_ PDWORD Error,
    _Out_opt_ PBOOL Existed);

DWORD
Copy2RemoveStreams(
    _In_ HANDLE Dest,
    _In_ PCWSTR DestName,
    _In_opt_ const COPY2_STREAM *Keep,
    _In_ ULONG KeepCount);

VOID
Copy2CopyEas(
    _In_ HANDLE Source,
    _In_ HANDLE Dest);

#endif /* _COPYSTREAMS_H_ */
