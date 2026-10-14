/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     LoadImageW with an icon resource whose images are PNG compressed
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

START_TEST(LoadImagePngIcon)
{
    static const struct
    {
        int cx;
        int cy;
    } sizes[] = { { 11, 11 }, { 16, 16 }, { 20, 20 }, { 32, 32 }, { 48, 48 }, { 64, 64 }, { 11, 20 } };
    HINSTANCE hInst = GetModuleHandleW(NULL);
    UINT i;

    for (i = 0; i < ARRAYSIZE(sizes); i++)
    {
        HICON hIcon;
        ICONINFO ii;
        BITMAP bm;

        SetLastError(0xdeadbeef);
        hIcon = LoadImageW(hInst, MAKEINTRESOURCEW(IDI_TEST_PNG), IMAGE_ICON, sizes[i].cx, sizes[i].cy, 0);
        ok(hIcon != NULL, "%ux%u: LoadImageW failed, error %lu\n", sizes[i].cx, sizes[i].cy, GetLastError());
        if (!hIcon)
            continue;

        ZeroMemory(&bm, sizeof(bm));
        ZeroMemory(&ii, sizeof(ii));
        ok(GetIconInfo(hIcon, &ii), "%ux%u: GetIconInfo failed, error %lu\n", sizes[i].cx, sizes[i].cy, GetLastError());
        if (ii.hbmColor)
        {
            ok(GetObjectW(ii.hbmColor, sizeof(bm), &bm) != 0, "GetObject failed\n");
            ok(bm.bmWidth == sizes[i].cx, "%ux%u: width is %ld\n", sizes[i].cx, sizes[i].cy, bm.bmWidth);
            ok(bm.bmHeight == sizes[i].cy, "%ux%u: height is %ld\n", sizes[i].cx, sizes[i].cy, bm.bmHeight);
            DeleteObject(ii.hbmColor);
        }
        else
        {
            ok(FALSE, "%ux%u: no color bitmap\n", sizes[i].cx, sizes[i].cy);
        }
        if (ii.hbmMask)
            DeleteObject(ii.hbmMask);
        DestroyIcon(hIcon);
    }
}
