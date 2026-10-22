/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for NtGdiSetDIBitsToDeviceInternal
 * PROGRAMMERS:
 */

#include "../win32nt.h"

void
ReadBits(HDC hDC, PDWORD OutBits)
{
    int x,y;

    for (y = 0; y < 8; y++)
    {
        DWORD Row = 0;
        for (x = 0; x < 8; x++)
            Row |= (0x80 & GetPixel(hDC, 2 + x, 3 + y)) >> x;
        OutBits[y] = Row;
    }
}


/* Returns a buffer of cb bytes that ends exactly at an inaccessible page */
static PBYTE
AllocBeforeGuard(SIZE_T cb, PVOID *ppBase)
{
    DWORD dwOld;
    BOOL bOk;
    PBYTE pb = VirtualAlloc(NULL, 2 * 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    ok(pb != NULL, "VirtualAlloc failed\n");
    bOk = VirtualProtect(pb + 0x1000, 0x1000, PAGE_NOACCESS, &dwOld);
    ok(bOk, "VirtualProtect failed\n");
    *ppBase = pb;
    return pb + 0x1000 - cb;
}

/*
 * Contract test: the info / bits lengths are honoured. Bad lengths and headers
 * are rejected, nothing is read past the given lengths (the buffers end at a
 * guard page), and a valid call still works.
 */
static void
Test_Contract(void)
{
    HDC hdcMem;
    HBITMAP hbmDst, hbmOld;
    PVOID pvDst, pBaseInfo, pBaseBits;
    PBYTE pbmi, pbits;
    BITMAPINFO bmiDst;
    BITMAPINFOHEADER *bih;
    INT ret;
    DWORD i;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } full;

    memset(&bmiDst, 0, sizeof(bmiDst));
    bmiDst.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmiDst.bmiHeader.biWidth = 16;
    bmiDst.bmiHeader.biHeight = 16;
    bmiDst.bmiHeader.biPlanes = 1;
    bmiDst.bmiHeader.biBitCount = 32;
    hbmDst = CreateDIBSection(NULL, &bmiDst, DIB_RGB_COLORS, &pvDst, NULL, 0);
    hdcMem = CreateCompatibleDC(NULL);
    ok(hbmDst && hdcMem, "setup failed\n");
    if (!hbmDst || !hdcMem) return;
    hbmOld = SelectObject(hdcMem, hbmDst);

    memset(&full, 0, sizeof(full));
    full.h.biSize = sizeof(BITMAPINFOHEADER);
    full.h.biWidth = 8;
    full.h.biHeight = 8;
    full.h.biPlanes = 1;
    full.h.biBitCount = 8;
    full.h.biCompression = BI_RGB;
    for (i = 0; i < 256; i++)
        full.c[i].rgbRed = (BYTE)i;

    pbits = AllocBeforeGuard(64, &pBaseBits);
    memset(pbits, 3, 64);
    pbmi = AllocBeforeGuard(sizeof(full), &pBaseInfo);
    memcpy(pbmi, &full, sizeof(full));
    bih = (BITMAPINFOHEADER *)pbmi;

    /* Valid call */
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, sizeof(full), FALSE, NULL);
    ok(ret == 8, "valid call returned %d\n", ret);

    /* Zero / tiny lengths */
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, 0, FALSE, NULL);
    ok(ret == 0, "cjMaxInfo == 0 returned %d\n", ret);
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, 4, FALSE, NULL);
    ok(ret == 0, "cjMaxInfo == 4 returned %d\n", ret);
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         0, sizeof(full), FALSE, NULL);
    ok(ret == 0, "cjMaxBits == 0 returned %d\n", ret);

    /* Garbage / inconsistent biSize */
    bih->biSize = 0x7FFFFFF0;
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, sizeof(full), FALSE, NULL);
    ok(ret == 0, "garbage biSize returned %d\n", ret);
    bih->biSize = sizeof(BITMAPINFOHEADER);
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, sizeof(BITMAPINFOHEADER) - 4, FALSE, NULL);
    ok(ret == 0, "short header returned %d\n", ret);

    /* Garbage dimensions */
    bih->biWidth = -8;
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, sizeof(full), FALSE, NULL);
    ok(ret == 0, "negative width returned %d\n", ret);
    bih->biWidth = 8;
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 0x80000000,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, sizeof(full), FALSE, NULL);
    ok(ret == 0, "huge ScanLines returned %d\n", ret);

    /* Bits shorter than the image: must fail without reading past the end */
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits + 56, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         8, sizeof(full), FALSE, NULL);
    ok(ret == 0, "short bits returned %d\n", ret);

    /* Claimed lengths run into the guard page: fail cleanly */
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         0x10000, sizeof(full), FALSE, NULL);
    ok(ret == 0, "bits length beyond mapping returned %d\n", ret);
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, sizeof(full) + 4096, FALSE, NULL);
    ok(ret == 0, "info length beyond mapping returned %d\n", ret);
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, 0xFFFFFFF0, FALSE, NULL);
    ok(ret == 0, "huge cjMaxInfo returned %d\n", ret);

    /* Colour table not provided: nothing may be read behind the header */
    memcpy(pbmi, &full, sizeof(full));
    pbmi = AllocBeforeGuard(sizeof(BITMAPINFOHEADER), &pBaseInfo);
    memcpy(pbmi, &full, sizeof(BITMAPINFOHEADER));
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                         64, sizeof(BITMAPINFOHEADER), FALSE, NULL);
    ok(ret == 8 || ret == 0, "header-only RGB_COLORS returned %d\n", ret);
    ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                         pbits, (LPBITMAPINFO)pbmi, DIB_PAL_COLORS,
                                         64, sizeof(BITMAPINFOHEADER), FALSE, NULL);
    ok(ret == 8 || ret == 0, "header-only PAL_COLORS returned %d\n", ret);

    /* PAL_COLORS with a partial table: only the provided entries are used */
    {
        struct { BITMAPINFOHEADER h; WORD w[4]; } pal;
        pbmi = AllocBeforeGuard(sizeof(pal), &pBaseInfo);
        memcpy(&pal.h, &full.h, sizeof(pal.h));
        for (i = 0; i < 4; i++) pal.w[i] = (WORD)i;
        memcpy(pbmi, &pal, sizeof(pal));
        ret = NtGdiSetDIBitsToDeviceInternal(hdcMem, 0, 0, 8, 8, 0, 0, 0, 8,
                                             pbits, (LPBITMAPINFO)pbmi, DIB_PAL_COLORS,
                                             64, sizeof(pal), FALSE, NULL);
        ok(ret == 8 || ret == 0, "partial PAL_COLORS returned %d\n", ret);
    }

    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    DeleteObject(hbmDst);
    VirtualFree(pBaseInfo, 0, MEM_RELEASE);
    VirtualFree(pBaseBits, 0, MEM_RELEASE);
}


