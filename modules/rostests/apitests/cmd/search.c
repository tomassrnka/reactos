/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test which files cmd runs for a command name without an extension
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <strsafe.h>

static WCHAR s_Dir[MAX_PATH];
static WCHAR s_ComSpec[MAX_PATH];

typedef struct SEARCH_ENTRY
{
    INT line;
    PCWSTR pszCommand;
    PCWSTR pszRan;      /* the file that is left behind if cmd ran the right program, NULL if none must run */
    BOOL bExit42;       /* the program run is the probe, which exits with 42 */
    DWORD dwExit;       /* the exit code of cmd when no program ran, 0 if not checked */
} SEARCH_ENTRY;

/* In the directory: tool (text) and tool.exe (probe), tool2 (probe, no extension),
 * tool3 (text) and tool3.bat (batch file). Measured on Windows Server 2008 R2 and
 * Windows 10: a name without an extension runs a file with a PATHEXT extension and
 * does not run the file with exactly that name */
static const SEARCH_ENTRY s_Entries[] =
{
    { __LINE__, L"tool", L"tool.exe.out", TRUE },
    { __LINE__, L"tool.exe", L"tool.exe.out", TRUE },
    { __LINE__, L"tool2 & exit !errorlevel!", NULL, FALSE, 9009 },
    { __LINE__, L"tool3", L"tool3.bat.out", FALSE },
};

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

static PWSTR s_pEnv;

/* The environment of the test with PATH and PATHEXT set to what the test needs, and without
 * an ERRORLEVEL variable, which would hide the error level of cmd */
static PWSTR BuildEnvironment(PCWSTR pszPath)
{
    PWSTR pszStrings = GetEnvironmentStringsW(), psz, pszOut, pszEnd;
    SIZE_T cch = 0;

    if (!pszStrings)
        return NULL;
    for (psz = pszStrings; *psz; psz += wcslen(psz) + 1)
        cch += wcslen(psz) + 1;
    cch += wcslen(pszPath) + 64;
    pszOut = HeapAlloc(GetProcessHeap(), 0, cch * sizeof(WCHAR));
    if (!pszOut)
    {
        FreeEnvironmentStringsW(pszStrings);
        return NULL;
    }
    pszEnd = pszOut;
    for (psz = pszStrings; *psz; psz += wcslen(psz) + 1)
    {
        if (_wcsnicmp(psz, L"PATH=", 5) && _wcsnicmp(psz, L"PATHEXT=", 8) &&
            _wcsnicmp(psz, L"ERRORLEVEL=", 11))
        {
            wcscpy(pszEnd, psz);
            pszEnd += wcslen(pszEnd) + 1;
        }
    }
    FreeEnvironmentStringsW(pszStrings);
    StringCchPrintfW(pszEnd, wcslen(pszPath) + 8, L"PATH=%s", pszPath);
    pszEnd += wcslen(pszEnd) + 1;
    wcscpy(pszEnd, L"PATHEXT=.COM;.EXE;.BAT;.CMD");
    pszEnd += wcslen(pszEnd) + 1;
    *pszEnd = UNICODE_NULL;
    return pszOut;
}

