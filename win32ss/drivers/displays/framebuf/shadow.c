/*
 * PROJECT:     ReactOS Generic Framebuffer display driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Drawing through a system memory shadow of the frame buffer
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * The screen surface given to GDI is device managed and has no bits. GDI
 * draws on it by calling the functions below (or, for anything that is not
 * hooked, by reading and writing it through DrvCopyBits). Each of them lets
 * the engine draw on a bitmap in system memory that holds the screen
 * contents, then copies the rectangles it changed to the frame buffer.
 *
 * The frame buffer is mapped write-combined, which makes reading it very
 * slow. GDI reads the screen for every screen to screen blit (moving or
 * scrolling a window), blend and XOR, so it reads the cached copy instead,
 * and the frame buffer is only ever written, line by line.
 *
 * StretchBlt is not hooked: win32k gives DrvStretchBltROP the target
 * rectangle well-ordered, without the mirroring, which only its own
 * EngStretchBltROP fallback applies. Through DrvCopyBits it is right.
 */

#include "framebuf.h"

typedef struct _FB_ENUMRECTS
{
    ULONG c;
    RECTL arcl[16];
} FB_ENUMRECTS;

/* The PDEVs of this driver that have a screen surface. A device managed
 * surface of another driver can reach us in a blit between two displays,
 * and its dhsurf means nothing to this driver */
#define FB_MAX_SCREENS 16
static PPDEV gapdevScreens[FB_MAX_SCREENS];

BOOL
FbRegisterScreen(
    _In_ PPDEV ppdev)
{
    ULONG i;

    for (i = 0; i < FB_MAX_SCREENS; i++)
    {
        if (InterlockedCompareExchangePointer((PVOID *)&gapdevScreens[i],
                                              ppdev, NULL) == NULL)
        {
            return TRUE;
        }
    }

    return FALSE;
}

VOID
FbUnregisterScreen(
    _In_ PPDEV ppdev)
{
    ULONG i;

    for (i = 0; i < FB_MAX_SCREENS; i++)
    {
        InterlockedCompareExchangePointer((PVOID *)&gapdevScreens[i],
                                          NULL, ppdev);
    }
}

/* A device managed surface of this driver: the screen of one of its PDEVs */
static
BOOL
FbIsScreen(
    _In_ SURFOBJ *pso)
{
    ULONG i;

    if (pso->iType != STYPE_DEVICE || pso->dhsurf == NULL)
        return FALSE;

    for (i = 0; i < FB_MAX_SCREENS; i++)
    {
        if ((DHSURF)gapdevScreens[i] == pso->dhsurf)
            return TRUE;
    }

    return FALSE;
}

/* The surface the engine draws on instead of pso: the shadow of a screen,
 * pso itself for a bitmap, NULL for another driver's device surface */
static
SURFOBJ *
FbShadow(
    _In_ SURFOBJ *pso)
{
    if (pso->iType == STYPE_BITMAP)
        return pso;

    if (FbIsScreen(pso))
        return ((PPDEV)pso->dhsurf)->psoShadow;

    return NULL;
}

static
VOID
FbCopyRect(
    _In_ PPDEV ppdev,
    _In_ const RECTL *prcl)
{
    SURFOBJ *psoShadow = ppdev->psoShadow;
    ULONG cjLine = (prcl->right - prcl->left) * ppdev->BytesPerPixel;
    LONG cLines = prcl->bottom - prcl->top;
    PBYTE pjSrc, pjDst;

    pjSrc = (PBYTE)psoShadow->pvScan0 + prcl->top * psoShadow->lDelta +
            prcl->left * ppdev->BytesPerPixel;
    pjDst = (PBYTE)ppdev->ScreenPtr + prcl->top * ppdev->ScreenDelta +
            prcl->left * ppdev->BytesPerPixel;

    while (cLines-- > 0)
    {
        RtlCopyMemory(pjDst, pjSrc, cjLine);
        pjSrc += psoShadow->lDelta;
        pjDst += ppdev->ScreenDelta;
    }
}

/*
 * Copy the part of the shadow that an operation may have changed to the
 * frame buffer: prcl (any orientation, NULL for the whole screen) within the
 * clip region. Called whatever the engine returned, as it may have drawn part
 * of an operation before failing.
 */