START_TEST(NtGdiSetDIBitsToDeviceInternal)
{
    static const DWORD InBits[8] = { 0x81, 0x7E, 0x5A, 0x7E, 0x7E, 0x42, 0x7E, 0x81 };
    DWORD OutBits[8];
    XFORM xform;

    HWND hWnd = CreateWindowW(L"Static", NULL, WS_VISIBLE,
                              100, 100, 200, 200,
                              NULL, NULL, NULL, NULL);
    /* This DC has an nonzero origin */
    HDC hDC = GetDC(hWnd);
    struct
    {
        BITMAPINFOHEADER bmiHeader;
        RGBQUAD bmiColors[2];
    } bmi;
    int x, y;

    Test_Contract();

    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 8;
    bmi.bmiHeader.biHeight = -8;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 1;
    bmi.bmiHeader.biCompression = 0;
    bmi.bmiHeader.biSizeImage = 0;
    bmi.bmiHeader.biXPelsPerMeter = 0;
    bmi.bmiHeader.biYPelsPerMeter = 0;
    bmi.bmiHeader.biClrUsed = 0;
    bmi.bmiHeader.biClrImportant = 0;
    *(DWORD *)&bmi.bmiColors[0] = 0x000000;
    *(DWORD *)&bmi.bmiColors[1] = 0xFFFFFF;

    /* The destination coordinates are relative to the DC origin */
    TEST(NtGdiSetDIBitsToDeviceInternal(hDC, 2, 3, 8, 8, 0, 0, 0, 8,
                                        (PVOID)InBits, (BITMAPINFO *)&bmi, DIB_RGB_COLORS,
                                        sizeof(InBits), sizeof(bmi), TRUE, NULL));

    /* Now get the data from the screen, and see if it matches */
    ReadBits(hDC, OutBits);

    TEST(memcmp(InBits, OutBits, sizeof(InBits)) == 0);

    /* Change transformation */
    GetWorldTransform(hDC, &xform);
    xform.eM11 = 2;
    xform.eM22 = 2;
    xform.eDx = 10;
    SetWorldTransform(hDC, &xform);

    TEST(NtGdiSetDIBitsToDeviceInternal(hDC, 2, 3, 8, 8, 0, 0, 0, 8,
                                        (PVOID)InBits, (BITMAPINFO *)&bmi, DIB_RGB_COLORS,
                                        sizeof(InBits), sizeof(bmi), TRUE, NULL));

    xform.eM11 = 1;
    xform.eM22 = 1;
    xform.eDx = 0;
    SetWorldTransform(hDC, &xform);

    /* Now get the data from the screen, and see if it matches */
    for (y = 0; y < 8; y++)
    {
        DWORD Row = 0;
        for (x = 0; x < 8; x++)
            Row |= (0x80 & GetPixel(hDC, 2 + x, 3 + y)) >> x;
        OutBits[y] = Row;
    }

    TEST(memcmp(InBits, OutBits, sizeof(InBits)) == 0);


    ReleaseDC(hWnd, hDC);
    DestroyWindow(hWnd);

}
