/*
 *
 * PROJECT:         ReactOS Software Control Panel
 * FILE:            dll/cpl/telephon/telephon.c
 * PURPOSE:         ReactOS Software Control Panel
 * PROGRAMMER:      Dmitry Chapyshev (dmitry@reactos.org)
 * UPDATE HISTORY:
 *    10-19-2007  Created
 */

#define WIN32_NO_STATUS
#include <stdarg.h>
#include <stdlib.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <cpl.h>

#include "resource.h"

static HINSTANCE hApplet;

/* Control Panel Callback */
LONG CALLBACK
CPlApplet(HWND hwndCPl, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
    CPLINFO *CPlInfo;
    WCHAR szTitle[128], szText[512];

    switch (uMsg)
    {
        case CPL_INIT:
            return TRUE;

        case CPL_GETCOUNT:
            return 1;

        case CPL_INQUIRE:
            CPlInfo = (CPLINFO*)lParam2;
            CPlInfo->lData = 0;
            CPlInfo->idIcon = IDI_CPLSYSTEM;
            CPlInfo->idName = IDS_CPLSYSTEMNAME;
            CPlInfo->idInfo = IDS_CPLSYSTEMDESCRIPTION;
            break;

        case CPL_DBLCLK:
        {
            /* The dialog lives in tapi32's internalConfig, which is still a stub */
            if (!LoadStringW(hApplet, IDS_CPLSYSTEMNAME, szTitle, _countof(szTitle)) ||
                !LoadStringW(hApplet, IDS_CPLNOTAVAILABLE, szText, _countof(szText)))
            {
                return 1;
            }
            MessageBoxW(hwndCPl, szText, szTitle, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }

    return FALSE;
}

BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
        hApplet = hinstDLL;
    return TRUE;
}
