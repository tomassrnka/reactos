/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the command line that cmd's start gives a program
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <strsafe.h>

static WCHAR s_Dir[MAX_PATH];
static WCHAR s_Out[MAX_PATH];
static WCHAR s_ComSpec[MAX_PATH];

/* Folds each run of blanks outside quotes into one */
static void FoldBlanks(PWSTR psz)
{
    PWSTR pszOut = psz;
    BOOL bBlank = FALSE, bQuoted = FALSE;

    for (; *psz; psz++)
    {
        if (*psz == L'"')
            bQuoted = !bQuoted;
        if (*psz == L' ' && !bQuoted)
        {
            if (bBlank)
                continue;
            bBlank = TRUE;
        }
        else
        {
            bBlank = FALSE;
        }
        *pszOut++ = *psz;
    }
    *pszOut = UNICODE_NULL;
}

/* The batch file's echo output is decoded as ASCII */
static BOOL IsAscii(PCWSTR psz)
{
    for (; *psz; psz++)
    {
        if (*psz > 0x7F)
            return FALSE;
    }
    return TRUE;
}

typedef struct START_ENTRY
{
    INT line;
    BOOL bInDir;        /* run cmd with the probe directory as the current one */
    PCWSTR pszStart;    /* %1 = probe directory */
    PCWSTR pszExpected; /* %1 = probe directory */
    BOOL bBatch;        /* expected: the end of the line, with whitespace runs folded */
} START_ENTRY;

static const START_ENTRY s_Entries[] =
{
    { __LINE__, FALSE, L"start /wait \"\" \"%1\\probe.exe\" a \"b c\"", L"\"%1\\probe.exe\"  a \"b c\"" },
    { __LINE__, FALSE, L"start /wait \"\" \"%1\\probe.exe\"", L"\"%1\\probe.exe\" " },
    { __LINE__, FALSE, L"start /wait \"\" \"%1\\probe\" a", L"\"%1\\probe\"  a" },
    { __LINE__, FALSE, L"start /wait /b \"\" \"%1\\probe.exe\" a", L"\"%1\\probe.exe\"  a" },
    { __LINE__, FALSE, L"start /wait \"\" \"%1\\probe.exe\"   a   b", L"\"%1\\probe.exe\"    a   b" },
    { __LINE__, TRUE,  L"start /wait \"\" probe.exe a", L"probe.exe  a" },
    { __LINE__, TRUE,  L"start /wait \"\" probe a", L"probe  a" },
    /* A batch file runs in a new cmd that saves its %CMDCMDLINE%. The spacing and the
     * quoting of ComSpec differ from Windows, so only the batch file and its
     * argument are compared */
    { __LINE__, FALSE, L"start /wait \"\" \"%1\\probe.bat\" a", L" /K \"%1\\probe.bat\" a", TRUE },
    { __LINE__, FALSE, L"start /wait \"\" \"%1\\probe.bat\"", L" /K \"%1\\probe.bat\"", TRUE },
};

static PWSTR FormatWithDir(PCWSTR pszFormat)
{
    PWSTR pszOut = NULL;
    DWORD_PTR Args[1] = { (DWORD_PTR)s_Dir };
    FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_ARGUMENT_ARRAY,
                   pszFormat, 0, 0, (PWSTR)&pszOut, 0, (va_list *)Args);
    return pszOut;
}

