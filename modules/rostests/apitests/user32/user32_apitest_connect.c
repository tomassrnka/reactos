/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Helper for the ProcessConnectTerminate test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <stdlib.h>
#include <windef.h>
#include <winbase.h>

/* Imports no user32, so its first win32k call is the one in loading user32 */
int wmain(int argc, WCHAR **argv)
{
    HANDLE Started, Connected;

    if (argc < 3)
        return 1;
    Started = (HANDLE)(ULONG_PTR)_wcstoui64(argv[1], NULL, 16);
    Connected = (HANDLE)(ULONG_PTR)_wcstoui64(argv[2], NULL, 16);

    SetEvent(Started);
    if (!LoadLibraryW(L"user32.dll"))
        return 2;
    SetEvent(Connected);
    Sleep(60000);
    return 3;
}
