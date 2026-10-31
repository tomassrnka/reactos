/*
 * PROJECT:     ReactOS user32.dll
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     CalculatePopupWindowPosition
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <user32.h>

#ifndef TPM_WORKAREA
#define TPM_WORKAREA 0x00010000
#endif

/* TPM_* bits Windows accepts here: everything up to TPM_WORKAREA except 0x200 */
#define CPWP_VALID_FLAGS 0x0001FDFF

static LONG
PopupClampHighWins(LONG Pos, LONG Size, LONG Low, LONG High)
{
    /* the window is pushed in from the left first, then from the right: the right edge wins when it does not fit */
    if (Pos < Low)
        Pos = Low;
    if (Pos + Size > High)
        Pos = High - Size;
    return Pos;
}

static LONG
PopupClampLowWins(LONG Pos, LONG Size, LONG Low, LONG High)
{
    /* the other way round: the left (top) edge wins */
    if (Pos + Size > High)
        Pos = High - Size;
    if (Pos < Low)
        Pos = Low;
    return Pos;
}

static BOOL
PopupFits(LONG Pos, LONG Size, LONG Low, LONG High)
{
    return Pos >= Low && Pos + Size <= High;
}

/*
 * @implemented
 *
 * The anchor point is aligned according to the TPM_*ALIGN flags (centre wins over right,
 * and over bottom: TPM_CENTERALIGN and TPM_VCENTERALIGN are tested first) and the window
 * is moved into the work area of the anchor's monitor. When it then overlaps the exclude
 * rectangle it is placed beside it instead: first along the primary axis (horizontal, or
 * vertical with TPM_VERTICAL) on the requested side, then on the opposite one, then along
 * the other axis; if nothing fits the aligned position stays.
 */
BOOL
WINAPI
CalculatePopupWindowPosition(
    _In_ const POINT *anchorPoint,
    _In_ const SIZE *windowSize,
    _In_ UINT flags,
    _In_opt_ RECT *excludeRect,
    _Out_ RECT *popupWindowPosition)
{
    MONITORINFO mi;
    HMONITOR hMon;
    RECT Area;
    LONG x, y, w, h, cx, cy;
    BOOL HAlignRight, VAlignBottom;

    if (!anchorPoint || !windowSize || !popupWindowPosition)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (flags & ~CPWP_VALID_FLAGS)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }

    w = windowSize->cx;
    h = windowSize->cy;
    if (w < 0 || h < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    hMon = MonitorFromPoint(*anchorPoint, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi);
    if (!hMon || !GetMonitorInfoW(hMon, &mi))
        return FALSE;

    /* the work area when asked for or when the anchor lies in it, else the whole monitor */
    if ((flags & TPM_WORKAREA) || PtInRect(&mi.rcWork, *anchorPoint))
        Area = mi.rcWork;
    else
        Area = mi.rcMonitor;

    x = anchorPoint->x;
    y = anchorPoint->y;
    HAlignRight = FALSE;
    VAlignBottom = FALSE;

    if (flags & TPM_CENTERALIGN)
        x -= w / 2;
    else if (flags & TPM_RIGHTALIGN)
    {
        x -= w;
        HAlignRight = TRUE;
    }

    if (flags & TPM_VCENTERALIGN)
        y -= h / 2;
    else if (flags & TPM_BOTTOMALIGN)
    {
        y -= h;
        VAlignBottom = TRUE;
    }

    cx = HAlignRight ? PopupClampHighWins(x, w, Area.left, Area.right)
                     : PopupClampLowWins(x, w, Area.left, Area.right);
    cy = VAlignBottom ? PopupClampHighWins(y, h, Area.top, Area.bottom)
                      : PopupClampLowWins(y, h, Area.top, Area.bottom);

    if (excludeRect &&
        excludeRect->left < excludeRect->right && excludeRect->top < excludeRect->bottom &&
        w > 0 && h > 0 &&
        cx < excludeRect->right && cx + w > excludeRect->left &&
        cy < excludeRect->bottom && cy + h > excludeRect->top)
    {
        LONG HPos[2], VPos[2], i;
        BOOL Vertical = !!(flags & TPM_VERTICAL);
        BOOL Placed = FALSE;
        int Pass;

        /* requested side first, then the opposite one */
        HPos[0] = HAlignRight ? excludeRect->left - w : excludeRect->right;
        HPos[1] = HAlignRight ? excludeRect->right : excludeRect->left - w;
        VPos[0] = VAlignBottom ? excludeRect->top - h : excludeRect->bottom;
        VPos[1] = VAlignBottom ? excludeRect->bottom : excludeRect->top - h;

        for (Pass = 0; Pass < 2 && !Placed; Pass++)
        {
            BOOL DoVertical = Vertical ? (Pass == 0) : (Pass == 1);

            for (i = 0; i < 2; i++)
            {
                if (DoVertical)
                {
                    if (PopupFits(VPos[i], h, Area.top, Area.bottom))
                    {
                        cy = VPos[i];
                        Placed = TRUE;
                        break;
                    }
                }
                else if (PopupFits(HPos[i], w, Area.left, Area.right))
                {
                    cx = HPos[i];
                    Placed = TRUE;
                    break;
                }
            }
        }
    }

    popupWindowPosition->left = cx;
    popupWindowPosition->top = cy;
    popupWindowPosition->right = cx + w;
    popupWindowPosition->bottom = cy + h;
    return TRUE;
}
