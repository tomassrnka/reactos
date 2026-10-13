/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a file written by a redirected echo keeps the bytes of the batch file
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <strsafe.h>

static WCHAR s_Dir[MAX_PATH];
static WCHAR s_ComSpec[MAX_PATH];

/* The batch file echoes every byte from 0x80 to 0xFF to a file, 16 bytes per line.
 * Windows reads the lines of the batch file in the code page of the console and writes
 * them back in the same one, so the file has the same bytes as the lines */
static BOOL WriteAll(HANDLE hFile, const void *pData, DWORD cb)
{
    DWORD cbWritten;

    return WriteFile(hFile, pData, cb, &cbWritten, NULL) && cbWritten == cb;
}

static BOOL WriteBatch(PCWSTR pszBat, PBYTE pExpected, DWORD *pcbExpected)
{
    HANDLE hFile;
    char szLine[64];
    DWORD i, Base;
    int cch;
    BOOL bOk;

    hFile = CreateFileW(pszBat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    bOk = WriteAll(hFile, "@echo off\r\n", 11);
    *pcbExpected = 0;
    for (Base = 0x80; Base < 0x100; Base += 16)
    {
        cch = sprintf(szLine, "echo ");
        for (i = 0; i < 16; i++)
        {
            szLine[cch++] = (char)(Base + i);
            pExpected[(*pcbExpected)++] = (BYTE)(Base + i);
        }
        pExpected[(*pcbExpected)++] = '\r';
        pExpected[(*pcbExpected)++] = '\n';
        /* The output file is in the current directory of the test: its name has to be ASCII
         * because the batch file is read in the code page of the console */
        cch += sprintf(szLine + cch, ">>e.out\r\n");
        bOk = bOk && WriteAll(hFile, szLine, cch);
    }
    CloseHandle(hFile);
    return bOk;
}

/* Runs a command line hidden, in a job object when it can be assigned so that a timeout stops
 * the whole tree. Returns FALSE if cmd could not start. */
static BOOL RunHidden(PWSTR pszCmdLine, PCWSTR pszDir, PVOID pEnv, DWORD *pdwExit, DWORD *pdwPid)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    BOOL bInJob = FALSE;

    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, pszCmdLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | (pEnv ? CREATE_UNICODE_ENVIRONMENT : 0),
                        pEnv, pszDir, &si, &pi))
    {
        skip("CreateProcessW failed: %lu\n", GetLastError());
        if (hJob)
            CloseHandle(hJob);
        return FALSE;
    }
    if (hJob)
        bInJob = AssignProcessToJobObject(hJob, pi.hProcess);
    if (ResumeThread(pi.hThread) == (DWORD)-1)
    {
        skip("ResumeThread failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (hJob)
            CloseHandle(hJob);
        return FALSE;
    }

    if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0)
    {
        ok(FALSE, "cmd did not finish\n");
        /* Without a job only cmd is stopped, the programs it started may stay */
        if (!bInJob || !TerminateJobObject(hJob, 1))
            TerminateProcess(pi.hProcess, 1);
        ok(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0, "cmd was not stopped\n");
    }
    *pdwExit = (DWORD)-1;
    ok(GetExitCodeProcess(pi.hProcess, pdwExit), "GetExitCodeProcess failed: %lu\n", GetLastError());
    *pdwPid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hJob)
        CloseHandle(hJob);
    return TRUE;
}

static void DoCodePage(UINT CodePage, PCWSTR pszOut, const BYTE *pExpected, DWORD cbExpected)
{
    WCHAR szCmd[MAX_PATH * 3];
    BYTE Data[512];
    DWORD cbRead = 0, i, cDiff = 0, dwExit = 0, dwPid;
    HANDLE hFile;

    DeleteFileW(pszOut);
    /* The batch file is not run if the code page cannot be set */
    if (FAILED(StringCchPrintfW(szCmd, _countof(szCmd),
                                L"\"%s\" /D /V:OFF /C \"chcp %u >nul && call e.bat || exit 99\"",
                                s_ComSpec, CodePage)))
    {
        skip("The command line is too long\n");
        return;
    }
    if (!RunHidden(szCmd, s_Dir, NULL, &dwExit, &dwPid))
        return;
    ok(dwExit == 0, "Code page %u: cmd exit code %lu\n", CodePage, dwExit);

    hFile = CreateFileW(pszOut, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "Code page %u: no output file: %lu\n", CodePage, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    ok(ReadFile(hFile, Data, sizeof(Data), &cbRead, NULL), "Code page %u: ReadFile failed: %lu\n",
       CodePage, GetLastError());
    CloseHandle(hFile);

    ok(cbRead == cbExpected, "Code page %u: %lu bytes written, expected %lu\n", CodePage, cbRead, cbExpected);
    for (i = 0; i < cbRead && i < cbExpected; i++)
    {
        if (Data[i] != pExpected[i])
            cDiff++;
    }
    ok(cDiff == 0, "Code page %u: %lu bytes differ from the echoed lines\n", CodePage, cDiff);
}

START_TEST(echoenc)
{
    WCHAR szTemp[MAX_PATH], szBat[MAX_PATH], szOut[MAX_PATH];
    BYTE Expected[512];
    DWORD cbExpected = 0, cch;
    UINT i;
    static const UINT s_CodePages[] = { 437, 850, 866, 1251, 1252 };

    cch = GetTempPathW(_countof(szTemp) - 40, szTemp);
    if (!cch || cch >= _countof(szTemp) - 40)
    {
        skip("Cannot get the temporary path\n");
        return;
    }
    if (FAILED(StringCchPrintfW(s_Dir, _countof(s_Dir), L"%scmd echoenc %lu", szTemp, GetCurrentProcessId())) ||
        FAILED(StringCchPrintfW(szBat, _countof(szBat), L"%s\\e.bat", s_Dir)) ||
        FAILED(StringCchPrintfW(szOut, _countof(szOut), L"%s\\e.out", s_Dir)))
    {
        skip("The temporary path is too long\n");
        return;
    }
    if (!CreateDirectoryW(s_Dir, NULL))
    {
        skip("Cannot create %S: %lu\n", s_Dir, GetLastError());
        return;
    }
    cch = GetEnvironmentVariableW(L"ComSpec", s_ComSpec, _countof(s_ComSpec));
    if (!cch || cch >= _countof(s_ComSpec))
        StringCchCopyW(s_ComSpec, _countof(s_ComSpec), L"cmd.exe");

    if (WriteBatch(szBat, Expected, &cbExpected))
    {
        for (i = 0; i < _countof(s_CodePages); i++)
            DoCodePage(s_CodePages[i], szOut, Expected, cbExpected);
    }
    else
    {
        skip("Cannot create %S: %lu\n", szBat, GetLastError());
    }

    DeleteFileW(szOut);
    DeleteFileW(szBat);
    ok(RemoveDirectoryW(s_Dir), "RemoveDirectoryW failed: %lu\n", GetLastError());
}
