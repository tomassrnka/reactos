/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Destination reads of the generic DIB StretchBlt
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "win32k.h"

#define SRC_W 8
#define SRC_H 8
#define DST_W 4
#define DST_H 4

static ULONG SrcBits[SRC_W * SRC_H];
static ULONG DstBits[DST_W * DST_H];
static SURFOBJ SrcSurf, DstSurf;
static ULONG DstReads, SrcReads;

static ULONG MockGetPixel(SURFOBJ *pso, LONG x, LONG y)
{
    if (pso == &DstSurf)
        DstReads++;
    else if (pso == &SrcSurf)
        SrcReads++;
    return ((ULONG *)((PBYTE)pso->pvScan0 + y * pso->lDelta))[x];
}

static VOID MockPutPixel(SURFOBJ *pso, LONG x, LONG y, ULONG c)
{
    ((ULONG *)((PBYTE)pso->pvScan0 + y * pso->lDelta))[x] = c;
}

DIB_FUNCTIONS DibFunctionsForBitmapFormat[BMF_8RLE + 1] =
{
    [BMF_32BPP] = { MockPutPixel, MockGetPixel },
};

UCHAR gajBitsPerFormat[BMF_8RLE + 1] = { 0, 1, 4, 8, 16, 24, 32, 4, 8 };

/* Bitwise ROP3: bit (P << 2 | S << 1 | D) of the index is the result. */
ULONG DIB_DoRop(ULONG Rop, ULONG Dest, ULONG Source, ULONG Pattern)
{
    ULONG Result = 0, i, Index;

    for (i = 0; i < 32; i++)
    {
        Index = ((Pattern >> i) & 1) << 2 | ((Source >> i) & 1) << 1 | ((Dest >> i) & 1);
        Result |= ((Rop >> Index) & 1) << i;
    }
    return Result;
}

VOID FASTCALL RECTL_vMakeWellOrdered(RECTL *prcl)
{
    LONG t;

    if (prcl->left > prcl->right) { t = prcl->left; prcl->left = prcl->right; prcl->right = t; }
    if (prcl->top > prcl->bottom) { t = prcl->top; prcl->top = prcl->bottom; prcl->bottom = t; }
}

static void init_surface(SURFOBJ *pso, ULONG *bits, LONG cx, LONG cy)
{
    memset(pso, 0, sizeof(*pso));
    pso->sizlBitmap.cx = cx;
    pso->sizlBitmap.cy = cy;
    pso->pvBits = pso->pvScan0 = bits;
    pso->lDelta = cx * sizeof(ULONG);
    pso->iBitmapFormat = BMF_32BPP;
}

static void stretch(ROP4 Rop)
{
    RECTL DstRect = { 0, 0, DST_W, DST_H }, SrcRect = { 0, 0, SRC_W, SRC_H };
    POINTL Origin = { 0, 0 };
    LONG i;

    for (i = 0; i < SRC_W * SRC_H; i++)
        SrcBits[i] = 0x00010101 * i;
    for (i = 0; i < DST_W * DST_H; i++)
        DstBits[i] = 0x00F0F0F0;
    init_surface(&SrcSurf, SrcBits, SRC_W, SRC_H);
    init_surface(&DstSurf, DstBits, DST_W, DST_H);
    DstReads = SrcReads = 0;
    ok(DIB_XXBPP_StretchBlt(&DstSurf, &SrcSurf, NULL, NULL, &DstRect, &SrcRect,
                            &Origin, NULL, &Origin, NULL, Rop),
       "DIB_XXBPP_StretchBlt failed for ROP4 0x%lx\n", Rop);
}

START_TEST(StretchBlt)
{
    LONG x, y;
    ULONG Expected;
    BOOL Same = TRUE;

    /* SRCCOPY does not depend on the destination: reading video memory per pixel made it slow */
    stretch(0xCCCC);
    ok(DstReads == 0, "SRCCOPY read the destination %lu times\n", DstReads);
    ok(SrcReads == DST_W * DST_H, "SRCCOPY read the source %lu times\n", SrcReads);
    for (y = 0; y < DST_H; y++)
        for (x = 0; x < DST_W; x++)
            Same = Same && DstBits[y * DST_W + x] == SrcBits[(y * 2) * SRC_W + x * 2];
    ok(Same, "SRCCOPY result differs\n");

    /* SRCINVERT needs every destination pixel */
    stretch(0x6666);
    ok(DstReads == DST_W * DST_H, "SRCINVERT read the destination %lu times\n", DstReads);
    Same = TRUE;
    for (y = 0; y < DST_H; y++)
        for (x = 0; x < DST_W; x++)
        {
            Expected = 0x00F0F0F0 ^ SrcBits[(y * 2) * SRC_W + x * 2];
            Same = Same && DstBits[y * DST_W + x] == Expected;
        }
    ok(Same, "SRCINVERT result differs\n");

    /* PATCOPY with a solid brush uses neither source nor destination */
    {
        RECTL DstRect = { 0, 0, DST_W, DST_H }, SrcRect = { 0, 0, SRC_W, SRC_H };
        POINTL Origin = { 0, 0 };
        BRUSHOBJ Brush;

        memset(&Brush, 0, sizeof(Brush));
        Brush.iSolidColor = 0x00123456;
        init_surface(&DstSurf, DstBits, DST_W, DST_H);
        DstReads = SrcReads = 0;
        ok(DIB_XXBPP_StretchBlt(&DstSurf, &SrcSurf, NULL, NULL, &DstRect, &SrcRect,
                                &Origin, &Brush, &Origin, NULL, 0xF0F0),
           "DIB_XXBPP_StretchBlt failed for PATCOPY\n");
        ok(DstReads == 0 && SrcReads == 0, "PATCOPY read the destination %lu and the source %lu times\n",
           DstReads, SrcReads);
        ok(DstBits[0] == 0x00123456 && DstBits[DST_W * DST_H - 1] == 0x00123456,
           "PATCOPY result 0x%lx 0x%lx\n", DstBits[0], DstBits[DST_W * DST_H - 1]);
    }
}
