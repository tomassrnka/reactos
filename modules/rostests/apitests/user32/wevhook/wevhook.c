/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     DLL with a WinEvent hook procedure; built twice at different image bases
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>

static volatile LONG HookCount;

VOID WINAPI
WevHookProc(HWINEVENTHOOK hHook, DWORD Event, HWND hWnd, LONG idObject, LONG idChild, DWORD EventThread, DWORD EventTime)
{
    InterlockedIncrement(&HookCount);
}

LONG WINAPI
WevGetCount(VOID)
{
    return HookCount;
}

BOOL WINAPI
DllMain(HINSTANCE hInstDll, DWORD Reason, LPVOID Reserved)
{
    return TRUE;
}
