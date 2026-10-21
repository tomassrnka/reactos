/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Contract test for NtGdiStretchDIBitsInternal: info / bits
 *                  lengths are honoured, error paths are clean, valid calls work
 */

#include "../win32nt.h"

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

#define STRETCH(hdc, pbits, pbmi, use, rop, cjinfo, cjbits) \
    NtGdiStretchDIBitsInternal(hdc, 0, 0, 8, 8, 0, 0, 8, 8, pbits, \
                               (LPBITMAPINFO)(pbmi), use, rop, cjinfo, cjbits, NULL)

START_TEST(NtGdiStretchDIBitsInternal)
{
    BITMAPINFO bmiDst;
    HBITMAP hbmDst, hbmOld;
    HDC hdcMem;
    PVOID pvDst, pBaseInfo, pBaseBits, pBaseHdr;
    PBYTE pbmi, pbits, phdr;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } full;
    INT ret, rop;
    DWORD i;

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
        full.c[i].rgbGreen = (BYTE)i;

    pbits = AllocBeforeGuard(64, &pBaseBits);
    memset(pbits, 7, 64);
    pbmi = AllocBeforeGuard(sizeof(full), &pBaseInfo);
    memcpy(pbmi, &full, sizeof(full));

    /* Valid calls, both code paths (SRCCOPY and other rops) */
    ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, SRCCOPY, sizeof(full), 64);
    ok(ret == 8, "valid SRCCOPY returned %d\n", ret);
    ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, SRCINVERT, sizeof(full), 64);
    ok(ret == 8, "valid SRCINVERT returned %d\n", ret);

    /* Too small / garbage info sizes */
    for (rop = 0; rop < 2; rop++)
    {
        DWORD dwRop = rop ? SRCINVERT : SRCCOPY;
        ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, dwRop, 0, 64);
        ok(ret == 0, "cjMaxInfo == 0 returned %d\n", ret);
        ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, dwRop, 8, 64);
        ok(ret == 0, "cjMaxInfo == 8 returned %d\n", ret);
        ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, dwRop, sizeof(full) + 0x2000, 64);
        ok(ret == 0, "cjMaxInfo beyond the mapping returned %d\n", ret);
        ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, dwRop, 0xFFFFFFF0, 64);
        ok(ret == 0, "huge cjMaxInfo returned %d\n", ret);
        ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, dwRop, sizeof(full), 0x10000);
        ok(ret == 0, "cjMaxBits beyond the mapping returned %d\n", ret);
    }

    ((BITMAPINFOHEADER *)pbmi)->biSize = 0x7FFFFFF0;
    ret = STRETCH(hdcMem, pbits, pbmi, DIB_RGB_COLORS, SRCCOPY, sizeof(full), 64);
    ok(ret == 0, "garbage biSize returned %d\n", ret);
    ((BITMAPINFOHEADER *)pbmi)->biSize = sizeof(BITMAPINFOHEADER);

    /* Invalid pointers / handles */
    ret = STRETCH(hdcMem, pbits, (PVOID)(LONG_PTR)0x81000000, DIB_RGB_COLORS, SRCCOPY, sizeof(full), 64);
    ok(ret == 0, "kernel pbmi returned %d\n", ret);
    ret = STRETCH((HDC)0x12345, pbits, pbmi, DIB_RGB_COLORS, SRCCOPY, sizeof(full), 64);
    ok(ret == 0, "invalid hdc (SRCCOPY) returned %d\n", ret);
    ret = STRETCH((HDC)0x12345, pbits, pbmi, DIB_RGB_COLORS, SRCINVERT, sizeof(full), 64);
    ok(ret == 0, "invalid hdc returned %d\n", ret);

    /* Header only: the colour table must not be read (nor uninitialized pool) */
    phdr = AllocBeforeGuard(sizeof(BITMAPINFOHEADER), &pBaseHdr);
    memcpy(phdr, &full, sizeof(BITMAPINFOHEADER));
    for (rop = 0; rop < 2; rop++)
    {
        DWORD dwRop = rop ? SRCINVERT : SRCCOPY;
        ret = STRETCH(hdcMem, pbits, phdr, DIB_RGB_COLORS, dwRop, sizeof(BITMAPINFOHEADER), 64);
        ok(ret == 8 || ret == 0, "header-only RGB_COLORS returned %d\n", ret);
        ret = STRETCH(hdcMem, pbits, phdr, DIB_PAL_COLORS, dwRop, sizeof(BITMAPINFOHEADER), 64);
        ok(ret == 8 || ret == 0, "header-only PAL_COLORS returned %d\n", ret);
    }

    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    DeleteObject(hbmDst);
    VirtualFree(pBaseInfo, 0, MEM_RELEASE);
    VirtualFree(pBaseBits, 0, MEM_RELEASE);
    VirtualFree(pBaseHdr, 0, MEM_RELEASE);
}
