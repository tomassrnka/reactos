/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Contract test for NtGdiCreateDIBitmapInternal: the BITMAPINFO
 *                  length must be honoured, bad headers are rejected, and the
 *                  kernel must not read past the given length.
 */

#include "../win32nt.h"

/* Returns a buffer of cb bytes that ends exactly at an inaccessible page */
static PBYTE
AllocBeforeGuard(SIZE_T cb, PVOID *ppBase)
{
    DWORD dwOld;
    PBYTE pb = VirtualAlloc(NULL, 2 * 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    BOOL bOk;

    ok(pb != NULL, "VirtualAlloc failed\n");
    bOk = VirtualProtect(pb + 0x1000, 0x1000, PAGE_NOACCESS, &dwOld);
    ok(bOk, "VirtualProtect failed\n");
    *ppBase = pb;
    return pb + 0x1000 - cb;
}

static void
InitHeader(BITMAPINFOHEADER *bih, WORD bpp)
{
    memset(bih, 0, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = 8;
    bih->biHeight = 8;
    bih->biPlanes = 1;
    bih->biBitCount = bpp;
    bih->biCompression = BI_RGB;
}

START_TEST(NtGdiCreateDIBitmapInternal)
{
    HDC hdc = CreateCompatibleDC(NULL);
    HBITMAP hbm;
    PVOID pBase;
    PBYTE pbmi;
    BYTE abBits[64];
    BITMAPINFOHEADER *bih;

    ok(hdc != NULL, "CreateCompatibleDC failed\n");
    memset(abBits, 0x55, sizeof(abBits));

    /* Valid call: full header + colour table */
    {
        struct { BITMAPINFOHEADER h; RGBQUAD c[2]; } bmi;
        InitHeader(&bmi.h, 1);
        bmi.c[0].rgbBlue = bmi.c[0].rgbGreen = bmi.c[0].rgbRed = 0;
        bmi.c[1].rgbBlue = bmi.c[1].rgbGreen = bmi.c[1].rgbRed = 0xFF;
        hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                          (LPBITMAPINFO)&bmi, DIB_RGB_COLORS,
                                          sizeof(bmi), sizeof(abBits), 0, NULL);
        ok(hbm != NULL, "valid call failed\n");
        if (hbm) DeleteObject(hbm);
    }

    /* Header-only buffer ending at a guard page, 1bpp + CBM_INIT: the kernel
       used to look at the colour table behind the header */
    pbmi = AllocBeforeGuard(sizeof(BITMAPINFOHEADER), &pBase);
    InitHeader((BITMAPINFOHEADER *)pbmi, 1);
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      sizeof(BITMAPINFOHEADER), sizeof(abBits), 0, NULL);
    ok(hbm != NULL, "header-only 1bpp call should succeed\n");
    if (hbm) DeleteObject(hbm);

    /* Same for 8bpp: no colour table available, must not be read */
    InitHeader((BITMAPINFOHEADER *)pbmi, 8);
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT | CBM_CREATDIB, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      sizeof(BITMAPINFOHEADER), sizeof(abBits), 0, NULL);
    if (hbm) DeleteObject(hbm);
    ok(1, "survived 8bpp header-only call\n");

    /* Zero / tiny lengths are rejected */
    SetLastError(0);
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      0, sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "cjMaxInitInfo == 0 must fail\n");
    if (hbm) DeleteObject(hbm);

    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      3, sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "cjMaxInitInfo == 3 must fail\n");
    if (hbm) DeleteObject(hbm);

    /* biSize larger than the supplied length */
    InitHeader((BITMAPINFOHEADER *)pbmi, 8);
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      sizeof(BITMAPCOREHEADER), sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "biSize > cjMaxInitInfo must fail\n");
    if (hbm) DeleteObject(hbm);

    /* Garbage biSize values */
    bih = (BITMAPINFOHEADER *)pbmi;
    bih->biSize = 0xFFFFFFF0;
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      sizeof(BITMAPINFOHEADER), sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "garbage biSize must fail\n");
    if (hbm) DeleteObject(hbm);

    bih->biSize = 20;
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      sizeof(BITMAPINFOHEADER), sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "biSize between core and info header must fail\n");
    if (hbm) DeleteObject(hbm);

    /* Claimed length runs into the guard page: must fail cleanly, not crash */
    InitHeader((BITMAPINFOHEADER *)pbmi, 8);
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      sizeof(BITMAPINFOHEADER) + 1024, sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "length beyond the mapped range must fail\n");
    if (hbm) DeleteObject(hbm);

    /* Oversized length is capped, not trusted */
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)pbmi, DIB_RGB_COLORS,
                                      0xFFFFFFF0, sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "huge cjMaxInitInfo must fail\n");
    if (hbm) DeleteObject(hbm);

    /* Invalid pointer */
    hbm = NtGdiCreateDIBitmapInternal(hdc, 8, 8, CBM_INIT, abBits,
                                      (LPBITMAPINFO)(LONG_PTR)0x81000000, DIB_RGB_COLORS,
                                      sizeof(BITMAPINFOHEADER), sizeof(abBits), 0, NULL);
    ok(hbm == NULL, "kernel pointer must fail\n");
    if (hbm) DeleteObject(hbm);

    VirtualFree(pBase, 0, MEM_RELEASE);
    DeleteDC(hdc);
}
