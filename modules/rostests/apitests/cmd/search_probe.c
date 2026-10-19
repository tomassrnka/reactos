/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Child program for the cmd search test: leaves a file next to itself
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windef.h>
#include <winbase.h>

/* Creates "<its own path>.out" and exits with 42 */
int wmain(void)
{
    WCHAR szPath[MAX_PATH + 5];
    DWORD cch = GetModuleFileNameW(NULL, szPath, MAX_PATH);
    HANDLE hFile;

    if (!cch || cch >= MAX_PATH)
        return 1;
    lstrcpyW(szPath + cch, L".out");
    hFile = CreateFileW(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return 2;
    CloseHandle(hFile);
    return 42;
}
