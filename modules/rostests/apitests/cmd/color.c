/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test which console attributes cmd changes: a plain "cmd /C" child must keep the
 *              colours of the text already on screen, only /T:, DefaultColor, COLOR and CLS repaint
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <strsafe.h>

#define FILL_ATTR   0x2A    /* what the screen buffer is painted with before cmd runs */
#define TEXT_ATTR   0x6E    /* the current text attribute before cmd runs */
#define NO_REG      0xFFFFFFFF
#define ENV_OUT     L"CMD_COLORTEST_OUT"

typedef struct COLOR_ENTRY
{
    INT line;
    PCWSTR pszArgs;     /* after the program name */
    DWORD dwDefColor;   /* HKCU DefaultColor, or NO_REG for none */
    DWORD dwExit;
    WORD wFill;         /* attribute of every cell of the buffer afterwards */
    WORD wText;         /* current text attribute afterwards */
} COLOR_ENTRY;

/* The results of Windows 10 cmd.exe, measured with the same sequence */
static const COLOR_ENTRY s_Entries[] =
{
    /* A child started with /C leaves the screen and the text attribute alone, even with /T: or
     * DefaultColor: those only apply to an interactive instance */
    { __LINE__, L"/C rem", NO_REG, 0, FILL_ATTR, TEXT_ATTR },
    { __LINE__, L"/T:1F /C rem", NO_REG, 0, FILL_ATTR, TEXT_ATTR },
    { __LINE__, L"/C rem", 0x1F, 0, FILL_ATTR, TEXT_ATTR },
    { __LINE__, L"/T:11 /K exit", NO_REG, 0, FILL_ATTR, TEXT_ATTR },
    /* An interactive instance paints everything when it is asked to */
    { __LINE__, L"/K exit", NO_REG, 0, FILL_ATTR, TEXT_ATTR },
    { __LINE__, L"/T:1F /K exit", NO_REG, 0, 0x1F, 0x1F },
    { __LINE__, L"/K exit", 0x1F, 0, 0x1F, 0x1F },
    { __LINE__, L"/T:2E /K exit", 0x1F, 0, 0x2E, 0x2E },
    { __LINE__, L"/T:1F /K cls&exit", NO_REG, 0, 0x1F, 0x1F },
    /* COLOR repaints the whole buffer, unless foreground and background are the same */
    { __LINE__, L"/C color 1f", NO_REG, 0, 0x1F, 0x1F },
    { __LINE__, L"/C color a", NO_REG, 0, 0x0A, 0x0A },
    { __LINE__, L"/C color 11 2>nul", NO_REG, 1, FILL_ATTR, TEXT_ATTR },
    /* COLOR without arguments goes back to the default colour: that of /T: or DefaultColor, or in
     * an interactive instance the attribute it started with. A "cmd /C" with none has no default */
    { __LINE__, L"/C color", NO_REG, 1, FILL_ATTR, TEXT_ATTR },
    { __LINE__, L"/T:1F /C color", NO_REG, 0, 0x1F, 0x1F },
    { __LINE__, L"/C color", 0x1F, 0, 0x1F, 0x1F },
    { __LINE__, L"/K color&exit", NO_REG, 0, TEXT_ATTR, TEXT_ATTR },
    /* CLS clears the whole buffer to the current text attribute */
    { __LINE__, L"/C cls", NO_REG, 0, TEXT_ATTR, TEXT_ATTR },
    { __LINE__, L"/T:1F /C cls", NO_REG, 0, TEXT_ATTR, TEXT_ATTR },
};

/* Each check of the child is a line of the result file: "1" passed, "0 message" failed */
#define CHECKS_PER_ENTRY 4

static HANDLE s_hResult = INVALID_HANDLE_VALUE;
static WCHAR s_ComSpec[MAX_PATH];

static void Report(BOOL bOk, PCSTR pszFormat, ...)
{
    char sz[512];
    DWORD cb;
    va_list args;

    if (bOk)
    {
        WriteFile(s_hResult, "1\r\n", 3, &cb, NULL);
        return;
    }
    StringCchCopyA(sz, sizeof(sz), "0 ");
    va_start(args, pszFormat);
    StringCchVPrintfA(sz + 2, sizeof(sz) - 2, pszFormat, args);
    va_end(args);
    StringCchCatA(sz, sizeof(sz), "\r\n");
    WriteFile(s_hResult, sz, (DWORD)strlen(sz), &cb, NULL);
}

