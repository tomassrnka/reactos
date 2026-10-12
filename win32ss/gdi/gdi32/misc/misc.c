/*
 *  ReactOS GDI lib
 *  Copyright (C) 2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/*
 * PROJECT:         ReactOS gdi32.dll
 * FILE:            win32ss/gdi/gdi32/misc/misc.c
 * PURPOSE:         Miscellaneous functions
 * PROGRAMMER:      Thomas Weidenmueller <w3seek@reactos.com>
 * UPDATE HISTORY:
 *      2004/09/04  Created
 */

#include <precomp.h>

#define NDEBUG
#include <debug.h>

PGDI_TABLE_ENTRY GdiHandleTable = NULL;
PGDI_SHARED_HANDLE_TABLE GdiSharedHandleTable = NULL;
HANDLE CurrentProcessId = NULL;
DWORD GDI_BatchLimit = 1;
extern PGDIHANDLECACHE GdiHandleCache;

/*
 * @implemented
 */
BOOL
WINAPI
GdiFlush(VOID)
{
    NtGdiFlush();
    return TRUE;
}

/*
 * @unimplemented
 */
INT
WINAPI
Escape(
    _In_ HDC hdc,
    _In_ INT nEscape,
    _In_ INT cbInput,
    _In_ LPCSTR lpvInData,
    _Out_ LPVOID lpvOutData)
{
    INT retValue = SP_ERROR;
    ULONG ulObjType;

    ulObjType = GDI_HANDLE_GET_TYPE(hdc);

    if (ulObjType == GDILoObjType_LO_METADC16_TYPE)
    {
        return METADC_ExtEscape(hdc, nEscape, cbInput, lpvInData, 0, lpvOutData);
    }

    switch (nEscape)
    {
        case ABORTDOC:
            /* Note: Windows checks if the handle has any user data for the ABORTDOC command
             * ReactOS copies this behavior to be compatible with windows 2003
             */
            if (GdiGetDcAttr(hdc) == NULL)
            {
                GdiSetLastError(ERROR_INVALID_HANDLE);
                retValue = FALSE;
            }
            else
            {
                retValue = AbortDoc(hdc);
            }
            break;

        case DRAFTMODE:
        case FLUSHOUTPUT:
        case SETCOLORTABLE:
            /* Note 1: DRAFTMODE, FLUSHOUTPUT, SETCOLORTABLE are outdated */
            /* Note 2: Windows checks if the handle has any user data for the DRAFTMODE, FLUSHOUTPUT, SETCOLORTABLE commands
             * ReactOS copies this behavior to be compatible with windows 2003
             */
            if (GdiGetDcAttr(hdc) == NULL)
            {
                GdiSetLastError(ERROR_INVALID_HANDLE);
            }
            retValue = FALSE;
            break;

        case SETABORTPROC:
            /* Note: Windows checks if the handle has any user data for the SETABORTPROC command
             * ReactOS copies this behavior to be compatible with windows 2003
             */
            if (GdiGetDcAttr(hdc) == NULL)
            {
                GdiSetLastError(ERROR_INVALID_HANDLE);
                retValue = FALSE;
            }
            retValue = SetAbortProc(hdc, (ABORTPROC)lpvInData);
            break;

        case GETCOLORTABLE:
            retValue = GetSystemPaletteEntries(hdc, (UINT)*lpvInData, 1, (LPPALETTEENTRY)lpvOutData);
            if (!retValue)
            {
                retValue = SP_ERROR;
            }
            break;

        case ENDDOC:
            /* Note: Windows checks if the handle has any user data for the ENDDOC command
             * ReactOS copies this behavior to be compatible with windows 2003
             */
            if (GdiGetDcAttr(hdc) == NULL)
            {
                GdiSetLastError(ERROR_INVALID_HANDLE);
                retValue = FALSE;
            }
            retValue = EndDoc(hdc);
            break;

        case GETSCALINGFACTOR:
            /* Note GETSCALINGFACTOR is outdated have been replace by GetDeviceCaps */
            if (ulObjType == GDI_OBJECT_TYPE_DC)
            {
                if (lpvOutData)
                {
                    PPOINT ptr = (PPOINT)lpvOutData;
                    ptr->x = 0;
                    ptr->y = 0;
                }
            }
            retValue = FALSE;
            break;

        case GETEXTENDEDTEXTMETRICS:
            retValue = GetETM(hdc, (EXTTEXTMETRIC *)lpvOutData) != 0;
            break;

        case STARTDOC:
        {
            DOCINFOA di;

            /* Note: Windows checks if the handle has any user data for the STARTDOC command
             * ReactOS copies this behavior to be compatible with windows 2003
             */
            if (GdiGetDcAttr(hdc) == NULL)
            {
                GdiSetLastError(ERROR_INVALID_HANDLE);
                retValue = FALSE;
            }

            di.cbSize = sizeof(DOCINFOA);
            di.lpszOutput = 0;
            di.lpszDatatype = 0;
            di.fwType = 0;
            di.lpszDocName = lpvInData;

            /* NOTE : doc for StartDocA/W at msdn https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-startdoca */
            retValue = StartDocA(hdc, &di);

            /* Check if StartDocA failed */
            if (retValue < 0)
            {
                {
                    retValue = GetLastError();

                    /* Translate StartDocA error code to STARTDOC error code
                     * see msdn https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-escape
                     */
                    switch(retValue)
                    {
                    case ERROR_NOT_ENOUGH_MEMORY:
                        retValue = SP_OUTOFMEMORY;
                        break;

                    case ERROR_PRINT_CANCELLED:
                        retValue = SP_USERABORT;
                        break;

                    case ERROR_DISK_FULL:
                        retValue = SP_OUTOFDISK;
                        break;

                    default:
                        retValue = SP_ERROR;
                        break;
                    }
                }
            }
        }
        break;

        default:
            UNIMPLEMENTED;
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    }

    return retValue;
}

INT
WINAPI
ExtEscape(HDC hDC,
          int nEscape,
          int cbInput,
          LPCSTR lpszInData,
          int cbOutput,
          LPSTR lpszOutData)
{
    return NtGdiExtEscape(hDC, NULL, 0, nEscape, cbInput, (LPSTR)lpszInData, cbOutput, lpszOutData);
}