static
VOID
FbUpdateScreen(
    _In_ SURFOBJ *pso,
    _In_opt_ const RECTL *prcl,
    _In_opt_ CLIPOBJ *pco)
{
    PPDEV ppdev;
    RECTL rclBounds, rcl;
    FB_ENUMRECTS EnumRects;
    BOOL bMore;
    ULONG i;

    /* Only drawing on the screen needs to reach the frame buffer */
    if (!FbIsScreen(pso))
        return;

    ppdev = (PPDEV)pso->dhsurf;

    /* While the display is disabled, the shadow is copied when it comes back */
    if (!ppdev->bScreenEnabled)
        return;

    rclBounds.left = 0;
    rclBounds.top = 0;
    rclBounds.right = ppdev->ScreenWidth;
    rclBounds.bottom = ppdev->ScreenHeight;

    if (prcl != NULL)
    {
        rclBounds.left = max(rclBounds.left, min(prcl->left, prcl->right));
        rclBounds.top = max(rclBounds.top, min(prcl->top, prcl->bottom));
        rclBounds.right = min(rclBounds.right, max(prcl->left, prcl->right));
        rclBounds.bottom = min(rclBounds.bottom, max(prcl->top, prcl->bottom));
    }

    if (pco != NULL && pco->iDComplexity != DC_TRIVIAL)
    {
        rclBounds.left = max(rclBounds.left, pco->rclBounds.left);
        rclBounds.top = max(rclBounds.top, pco->rclBounds.top);
        rclBounds.right = min(rclBounds.right, pco->rclBounds.right);
        rclBounds.bottom = min(rclBounds.bottom, pco->rclBounds.bottom);
    }

    if (rclBounds.left >= rclBounds.right || rclBounds.top >= rclBounds.bottom)
        return;

    if (pco == NULL || pco->iDComplexity != DC_COMPLEX)
    {
        FbCopyRect(ppdev, &rclBounds);
        return;
    }

    /* Only copy the visible parts, a window partly covered by others has a
     * clip region much smaller than its bounds */
    CLIPOBJ_cEnumStart(pco, FALSE, CT_RECTANGLES, CD_ANY, 0);
    do
    {
        bMore = CLIPOBJ_bEnum(pco, sizeof(EnumRects), (PULONG)&EnumRects);
        for (i = 0; i < EnumRects.c; i++)
        {
            rcl.left = max(rclBounds.left, EnumRects.arcl[i].left);
            rcl.top = max(rclBounds.top, EnumRects.arcl[i].top);
            rcl.right = min(rclBounds.right, EnumRects.arcl[i].right);
            rcl.bottom = min(rclBounds.bottom, EnumRects.arcl[i].bottom);
            if (rcl.left < rcl.right && rcl.top < rcl.bottom)
                FbCopyRect(ppdev, &rcl);
        }
    } while (bMore);
}

/* Copy the whole shadow to the frame buffer */
VOID
FbUpdateWholeScreen(
    _In_ PPDEV ppdev)
{
    RECTL rcl;

    rcl.left = 0;
    rcl.top = 0;
    rcl.right = ppdev->ScreenWidth;
    rcl.bottom = ppdev->ScreenHeight;
    FbCopyRect(ppdev, &rcl);
}

BOOL APIENTRY
DrvBitBlt(
    _Inout_ SURFOBJ *psoTrg,
    _In_opt_ SURFOBJ *psoSrc,
    _In_opt_ SURFOBJ *psoMask,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ RECTL *prclTrg,
    _In_opt_ POINTL *pptlSrc,
    _In_opt_ POINTL *pptlMask,
    _In_opt_ BRUSHOBJ *pbo,
    _In_opt_ POINTL *pptlBrush,
    _In_ ROP4 rop4)
{
    SURFOBJ *psoShadowDst, *psoShadowSrc = NULL;
    BOOL bRet;

    psoShadowDst = FbShadow(psoTrg);
    if (psoSrc != NULL)
        psoShadowSrc = FbShadow(psoSrc);
    if (psoShadowDst == NULL || (psoSrc != NULL && psoShadowSrc == NULL))
        return FALSE;

    bRet = EngBitBlt(psoShadowDst, psoShadowSrc, psoMask, pco, pxlo,
                     prclTrg, pptlSrc, pptlMask, pbo, pptlBrush, rop4);
    FbUpdateScreen(psoTrg, prclTrg, pco);

    return bRet;
}

BOOL APIENTRY
DrvCopyBits(
    _In_ SURFOBJ *psoDest,
    _In_ SURFOBJ *psoSrc,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ RECTL *prclDest,
    _In_ POINTL *pptlSrc)
{
    SURFOBJ *psoShadowDst, *psoShadowSrc = NULL;
    BOOL bRet;

    psoShadowDst = FbShadow(psoDest);
    if (psoSrc != NULL)
        psoShadowSrc = FbShadow(psoSrc);
    if (psoShadowDst == NULL || (psoSrc != NULL && psoShadowSrc == NULL))
        return FALSE;

    bRet = EngCopyBits(psoShadowDst, psoShadowSrc, pco, pxlo,
                       prclDest, pptlSrc);
    FbUpdateScreen(psoDest, prclDest, pco);

    return bRet;
}