/* The probe writes UTF-16; the batch file's echo writes ANSI with a line break */
static BOOL ReadProbeOutput(PWSTR pszBuf, DWORD cchBuf)
{
    HANDLE hFile;
    BYTE Data[MAX_PATH * 6];
    DWORD cbRead = 0;
    BOOL bOk;
    size_t cch;

    hFile = CreateFileW(s_Out, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    bOk = ReadFile(hFile, Data, sizeof(Data) - sizeof(WCHAR), &cbRead, NULL);
    CloseHandle(hFile);
    if (!bOk || cbRead == 0)
        return FALSE;
    Data[cbRead] = Data[cbRead + 1] = 0;

    if (cbRead >= 2 && Data[1] == 0)
    {
        StringCchCopyW(pszBuf, cchBuf, (PCWSTR)Data);
    }
    else
    {
        MultiByteToWideChar(CP_ACP, 0, (PCSTR)Data, -1, pszBuf, cchBuf); /* ASCII here */
        cch = wcslen(pszBuf);
        while (cch && (pszBuf[cch - 1] == L'\r' || pszBuf[cch - 1] == L'\n'))
            pszBuf[--cch] = UNICODE_NULL;
    }
    return TRUE;
}

static void DoEntry(const START_ENTRY *pEntry)
{
    WCHAR szCmd[MAX_PATH * 3], szActual[MAX_PATH * 3];
    PWSTR pszStart, pszExpected;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    UINT i;

    if (pEntry->bBatch && !IsAscii(s_Dir))
    {
        skip("Line %d: the temporary directory is not ASCII\n", pEntry->line);
        return;
    }

    pszStart = FormatWithDir(pEntry->pszStart);
    pszExpected = FormatWithDir(pEntry->pszExpected);
    if (!pszStart || !pszExpected)
    {
        skip("Line %d: FormatMessageW failed\n", pEntry->line);
        goto Quit;
    }

    StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" /c %s", s_ComSpec, pszStart);

    DeleteFileW(s_Out);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, szCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                        pEntry->bInDir ? s_Dir : NULL, &si, &pi))
    {
        skip("Line %d: CreateProcessW failed: %lu\n", pEntry->line, GetLastError());
        goto Quit;
    }
    ok(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0, "Line %d: cmd did not finish\n", pEntry->line);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* Server 2008 R2 can return from start /wait /b before the program has
     * written its file, which it holds unshared while writing */
    for (i = 0; i < 50 && !ReadProbeOutput(szActual, _countof(szActual)); i++)
        Sleep(100);

    if (i == 50)
    {
        ok(FALSE, "Line %d: the probe did not run\n", pEntry->line);
        goto Quit;
    }
    if (pEntry->bBatch)
    {
        size_t cchActual, cchExpected;
        FoldBlanks(szActual);
        cchActual = wcslen(szActual);
        cchExpected = wcslen(pszExpected);
        ok(cchActual >= cchExpected && !wcscmp(szActual + cchActual - cchExpected, pszExpected),
           "Line %d: command line [%S], expected it to end with [%S]\n", pEntry->line, szActual, pszExpected);
    }
    else
    {
        ok(!wcscmp(szActual, pszExpected), "Line %d: command line [%S], expected [%S]\n",
           pEntry->line, szActual, pszExpected);
    }
    trace("Line %d: [%S]\n", pEntry->line, szActual);

Quit:
    LocalFree(pszStart);
    LocalFree(pszExpected);
}

START_TEST(start)
{
    WCHAR szTemp[MAX_PATH], szProbe[MAX_PATH], szCopy[MAX_PATH], szBat[MAX_PATH];
    HANDLE hBat;
    PWSTR pch;
    UINT i;

    /* The probe is next to the test, in testdata */
    GetModuleFileNameW(NULL, szProbe, _countof(szProbe));
    pch = wcsrchr(szProbe, L'\\');
    if (pch)
        *pch = UNICODE_NULL;
    StringCchCatW(szProbe, _countof(szProbe), L"\\testdata\\cmd_start_probe.exe");

    GetTempPathW(_countof(szTemp), szTemp);
    StringCchPrintfW(s_Dir, _countof(s_Dir), L"%scmd start %lu", szTemp, GetCurrentProcessId());
    CreateDirectoryW(s_Dir, NULL);
    StringCchPrintfW(szCopy, _countof(szCopy), L"%s\\probe.exe", s_Dir);
    if (!CopyFileW(szProbe, szCopy, FALSE))
    {
        skip("Cannot copy %S: %lu\n", szProbe, GetLastError());
        RemoveDirectoryW(s_Dir);
        return;
    }
    StringCchPrintfW(s_Out, _countof(s_Out), L"%s\\out.txt", s_Dir);
    SetEnvironmentVariableW(L"CMD_START_PROBE_OUT", s_Out);
    if (!GetEnvironmentVariableW(L"ComSpec", s_ComSpec, _countof(s_ComSpec)))
        StringCchCopyW(s_ComSpec, _countof(s_ComSpec), L"cmd.exe");

    StringCchPrintfW(szBat, _countof(szBat), L"%s\\probe.bat", s_Dir);
    hBat = CreateFileW(szBat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(hBat != INVALID_HANDLE_VALUE, "Cannot create %S: %lu\n", szBat, GetLastError());
    if (hBat != INVALID_HANDLE_VALUE)
    {
        static const char Bat[] = "@echo %CMDCMDLINE%>\"%CMD_START_PROBE_OUT%\"\r\n@exit\r\n";
        DWORD cbWritten;
        WriteFile(hBat, Bat, sizeof(Bat) - 1, &cbWritten, NULL);
        CloseHandle(hBat);
    }

    for (i = 0; i < _countof(s_Entries); i++)
        DoEntry(&s_Entries[i]);

    SetEnvironmentVariableW(L"CMD_START_PROBE_OUT", NULL);
    DeleteFileW(s_Out);
    DeleteFileW(szCopy);
    DeleteFileW(szBat);
    ok(RemoveDirectoryW(s_Dir), "RemoveDirectoryW failed: %lu\n", GetLastError());
}
