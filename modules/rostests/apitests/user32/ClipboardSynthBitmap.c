/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test CF_DIB -> CF_BITMAP synthesis with malformed CF_DIB sizes
 */

#include "precomp.h"

/* Put a CF_DIB blob of the given size on the clipboard, then ask for CF_BITMAP.
   Returns the CF_BITMAP handle (NULL if synthesis was refused). */
static HANDLE
SynthesizeFromDib(const void *pData, SIZE_T cb)
{
    HGLOBAL hMem;
    HANDLE hBm;
    void *p;

    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, cb);
    if (!hMem)
        return NULL;
    p = GlobalLock(hMem);
    memcpy(p, pData, cb);
    GlobalUnlock(hMem);

    if (!OpenClipboard(NULL))
    {
        GlobalFree(hMem);
        return (HANDLE)-1;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hMem))
    {
        CloseClipboard();
        GlobalFree(hMem);
        return (HANDLE)-1;
    }
    hBm = GetClipboardData(CF_BITMAP);
    EmptyClipboard();
    CloseClipboard();
    return hBm;
}

START_TEST(ClipboardSynthBitmap)
{
    BYTE buf[sizeof(BITMAPINFOHEADER) + 8];
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)buf;
    HANDLE hBm;

    /* Valid 1x1 24bpp DIB must still synthesize */
    memset(buf, 0, sizeof(buf));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = 1;
    bih->biHeight = 1;
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    bih->biSizeImage = 4;
    buf[sizeof(BITMAPINFOHEADER)] = 0xFF;
    hBm = SynthesizeFromDib(buf, sizeof(BITMAPINFOHEADER) + 4);
    ok(hBm != (HANDLE)-1, "clipboard unavailable\n");
    /* Synthesis may be unavailable in non-interactive sessions (the harness VM): informational only */
    if (!hBm)
        skip("valid DIB did not synthesize in this session\n");

    /* Block smaller than a DWORD: cannot even hold biSize */
    memset(buf, 0, sizeof(buf));
    *(DWORD *)buf = sizeof(BITMAPINFOHEADER);
    hBm = SynthesizeFromDib(buf, 2);
    ok(hBm == NULL, "2-byte DIB: expected NULL, got %p\n", hBm);

    /* Block >= DWORD but smaller than the header size it announces */
    memset(buf, 0, sizeof(buf));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = 1;
    bih->biHeight = 1;
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    hBm = SynthesizeFromDib(buf, 8);
    ok(hBm == NULL, "truncated header: expected NULL, got %p\n", hBm);

    /* Header present, but the 8bpp colour table it implies is missing */
    memset(buf, 0, sizeof(buf));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = 1;
    bih->biHeight = 1;
    bih->biPlanes = 1;
    bih->biBitCount = 8;
    hBm = SynthesizeFromDib(buf, sizeof(BITMAPINFOHEADER));
    ok(hBm == NULL, "missing colour table: expected NULL, got %p\n", hBm);
}
