/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the extended errors of PrintDlgA/PrintDlgW
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <commdlg.h>
#include <cderr.h>

static void
TestPrintDlgW(void)
{
    PRINTDLGW pd;
    BOOL bRet;

    /* A failed call leaves an extended error ... */
    bRet = PrintDlgW(NULL);
    ok(!bRet, "PrintDlgW(NULL) returned %d\n", bRet);
    ok(CommDlgExtendedError() == CDERR_INITIALIZATION, "Extended error 0x%lx\n", CommDlgExtendedError());

    /* ... which a later successful call does not report again */
    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDEFAULT;
    bRet = PrintDlgW(&pd);
    if (!bRet && CommDlgExtendedError() == PDERR_NODEFAULTPRN)
    {
        skip("No default printer\n");
        return;
    }
    ok(bRet, "PrintDlgW(PD_RETURNDEFAULT) failed with extended error 0x%lx\n", CommDlgExtendedError());
    ok(CommDlgExtendedError() == 0, "Extended error 0x%lx after success\n", CommDlgExtendedError());
    GlobalFree(pd.hDevMode);
    GlobalFree(pd.hDevNames);

    /* A printer device context is returned, or its failure is reported */
    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDEFAULT | PD_RETURNDC;
    bRet = PrintDlgW(&pd);
    if (bRet)
    {
        ok(pd.hDC != NULL, "No device context\n");
        ok(CommDlgExtendedError() == 0, "Extended error 0x%lx after success\n", CommDlgExtendedError());
    }
    else
    {
        ok(pd.hDC == NULL, "Device context %p after failure\n", pd.hDC);
        ok(CommDlgExtendedError() == PDERR_CREATEICFAILURE, "Extended error 0x%lx\n", CommDlgExtendedError());
    }
    if (pd.hDC)
        DeleteDC(pd.hDC);
    GlobalFree(pd.hDevMode);
    GlobalFree(pd.hDevNames);
}

static void
TestPrintDlgA(void)
{
    PRINTDLGA pd;
    BOOL bRet;

    bRet = PrintDlgA(NULL);
    ok(!bRet, "PrintDlgA(NULL) returned %d\n", bRet);
    ok(CommDlgExtendedError() == CDERR_INITIALIZATION, "Extended error 0x%lx\n", CommDlgExtendedError());

    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDEFAULT | PD_RETURNDC;
    bRet = PrintDlgA(&pd);
    if (!bRet && CommDlgExtendedError() == PDERR_NODEFAULTPRN)
    {
        skip("No default printer\n");
        return;
    }
    if (bRet)
    {
        ok(pd.hDC != NULL, "No device context\n");
        ok(CommDlgExtendedError() == 0, "Extended error 0x%lx after success\n", CommDlgExtendedError());
    }
    else
    {
        ok(pd.hDC == NULL, "Device context %p after failure\n", pd.hDC);
        ok(CommDlgExtendedError() == PDERR_CREATEICFAILURE, "Extended error 0x%lx\n", CommDlgExtendedError());
    }
    if (pd.hDC)
        DeleteDC(pd.hDC);
    GlobalFree(pd.hDevMode);
    GlobalFree(pd.hDevNames);
}

START_TEST(PrintDlg)
{
    TestPrintDlgW();
    TestPrintDlgA();
}