INT
WINAPI
NamedEscape(HDC hdc,
            PWCHAR pDriver,
            INT iEsc,
            INT cjIn,
            LPSTR pjIn,
            INT cjOut,
            LPSTR pjOut)
{
    /* FIXME metadc, metadc are done most in user mode, and we do not support it
     * Windows 2000/XP/Vista ignore the current hdc, that are being pass and always set hdc to NULL
     * when it calls to NtGdiExtEscape from NamedEscape
     */
    return NtGdiExtEscape(NULL,pDriver,wcslen(pDriver),iEsc,cjIn,pjIn,cjOut,pjOut);
}

/*
 * @implemented
 */
int
WINAPI
DrawEscape(HDC  hDC,
           INT nEscape,
           INT cbInput,
           LPCSTR lpszInData)
{
    if (GDI_HANDLE_GET_TYPE(hDC) == GDI_OBJECT_TYPE_DC)
        return NtGdiDrawEscape(hDC, nEscape, cbInput, (LPSTR) lpszInData);

    if (GDI_HANDLE_GET_TYPE(hDC) != GDI_OBJECT_TYPE_METADC)
    {
        PLDC pLDC = GdiGetLDC(hDC);
        if ( pLDC )
        {
            if (pLDC->Flags & LDC_META_PRINT)
            {
//           if (nEscape != QUERYESCSUPPORT)
//              return EMFDRV_WriteEscape(hDC, nEscape, cbInput, lpszInData, EMR_DRAWESCAPE);

                return NtGdiDrawEscape(hDC, nEscape, cbInput, (LPSTR) lpszInData);
            }
        }
        SetLastError(ERROR_INVALID_HANDLE);
    }
    return 0;
}

#define ALPHABLEND_NONE             0
#define ALPHABLEND_BINARY           1
#define ALPHABLEND_FULL             2

typedef struct _MARGINS {
    int cxLeftWidth;
    int cxRightWidth;
    int cyTopHeight;
    int cyBottomHeight;
} MARGINS, *PMARGINS;

enum SIZINGTYPE {
    ST_TRUESIZE = 0,
    ST_STRETCH = 1,
    ST_TILE = 2,
};

#define TransparentBlt GdiTransparentBlt
#define AlphaBlend GdiAlphaBlend

/***********************************************************************
 *      UXTHEME_StretchBlt
 *
 * Pseudo TransparentBlt/StretchBlt
 */
static BOOL UXTHEME_DoBlt(HDC hdcDst, int nXOriginDst, int nYOriginDst, int nWidthDst, int nHeightDst,
                          HDC hdcSrc, int nXOriginSrc, int nYOriginSrc, int nWidthSrc, int nHeightSrc,
                          INT transparent, COLORREF transcolor)
{
    static const BLENDFUNCTION blendFunc =
    {
      AC_SRC_OVER, /* BlendOp */
      0,           /* BlendFlag */
      255,         /* SourceConstantAlpha */
      AC_SRC_ALPHA /* AlphaFormat */
    };

    BOOL ret = TRUE;
    int old_stretch_mode;
    POINT old_brush_org;

    old_stretch_mode = SetStretchBltMode(hdcDst, HALFTONE);
    SetBrushOrgEx(hdcDst, nXOriginDst, nYOriginDst, &old_brush_org);

    if (transparent == ALPHABLEND_BINARY) {
        /* Ensure we don't pass any negative values to TransparentBlt */
        ret = TransparentBlt(hdcDst, nXOriginDst, nYOriginDst, abs(nWidthDst), abs(nHeightDst),
                              hdcSrc, nXOriginSrc, nYOriginSrc, abs(nWidthSrc), abs(nHeightSrc),
                              transcolor);
    } else if ((transparent == ALPHABLEND_NONE) ||
        !AlphaBlend(hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                    hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc,
                    blendFunc))
    {
        ret = StretchBlt(hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                          hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc,
                          SRCCOPY);
    }

    SetBrushOrgEx(hdcDst, old_brush_org.x, old_brush_org.y, NULL);
    SetStretchBltMode(hdcDst, old_stretch_mode);

    return ret;
}

/* The source pixel Windows' theme drawing takes for destination pixel d when s pixels become n:
   the one under the centre of d, on a tie the right one across and the upper one down */
static inline int UXTHEME_SourceColumn(int d, int s, int n)
{
    return (int)(((2LL * d + 1) * s) / (2LL * n));
}

static inline int UXTHEME_SourceRow(int d, int s, int n)
{
    return (int)(((2LL * d + 1) * s - 1) / (2LL * n));
}

/* The colour key as a pixel of the source's format reads once expanded to 32 bpp */
static COLORREF UXTHEME_ExpandedKey(HDC hdcSrc, COLORREF transcolor)
{
    BITMAPINFO bmi;
    HDC hdcKey = CreateCompatibleDC(hdcSrc), hdc32 = CreateCompatibleDC(hdcSrc);
    HBITMAP hbmKey = CreateCompatibleBitmap(hdcSrc, 1, 1), hbm32;
    HGDIOBJ hbmOldKey = NULL, hbmOld32 = NULL;
    PDWORD pdw;
    COLORREF ret = transcolor;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 1;
    bmi.bmiHeader.biHeight = 1;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hbm32 = CreateDIBSection(hdcSrc, &bmi, DIB_RGB_COLORS, (PVOID *)&pdw, NULL, 0);
    if (hdcKey && hdc32 && hbmKey && hbm32)
    {
        hbmOldKey = SelectObject(hdcKey, hbmKey);
        hbmOld32 = SelectObject(hdc32, hbm32);
        SetPixelV(hdcKey, 0, 0, transcolor);
        if (BitBlt(hdc32, 0, 0, 1, 1, hdcKey, 0, 0, SRCCOPY))
        {
            GdiFlush();
            ret = RGB((*pdw >> 16) & 0xFF, (*pdw >> 8) & 0xFF, *pdw & 0xFF);
        }
    }
    if (hbmOldKey) SelectObject(hdcKey, hbmOldKey);
    if (hbmOld32) SelectObject(hdc32, hbmOld32);
    if (hbmKey) DeleteObject(hbmKey);
    if (hbm32) DeleteObject(hbm32);
    if (hdcKey) DeleteDC(hdcKey);
    if (hdc32) DeleteDC(hdc32);
    return ret;
}