static BOOL Exists(PCWSTR pszName)
{
    WCHAR szPath[MAX_PATH];

    if (FAILED(StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", s_Dir, pszName)))
        return FALSE;
    return GetFileAttributesW(szPath) != INVALID_FILE_ATTRIBUTES;
}

static BOOL WriteText(PCWSTR pszName, PCSTR pszText)
{
    WCHAR szPath[MAX_PATH];
    HANDLE hFile;
    DWORD cbWritten, cbText = (DWORD)strlen(pszText);
    BOOL bOk;

    if (FAILED(StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", s_Dir, pszName)))
        return FALSE;
    hFile = CreateFileW(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    bOk = WriteFile(hFile, pszText, cbText, &cbWritten, NULL) && cbWritten == cbText;
    CloseHandle(hFile);
    return bOk;
}

static void DoEntry(const SEARCH_ENTRY *pEntry)
{
    WCHAR szCmd[MAX_PATH * 3], szOut[MAX_PATH];
    DWORD dwExit = 0, dwPid;
    PCWSTR apszOut[] = { L"tool.exe.out", L"tool2.out", L"tool3.bat.out" };
    UINT i;

    for (i = 0; i < _countof(apszOut); i++)
    {
        StringCchPrintfW(szOut, _countof(szOut), L"%s\\%s", s_Dir, apszOut[i]);
        if (!DeleteFileW(szOut) && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            skip("Line %d: cannot delete %S: %lu\n", pEntry->line, szOut, GetLastError());
            return;
        }
    }

    /* The current directory is searched first. Registry settings like AutoRun and delayed
     * expansion would change the run, and the environment decides which files are programs */
    if (FAILED(StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" /D /V:ON /C \"%s\"", s_ComSpec, pEntry->pszCommand)))
    {
        skip("Line %d: the command line is too long\n", pEntry->line);
        return;
    }
    if (!RunHidden(szCmd, s_Dir, s_pEnv, &dwExit, &dwPid))
        return;

    for (i = 0; i < _countof(apszOut); i++)
    {
        BOOL bExpected = pEntry->pszRan && !wcscmp(pEntry->pszRan, apszOut[i]);

        ok(Exists(apszOut[i]) == bExpected, "Line %d: [%S] %s %S\n", pEntry->line, pEntry->pszCommand,
           bExpected ? "did not run" : "ran", apszOut[i]);
    }
    if (pEntry->bExit42)
        ok(dwExit == 42, "Line %d: [%S] exit code %lu, expected 42\n", pEntry->line, pEntry->pszCommand, dwExit);
    else if (pEntry->pszRan)
        ok(dwExit != 42, "Line %d: [%S] exit code is that of the probe\n", pEntry->line, pEntry->pszCommand);
    else if (pEntry->dwExit)
        ok(dwExit == pEntry->dwExit, "Line %d: [%S] exit code %lu, expected %lu\n",
           pEntry->line, pEntry->pszCommand, dwExit, pEntry->dwExit);
}

START_TEST(search)
{
    WCHAR szProbe[MAX_PATH], szTemp[MAX_PATH], szFile[MAX_PATH];
    PWSTR pch;
    DWORD cch;
    UINT i;
    static const PCWSTR apszFiles[] = { L"tool", L"tool.exe", L"tool.exe.out", L"tool2", L"tool2.out",
                                        L"tool3", L"tool3.bat", L"tool3.bat.out" };

    /* The probe is next to the test, in testdata */
    cch = GetModuleFileNameW(NULL, szProbe, _countof(szProbe));
    if (!cch || cch >= _countof(szProbe))
    {
        skip("Cannot get the path of the test\n");
        return;
    }
    pch = wcsrchr(szProbe, L'\\');
    if (pch)
        *pch = UNICODE_NULL;
    if (FAILED(StringCchCatW(szProbe, _countof(szProbe), L"\\testdata\\cmd_search_probe.exe")))
    {
        skip("The path of the probe is too long\n");
        return;
    }

    cch = GetTempPathW(_countof(szTemp) - 40, szTemp);
    if (!cch || cch >= _countof(szTemp) - 40)
    {
        skip("Cannot get the temporary path\n");
        return;
    }
    if (FAILED(StringCchPrintfW(s_Dir, _countof(s_Dir), L"%scmd search %lu", szTemp, GetCurrentProcessId())))
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
    s_pEnv = BuildEnvironment(s_Dir);

    if (s_pEnv &&
        WriteText(L"tool", "not a program\r\n") &&
        WriteText(L"tool3", "not a batch file\r\n") &&
        WriteText(L"tool3.bat", "@echo off\r\necho ran>tool3.bat.out\r\n") &&
        SUCCEEDED(StringCchPrintfW(szFile, _countof(szFile), L"%s\\tool.exe", s_Dir)) &&
        CopyFileW(szProbe, szFile, FALSE) &&
        SUCCEEDED(StringCchPrintfW(szFile, _countof(szFile), L"%s\\tool2", s_Dir)) &&
        CopyFileW(szProbe, szFile, FALSE))
    {
        for (i = 0; i < _countof(s_Entries); i++)
            DoEntry(&s_Entries[i]);
    }
    else
    {
        skip("Cannot set up the files in %S: %lu\n", s_Dir, GetLastError());
    }

    for (i = 0; i < _countof(apszFiles); i++)
    {
        if (SUCCEEDED(StringCchPrintfW(szFile, _countof(szFile), L"%s\\%s", s_Dir, apszFiles[i])))
            DeleteFileW(szFile);
    }
    ok(RemoveDirectoryW(s_Dir), "RemoveDirectoryW failed: %lu\n", GetLastError());
    if (s_pEnv)
        HeapFree(GetProcessHeap(), 0, s_pEnv);
}