BOOL APIENTRY
DrvLineTo(
    _Inout_ SURFOBJ *pso,
    _In_ CLIPOBJ *pco,
    _In_ BRUSHOBJ *pbo,
    _In_ LONG x1,
    _In_ LONG y1,
    _In_ LONG x2,
    _In_ LONG y2,
    _In_opt_ RECTL *prclBounds,
    _In_ MIX mix)
{
    RECTL rcl;
    SURFOBJ *psoShadowDst;
    BOOL bRet;

    psoShadowDst = FbShadow(pso);
    if (psoShadowDst == NULL)
        return FALSE;

    bRet = EngLineTo(psoShadowDst, pco, pbo, x1, y1, x2, y2, prclBounds, mix);
    {
        /* Both end points included, whichever way the line is drawn */
        rcl.left = min(x1, x2);
        rcl.top = min(y1, y2);
        rcl.right = max(x1, x2) + 1;
        rcl.bottom = max(y1, y2) + 1;
        FbUpdateScreen(pso, &rcl, pco);
    }

    return bRet;
}

BOOL APIENTRY
DrvAlphaBlend(
    _Inout_ SURFOBJ *psoDest,
    _In_ SURFOBJ *psoSrc,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ RECTL *prclDest,
    _In_ RECTL *prclSrc,
    _In_ BLENDOBJ *pBlendObj)
{
    SURFOBJ *psoShadowDst, *psoShadowSrc = NULL;
    BOOL bRet;

    psoShadowDst = FbShadow(psoDest);
    if (psoSrc != NULL)
        psoShadowSrc = FbShadow(psoSrc);
    if (psoShadowDst == NULL || (psoSrc != NULL && psoShadowSrc == NULL))
        return FALSE;

    bRet = EngAlphaBlend(psoShadowDst, psoShadowSrc, pco, pxlo,
                         prclDest, prclSrc, pBlendObj);
    FbUpdateScreen(psoDest, prclDest, pco);

    return bRet;
}

BOOL APIENTRY
DrvTransparentBlt(
    _Inout_ SURFOBJ *psoDst,
    _In_ SURFOBJ *psoSrc,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ RECTL *prclDst,
    _In_ RECTL *prclSrc,
    _In_ ULONG iTransColor,
    _In_ ULONG ulReserved)
{
    SURFOBJ *psoShadowDst, *psoShadowSrc = NULL;
    BOOL bRet;

    psoShadowDst = FbShadow(psoDst);
    if (psoSrc != NULL)
        psoShadowSrc = FbShadow(psoSrc);
    if (psoShadowDst == NULL || (psoSrc != NULL && psoShadowSrc == NULL))
        return FALSE;

    bRet = EngTransparentBlt(psoShadowDst, psoShadowSrc, pco, pxlo,
                             prclDst, prclSrc, iTransColor, ulReserved);
    FbUpdateScreen(psoDst, prclDst, pco);

    return bRet;
}

BOOL APIENTRY
DrvGradientFill(
    _Inout_ SURFOBJ *psoDest,
    _In_ CLIPOBJ *pco,
    _In_opt_ XLATEOBJ *pxlo,
    _In_ TRIVERTEX *pVertex,
    _In_ ULONG nVertex,
    _In_ PVOID pMesh,
    _In_ ULONG nMesh,
    _In_ RECTL *prclExtents,
    _In_ POINTL *pptlDitherOrg,
    _In_ ULONG ulMode)
{
    SURFOBJ *psoShadowDst;
    RECTL rcl;
    ULONG i;
    BOOL bRet;

    psoShadowDst = FbShadow(psoDest);
    if (psoShadowDst == NULL)
        return FALSE;

    bRet = EngGradientFill(psoShadowDst, pco, pxlo, pVertex, nVertex,
                           pMesh, nMesh, prclExtents, pptlDitherOrg, ulMode);

    /* Everything drawn is within the bounds of the vertices, which for
     * triangles EngGradientFill moves by the dither origin (the DC origin);
     * a pixel more on each side covers the rounding of the edges */
    if (nVertex != 0)
    {
        rcl.left = rcl.right = pVertex[0].x;
        rcl.top = rcl.bottom = pVertex[0].y;
        for (i = 1; i < nVertex; i++)
        {
            rcl.left = min(rcl.left, pVertex[i].x);
            rcl.top = min(rcl.top, pVertex[i].y);
            rcl.right = max(rcl.right, pVertex[i].x);
            rcl.bottom = max(rcl.bottom, pVertex[i].y);
        }
        if (ulMode == GRADIENT_FILL_TRIANGLE && pptlDitherOrg != NULL)
        {
            rcl.left += pptlDitherOrg->x;
            rcl.right += pptlDitherOrg->x;
            rcl.top += pptlDitherOrg->y;
            rcl.bottom += pptlDitherOrg->y;
        }
        rcl.left--;
        rcl.top--;
        rcl.right += 2;
        rcl.bottom += 2;
        FbUpdateScreen(psoDest, &rcl, pco);
    }

    return bRet;
}
