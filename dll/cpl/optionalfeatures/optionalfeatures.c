/*
 * PROJECT:     ReactOS-Nitro Optional Features Control Panel (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     "Turn Windows features on or off" Control Panel applet. This is a
 *              thin launcher that hosts the optfeatures.exe manager (base/
 *              applications/featmgr) so the two-tier feature/addon manager is
 *              reachable the Windows-native way: Control Panel -> this applet,
 *              and the matching entry inside Add/Remove Programs (appwiz).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include "optionalfeatures.h"

#include <cpl.h>
#include <shellapi.h>

HINSTANCE hApplet = NULL;

/* Launch the hosted feature manager (optfeatures.exe, shipped to system32). */
static void LaunchManager(HWND hWnd)
{
    HINSTANCE hRet = ShellExecuteW(hWnd, L"open", L"optfeatures.exe", NULL,
                                   NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hRet <= 32)
    {
        WCHAR title[64], text[256];
        LoadStringW(hApplet, IDS_CPLNAME, title, ARRAYSIZE(title));
        LoadStringW(hApplet, IDS_LAUNCHFAILED, text, ARRAYSIZE(text));
        MessageBoxW(hWnd, text, title, MB_OK | MB_ICONERROR);
    }
}

LONG CALLBACK CPlApplet(HWND hWnd, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
    switch (uMsg)
    {
        case CPL_INIT:
            return TRUE;

        case CPL_GETCOUNT:
            return 1;

        case CPL_INQUIRE:
        {
            CPLINFO *CPlInfo = (CPLINFO *)lParam2;
            CPlInfo->lData = 0;
            CPlInfo->idIcon = IDI_CPLICON;
            CPlInfo->idName = IDS_CPLNAME;
            CPlInfo->idInfo = IDS_CPLDESCRIPTION;
            break;
        }

        case CPL_DBLCLK:
            LaunchManager(hWnd);
            break;
    }

    UNREFERENCED_PARAMETER(lParam1);
    return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
            hApplet = hinstDLL;
            break;
    }

    return TRUE;
}
