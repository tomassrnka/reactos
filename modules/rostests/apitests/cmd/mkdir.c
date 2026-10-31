/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the cmd.exe md command: several new levels at once, also behind a \\?\ prefix
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * md walks up the path one component at a time when the whole path cannot be created. It started with the
 * first component of \\?\C:\dir\sub and \\server\share\dir\sub, which are not directories, and gave up: an
 * sshfs-win or WinFsp share could not take "md \\sshfs.k\user@host\path\a\b".
 */

#include "precomp.h"
#include <strsafe.h>

static WCHAR s_ComSpec[MAX_PATH];

/* Runs "cmd.exe /d /c md <path>" hidden and returns its exit code */
static DWORD RunMd(PCWSTR pszPath)
{
    WCHAR szCmd[2048];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD dwExit = (DWORD)-1;

    StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" /d /c md \"%s\"", s_ComSpec, pszPath);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, szCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcess failed, error %lu\n", GetLastError());
        return dwExit;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &dwExit);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return dwExit;
}

static BOOL IsDir(PCWSTR pszPath)
{
    DWORD dwAttr = GetFileAttributesW(pszPath);

    return dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_DIRECTORY);
}

static void
Remove3(PCWSTR pszBase, PCWSTR pszA, PCWSTR pszB, PCWSTR pszC)
{
    WCHAR sz[MAX_PATH];

    StringCchPrintfW(sz, _countof(sz), L"%s\\%s\\%s\\%s", pszBase, pszA, pszB, pszC);
    RemoveDirectoryW(sz);
    StringCchPrintfW(sz, _countof(sz), L"%s\\%s\\%s", pszBase, pszA, pszB);
    RemoveDirectoryW(sz);
    StringCchPrintfW(sz, _countof(sz), L"%s\\%s", pszBase, pszA);
    RemoveDirectoryW(sz);
}

START_TEST(mkdir)
{
    WCHAR szBase[MAX_PATH], szPath[MAX_PATH], szSub[MAX_PATH];
    DWORD dwExit, cch;

    cch = GetEnvironmentVariableW(L"ComSpec", s_ComSpec, _countof(s_ComSpec));
    if (!cch || cch >= _countof(s_ComSpec))
    {
        skip("no ComSpec\n");
        return;
    }

    cch = GetTempPathW(_countof(szBase), szBase);
    if (!cch || cch >= _countof(szBase) - 40)
    {
        skip("no temp directory\n");
        return;
    }
    if (szBase[cch - 1] == L'\\')
        szBase[cch - 1] = 0;
    StringCchPrintfW(szSub, _countof(szSub), L"%s\\cmdmkdir%lu", szBase, GetCurrentProcessId());
    ok(CreateDirectoryW(szSub, NULL), "cannot create %ls, error %lu\n", szSub, GetLastError());

    /* plain path, three new levels */
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\a\\b\\c", szSub);
    dwExit = RunMd(szPath);
    ok(dwExit == 0, "md %ls: exit code %lu\n", szPath, dwExit);
    ok(IsDir(szPath), "%ls was not created\n", szPath);
    Remove3(szSub, L"a", L"b", L"c");

    /* behind \\?\ */
    StringCchPrintfW(szPath, _countof(szPath), L"\\\\?\\%s\\x\\y\\z", szSub);
    dwExit = RunMd(szPath);
    ok(dwExit == 0, "md %ls: exit code %lu\n", szPath, dwExit);
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\x\\y\\z", szSub);
    ok(IsDir(szPath), "%ls was not created\n", szPath);
    Remove3(szSub, L"x", L"y", L"z");

    /* behind \\.\ */
    StringCchPrintfW(szPath, _countof(szPath), L"\\\\.\\%s\\p\\q\\r", szSub);
    dwExit = RunMd(szPath);
    ok(dwExit == 0, "md %ls: exit code %lu\n", szPath, dwExit);
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\p\\q\\r", szSub);
    ok(IsDir(szPath), "%ls was not created\n", szPath);
    Remove3(szSub, L"p", L"q", L"r");

    /* a name that exists is an error, as before */
    StringCchPrintfW(szPath, _countof(szPath), L"%s", szSub);
    dwExit = RunMd(szPath);
    ok(dwExit != 0, "md of an existing directory succeeded\n");

    RemoveDirectoryW(szSub);
}