/* Runs in the child, which has a console of its own that the test may paint */
static void DoEntry(const COLOR_ENTRY *pEntry, HANDLE hOut, HANDLE hIn)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    COORD pos = { 0, 0 };
    HKEY hKey = NULL;
    WCHAR szCmd[MAX_PATH + 64];
    DWORD dwWritten, dwExit = 0xDEAD, dwWait = WAIT_FAILED, y, x, dwRead, dwDisp = 0, dwOld = 0, dwOldType = 0;
    DWORD dwOldSize = sizeof(dwOld);
    BOOL bKeyExisted = FALSE, bValExisted = FALSE;
    WORD *pRow;
    DWORD dwWrong = 0, dwFirstX = 0, dwFirstY = 0;
    WORD wFirst = 0;

    GetConsoleScreenBufferInfo(hOut, &csbi);
    FillConsoleOutputAttribute(hOut, FILL_ATTR, csbi.dwSize.X * csbi.dwSize.Y, pos, &dwWritten);
    SetConsoleTextAttribute(hOut, TEXT_ATTR);

    if (pEntry->dwDefColor != NO_REG)
    {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Command Processor", 0, NULL, 0,
                            KEY_ALL_ACCESS, NULL, &hKey, &dwDisp) != ERROR_SUCCESS)
        {
            Report(FALSE, "Line %d: cannot set DefaultColor", pEntry->line);
            return;
        }
        bKeyExisted = (dwDisp == REG_OPENED_EXISTING_KEY);
        bValExisted = (RegQueryValueExW(hKey, L"DefaultColor", NULL, &dwOldType, (LPBYTE)&dwOld,
                                        &dwOldSize) == ERROR_SUCCESS);
        RegSetValueExW(hKey, L"DefaultColor", 0, REG_DWORD, (LPBYTE)&pEntry->dwDefColor, sizeof(DWORD));
    }

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hIn;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" %s", s_ComSpec, pEntry->pszArgs);
    if (CreateProcessW(NULL, szCmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        dwWait = WaitForSingleObject(pi.hProcess, 30000);
        if (dwWait != WAIT_OBJECT_0)
        {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        GetExitCodeProcess(pi.hProcess, &dwExit);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Report(dwWait == WAIT_OBJECT_0, "Line %d: cmd did not finish", pEntry->line);
    Report(dwExit == pEntry->dwExit, "Line %d: exit code %lu, expected %lu",
           pEntry->line, dwExit, pEntry->dwExit);

    GetConsoleScreenBufferInfo(hOut, &csbi);
    Report((csbi.wAttributes & 0xFF) == pEntry->wText, "Line %d: text attribute %02X, expected %02X",
           pEntry->line, csbi.wAttributes, pEntry->wText);

    pRow = HeapAlloc(GetProcessHeap(), 0, csbi.dwSize.X * sizeof(WORD));
    if (pRow)
    {
        for (y = 0; y < (DWORD)csbi.dwSize.Y; y++)
        {
            COORD row;
            row.X = 0;
            row.Y = (SHORT)y;
            dwRead = 0;
            if (!ReadConsoleOutputAttribute(hOut, pRow, csbi.dwSize.X, row, &dwRead))
                dwRead = 0;
            for (x = 0; x < (DWORD)csbi.dwSize.X; x++)
            {
                WORD wCell = (x < dwRead) ? pRow[x] : 0xFFFF;
                if (wCell != pEntry->wFill && !dwWrong++)
                {
                    dwFirstX = x;
                    dwFirstY = y;
                    wFirst = wCell;
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, pRow);
    }
    else
    {
        dwWrong = 1;
    }
    Report(dwWrong == 0, "Line %d: %lu cells do not have the attribute %02X, the first one (%lu,%lu) has %02X",
           pEntry->line, dwWrong, pEntry->wFill, dwFirstX, dwFirstY, wFirst);

    if (hKey)
    {
        if (bValExisted)
            RegSetValueExW(hKey, L"DefaultColor", 0, dwOldType, (LPBYTE)&dwOld, dwOldSize);
        else
            RegDeleteValueW(hKey, L"DefaultColor");
        RegCloseKey(hKey);
        if (!bKeyExisted)
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Command Processor");
    }
}

static void RunChild(PCWSTR pszResultFile)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hOut, hIn;
    UINT i;

    s_hResult = CreateFileW(pszResultFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_hResult == INVALID_HANDLE_VALUE)
        return;
    hOut = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, OPEN_EXISTING, 0, NULL);
    hIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);
    if (hOut != INVALID_HANDLE_VALUE && hIn != INVALID_HANDLE_VALUE)
    {
        for (i = 0; i < _countof(s_Entries); i++)
            DoEntry(&s_Entries[i], hOut, hIn);
    }
    CloseHandle(s_hResult);
}

START_TEST(color)
{
    WCHAR szSelf[MAX_PATH], szResult[MAX_PATH], szCmdLine[MAX_PATH + 16], szMode[MAX_PATH];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE hFile;
    DWORD cch, cbFile = 0, cbRead = 0, dwWait;
    PSTR pszFile, pch;
    UINT cChecks = 0;

    cch = GetEnvironmentVariableW(L"ComSpec", s_ComSpec, _countof(s_ComSpec));
    if (!cch || cch >= _countof(s_ComSpec))
        StringCchCopyW(s_ComSpec, _countof(s_ComSpec), L"cmd.exe");

    /* The child instance of the test paints the screen buffer of its own console */
    if (GetEnvironmentVariableW(ENV_OUT, szMode, _countof(szMode)))
    {
        RunChild(szMode);
        return;
    }

    cch = GetModuleFileNameW(NULL, szSelf, _countof(szSelf));
    if (!cch || cch >= _countof(szSelf) ||
        !GetTempPathW(_countof(szResult), szResult) ||
        FAILED(StringCchPrintfW(szResult + wcslen(szResult), _countof(szResult) - wcslen(szResult),
                                L"cmdcolor-%lu.txt", GetCurrentProcessId())) ||
        FAILED(StringCchPrintfW(szCmdLine, _countof(szCmdLine), L"\"%s\" color", szSelf)))
    {
        skip("Cannot name the test\n");
        return;
    }
    SetEnvironmentVariableW(ENV_OUT, szResult);
    if (!CreateProcessW(NULL, szCmdLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
    {
        skip("CreateProcessW failed: %lu\n", GetLastError());
        SetEnvironmentVariableW(ENV_OUT, NULL);
        return;
    }
    SetEnvironmentVariableW(ENV_OUT, NULL);
    dwWait = WaitForSingleObject(pi.hProcess, 300000);
    if (dwWait != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ok(dwWait == WAIT_OBJECT_0, "The test with its own console did not finish\n");

    hFile = CreateFileW(szResult, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        ok(FALSE, "No result of the test with its own console\n");
        return;
    }
    cbFile = GetFileSize(hFile, NULL);
    pszFile = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbFile + 1);
    if (pszFile && ReadFile(hFile, pszFile, cbFile, &cbRead, NULL))
    {
        for (pch = pszFile; *pch; )
        {
            PSTR pszEnd = strchr(pch, '\n');
            if (pszEnd)
            {
                if (pszEnd > pch && pszEnd[-1] == '\r')
                    pszEnd[-1] = 0;
                *pszEnd = 0;
            }
            cChecks++;
            ok(pch[0] == '1', "%s\n", pch + 2);
            pch = pszEnd ? pszEnd + 1 : pch + strlen(pch);
        }
    }
    CloseHandle(hFile);
    DeleteFileW(szResult);
    ok(cChecks == _countof(s_Entries) * CHECKS_PER_ENTRY, "%u checks, expected %u\n",
       cChecks, (UINT)(_countof(s_Entries) * CHECKS_PER_ENTRY));
    if (pszFile)
        HeapFree(GetProcessHeap(), 0, pszFile);
}