static BOOL UXTHEME_ResizeBlt(HDC hdcDst, int nXOriginDst, int nYOriginDst, int nWidthDst, int nHeightDst,
                              HDC hdcSrc, int nXOriginSrc, int nYOriginSrc, int nWidthSrc, int nHeightSrc,
                              INT transparent, COLORREF transcolor)
{
    BITMAPINFO bmi;
    XFORM xf;
    POINT ptDev[2];
    RECT rcDev, rcClip, rcVis;
    HDC hdcIn = NULL, hdcOut = NULL;
    HBITMAP hbmIn = NULL, hbmOut = NULL;
    HGDIOBJ hbmOldIn = NULL, hbmOldOut = NULL;
    PDWORD pdwIn, pdwOut, pdwRow;
    PINT pnColumn = NULL;
    BOOL ret = FALSE;
    int x, y, cxDev, cyDev, cxVis, cyVis, xIn, yIn, cxIn, cyIn, iSaved;

    /* Windows resizes to device pixels; rotated, mirrored or right-to-left DCs are left to GDI's stretch */
    if ((LONGLONG)nXOriginDst + nWidthDst > MAXLONG || (LONGLONG)nYOriginDst + nHeightDst > MAXLONG)
        goto stretch;
    if (GetLayout(hdcDst) & LAYOUT_RTL)
        goto stretch;
    if (GetGraphicsMode(hdcDst) == GM_ADVANCED && (!GetWorldTransform(hdcDst, &xf) || xf.eM12 != 0.0f || xf.eM21 != 0.0f))
        goto stretch;
    ptDev[0].x = nXOriginDst;
    ptDev[0].y = nYOriginDst;
    ptDev[1].x = nXOriginDst + nWidthDst;
    ptDev[1].y = nYOriginDst + nHeightDst;
    if (!LPtoDP(hdcDst, ptDev, 2) || ptDev[1].x <= ptDev[0].x || ptDev[1].y <= ptDev[0].y)
        goto stretch;
    SetRect(&rcDev, ptDev[0].x, ptDev[0].y, ptDev[1].x, ptDev[1].y);
    cxDev = rcDev.right - rcDev.left;
    cyDev = rcDev.bottom - rcDev.top;

    iSaved = SaveDC(hdcDst);
    if (!iSaved)
        goto stretch;
    if (GetGraphicsMode(hdcDst) == GM_ADVANCED)
    {
        ModifyWorldTransform(hdcDst, NULL, MWT_IDENTITY);
        SetGraphicsMode(hdcDst, GM_COMPATIBLE);
    }
    SetMapMode(hdcDst, MM_TEXT);
    SetViewportOrgEx(hdcDst, 0, 0, NULL);
    SetWindowOrgEx(hdcDst, 0, 0, NULL);

    /* Only the visible part is resized; the pixel rule keeps the full destination */
    rcVis = rcDev;
    switch (GetClipBox(hdcDst, &rcClip))
    {
        case NULLREGION:
            ret = TRUE;
            goto done;
        case ERROR:
            break;
        default:
            if (!IntersectRect(&rcVis, &rcDev, &rcClip))
            {
                ret = TRUE;
                goto done;
            }
            break;
    }
    cxVis = rcVis.right - rcVis.left;
    cyVis = rcVis.bottom - rcVis.top;
    xIn = UXTHEME_SourceColumn(rcVis.left - rcDev.left, nWidthSrc, cxDev);
    cxIn = UXTHEME_SourceColumn(rcVis.right - 1 - rcDev.left, nWidthSrc, cxDev) + 1 - xIn;
    yIn = UXTHEME_SourceRow(rcVis.top - rcDev.top, nHeightSrc, cyDev);
    cyIn = UXTHEME_SourceRow(rcVis.bottom - 1 - rcDev.top, nHeightSrc, cyDev) + 1 - yIn;

    /* Larger copies are left to GDI's stretch */
    if (cxVis > 0x4000 || cyVis > 0x4000 || cxIn > 0x4000 || cyIn > 0x4000 ||
        (ULONGLONG)cxVis * cyVis > 0x1000000 || (ULONGLONG)cxIn * cyIn > 0x1000000)
    {
        RestoreDC(hdcDst, iSaved);
        goto stretch;
    }

    hdcIn = CreateCompatibleDC(hdcSrc);
    hdcOut = CreateCompatibleDC(hdcSrc);
    pnColumn = HeapAlloc(GetProcessHeap(), 0, cxVis * sizeof(INT));
    if (!hdcIn || !hdcOut || !pnColumn)
        goto done;
    /* A monochrome source takes the destination's colours */
    SetTextColor(hdcIn, GetTextColor(hdcDst));
    SetBkColor(hdcIn, GetBkColor(hdcDst));

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biWidth = cxIn;
    bmi.bmiHeader.biHeight = -cyIn;
    hbmIn = CreateDIBSection(hdcSrc, &bmi, DIB_RGB_COLORS, (PVOID *)&pdwIn, NULL, 0);
    bmi.bmiHeader.biWidth = cxVis;
    bmi.bmiHeader.biHeight = -cyVis;
    hbmOut = CreateDIBSection(hdcSrc, &bmi, DIB_RGB_COLORS, (PVOID *)&pdwOut, NULL, 0);
    if (!hbmIn || !hbmOut)
        goto done;
    hbmOldIn = SelectObject(hdcIn, hbmIn);
    hbmOldOut = SelectObject(hdcOut, hbmOut);

    if (!BitBlt(hdcIn, 0, 0, cxIn, cyIn, hdcSrc, nXOriginSrc + xIn, nYOriginSrc + yIn, SRCCOPY))
        goto done;
    GdiFlush();

    for (x = 0; x < cxVis; x++)
        pnColumn[x] = UXTHEME_SourceColumn(rcVis.left - rcDev.left + x, nWidthSrc, cxDev) - xIn;
    for (y = 0; y < cyVis; y++)
    {
        pdwRow = pdwIn + (UXTHEME_SourceRow(rcVis.top - rcDev.top + y, nHeightSrc, cyDev) - yIn) * cxIn;
        for (x = 0; x < cxVis; x++)
            pdwOut[y * cxVis + x] = pdwRow[pnColumn[x]];
    }

    if (transparent == ALPHABLEND_BINARY)
        transcolor = UXTHEME_ExpandedKey(hdcSrc, transcolor);
    ret = UXTHEME_DoBlt(hdcDst, rcVis.left, rcVis.top, cxVis, cyVis,
                        hdcOut, 0, 0, cxVis, cyVis, transparent, transcolor);

done:
    RestoreDC(hdcDst, iSaved);
    if (hbmOldIn) SelectObject(hdcIn, hbmOldIn);
    if (hbmOldOut) SelectObject(hdcOut, hbmOldOut);
    if (hbmIn) DeleteObject(hbmIn);
    if (hbmOut) DeleteObject(hbmOut);
    if (hdcIn) DeleteDC(hdcIn);
    if (hdcOut) DeleteDC(hdcOut);
    if (pnColumn) HeapFree(GetProcessHeap(), 0, pnColumn);
    return ret;

stretch:
    return UXTHEME_DoBlt(hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                         hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc,
                         transparent, transcolor);
}

