/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for CM_Is_Dock_Station_Present
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <cfgmgr32.h>

START_TEST(CM_Is_Dock_Station_Present)
{
    HW_PROFILE_INFOW HwProfile;
    BOOL bDocked = FALSE;
    CONFIGRET cr;
    BOOL bPresent;

    /* Only a machine that reports itself docked (and not also undocked) may have a dock */
    if (GetCurrentHwProfileW(&HwProfile))
    {
        trace("dwDockInfo 0x%lx\n", HwProfile.dwDockInfo);
        bDocked = (HwProfile.dwDockInfo & (DOCKINFO_DOCKED | DOCKINFO_UNDOCKED)) == DOCKINFO_DOCKED;
    }

    bPresent = 0xAA;
    cr = CM_Is_Dock_Station_Present(&bPresent);
    ok_long(cr, CR_SUCCESS);
    if (bDocked)
        skip("The machine is docked\n");
    else
        ok_int(bPresent, FALSE);

    bPresent = 0xAA;
    cr = CM_Is_Dock_Station_Present_Ex(&bPresent, NULL);
    ok_long(cr, CR_SUCCESS);
    if (!bDocked)
        ok_int(bPresent, FALSE);

    cr = CM_Is_Dock_Station_Present(NULL);
    ok_long(cr, CR_INVALID_POINTER);
}
