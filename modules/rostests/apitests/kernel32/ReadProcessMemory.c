/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for cross-process ReadProcessMemory partial-copy semantics
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * ReadProcessMemory over a range that is only partially accessible in the
 * target must copy the valid prefix and report ERROR_PARTIAL_COPY with the
 * number of bytes transferred - it must NOT fail the whole transfer with zero
 * bytes. Chromium/Electron sandbox brokers, debuggers and minidump writers all
 * rely on this when they read a freshly created (and only partially committed)
 * child process.
 *
 * Three cases are verified against a real suspended child, using transfers
 * larger than 512 bytes so the kernel takes the MDL/mapped copy path:
 *   (a) fully-committed range   -> full bytes + success
 *   (b) committed -> uncommitted -> valid prefix + ERROR_PARTIAL_COPY
 *   (c) fully-uncommitted range  -> zero bytes + failure
 */

#include "precomp.h"

#include <strsafe.h>

static
HANDLE
StartSuspendedChild(VOID)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    BOOL Success;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine,
                    sizeof(CommandLine),
                    L"\"%ls\" ReadProcessMemory child",
                    FileName);

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);

    Success = CreateProcessW(FileName,
                             CommandLine,
                             NULL,
                             NULL,
                             FALSE,
                             CREATE_SUSPENDED,
                             NULL,
                             NULL,
                             &StartupInfo,
                             &ProcessInfo);
    if (!Success)
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return NULL;
    }

    /* Keep the child suspended; we never resume it, only poke it and kill it. */
    CloseHandle(ProcessInfo.hThread);
    return ProcessInfo.hProcess;
}

START_TEST(ReadProcessMemory)
{
    HANDLE hProcess;
    PUCHAR RemoteBase;
    PUCHAR CommittedPage;
    PUCHAR UncommittedPage;
    SIZE_T PageSize = 0x1000;
    SIZE_T BytesWritten = 0;
    SIZE_T BytesRead;
    BOOL Success;
    DWORD Error;
    PUCHAR Source;
    PUCHAR Buffer;
    SIZE_T i;
    BOOLEAN ContentOk;
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        /* The child is kept suspended and never actually runs. */
        return;
    }

    Source = HeapAlloc(GetProcessHeap(), 0, PageSize);
    Buffer = HeapAlloc(GetProcessHeap(), 0, 2 * PageSize);
    ok(Source != NULL && Buffer != NULL, "Allocation failed\n");
    if (Source == NULL || Buffer == NULL)
        return;
    FillMemory(Source, PageSize, 0xAB);

    hProcess = StartSuspendedChild();
    if (hProcess == NULL)
        return;

    /* Reserve two pages in the child, commit only the first, seed it. */
    RemoteBase = VirtualAllocEx(hProcess, NULL, 2 * PageSize, MEM_RESERVE, PAGE_READWRITE);
    ok(RemoteBase != NULL, "VirtualAllocEx(reserve) failed: %lu\n", GetLastError());
    if (RemoteBase == NULL)
    {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        return;
    }
    CommittedPage = RemoteBase;
    UncommittedPage = RemoteBase + PageSize;

    Success = (VirtualAllocEx(hProcess, CommittedPage, PageSize, MEM_COMMIT, PAGE_READWRITE) != NULL);
    ok(Success, "VirtualAllocEx(commit) failed: %lu\n", GetLastError());

    Success = WriteProcessMemory(hProcess, CommittedPage, Source, PageSize, &BytesWritten);
    ok(Success && BytesWritten == PageSize,
       "WriteProcessMemory seed failed: ret=%d written=%Iu\n", Success, BytesWritten);

    /* (a) Fully-committed range: full copy + success (the common path). */
    FillMemory(Buffer, 2 * PageSize, 0);
    BytesRead = 0;
    SetLastError(0xdeadbeef);
    Success = ReadProcessMemory(hProcess, CommittedPage, Buffer, PageSize, &BytesRead);
    ok(Success, "(a) full read should succeed, err=%lu\n", GetLastError());
    ok(BytesRead == PageSize, "(a) full read BytesRead=%Iu expected %Iu\n", BytesRead, PageSize);
    ContentOk = TRUE;
    for (i = 0; i < PageSize; i++)
        if (Buffer[i] != 0xAB) { ContentOk = FALSE; break; }
    ok(ContentOk, "(a) full read content mismatch at %Iu\n", i);

    /* (b) Committed -> uncommitted: valid prefix + ERROR_PARTIAL_COPY. */
    FillMemory(Buffer, 2 * PageSize, 0);
    BytesRead = 0;
    SetLastError(0xdeadbeef);
    Success = ReadProcessMemory(hProcess, CommittedPage, Buffer, 2 * PageSize, &BytesRead);
    Error = GetLastError();
    ok(!Success, "(b) straddling read should fail\n");
    ok(Error == ERROR_PARTIAL_COPY, "(b) GetLastError=%lu expected ERROR_PARTIAL_COPY(%u)\n",
       Error, ERROR_PARTIAL_COPY);
    ok(BytesRead == PageSize, "(b) partial BytesRead=%Iu expected %Iu\n", BytesRead, PageSize);
    ContentOk = TRUE;
    for (i = 0; i < PageSize; i++)
        if (Buffer[i] != 0xAB) { ContentOk = FALSE; break; }
    ok(ContentOk, "(b) valid-prefix content mismatch at %Iu\n", i);

    /* (c) Fully-uncommitted range: zero bytes + failure. */
    FillMemory(Buffer, 2 * PageSize, 0);
    BytesRead = 0xBADBAD;
    SetLastError(0xdeadbeef);
    Success = ReadProcessMemory(hProcess, UncommittedPage, Buffer, PageSize, &BytesRead);
    ok(!Success, "(c) fully-uncommitted read should fail\n");
    ok(BytesRead == 0, "(c) BytesRead=%Iu expected 0\n", BytesRead);

    TerminateProcess(hProcess, 0);
    CloseHandle(hProcess);
    HeapFree(GetProcessHeap(), 0, Source);
    HeapFree(GetProcessHeap(), 0, Buffer);
}