static BOOL UXTHEME_StretchBlt(HDC hdcDst, int nXOriginDst, int nYOriginDst, int nWidthDst, int nHeightDst,
                               HDC hdcSrc, int nXOriginSrc, int nYOriginSrc, int nWidthSrc, int nHeightSrc,
                               INT transparent, COLORREF transcolor)
{
    if ((nWidthDst != nWidthSrc || nHeightDst != nHeightSrc) &&
        nWidthDst > 0 && nHeightDst > 0 && nWidthSrc > 0 && nHeightSrc > 0)
    {
        return UXTHEME_ResizeBlt(hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                                 hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc,
                                 transparent, transcolor);
    }

    return UXTHEME_DoBlt(hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                         hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc,
                         transparent, transcolor);
}

/***********************************************************************
 *      UXTHEME_Blt
 *
 * Simplify sending same width/height for both source and dest
 */
static inline BOOL UXTHEME_Blt(HDC hdcDest, int nXOriginDest, int nYOriginDest, int nWidthDest, int nHeightDest,
                               HDC hdcSrc, int nXOriginSrc, int nYOriginSrc,
                               INT transparent, COLORREF transcolor)
{
    return UXTHEME_StretchBlt(hdcDest, nXOriginDest, nYOriginDest, nWidthDest, nHeightDest,
                              hdcSrc, nXOriginSrc, nYOriginSrc, nWidthDest, nHeightDest,
                              transparent, transcolor);
}

/***********************************************************************
 *      UXTHEME_SizedBlt
 *
 * Stretches or tiles, depending on sizingtype.
 */
static inline BOOL UXTHEME_SizedBlt (HDC hdcDst, int nXOriginDst, int nYOriginDst,
                                     int nWidthDst, int nHeightDst,
                                     HDC hdcSrc, int nXOriginSrc, int nYOriginSrc,
                                     int nWidthSrc, int nHeightSrc,
                                     int sizingtype,
                                     INT transparent, COLORREF transcolor)
{
    if (sizingtype == ST_TILE)
    {
        HDC hdcTemp;
        BOOL result = FALSE;

        if (!nWidthSrc || !nHeightSrc) return TRUE;

        /* For destination width/height less than or equal to source
           width/height, do not bother with memory bitmap optimization */
        if (nWidthSrc >= nWidthDst && nHeightSrc >= nHeightDst)
        {
            int bltWidth = min (nWidthDst, nWidthSrc);
            int bltHeight = min (nHeightDst, nHeightSrc);

            return UXTHEME_Blt (hdcDst, nXOriginDst, nYOriginDst, bltWidth, bltHeight,
                                hdcSrc, nXOriginSrc, nYOriginSrc,
                                transparent, transcolor);
        }

        /* Create a DC with a bitmap consisting of a tiling of the source
           bitmap, with standard GDI functions. This is faster than an
           iteration with UXTHEME_Blt(). */
        hdcTemp = CreateCompatibleDC(hdcSrc);
        if (hdcTemp != 0)
        {
            HBITMAP bitmapTemp;
            HBITMAP bitmapOrig;
            int nWidthTemp, nHeightTemp;
            int xOfs, xRemaining;
            int yOfs, yRemaining;
            int growSize;

            /* Calculate temp dimensions of integer multiples of source dimensions */
            nWidthTemp = ((nWidthDst + nWidthSrc - 1) / nWidthSrc) * nWidthSrc;
            nHeightTemp = ((nHeightDst + nHeightSrc - 1) / nHeightSrc) * nHeightSrc;
            bitmapTemp = CreateCompatibleBitmap(hdcSrc, nWidthTemp, nHeightTemp);
            bitmapOrig = SelectObject(hdcTemp, bitmapTemp);

            /* Initial copy of bitmap */
            BitBlt(hdcTemp, 0, 0, nWidthSrc, nHeightSrc, hdcSrc, nXOriginSrc, nYOriginSrc, SRCCOPY);

            /* Extend bitmap in the X direction. Growth of width is exponential */
            xOfs = nWidthSrc;
            xRemaining = nWidthTemp - nWidthSrc;
            growSize = nWidthSrc;
            while (xRemaining > 0)
            {
                growSize = min(growSize, xRemaining);
                BitBlt(hdcTemp, xOfs, 0, growSize, nHeightSrc, hdcTemp, 0, 0, SRCCOPY);
                xOfs += growSize;
                xRemaining -= growSize;
                growSize *= 2;
            }

            /* Extend bitmap in the Y direction. Growth of height is exponential */
            yOfs = nHeightSrc;
            yRemaining = nHeightTemp - nHeightSrc;
            growSize = nHeightSrc;
            while (yRemaining > 0)
            {
                growSize = min(growSize, yRemaining);
                BitBlt(hdcTemp, 0, yOfs, nWidthTemp, growSize, hdcTemp, 0, 0, SRCCOPY);
                yOfs += growSize;
                yRemaining -= growSize;
                growSize *= 2;
            }

            /* Use temporary hdc for source */
            result = UXTHEME_Blt (hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                          hdcTemp, 0, 0,
                          transparent, transcolor);

            SelectObject(hdcTemp, bitmapOrig);
            DeleteObject(bitmapTemp);
        }
        DeleteDC(hdcTemp);
        return result;
    }
    else
    {
        return UXTHEME_StretchBlt (hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                                   hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc,
                                   transparent, transcolor);
    }
}

