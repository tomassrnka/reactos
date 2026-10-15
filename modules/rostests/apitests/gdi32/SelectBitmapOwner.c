/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the DC a bitmap is selected into, across DeleteDC, SaveDC and RestoreDC
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* A bitmap goes into a DC only if no other DC holds it */
static BOOL
CanSelect(HBITMAP hbm)
{
    HDC hdc = CreateCompatibleDC(NULL);
    HGDIOBJ hOld;
    BOOL Ret;

    ok(hdc != NULL, "CreateCompatibleDC failed\n");
    if (!hdc)
        return FALSE;
    hOld = SelectObject(hdc, hbm);
    Ret = hOld != NULL;
    if (Ret)
        SelectObject(hdc, hOld);
    DeleteDC(hdc);
    return Ret;
}

static VOID
Test_DeleteDC(VOID)
{
    HDC hdc;
    HBITMAP hbm;
    ULONG i;

    hbm = CreateBitmap(8, 8, 1, 1, NULL);
    ok(hbm != NULL, "CreateBitmap failed\n");
    if (!hbm)
        return;

    /* A new DC for each round gets the handle of an earlier one back sooner or later */
    for (i = 0; i < 600; i++)
    {
        hdc = CreateCompatibleDC(NULL);
        if (!hdc)
        {
            ok(0, "CreateCompatibleDC failed in round %lu\n", i);
            break;
        }
        if (!SelectObject(hdc, hbm))
        {
            ok(0, "Round %lu: the bitmap does not go into a new DC\n", i);
            DeleteDC(hdc);
            break;
        }
        ok(!CanSelect(hbm), "Round %lu: the bitmap went into a second DC\n", i);
        DeleteDC(hdc);
    }
    ok(CanSelect(hbm), "The bitmap stays unusable after its DC was deleted\n");
    DeleteObject(hbm);
}

static VOID
Test_SaveRestore(VOID)
{
    HDC hdc;
    HBITMAP hbm1, hbm2;
    HGDIOBJ hOld;

    hdc = CreateCompatibleDC(NULL);
    hbm1 = CreateBitmap(8, 8, 1, 1, NULL);
    hbm2 = CreateBitmap(16, 16, 1, 1, NULL);
    ok(hdc && hbm1 && hbm2, "Setup failed\n");
    if (!hdc || !hbm1 || !hbm2)
        goto Exit;

    hOld = SelectObject(hdc, hbm1);
    ok(hOld != NULL, "SelectObject failed\n");
    ok(!CanSelect(hbm1), "The bitmap is in a DC, yet it went into another one\n");

    /* A restore with no change keeps the bitmap in the DC */
    ok(SaveDC(hdc) != 0, "SaveDC failed\n");
    ok(RestoreDC(hdc, -1), "RestoreDC failed\n");
    ok(SelectObject(hdc, hbm1) == hbm1, "The bitmap left the DC\n");
    ok(!CanSelect(hbm1), "After SaveDC and RestoreDC the bitmap went into a second DC\n");

    /* A restore after a change puts the old bitmap back and frees the new one (Windows also keeps the saved one held, ReactOS does not) */
    ok(SaveDC(hdc) != 0, "SaveDC failed\n");
    ok(SelectObject(hdc, hbm2) == hbm1, "SelectObject did not return the first bitmap\n");
    ok(!CanSelect(hbm2), "The second bitmap is in the DC, yet it went into another one\n");
    ok(RestoreDC(hdc, -1), "RestoreDC failed\n");
    ok(SelectObject(hdc, hbm1) == hbm1, "The restore did not bring the first bitmap back\n");
    ok(!CanSelect(hbm1), "After the restore the first bitmap went into a second DC\n");
    ok(CanSelect(hbm2), "After the restore the second bitmap is still held\n");

    /* Deleting the DC with a state saved frees both bitmaps */
    ok(SaveDC(hdc) != 0, "SaveDC failed\n");
    ok(SelectObject(hdc, hbm2) == hbm1, "SelectObject did not return the first bitmap\n");
    DeleteDC(hdc);
    hdc = NULL;
    ok(CanSelect(hbm1), "The first bitmap is held after its DC was deleted\n");
    ok(CanSelect(hbm2), "The second bitmap is held after its DC was deleted\n");

Exit:
    if (hdc)
        DeleteDC(hdc);
    if (hbm1)
        DeleteObject(hbm1);
    if (hbm2)
        DeleteObject(hbm2);
}

START_TEST(SelectBitmapOwner)
{
    Test_DeleteDC();
    Test_SaveRestore();
}
