/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that cmd starts the external programs of a pipeline directly
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <strsafe.h>

static WCHAR s_Dir[MAX_PATH];
static WCHAR s_ComSpec[MAX_PATH];

/* A bit mask of the probes of an entry */
#define PROBE1 1
#define PROBE2 2

typedef struct PIPE_ENTRY
{
    INT line;
    PCWSTR pszPipe;     /* %1 = probe 1, %2 = probe 2, %%i = %i for a FOR variable */
    UINT uDirect;       /* the probes that cmd must start itself, as Windows does */
    UINT uRuns;         /* the probes that must run at all */
    DWORD dwExit;       /* exit code of the whole command line: that of its last program */
    PCSTR pszInput1;    /* what probe 1 and probe 2 must read from their input, NULL if not checked */
    PCSTR pszInput2;
    PCWSTR pszArgs1;    /* the arguments after the program name of each probe */
    PCWSTR pszArgs2;
} PIPE_ENTRY;

static const PIPE_ENTRY s_Entries[] =
{
    { __LINE__, L"echo x | \"%2\"", PROBE2, PROBE2, 42, NULL, "x", NULL, L"" },
    { __LINE__, L"\"%1\" | \"%2\"", PROBE1 | PROBE2, PROBE1 | PROBE2, 42, NULL, "from-probe", L"", L"" },
    { __LINE__, L"\"%1\" a b | \"%2\" c", PROBE1 | PROBE2, PROBE1 | PROBE2, 42, NULL, "from-probe", L"a b", L"c" },
    { __LINE__, L"for %%i in (needle) do @echo x | \"%2\" %%i", PROBE2, PROBE2, 42, NULL, "x", NULL, L"needle" },
    /* Windows starts a program with a redirection directly too. ReactOS still runs it through
     * another cmd, so only its parent is traced for now */
    { __LINE__, L"\"%1\" 2>nul | \"%2\"", PROBE2, PROBE1 | PROBE2, 42, NULL, "from-probe", NULL, L"" },
    { __LINE__, L"\"%1\" | echo x", PROBE1, PROBE1, 0, NULL, NULL, L"", NULL },
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

static PWSTR FormatPipe(PCWSTR pszFormat, PCWSTR psz1, PCWSTR psz2)
{
    PWSTR pszOut = NULL;
    DWORD_PTR Args[2] = { (DWORD_PTR)psz1, (DWORD_PTR)psz2 };
    FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_ARGUMENT_ARRAY,
                   pszFormat, 0, 0, (PWSTR)&pszOut, 0, (va_list *)Args);
    return pszOut;
}

typedef struct PROBE_RESULT
{
    DWORD Ids[2];
    DWORD cbInput;
    BYTE Input[256];
    WCHAR szArgs[MAX_PATH * 2];
} PROBE_RESULT;

