/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Pixel-exact tests for SRCCOPY between 32 bpp DIB sections
 */

#include "precomp.h"

#define DIBW 128
#define DIBH 96
#define RGBMASK 0x00FFFFFF

typedef struct
{
    HDC hdc;
    HBITMAP hbm;
    HGDIOBJ hbmOld;
    ULONG *bits;
    int w, h;
    BOOL topdown;
} DIB;

static ULONG g_seed;

static ULONG Rnd(void)
{
    g_seed = g_seed * 1664525UL + 1013904223UL;
    return g_seed >> 8;
}

static BOOL CreateDib(DIB *d, int w, int h, BOOL topdown)
{
    BITMAPINFO bmi;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = topdown ? -h : h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    d->w = w;
    d->h = h;
    d->topdown = topdown;
    d->hdc = CreateCompatibleDC(NULL);
    d->hbm = CreateDIBSection(d->hdc, &bmi, DIB_RGB_COLORS, (void **)&d->bits, NULL, 0);
    if (!d->hdc || !d->hbm || !d->bits)
        return FALSE;
    d->hbmOld = SelectObject(d->hdc, d->hbm);
    return TRUE;
}

static void DeleteDib(DIB *d)
{
    SelectObject(d->hdc, d->hbmOld);
    DeleteObject(d->hbm);
    DeleteDC(d->hdc);
}

/* x and y are top-left based for both orientations */
static ULONG *Px(DIB *d, int x, int y)
{
    int row = d->topdown ? y : d->h - 1 - y;
    return d->bits + (size_t)row * d->w + x;
}

static void FillDib(DIB *d)
{
    int x, y;

    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++)
            *Px(d, x, y) = Rnd() & RGBMASK;
}

static void Snapshot(DIB *d, ULONG *copy)
{
    int x, y;

    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++)
            copy[y * d->w + x] = *Px(d, x, y) & RGBMASK;
}

static ULONG g_srcCopy[DIBW * DIBH];
static ULONG g_dstCopy[DIBW * DIBH];

/*
 * BitBlt SRCCOPY from src to dst and compare every destination pixel with the model:
 * a pixel changes only inside the destination rectangle, the destination bitmap and the clip region,
 * and takes the value the source had before the call (overlap-safe).
 */
static void CheckBlt(const char *what, DIB *dst, DIB *src, int dx, int dy, int w, int h, int sx, int sy, HRGN hrgnClip)
{
    int x, y, bad = 0, firstx = -1, firsty = -1;
    ULONG got = 0, want = 0;
    BOOL ret;

    Snapshot(src, g_srcCopy);
    Snapshot(dst, g_dstCopy);

    SelectClipRgn(dst->hdc, hrgnClip);
    ret = BitBlt(dst->hdc, dx, dy, w, h, src->hdc, sx, sy, SRCCOPY);
    SelectClipRgn(dst->hdc, NULL);
    GdiFlush();
    ok(ret, "%s: BitBlt failed, error %lu\n", what, GetLastError());

    for (y = 0; y < dst->h; y++)
    {
        for (x = 0; x < dst->w; x++)
        {
            ULONG exp = g_dstCopy[y * dst->w + x];
            ULONG act = *Px(dst, x, y) & RGBMASK;

            if (x >= dx && x < dx + w && y >= dy && y < dy + h &&
                (!hrgnClip || PtInRegion(hrgnClip, x, y)))
            {
                int srcx = sx + x - dx, srcy = sy + y - dy;
                exp = g_srcCopy[srcy * src->w + srcx];
            }

            if (act != exp)
            {
                if (!bad)
                {
                    firstx = x;
                    firsty = y;
                    got = act;
                    want = exp;
                }
                bad++;
            }
        }
    }
    ok(bad == 0, "%s: %d pixel(s) wrong, first at (%d,%d): got %06lx, expected %06lx\n",
       what, bad, firstx, firsty, got, want);
}

