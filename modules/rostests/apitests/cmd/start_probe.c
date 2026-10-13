/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Child program for the cmd start test: saves its command line
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windef.h>
#include <winbase.h>

int wmain(void)
{
    WCHAR szPath[MAX_PATH];
    LPCWSTR pszCmdLine = GetCommandLineW();
    HANDLE hFile;
    DWORD cbWritten;

    if (!GetEnvironmentVariableW(L"CMD_START_PROBE_OUT", szPath, sizeof(szPath) / sizeof(szPath[0])))
        return 1;

    hFile = CreateFileW(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return 2;
    WriteFile(hFile, pszCmdLine, lstrlenW(pszCmdLine) * sizeof(WCHAR), &cbWritten, NULL);
    CloseHandle(hFile);
    return 42;
}
