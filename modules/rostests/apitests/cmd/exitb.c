/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the ERRORLEVEL and the exit code that EXIT /B and CALL give batch files
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <stdio.h>

typedef struct EXITB_FILE
{
    PCSTR pszName;
    PCSTR pszContent;
} EXITB_FILE;

/* Expected values were measured on Windows 10 22H2 */
static const EXITB_FILE s_Files[] =
{
    { "x3.cmd",         "@exit /b 3\r\n" },
    { "x5.cmd",         "@exit 5\r\n" },
    { "xneg.cmd",       "@exit /b -1\r\n" },
    { "xplus.cmd",      "@exit /b +3\r\n" },
    { "xjunk.cmd",      "@cmd /c exit 4\r\n@exit /b 3x\r\n" },
    { "xnospace.cmd",   "@exit /b3\r\n" },
    { "xword.cmd",      "@cmd /c exit 4\r\n@exit /b abc\r\n" },
    { "xnone.cmd",      "@cmd /c exit 4\r\n@exit /b\r\n" },
    { "xeof.cmd",       "@cmd /c exit 4\r\n@goto :eof\r\n" },
    { "xzero.cmd",      "@cmd /c exit 4\r\n@exit /b 0\r\n" },
    { "xexit0.cmd",     "@cmd /c exit 4\r\n@exit 0\r\n" },
    { "xexitneg.cmd",   "@exit -1\r\n" },
    { "xecho.cmd",      "@cmd /c exit 4\r\n@echo hi\r\n" },
    { "xblock.cmd",     "@if 1==1 (exit /b 3)\r\n" },
    { "xsub.cmd",       "@call :sub\r\n@echo SUB=%errorlevel%\r\n@goto :eof\r\n:sub\r\n@exit /b 6\r\n" },
    { "n1.cmd",         "@call x3.cmd\r\n@echo L2=%errorlevel%\r\n@exit /b 7\r\n" },
    { "w_okx.cmd",      "@rem nothing\r\n" },
    { "w_call.cmd",     "@call x3.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_callend.cmd",  "@call x3.cmd\r\n" },
    { "w_nested.cmd",   "@call n1.cmd\r\n@echo L1=%errorlevel%\r\n" },
    { "w_nocall.cmd",   "@x3.cmd\r\n@echo NOTREACHED\r\n" },
    { "w_exit5.cmd",    "@call x5.cmd\r\n@echo NOTREACHED\r\n" },
    { "w_none.cmd",     "@call xnone.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_noneend.cmd",  "@call xnone.cmd\r\n" },
    { "w_eof.cmd",      "@call xeof.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_zero.cmd",     "@call xzero.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_block.cmd",    "@call xblock.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_neg.cmd",      "@call xneg.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_plus.cmd",     "@call xplus.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_junk.cmd",     "@call xjunk.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_nospace.cmd",  "@call xnospace.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_word.cmd",     "@call xword.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_xecho.cmd",    "@call xecho.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_if.cmd",       "@call x3.cmd\r\n@if errorlevel 3 (echo IF3) else (echo IFNO)\r\n" },
    { "w_ifcall.cmd",   "@if 1==1 call x3.cmd\r\n@echo EL=%errorlevel%\r\n" },
    { "w_orand.cmd",    "@call x3.cmd || echo OR-TAKEN\r\n@call x3.cmd && echo AND-TAKEN\r\n" },
    { "w_empty.cmd",    "@cmd /c exit 4\r\n@call w_okx.cmd\r\n@echo EL=%errorlevel%\r\n" },
};

typedef struct EXITB_ENTRY
{
    INT line;
    PCSTR pszArgs;      /* the command line after cmd.exe */
    DWORD dwExitCode;   /* the exit code of the cmd.exe process */
    PCSTR pszOutput;    /* its output, lines joined with | */
} EXITB_ENTRY;