static void Test_TwoDibs(BOOL srcTop, BOOL dstTop)
{
    static const int widths[] = { 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 33, 64, 65, 100 };
    static const int heights[] = { 1, 3, 16, 50 };
    DIB src, dst;
    char name[96];
    unsigned i, j;

    ok(CreateDib(&src, DIBW, DIBH, srcTop), "CreateDib\n");
    ok(CreateDib(&dst, DIBW, DIBH, dstTop), "CreateDib\n");
    if (!src.bits || !dst.bits)
        return;
    g_seed = 1234;
    FillDib(&src);
    FillDib(&dst);

    for (i = 0; i < ARRAYSIZE(widths); i++)
    {
        for (j = 0; j < ARRAYSIZE(heights); j++)
        {
            int w = widths[i], h = heights[j];

            sprintf(name, "src %s dst %s %dx%d same origin", srcTop ? "TD" : "BU", dstTop ? "TD" : "BU", w, h);
            CheckBlt(name, &dst, &src, 0, 0, w, h, 0, 0, NULL);
            sprintf(name, "src %s dst %s %dx%d offsets", srcTop ? "TD" : "BU", dstTop ? "TD" : "BU", w, h);
            CheckBlt(name, &dst, &src, 11 + i, 5 + j, w, h, 3 + j, 17 + i, NULL);
            sprintf(name, "src %s dst %s %dx%d odd offsets", srcTop ? "TD" : "BU", dstTop ? "TD" : "BU", w, h);
            CheckBlt(name, &dst, &src, 1, 2, w, h, 2, 1, NULL);
        }
    }

    /* the destination rectangle hangs over the edges of the destination bitmap */
    sprintf(name, "src %s dst %s over right/bottom edge", srcTop ? "TD" : "BU", dstTop ? "TD" : "BU");
    CheckBlt(name, &dst, &src, DIBW - 10, DIBH - 7, 30, 20, 4, 4, NULL);
    sprintf(name, "src %s dst %s over left/top edge", srcTop ? "TD" : "BU", dstTop ? "TD" : "BU");
    CheckBlt(name, &dst, &src, -9, -5, 30, 20, 40, 30, NULL);
    sprintf(name, "src %s dst %s whole bitmap shifted up-left", srcTop ? "TD" : "BU", dstTop ? "TD" : "BU");
    CheckBlt(name, &dst, &src, -4, -3, DIBW, DIBH, 0, 0, NULL);
    sprintf(name, "src %s dst %s whole bitmap shifted down-right", srcTop ? "TD" : "BU", dstTop ? "TD" : "BU");
    CheckBlt(name, &dst, &src, 8, 6, DIBW, DIBH, 0, 0, NULL);

    DeleteDib(&src);
    DeleteDib(&dst);
}