/* An edge across a compressed margin is resized along the margin and tiled along the edge */
static BOOL UXTHEME_ResizeTileBlt(HDC hdcDst, int nXOriginDst, int nYOriginDst, int nWidthDst, int nHeightDst,
                                  HDC hdcSrc, int nXOriginSrc, int nYOriginSrc, int nWidthSrc, int nHeightSrc,
                                  BOOL bAcross, INT transparent, COLORREF transcolor)
{
    BITMAPINFO bmi;
    PVOID pvBits;
    HDC hdcTmp;
    HBITMAP hbmTmp = NULL;
    HGDIOBJ hbmOld = NULL;
    int cxTmp = bAcross ? nWidthDst : nWidthSrc;
    int cyTmp = bAcross ? nHeightSrc : nHeightDst;
    BOOL ret = FALSE;

    hdcTmp = CreateCompatibleDC(hdcSrc);
    if (!hdcTmp)
        return FALSE;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cxTmp;
    bmi.bmiHeader.biHeight = -cyTmp;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hbmTmp = CreateDIBSection(hdcSrc, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    if (hbmTmp)
    {
        hbmOld = SelectObject(hdcTmp, hbmTmp);
        SetTextColor(hdcTmp, GetTextColor(hdcDst));
        SetBkColor(hdcTmp, GetBkColor(hdcDst));
        if (UXTHEME_ResizeBlt(hdcTmp, 0, 0, cxTmp, cyTmp, hdcSrc, nXOriginSrc, nYOriginSrc,
                              nWidthSrc, nHeightSrc, ALPHABLEND_NONE, 0))
        {
            if (transparent == ALPHABLEND_BINARY)
                transcolor = UXTHEME_ExpandedKey(hdcSrc, transcolor);
            ret = UXTHEME_SizedBlt(hdcDst, nXOriginDst, nYOriginDst, nWidthDst, nHeightDst,
                                   hdcTmp, 0, 0, cxTmp, cyTmp, ST_TILE, transparent, transcolor);
        }
        SelectObject(hdcTmp, hbmOld);
        DeleteObject(hbmTmp);
    }
    DeleteDC(hdcTmp);
    return ret;
}

/* Copy of an image that is selected into a DC GdiDrawStream does not own */
static HBITMAP UXTHEME_CopyImage(HDC hdc, HBITMAP hbm)
{
    BITMAP bm;
    BITMAPINFO bmi;
    PVOID pvBits;
    HBITMAP hbmCopy;

    if (!GetObjectW(hbm, sizeof(bm), &bm))
        return NULL;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hbmCopy = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    if (!hbmCopy)
        return NULL;

    if (GetDIBits(hdc, hbm, 0, bm.bmHeight, pvBits, &bmi, DIB_RGB_COLORS) != bm.bmHeight)
    {
        DeleteObject(hbmCopy);
        return NULL;
    }
    return hbmCopy;
}

/***********************************************************************
 *      UXTHEME_DrawImageBackground
 *
 * Draw an imagefile background
 */
static HRESULT UXTHEME_DrawImageBackground(HDC hdc, HBITMAP bmpSrc, RECT *prcSrc, INT transparent,
                                    COLORREF transparentcolor, BOOL borderonly, int sizingtype, MARGINS *psm, RECT *pRect)
{
    HRESULT hr = S_OK;
    HBITMAP bmpCopy = NULL;
    HGDIOBJ oldSrc;
    HDC hdcSrc;
    RECT rcDst;
    POINT dstSize;
    POINT srcSize;
    RECT rcSrc;
    MARGINS sm;

    rcDst = *pRect;
    rcSrc = *prcSrc;
    sm = *psm;

    hdcSrc = CreateCompatibleDC(hdc);
    if(!hdcSrc) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        return hr;
    }
    oldSrc = SelectObject(hdcSrc, bmpSrc);
    if (!oldSrc)
    {
        /* A bitmap is selected into one DC at a time; the caller holds this one */
        bmpCopy = UXTHEME_CopyImage(hdcSrc, bmpSrc);
        if (bmpCopy)
            oldSrc = SelectObject(hdcSrc, bmpCopy);
        if (!oldSrc)
        {
            if (bmpCopy)
                DeleteObject(bmpCopy);
            DeleteDC(hdcSrc);
            return E_FAIL;
        }
    }

    dstSize.x = rcDst.right-rcDst.left;
    dstSize.y = rcDst.bottom-rcDst.top;
    srcSize.x = rcSrc.right-rcSrc.left;
    srcSize.y = rcSrc.bottom-rcSrc.top;

    if(sizingtype == ST_TRUESIZE) {
        if(!UXTHEME_StretchBlt(hdc, rcDst.left, rcDst.top, dstSize.x, dstSize.y,
                                hdcSrc, rcSrc.left, rcSrc.top, srcSize.x, srcSize.y,
                                transparent, transparentcolor))
            hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else {
        HDC hdcDst = NULL;
        POINT org;
        int srcX[4], srcY[4], dstX[4], dstY[4], i, j;
        BOOL bOk;

        dstSize.x = abs(dstSize.x);
        dstSize.y = abs(dstSize.y);

        /* Margins outside the image are cut to it */
        sm.cxLeftWidth = max(0, min(sm.cxLeftWidth, srcSize.x));
        sm.cxRightWidth = max(0, min(sm.cxRightWidth, srcSize.x - sm.cxLeftWidth));
        sm.cyTopHeight = max(0, min(sm.cyTopHeight, srcSize.y));
        sm.cyBottomHeight = max(0, min(sm.cyBottomHeight, srcSize.y - sm.cyTopHeight));

        /* A destination smaller than both margins gets them in proportion, as on Windows */
        srcX[0] = 0;
        srcX[1] = sm.cxLeftWidth;
        srcX[2] = srcSize.x - sm.cxRightWidth;
        srcX[3] = srcSize.x;
        srcY[0] = 0;
        srcY[1] = sm.cyTopHeight;
        srcY[2] = srcSize.y - sm.cyBottomHeight;
        srcY[3] = srcSize.y;
        dstX[0] = 0;
        dstX[3] = dstSize.x;
        dstY[0] = 0;
        dstY[3] = dstSize.y;
        if (sm.cxLeftWidth + sm.cxRightWidth > dstSize.x)
        {
            dstX[1] = dstX[2] = (int)((LONGLONG)sm.cxLeftWidth * dstSize.x / (sm.cxLeftWidth + sm.cxRightWidth));
        }
        else
        {
            dstX[1] = sm.cxLeftWidth;
            dstX[2] = dstSize.x - sm.cxRightWidth;
        }
        if (sm.cyTopHeight + sm.cyBottomHeight > dstSize.y)
        {
            dstY[1] = dstY[2] = (int)((LONGLONG)sm.cyTopHeight * dstSize.y / (sm.cyTopHeight + sm.cyBottomHeight));
        }
        else
        {
            dstY[1] = sm.cyTopHeight;
            dstY[2] = dstSize.y - sm.cyBottomHeight;
        }

        hdcDst = hdc;
        OffsetViewportOrgEx(hdcDst, rcDst.left, rcDst.top, &org);

        /* Corners are resized, edges and the centre stretched or tiled; edges across a compressed margin are resized along it */
        for (j = 0; j < 3; j++)
        {
            for (i = 0; i < 3; i++)
            {
                int dw = dstX[i + 1] - dstX[i], dh = dstY[j + 1] - dstY[j];
                int sw = srcX[i + 1] - srcX[i], sh = srcY[j + 1] - srcY[j];

                if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
                    continue;
                if (i == 1 && j == 1 && borderonly)
                    continue;
                if (sizingtype == ST_TILE && (i == 1) != (j == 1) && ((i != 1 && dw != sw) || (j != 1 && dh != sh)))
                    bOk = UXTHEME_ResizeTileBlt(hdcDst, dstX[i], dstY[j], dw, dh,
                                                hdcSrc, rcSrc.left + srcX[i], rcSrc.top + srcY[j], sw, sh,
                                                i != 1, transparent, transparentcolor);
                else
                    bOk = UXTHEME_SizedBlt(hdcDst, dstX[i], dstY[j], dw, dh,
                                           hdcSrc, rcSrc.left + srcX[i], rcSrc.top + srcY[j], sw, sh,
                                           (i == 1 || j == 1) ? sizingtype : ST_STRETCH,
                                           transparent, transparentcolor);
                if (!bOk)
                {
                    hr = HRESULT_FROM_WIN32(GetLastError());
                    goto draw_error;
                }
            }
        }

draw_error:
        SetViewportOrgEx (hdcDst, org.x, org.y, NULL);
    }
    SelectObject(hdcSrc, oldSrc);
    DeleteDC(hdcSrc);
    if (bmpCopy) DeleteObject(bmpCopy);
    *pRect = rcDst;
    return hr;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
GdiDrawStream(HDC dc, ULONG l, PGDI_DRAW_STREAM pDS)
{
    if (!pDS || l != sizeof(*pDS))
    {
        DPRINT1("GdiDrawStream: Invalid params\n");
        return 0;
    }

    if (pDS->signature != 0x44727753 ||
        pDS->reserved != 0 ||
        pDS->unknown1 != 1 ||
        pDS->unknown2 != 9)
    {
        DPRINT1("GdiDrawStream: Got unknown pDS data\n");
        return 0;
    }

    {
        MARGINS sm = {pDS->leftSizingMargin, pDS->rightSizingMargin, pDS->topSizingMargin, pDS->bottomSizingMargin};
        INT transparent = 0;
        int sizingtype;

        if (pDS->drawOption & DS_TRANSPARENTALPHA)
            transparent = ALPHABLEND_FULL;
        else if (pDS->drawOption & DS_TRANSPARENTCLR)
            transparent = ALPHABLEND_BINARY;
        else
            transparent = ALPHABLEND_NONE;

        if (pDS->drawOption & DS_TILE)
            sizingtype = ST_TILE;
        else if (pDS->drawOption & DS_TRUESIZE)
            sizingtype = ST_TRUESIZE;
        else
            sizingtype = ST_STRETCH;

        if (pDS->rcDest.right < pDS->rcDest.left || pDS->rcDest.bottom < pDS->rcDest.top)
            return 0;

        /* Threads drawing the same theme image take turns selecting it */
        EnterCriticalSection(&gcsDrawStream);
        UXTHEME_DrawImageBackground(pDS->hDC,
                                    pDS->hImage,
                                    &pDS->rcSrc,
                                    transparent,
                                    pDS->crTransparent,
                                    FALSE,
                                    sizingtype,
                                    &sm,
                                    &pDS->rcDest);
        LeaveCriticalSection(&gcsDrawStream);
    }
    return 0;
}


/*
 * @implemented
 */
BOOL
WINAPI
GdiValidateHandle(HGDIOBJ hobj)
{
    PGDI_TABLE_ENTRY Entry = GdiHandleTable + GDI_HANDLE_GET_INDEX(hobj);
    if ( (Entry->Type & GDI_ENTRY_BASETYPE_MASK) != 0 &&
            ( (Entry->Type << GDI_ENTRY_UPPER_SHIFT) & GDI_HANDLE_TYPE_MASK ) ==
            GDI_HANDLE_GET_TYPE(hobj) )
    {
        HANDLE pid = (HANDLE)((ULONG_PTR)Entry->ProcessId & ~0x1);
        if(pid == NULL || pid == CurrentProcessId)
        {
            return TRUE;
        }
    }
    return FALSE;

}

/*
 * @implemented
 */
HGDIOBJ
WINAPI
GdiFixUpHandle(HGDIOBJ hGdiObj)
{
    PGDI_TABLE_ENTRY Entry;

    if (GDI_HANDLE_GET_UPPER(hGdiObj))
    {
        return hGdiObj;
    }

    /* FIXME is this right ?? */

    Entry = GdiHandleTable + GDI_HANDLE_GET_INDEX(hGdiObj);

    /* Rebuild handle for Object */
    return (HGDIOBJ)(((ULONG_PTR)(hGdiObj)) | (Entry->Type << GDI_ENTRY_UPPER_SHIFT));
}

/*
 * @implemented
 */
PVOID
WINAPI
GdiQueryTable(VOID)
{
    return (PVOID)GdiHandleTable;
}

BOOL GdiGetHandleUserData(HGDIOBJ hGdiObj, DWORD ObjectType, PVOID *UserData)
{
    PGDI_TABLE_ENTRY Entry = GdiHandleTable + GDI_HANDLE_GET_INDEX(hGdiObj);

    /* Check if twe have the correct type */
    if (GDI_HANDLE_GET_TYPE(hGdiObj) != ObjectType ||
        ((Entry->Type << GDI_ENTRY_UPPER_SHIFT) & GDI_HANDLE_TYPE_MASK) != ObjectType ||
        (Entry->Type & GDI_ENTRY_BASETYPE_MASK) != (ObjectType & GDI_ENTRY_BASETYPE_MASK))
    {
        return FALSE;
    }

    /* Check if we are the owner */
    if ((HANDLE)((ULONG_PTR)Entry->ProcessId & ~0x1) != CurrentProcessId)
    {
        return FALSE;
    }

    *UserData = Entry->UserData;
    return TRUE;
}

PLDC
FASTCALL
GdiGetLDC(HDC hdc)
{
    PDC_ATTR pdcattr;

    /* Get the DC attribute */
    pdcattr = GdiGetDcAttr(hdc);
    if (pdcattr == NULL)
    {
        return NULL;
    }

    /* Return the LDC pointer */
    return pdcattr->pvLDC;
}

BOOL
FASTCALL
GdiSetLDC(HDC hdc, PVOID pvLDC)
{
    PDC_ATTR pdcattr;

    /* Get the DC attribute */
    pdcattr = GdiGetDcAttr(hdc);
    if (pdcattr == NULL)
    {
        return FALSE;
    }

    /* Set the LDC pointer */
    pdcattr->pvLDC = pvLDC;
    return TRUE;
}


VOID GdiSAPCallback(PLDC pldc)
{
    DWORD Time, NewTime = GetTickCount();

    Time = NewTime - pldc->CallBackTick;

    if ( Time < SAPCALLBACKDELAY) return;

    pldc->CallBackTick = NewTime;

    if ( !pldc->pAbortProc(pldc->hDC, 0) )
    {
        CancelDC(pldc->hDC);
        AbortDoc(pldc->hDC);
    }
}

/*
 * @implemented
 */
DWORD
WINAPI
GdiSetBatchLimit(DWORD	Limit)
{
    DWORD OldLimit = GDI_BatchLimit;

    if ( (!Limit) ||
            (Limit >= GDI_BATCH_LIMIT))
    {
        return Limit;
    }

    GdiFlush();
    GDI_BatchLimit = Limit;
    return OldLimit;
}


/*
 * @implemented
 */
DWORD
WINAPI
GdiGetBatchLimit(VOID)
{
    return GDI_BatchLimit;
}


/*
 * @implemented
 */
VOID
WINAPI
GdiSetLastError(DWORD dwErrCode)
{
    NtCurrentTeb()->LastErrorValue = (ULONG) dwErrCode;
}

HGDIOBJ
FASTCALL
hGetPEBHandle(HANDLECACHETYPE Type, COLORREF cr)
{
    int Number, Offset, MaxNum, GdiType;
    HANDLE Lock;
    HGDIOBJ Handle = NULL;

    Lock = InterlockedCompareExchangePointer( (PVOID*)&GdiHandleCache->ulLock,
            NtCurrentTeb(),
            NULL );

    if (Lock) return Handle;

    Number = GdiHandleCache->ulNumHandles[Type];

    if (Type == hctBrushHandle)
    {
       Offset = 0;
       MaxNum = CACHE_BRUSH_ENTRIES;
       GdiType = GDILoObjType_LO_BRUSH_TYPE;
    }
    else if (Type == hctPenHandle)
    {
       Offset = CACHE_BRUSH_ENTRIES;
       MaxNum = CACHE_PEN_ENTRIES;
       GdiType = GDILoObjType_LO_PEN_TYPE;
    }
    else if (Type == hctRegionHandle)
    {
       Offset = CACHE_BRUSH_ENTRIES+CACHE_PEN_ENTRIES;
       MaxNum = CACHE_REGION_ENTRIES;
       GdiType = GDILoObjType_LO_REGION_TYPE;
    }
    else // Font is not supported here.
    {
       return Handle;
    }

    if ( Number && Number <= MaxNum )
    {
       PBRUSH_ATTR pBrush_Attr;
       HGDIOBJ *hPtr;
       hPtr = GdiHandleCache->Handle + Offset;
       Handle = hPtr[Number - 1];

       if (GdiGetHandleUserData( Handle, GdiType, (PVOID) &pBrush_Attr))
       {
          if (pBrush_Attr->AttrFlags & ATTR_CACHED)
          {
             DPRINT("Get Handle! Type %d Count %lu PEB 0x%p\n", Type, GdiHandleCache->ulNumHandles[Type], NtCurrentTeb()->ProcessEnvironmentBlock);
             pBrush_Attr->AttrFlags &= ~ATTR_CACHED;
             hPtr[Number - 1] = NULL;
             GdiHandleCache->ulNumHandles[Type]--;
             if ( Type == hctBrushHandle ) // Handle only brush.
             {
                if ( pBrush_Attr->lbColor != cr )
                {
                   pBrush_Attr->lbColor = cr ;
                   pBrush_Attr->AttrFlags |= ATTR_NEW_COLOR;
                }
             }
          }
       }
       else
       {
          Handle = NULL;
       }
    }
    (void)InterlockedExchangePointer((PVOID*)&GdiHandleCache->ulLock, Lock);
    return Handle;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
bMakePathNameW(LPWSTR lpBuffer,LPCWSTR lpFileName,LPWSTR *lpFilePart,DWORD unknown)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

/*
 * @implemented
 * Synchronized with WINE dlls/gdi32/driver.c
 */
DEVMODEW *
WINAPI
GdiConvertToDevmodeW(const DEVMODEA *dmA)
{
    DEVMODEW *dmW;
    WORD dmW_size, dmA_size;

    dmA_size = dmA->dmSize;

    /* this is the minimal dmSize that XP accepts */
    if (dmA_size < FIELD_OFFSET(DEVMODEA, dmFields))
        return NULL;

    if (dmA_size > sizeof(DEVMODEA))
        dmA_size = sizeof(DEVMODEA);

    dmW_size = dmA_size + CCHDEVICENAME;
    if (dmA_size >= FIELD_OFFSET(DEVMODEA, dmFormName) + CCHFORMNAME)
        dmW_size += CCHFORMNAME;

    dmW = HeapAlloc(GetProcessHeap(), 0, dmW_size + dmA->dmDriverExtra);
    if (!dmW) return NULL;

    MultiByteToWideChar(CP_ACP, 0, (const char*) dmA->dmDeviceName, -1,
                                   dmW->dmDeviceName, CCHDEVICENAME);
    /* copy slightly more, to avoid long computations */
    memcpy(&dmW->dmSpecVersion, &dmA->dmSpecVersion, dmA_size - CCHDEVICENAME);

    if (dmA_size >= FIELD_OFFSET(DEVMODEA, dmFormName) + CCHFORMNAME)
    {
        if (dmA->dmFields & DM_FORMNAME)
            MultiByteToWideChar(CP_ACP, 0, (const char*) dmA->dmFormName, -1,
                                       dmW->dmFormName, CCHFORMNAME);
        else
            dmW->dmFormName[0] = 0;

        if (dmA_size > FIELD_OFFSET(DEVMODEA, dmLogPixels))
            memcpy(&dmW->dmLogPixels, &dmA->dmLogPixels, dmA_size - FIELD_OFFSET(DEVMODEA, dmLogPixels));
    }

    if (dmA->dmDriverExtra)
        memcpy((char *)dmW + dmW_size, (const char *)dmA + dmA_size, dmA->dmDriverExtra);

    dmW->dmSize = dmW_size;

    return dmW;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
GdiRealizationInfo(HDC hdc,
                   PREALIZATION_INFO pri)
{
    // ATM we do not support local font data and Language Pack.
    return NtGdiGetRealizationInfo(hdc, pri, (HFONT) NULL);
}

BOOL
WINAPI
GetFontFileData(
    _In_ DWORD uFileCollectionID,
    _In_ DWORD uFileIndex,
    _In_ UINT64 pullFileOffset,
    _Out_writes_bytes_(cjBuf) PVOID pvBuf,
    _In_ SIZE_T cjBuf)
{
    return NtGdiGetFontFileData(uFileCollectionID, uFileIndex, &pullFileOffset, pvBuf, cjBuf);
}

BOOL
WINAPI
GetFontFileInfo(
    _In_ DWORD uFileCollectionID,
    _In_ DWORD uFileIndex,
    _Out_writes_bytes_(cjSize) PFONT_FILE_INFO pffi,
    _In_ SIZE_T cjSize,
    _Out_opt_ PSIZE_T pcjActualSize)
{
    return NtGdiGetFontFileInfo(uFileCollectionID, uFileIndex, pffi, cjSize, pcjActualSize);
}

BOOL
WINAPI
GetFontRealizationInfo(
    _In_ HDC hdc,
    _Out_ PFONT_REALIZATION_INFO info)
{
    return NtGdiGetFontRealizationInfo(hdc, info);
}

/*
 * @halfplemented
 */
VOID WINAPI GdiInitializeLanguagePack(DWORD InitParam)
{
    /* Lpk function pointers to be passed to user32 */
#if 0
    FARPROC hookfuncs[4];
#endif

#ifdef LANGPACK
    if (!LoadLPK(LPK_INIT)) // no lpk found!
#endif
        return;

     /* Call InitializeLpkHooks with 4 procedure addresses
        loaded from lpk.dll but currently only one of them is currently implemented.
        Then InitializeLpkHooks (in user32) uses these to replace certain internal functions
        and ORs a DWORD being used also by ClientThreadSetup and calls
        NtUserOneParam with parameter 54 which is ONEPARAM_ROUTINE_REGISTERLPK
        which most likely changes the value of dwLpkEntryPoints in the
        PROCESSINFO struct */

#if 0
        hookfuncs[0] = GetProcAddress(hLpk, "LpkPSMTextOut");
        InitializeLpkHooks(hookfuncs);
#endif

    gbLpk = TRUE;
}

BOOL
WINAPI
GdiAddGlsBounds(HDC hdc,LPRECT prc)
{
    return NtGdiSetBoundsRect(hdc, prc, DCB_WINDOWMGR|DCB_ACCUMULATE ) ? TRUE : FALSE;
}

BOOL
WINAPI
GetBoundsRectAlt(HDC hdc,LPRECT prc,UINT flags)
{
    return NtGdiGetBoundsRect(hdc, prc, flags);
}

BOOL
WINAPI
SetBoundsRectAlt(HDC hdc,LPRECT prc,UINT flags)
{
    return NtGdiSetBoundsRect(hdc, prc, flags );
}

/*
 * @unimplemented
 */
BOOL
WINAPI
GdiAddGlsRecord(HDC hdc,
                DWORD unknown1,
                LPCSTR unknown2,
                LPRECT unknown3)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

