/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetFileType on a handle with a pending synchronous read
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static HANDLE g_hReadPipe;
static DWORD g_dwFileType;

static DWORD WINAPI PipeReaderThread(_In_ PVOID Param)
{
    CHAR Buffer[4];
    DWORD cbRead = 0;

    if (!ReadFile(g_hReadPipe, Buffer, sizeof(Buffer), &cbRead, NULL))
        return MAXDWORD;
    return cbRead;
}

static DWORD WINAPI FileTypeThread(_In_ PVOID Param)
{
    g_dwFileType = GetFileType(g_hReadPipe);
    return 0;
}

START_TEST(GetFileType)
{
    HANDLE hWritePipe, hReader, hQuery;
    DWORD dwWait, cbWritten, dwExitCode, i;
    BOOL Success, IoPending = FALSE;

    Success = CreatePipe(&g_hReadPipe, &hWritePipe, NULL, 0);
    ok(Success, "CreatePipe failed, error %lu\n", GetLastError());
    if (!Success)
        return;

    ok_long(GetFileType(g_hReadPipe), FILE_TYPE_PIPE);

    /* Block a thread in a synchronous read on the empty pipe */
    hReader = CreateThread(NULL, 0, PipeReaderThread, NULL, 0, NULL);
    ok(hReader != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (!hReader)
    {
        CloseHandle(hWritePipe);
        CloseHandle(g_hReadPipe);
        return;
    }

    /* The read holds the file object lock while its IRP is pending */
    for (i = 0; i < 500; i++)
    {
        if (!GetThreadIOPendingFlag(hReader, &IoPending) || IoPending)
            break;
        Sleep(10);
    }

    hQuery = NULL;
    if (!IoPending)
    {
        skip("The read did not become pending\n");
    }
    else
    {
        /* GetFileType must not wait for the pending read */
        g_dwFileType = FILE_TYPE_UNKNOWN;
        hQuery = CreateThread(NULL, 0, FileTypeThread, NULL, 0, NULL);
        ok(hQuery != NULL, "CreateThread failed, error %lu\n", GetLastError());
        if (hQuery)
        {
            dwWait = WaitForSingleObject(hQuery, 5000);
            ok(dwWait == WAIT_OBJECT_0,
               "GetFileType blocked behind the pending read (wait %lu)\n", dwWait);
            if (dwWait == WAIT_OBJECT_0)
                ok_long(g_dwFileType, FILE_TYPE_PIPE);
        }
    }

    /* Release the reader */
    Success = WriteFile(hWritePipe, "x", 1, &cbWritten, NULL);
    ok(Success, "WriteFile failed, error %lu\n", GetLastError());
    dwWait = WaitForSingleObject(hReader, 5000);
    ok_long(dwWait, WAIT_OBJECT_0);
    Success = GetExitCodeThread(hReader, &dwExitCode);
    ok(Success, "GetExitCodeThread failed, error %lu\n", GetLastError());
    ok_long(dwExitCode, 1);

    if (hQuery)
    {
        dwWait = WaitForSingleObject(hQuery, 5000);
        ok_long(dwWait, WAIT_OBJECT_0);
        ok_long(g_dwFileType, FILE_TYPE_PIPE);
        CloseHandle(hQuery);
    }

    CloseHandle(hReader);
    CloseHandle(hWritePipe);
    CloseHandle(g_hReadPipe);
}