/* Reads the result file of a probe and cuts the program name off its command line */
static BOOL ReadResult(PCWSTR pszProbe, PROBE_RESULT *pResult)
{
    WCHAR szOut[MAX_PATH];
    HANDLE hFile;
    DWORD cbRead = 0, cbRest;
    BOOL bOk;
    PWSTR psz;

    if (FAILED(StringCchPrintfW(szOut, _countof(szOut), L"%s.out", pszProbe)))
        return FALSE;
    hFile = CreateFileW(szOut, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    ZeroMemory(pResult, sizeof(*pResult));
    bOk = ReadFile(hFile, pResult, 3 * sizeof(DWORD), &cbRead, NULL) && cbRead == 3 * sizeof(DWORD) &&
          pResult->cbInput <= sizeof(pResult->Input);
    if (bOk)
        bOk = ReadFile(hFile, pResult->Input, pResult->cbInput, &cbRead, NULL) && cbRead == pResult->cbInput;
    if (bOk)
    {
        bOk = ReadFile(hFile, pResult->szArgs, sizeof(pResult->szArgs) - sizeof(WCHAR), &cbRest, NULL);
        pResult->szArgs[cbRest / sizeof(WCHAR)] = UNICODE_NULL;
    }
    CloseHandle(hFile);
    if (!bOk)
        return FALSE;

    /* Skip the program name, which may be quoted, and the blanks around the arguments */
    psz = pResult->szArgs;
    if (*psz == L'"')
    {
        psz = wcschr(psz + 1, L'"');
        if (psz)
            psz++;
    }
    else
    {
        while (*psz && *psz != L' ')
            psz++;
    }
    if (!psz)
        psz = pResult->szArgs + wcslen(pResult->szArgs);
    while (*psz == L' ')
        psz++;
    MoveMemory(pResult->szArgs, psz, (wcslen(psz) + 1) * sizeof(WCHAR));
    cbRest = (DWORD)wcslen(pResult->szArgs);
    while (cbRest && pResult->szArgs[cbRest - 1] == L' ')
        pResult->szArgs[--cbRest] = UNICODE_NULL;

    /* An echo adds its blank before the pipe and a line break */
    while (pResult->cbInput && (pResult->Input[pResult->cbInput - 1] == '\r' ||
                                pResult->Input[pResult->cbInput - 1] == '\n' ||
                                pResult->Input[pResult->cbInput - 1] == ' '))
    {
        pResult->cbInput--;
    }
    return TRUE;
}

static void DoEntry(const PIPE_ENTRY *pEntry, PCWSTR pszProbe1, PCWSTR pszProbe2)
{
    WCHAR szCmd[MAX_PATH * 4], szOut[MAX_PATH];
    PWSTR pszPipe;
    DWORD dwExit = 0, dwCmdPid = 0;
    PCWSTR apszProbe[2] = { pszProbe1, pszProbe2 };
    PCSTR apszInput[2] = { pEntry->pszInput1, pEntry->pszInput2 };
    PCWSTR apszArgs[2] = { pEntry->pszArgs1, pEntry->pszArgs2 };
    PROBE_RESULT Result;
    UINT i;

    pszPipe = FormatPipe(pEntry->pszPipe, pszProbe1, pszProbe2);
    if (!pszPipe)
    {
        skip("Line %d: FormatMessageW failed\n", pEntry->line);
        return;
    }

    for (i = 0; i < 2; i++)
    {
        if (SUCCEEDED(StringCchPrintfW(szOut, _countof(szOut), L"%s.out", apszProbe[i])))
            DeleteFileW(szOut);
    }

    /* Registry settings like AutoRun and delayed expansion would change the run */
    if (FAILED(StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" /D /V:OFF /C \"%s\"", s_ComSpec, pszPipe)))
    {
        skip("Line %d: the command line is too long\n", pEntry->line);
        goto Quit;
    }
    if (!RunHidden(szCmd, NULL, NULL, &dwExit, &dwCmdPid))
        goto Quit;

    ok(dwExit == pEntry->dwExit, "Line %d: exit code %lu, expected %lu\n", pEntry->line, dwExit, pEntry->dwExit);

    for (i = 0; i < 2; i++)
    {
        UINT uBit = 1 << i;

        if (!(pEntry->uRuns & uBit))
            continue;
        if (!ReadResult(apszProbe[i], &Result))
        {
            ok(FALSE, "Line %d: probe %u did not run\n", pEntry->line, i + 1);
            continue;
        }
        if (pEntry->uDirect & uBit)
        {
            ok(Result.Ids[0] == dwCmdPid, "Line %d: probe %u has parent %lu, expected the cmd %lu\n",
               pEntry->line, i + 1, Result.Ids[0], dwCmdPid);
        }
        else
        {
            trace("Line %d: probe %u has parent %lu, cmd is %lu\n", pEntry->line, i + 1, Result.Ids[0], dwCmdPid);
        }
        if (apszInput[i])
        {
            ok(Result.cbInput == strlen(apszInput[i]) && !memcmp(Result.Input, apszInput[i], Result.cbInput),
               "Line %d: probe %u read %lu bytes of input, expected [%s]\n",
               pEntry->line, i + 1, Result.cbInput, apszInput[i]);
        }
        if (apszArgs[i])
        {
            ok(!wcscmp(Result.szArgs, apszArgs[i]), "Line %d: probe %u arguments [%S], expected [%S]\n",
               pEntry->line, i + 1, Result.szArgs, apszArgs[i]);
        }
    }

Quit:
    LocalFree(pszPipe);
}

START_TEST(pipe)
{
    WCHAR szProbe[MAX_PATH], szTemp[MAX_PATH], szProbe1[MAX_PATH], szProbe2[MAX_PATH], szOut[MAX_PATH];
    PWSTR pch;
    DWORD cch;
    UINT i;

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
    if (FAILED(StringCchCatW(szProbe, _countof(szProbe), L"\\testdata\\cmd_pipe_probe.exe")))
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
    if (FAILED(StringCchPrintfW(s_Dir, _countof(s_Dir), L"%scmd pipe %lu", szTemp, GetCurrentProcessId())) ||
        FAILED(StringCchPrintfW(szProbe1, _countof(szProbe1), L"%s\\probe1.exe", s_Dir)) ||
        FAILED(StringCchPrintfW(szProbe2, _countof(szProbe2), L"%s\\probe2.exe", s_Dir)))
    {
        skip("The temporary path is too long\n");
        return;
    }
    if (!CreateDirectoryW(s_Dir, NULL))
    {
        skip("Cannot create %S: %lu\n", s_Dir, GetLastError());
        return;
    }
    if (!CopyFileW(szProbe, szProbe1, FALSE) || !CopyFileW(szProbe, szProbe2, FALSE))
    {
        skip("Cannot copy %S: %lu\n", szProbe, GetLastError());
        DeleteFileW(szProbe1);
        DeleteFileW(szProbe2);
        RemoveDirectoryW(s_Dir);
        return;
    }
    cch = GetEnvironmentVariableW(L"ComSpec", s_ComSpec, _countof(s_ComSpec));
    if (!cch || cch >= _countof(s_ComSpec))
        StringCchCopyW(s_ComSpec, _countof(s_ComSpec), L"cmd.exe");

    for (i = 0; i < _countof(s_Entries); i++)
        DoEntry(&s_Entries[i], szProbe1, szProbe2);

    if (SUCCEEDED(StringCchPrintfW(szOut, _countof(szOut), L"%s.out", szProbe1)))
        DeleteFileW(szOut);
    if (SUCCEEDED(StringCchPrintfW(szOut, _countof(szOut), L"%s.out", szProbe2)))
        DeleteFileW(szOut);
    DeleteFileW(szProbe1);
    DeleteFileW(szProbe2);
    ok(RemoveDirectoryW(s_Dir), "RemoveDirectoryW failed: %lu\n", GetLastError());
}