static const EXITB_ENTRY s_Entries[] =
{
    /* CMD /C of a batch file: the exit code of EXIT /B n */
    { __LINE__, "/c x3.cmd",                                    3, "" },
    { __LINE__, "/c \"x3.cmd\"",                                3, "" },
    { __LINE__, "/c call x3.cmd",                               3, "" },
    { __LINE__, "/c \"call x3.cmd\"",                           3, "" },
    { __LINE__, "/c cmd /c x3.cmd",                             3, "" },
    { __LINE__, "/c \"x3.cmd & echo AFTER\"",                   0, "AFTER|" },
    { __LINE__, "/c x5.cmd",                                    5, "" },
    { __LINE__, "/c xblock.cmd",                                3, "" },
    { __LINE__, "/c xneg.cmd",                                  0xFFFFFFFF, "" },
    { __LINE__, "/c xplus.cmd",                                 3, "" },
    { __LINE__, "/c xjunk.cmd",                                 3, "" },
    { __LINE__, "/c xnospace.cmd",                              3, "" },
    { __LINE__, "/c xexitneg.cmd",                              0xFFFFFFFF, "" },
    /* EXIT /B with no number, or GOTO :EOF, succeed, though ERRORLEVEL stays */
    { __LINE__, "/c xnone.cmd",                                 0, "" },
    { __LINE__, "/c xeof.cmd",                                  0, "" },
    { __LINE__, "/c xword.cmd",                                 0, "" },
    { __LINE__, "/c xzero.cmd",                                 0, "" },
    { __LINE__, "/c xexit0.cmd",                                0, "" },
    { __LINE__, "/c xecho.cmd",                                 0, "hi|" },
    /* A file run without CALL ends the calling file */
    { __LINE__, "/c w_nocall.cmd",                              3, "" },
    /* EXIT without /B ends everything */
    { __LINE__, "/c w_exit5.cmd",                               5, "" },
    /* ERRORLEVEL after CALL */
    { __LINE__, "/c w_call.cmd",                                0, "EL=3|" },
    { __LINE__, "/c w_callend.cmd",                             3, "" },
    { __LINE__, "/c \"call x3.cmd & echo INLINE=%errorlevel%\"", 0, "INLINE=0|" },
    { __LINE__, "/v:on /c \"call x3.cmd & echo INLINE=!errorlevel!\"", 0, "INLINE=3|" },
    { __LINE__, "/c w_if.cmd",                                  0, "IF3|" },
    { __LINE__, "/c w_ifcall.cmd",                              0, "EL=3|" },
    { __LINE__, "/c w_orand.cmd",                               3, "OR-TAKEN|" },
    { __LINE__, "/c w_block.cmd",                               0, "EL=3|" },
    { __LINE__, "/c w_neg.cmd",                                 0, "EL=-1|" },
    { __LINE__, "/c w_plus.cmd",                                0, "EL=3|" },
    { __LINE__, "/c w_junk.cmd",                                0, "EL=3|" },
    { __LINE__, "/c w_nospace.cmd",                             0, "EL=3|" },
    /* Nested calls */
    { __LINE__, "/c w_nested.cmd",                              0, "L2=3|L1=7|" },
    { __LINE__, "/c n1.cmd",                                    7, "L2=3|" },
    { __LINE__, "/c xsub.cmd",                                  0, "SUB=6|" },
    /* An ERRORLEVEL that a file leaves stays */
    { __LINE__, "/c w_none.cmd",                                0, "EL=4|" },
    { __LINE__, "/c w_noneend.cmd",                             4, "" },
    { __LINE__, "/c w_eof.cmd",                                 0, "EL=4|" },
    { __LINE__, "/c w_word.cmd",                                0, "EL=4|" },
    { __LINE__, "/c w_xecho.cmd",                               0, "hi|EL=4|" },
    { __LINE__, "/c w_empty.cmd",                               0, "EL=4|" },
    { __LINE__, "/c w_zero.cmd",                                0, "EL=0|" },
};

static char s_Dir[MAX_PATH];

static BOOL WriteFixture(const EXITB_FILE *pFile)
{
    char szPath[MAX_PATH];
    HANDLE hFile;
    DWORD cbWritten;

    sprintf(szPath, "%s\\%s", s_Dir, pFile->pszName);
    hFile = CreateFileA(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    WriteFile(hFile, pFile->pszContent, (DWORD)strlen(pFile->pszContent), &cbWritten, NULL);
    CloseHandle(hFile);
    return TRUE;
}

static void DoEntry(const EXITB_ENTRY *pEntry)
{
    char szCmd[MAX_PATH], szBuf[512], *pch, *pchOut;
    DWORD cbRead, cbTotal = 0, dwExitCode = 0xDEADBEEF;
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        skip("Line %d: CreatePipe failed: %lu\n", pEntry->line, GetLastError());
        return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    sprintf(szCmd, "cmd.exe %s", pEntry->pszArgs);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    if (!CreateProcessA(NULL, szCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, s_Dir, &si, &pi))
    {
        skip("Line %d: CreateProcess failed: %lu\n", pEntry->line, GetLastError());
        CloseHandle(hWrite);
        CloseHandle(hRead);
        return;
    }
    CloseHandle(hWrite);

    while (cbTotal < sizeof(szBuf) - 1 &&
           ReadFile(hRead, szBuf + cbTotal, sizeof(szBuf) - 1 - cbTotal, &cbRead, NULL) && cbRead)
    {
        cbTotal += cbRead;
    }
    szBuf[cbTotal] = ANSI_NULL;
    CloseHandle(hRead);

    ok(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0, "Line %d: cmd did not finish\n", pEntry->line);
    GetExitCodeProcess(pi.hProcess, &dwExitCode);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* Drop the carriage returns and join the lines with | */
    for (pch = pchOut = szBuf; *pch; pch++)
    {
        if (*pch == '\r')
            continue;
        *pchOut++ = (*pch == '\n') ? '|' : *pch;
    }
    *pchOut = ANSI_NULL;

    ok(dwExitCode == pEntry->dwExitCode, "Line %d [%s]: exit code %lu, expected %lu\n",
       pEntry->line, pEntry->pszArgs, dwExitCode, pEntry->dwExitCode);
    ok(!strcmp(szBuf, pEntry->pszOutput), "Line %d [%s]: output [%s], expected [%s]\n",
       pEntry->line, pEntry->pszArgs, szBuf, pEntry->pszOutput);
}

START_TEST(exitb)
{
    char szTemp[MAX_PATH], szPath[MAX_PATH];
    UINT i;

    GetTempPathA(_countof(szTemp), szTemp);
    sprintf(s_Dir, "%scmd exitb %lu", szTemp, GetCurrentProcessId());
    CreateDirectoryA(s_Dir, NULL);

    for (i = 0; i < _countof(s_Files); i++)
    {
        if (!WriteFixture(&s_Files[i]))
        {
            skip("Cannot create %s: %lu\n", s_Files[i].pszName, GetLastError());
            goto Cleanup;
        }
    }

    for (i = 0; i < _countof(s_Entries); i++)
        DoEntry(&s_Entries[i]);

Cleanup:
    for (i = 0; i < _countof(s_Files); i++)
    {
        sprintf(szPath, "%s\\%s", s_Dir, s_Files[i].pszName);
        DeleteFileA(szPath);
    }
    ok(RemoveDirectoryA(s_Dir), "RemoveDirectoryA failed: %lu\n", GetLastError());
}