static void Test_Clip(BOOL srcTop, BOOL dstTop)
{
    DIB src, dst;
    HRGN hrgn, hrgn2, hrgn3;
    char name[96];
    const char *o = srcTop ? (dstTop ? "TD->TD" : "TD->BU") : (dstTop ? "BU->TD" : "BU->BU");

    ok(CreateDib(&src, DIBW, DIBH, srcTop), "CreateDib\n");
    ok(CreateDib(&dst, DIBW, DIBH, dstTop), "CreateDib\n");
    if (!src.bits || !dst.bits)
        return;
    g_seed = 99;
    FillDib(&src);
    FillDib(&dst);

    /* single rectangle */
    hrgn = CreateRectRgn(13, 9, 77, 61);
    sprintf(name, "%s rect clip, blit larger", o);
    CheckBlt(name, &dst, &src, 5, 4, 100, 80, 6, 7, hrgn);
    sprintf(name, "%s rect clip, blit inside", o);
    CheckBlt(name, &dst, &src, 20, 20, 30, 20, 0, 0, hrgn);
    sprintf(name, "%s rect clip, odd width", o);
    CheckBlt(name, &dst, &src, 12, 8, 33, 40, 50, 20, hrgn);
    DeleteObject(hrgn);

    /* rectangle with a hole */
    hrgn = CreateRectRgn(4, 4, 120, 90);
    hrgn2 = CreateRectRgn(30, 20, 71, 55);
    CombineRgn(hrgn, hrgn, hrgn2, RGN_DIFF);
    sprintf(name, "%s hole clip", o);
    CheckBlt(name, &dst, &src, 0, 0, DIBW, DIBH, 0, 0, hrgn);
    sprintf(name, "%s hole clip, shifted", o);
    CheckBlt(name, &dst, &src, 9, 3, 100, 85, 11, 2, hrgn);
    DeleteObject(hrgn2);
    DeleteObject(hrgn);

    /* three separate rectangles, one of them one pixel wide */
    hrgn = CreateRectRgn(2, 2, 40, 30);
    hrgn2 = CreateRectRgn(50, 10, 51, 80);
    hrgn3 = CreateRectRgn(60, 40, 127, 95);
    CombineRgn(hrgn, hrgn, hrgn2, RGN_OR);
    CombineRgn(hrgn, hrgn, hrgn3, RGN_OR);
    sprintf(name, "%s three rect clip", o);
    CheckBlt(name, &dst, &src, 1, 1, 126, 94, 0, 0, hrgn);
    sprintf(name, "%s three rect clip, odd offsets", o);
    CheckBlt(name, &dst, &src, 3, 2, 120, 90, 1, 5, hrgn);
    DeleteObject(hrgn3);
    DeleteObject(hrgn2);
    DeleteObject(hrgn);

    DeleteDib(&src);
    DeleteDib(&dst);
}

static void Test_Overlap(BOOL topdown)
{
    static const int shifts[][2] =
    {
        { 0, 0 }, { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 }, { 2, 0 }, { -2, 0 }, { 3, 0 }, { -3, 0 },
        { 16, 0 }, { -16, 0 }, { 5, 7 }, { -5, -7 }, { 5, -7 }, { -5, 7 }, { 0, 16 }, { 0, -16 }, { 1, 1 }, { -1, -1 }
    };
    static const int widths[] = { 1, 2, 31, 60, 101 };
    DIB d;
    HRGN hrgn;
    char name[96];
    unsigned i, j;

    ok(CreateDib(&d, DIBW, DIBH, topdown), "CreateDib\n");
    if (!d.bits)
        return;

    for (i = 0; i < ARRAYSIZE(shifts); i++)
    {
        for (j = 0; j < ARRAYSIZE(widths); j++)
        {
            int w = widths[j], h = 60;
            int sx = 12, sy = 14;

            g_seed = 7 + i;
            FillDib(&d);
            sprintf(name, "%s overlap shift (%d,%d) width %d", topdown ? "TD" : "BU", shifts[i][0], shifts[i][1], w);
            CheckBlt(name, &d, &d, sx + shifts[i][0], sy + shifts[i][1], w, h, sx, sy, NULL);
        }
    }

    /* overlap with a clip region */
    hrgn = CreateRectRgn(20, 20, 90, 70);
    g_seed = 77;
    FillDib(&d);
    CheckBlt("overlap, rect clip, down-right", &d, &d, 25, 22, 90, 70, 10, 8, hrgn);
    g_seed = 78;
    FillDib(&d);
    CheckBlt("overlap, rect clip, up-left", &d, &d, 10, 8, 90, 70, 25, 22, hrgn);
    DeleteObject(hrgn);

    DeleteDib(&d);
}

START_TEST(BitBltCopy32)
{
    BOOL s, t;

    for (s = 0; s < 2; s++)
    {
        for (t = 0; t < 2; t++)
        {
            Test_TwoDibs(s, t);
            Test_Clip(s, t);
        }
        Test_Overlap(s);
    }
}
