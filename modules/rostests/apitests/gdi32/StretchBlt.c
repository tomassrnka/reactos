/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the StretchBlt API
 * COPYRIGHT:   Copyright 2020, 2021 Doug Lyons (douglyons at douglyons dot com)
 *              Some Code copied and modified from Wine gdi32:bitmap test.
 */

#include <stdarg.h>
#include <assert.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/test.h"
#include <debug.h>

/* Notes on using the StretchBlt function's general flip ability.
 *
 * The critical values for flipping are the signs of the values for DestWidth, DestHeight, SourceWidth and SourceHeight.
 * If we assign a '0' to values having a negative sign and a '1' to values having a positive sign these can be CaseWXYZ's.
 * Where the W, X, Y, and Z are replaced with the '0's and '1's representing the signs of the values in sequential order.
 *
 * If we take the normal StretchBlt function's copy/scaling with no flips, the generalized code can be represented as follows:
 *
 * StretchBlt(DestDC,   DestX,   DestY,   (DestWidth),   (DestHeight),
 *            SourceDC, SourceX, SourceY, (SourceWidth), (SourceHeight), SRCCOPY); // Case1111 (15) (all positive signs)
 *
 * For Horizontal flipping then the generalized form on the Destination side can be represented as follows:
 *
 * StretchBlt(DestDC,   DestX + DestWidth - 1,  DestY,   -DestWidth,   DestHeight,
 *            SourceDC, SourceX,                SourceY, SourceWidth,  SourceHeight, SRCCOPY);  // Case 0111 (7)
 *
 * and for Horizontal flipping the generalized form on the Source side can be represented as follows:
 *
 * StretchBlt(DestDC,   DestX,                     DestY,   DestWidth,    DestHeight,
 *            SourceDC, SourceX + SourceWidth - 1, SourceY, -SourceWidth, SourceHeight, SRCCOPY); // Case 1101 (13)
 *
 * I believe that the "- 1" is used because we are moving from the rightmost position back to the 0th position.
 *
 * But there are three "special" cases where no flip is done (there is a copy/scale only) and the "-1" is not used.
 * These are as follows:
 * 1)
 * StretchBlt(DestDC,   DestX + DestWidth,     DestY,   -DestWidth,   DestHeight,  // Both Widths negative
 *            SourceDC, SourceX + SourceWidth, SourceY, -SourceWidth, SourceHeight, SRCCOPY); // Case0101 (5)
 * 2)
 * StretchBlt(DestDC,   DestX,   DestY + DestHeight,     DestWidth,   -DestHeight, // Both heights negative
 *            SourceDC, SourceX, SourceY + SourceHeight, SourceWidth, -SourceHeight, SRCCOPY); // Case1010 (10)
 * 3)
 * StretchBlt(DestDC,   DestX + DestWidth,     DestY + DestHeight,     -DestWidth,   -DestHeight, // widths AND heights neg
 *            SourceDC, SourceX + SourceWidth, SourceY + SourceHeight, -SourceWidth, -SourceHeight, SRCCOPY); // Case0000 (0)
 *
 * I suspect that these are like this because of legacy reasons when StretchBlt did not support so many flip options.
 *
 * For Vertical flipping the generalized form on the Destination side can be represented as follows:
 *
 * StretchBlt(DestDC,   DestX,   DestY + DestHeight - 1,  DestWidth,   -DestHeight,
 *            SourceDC, SourceX, SourceY,                 SourceWidth, SourceHeight, SRCCOPY); // Case1011 (11)
 *
 * and for Vertical on the Source Side as folows:
 *
 * StretchBlt(DestDC,   DestX,   DestY,                      DestWidth,   -DestHeight,
 *            SourceDC, SourceX, SourceY + SourceHeight - 1, SourceWidth, -SourceHeight, SRCCOPY); // Case 1010 (10)
 */

static inline int get_dib_stride( int width, int bpp )
{
    return ((width * bpp + 31) >> 3) & ~3;
}

static inline int get_dib_image_size( const BITMAPINFO *info )
{
    return get_dib_stride( info->bmiHeader.biWidth, info->bmiHeader.biBitCount )
        * abs( info->bmiHeader.biHeight );
}

static void check_StretchBlt_stretch(HDC hdcDst, HDC hdcSrc, BITMAPINFO *dst_info, UINT32 *dstBuffer, UINT32 *srcBuffer,
                                     int nXOriginDest, int nYOriginDest, int nWidthDest, int nHeightDest,
                                     int nXOriginSrc, int nYOriginSrc, int nWidthSrc, int nHeightSrc,
                                     UINT32 *expected, int line)
{
    int dst_size = get_dib_image_size( dst_info );

    memset(dstBuffer, 0, dst_size);
    StretchBlt(hdcDst, nXOriginDest, nYOriginDest, nWidthDest, nHeightDest,
               hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc, SRCCOPY);
    ok(memcmp(dstBuffer, expected, dst_size) == 0,
        "StretchBlt \nexp { %08X, %08X, %08X, %08X } \ngot { %08X, %08X, %08X, %08X } \n"
        "destination { %d, %d, %d, %d } source { %d, %d, %d, %d } from line %d\n",
        expected[0], expected[1], expected[2], expected[3],
        dstBuffer[0], dstBuffer[1], dstBuffer[2], dstBuffer[3],
        nXOriginDest, nYOriginDest, nWidthDest, nHeightDest,
        nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc, line);
}

static void test_StretchBlt_stretch(HDC hdcDst, HDC hdcSrc, BITMAPINFO *dst_info, UINT32 *dstBuffer, UINT32 *srcBuffer,
                                    int nXOriginDest, int nYOriginDest, int nWidthDest, int nHeightDest,
                                    int nXOriginSrc, int nYOriginSrc, int nWidthSrc, int nHeightSrc,
                                    UINT32 *expected, int line, BOOL SrcTopDown, BOOL DstTopDown )
{
    int dst_size = get_dib_image_size( dst_info );

    memset(dstBuffer, 0, dst_size);
    StretchBlt(hdcDst, nXOriginDest, nYOriginDest, nWidthDest, nHeightDest,
               hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc, SRCCOPY);
    ok(memcmp(dstBuffer, expected, dst_size) == 0,
        "Case%c%c%c%c %s - %s \nexp { %08X, %08X, %08X, %08X } \ngot { %08X, %08X, %08X, %08X }\n"
        "destination { %d, %d, %d, %d } source { %d, %d, %d, %d } from line %d\n",
         (nWidthDest < 0) ? '0' : '1', (nHeightDest < 0) ? '0' : '1',
         (nWidthSrc < 0) ? '0' : '1', (nHeightSrc < 0) ? '0' : '1',
         SrcTopDown ? "SrcTopDown" : "SrcBottomUp", DstTopDown ? "DstTopDown" : "DstBottomUp",
        expected[0], expected[1], expected[2], expected[3],
        dstBuffer[0], dstBuffer[1], dstBuffer[2], dstBuffer[3],
        nXOriginDest, nYOriginDest, nWidthDest, nHeightDest,
        nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc, line);
}

static void test_StretchBlt(void)
{
    HBITMAP bmpDst, bmpSrc;
    HBITMAP oldDst, oldSrc;
    HDC hdcScreen, hdcDst, hdcSrc;
    UINT32 *dstBuffer, *srcBuffer;
    BITMAPINFO biDst, biSrc;
    UINT32 expected[256];
    RGBQUAD colors[2];

    memset(&biDst, 0, sizeof(BITMAPINFO));
    biDst.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    biDst.bmiHeader.biWidth = 16;
    biDst.bmiHeader.biHeight = -16;
    biDst.bmiHeader.biPlanes = 1;
    biDst.bmiHeader.biBitCount = 32;
    biDst.bmiHeader.biCompression = BI_RGB;
    memcpy(&biSrc, &biDst, sizeof(BITMAPINFO));

    hdcScreen = CreateCompatibleDC(NULL);
    hdcDst = CreateCompatibleDC(hdcScreen);
    hdcSrc = CreateCompatibleDC(hdcDst);

    bmpDst = CreateDIBSection(hdcScreen, &biDst, DIB_RGB_COLORS, (void**)&dstBuffer, NULL, 0);
    oldDst = SelectObject(hdcDst, bmpDst);

    bmpSrc = CreateDIBSection(hdcScreen, &biSrc, DIB_RGB_COLORS, (void**)&srcBuffer, NULL, 0);
    oldSrc = SelectObject(hdcSrc, bmpSrc);

    /* Top-down to top-down tests */
    srcBuffer[0] = 0xCAFED00D, srcBuffer[1] = 0xFEEDFACE;
    srcBuffer[16] = 0xFEDCBA98, srcBuffer[17] = 0x76543210;

    memset( expected, 0, get_dib_image_size( &biDst ) );
    expected[0] = 0xCAFED00D, expected[1] = 0xFEEDFACE;
    expected[16] = 0xFEDCBA98, expected[17] = 0x76543210;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 0, 0, 2, 2, expected, __LINE__);

    expected[0] = 0xCAFED00D, expected[1] = 0x00000000;
    expected[16] = 0x00000000, expected[17] = 0x00000000;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 1, 1, 0, 0, 1, 1, expected, __LINE__);

    expected[0] = 0xCAFED00D, expected[1] = 0xCAFED00D;
    expected[16] = 0xCAFED00D, expected[17] = 0xCAFED00D;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 0, 0, 1, 1, expected, __LINE__);

    /* This is an example of the dst width (height) == 1 exception, explored below */
    expected[0] = 0xCAFED00D, expected[1] = 0x00000000;
    expected[16] = 0x00000000, expected[17] = 0x00000000;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 1, 1, 0, 0, 2, 2, expected, __LINE__);

    expected[0] = 0xCAFED00D, expected[1] = 0x00000000;
    expected[16] = 0x00000000, expected[17] = 0x00000000;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 1, -2, -2, 1, 1, -2, -2, expected, __LINE__);

    expected[0] = 0x00000000, expected[1] = 0x00000000;
    expected[16] = 0x00000000, expected[17] = 0xCAFED00D, expected[18] = 0xFEEDFACE;
    expected[32] = 0x00000000, expected[33] = 0xFEDCBA98, expected[34] = 0x76543210;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 1, 2, 2, 0, 0, 2, 2, expected, __LINE__);

    /* when dst width is 1 merge src width - 1 pixels */
    memset( srcBuffer, 0, get_dib_image_size( &biSrc ) );
    srcBuffer[0] = 0x0000ff00, srcBuffer[1] = 0x0000f0f0, srcBuffer[2] = 0x0000cccc, srcBuffer[3] = 0x0000aaaa;
    srcBuffer[16] = 0xFEDCBA98, srcBuffer[17] = 0x76543210;

    memset( expected, 0, get_dib_image_size( &biDst ) );
    expected[0] = srcBuffer[0];
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 1, 1, 0, 0, 2, 1, expected, __LINE__);

    /* similarly in the vertical direction */
    memset( expected, 0, get_dib_image_size( &biDst ) );
    expected[0] = srcBuffer[0];
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 1, 1, 0, 0, 1, 2, expected, __LINE__);

    /* check that it's the dst size in device units that needs to be 1 */
    SetMapMode( hdcDst, MM_ISOTROPIC );
    SetWindowExtEx( hdcDst, 200, 200, NULL );
    SetViewportExtEx( hdcDst, 100, 100, NULL );

    SetMapMode( hdcDst, MM_TEXT );

    SelectObject(hdcDst, oldDst);
    DeleteObject(bmpDst);

    /* Top-down to bottom-up tests */
    memset( srcBuffer, 0, get_dib_image_size( &biSrc ) );
    srcBuffer[0] = 0xCAFED00D, srcBuffer[1] = 0xFEEDFACE;
    srcBuffer[16] = 0xFEDCBA98, srcBuffer[17] = 0x76543210;

    biDst.bmiHeader.biHeight = 16;
    bmpDst = CreateDIBSection(hdcScreen, &biDst, DIB_RGB_COLORS, (void**)&dstBuffer, NULL, 0);
    oldDst = SelectObject(hdcDst, bmpDst);

    memset( expected, 0, get_dib_image_size( &biDst ) );

    expected[224] = 0xFEDCBA98, expected[225] = 0x76543210;
    expected[240] = 0xCAFED00D, expected[241] = 0xFEEDFACE;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 0, 0, 2, 2, expected, __LINE__);

    SelectObject(hdcSrc, oldSrc);
    DeleteObject(bmpSrc);

    /* Bottom-up to bottom-up tests */
    biSrc.bmiHeader.biHeight = 16;
    bmpSrc = CreateDIBSection(hdcScreen, &biSrc, DIB_RGB_COLORS, (void**)&srcBuffer, NULL, 0);
    srcBuffer[224] = 0xCAFED00D, srcBuffer[225] = 0xFEEDFACE;
    srcBuffer[240] = 0xFEDCBA98, srcBuffer[241] = 0x76543210;
    oldSrc = SelectObject(hdcSrc, bmpSrc);

    memset( expected, 0, get_dib_image_size( &biDst ) );

    expected[224] = 0xCAFED00D, expected[225] = 0xFEEDFACE;
    expected[240] = 0xFEDCBA98, expected[241] = 0x76543210;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 0, 0, 2, 2, expected, __LINE__);

    SelectObject(hdcDst, oldDst);
    DeleteObject(bmpDst);

    /* Bottom-up to top-down tests */
    biDst.bmiHeader.biHeight = -16;
    bmpDst = CreateDIBSection(hdcScreen, &biDst, DIB_RGB_COLORS, (void**)&dstBuffer, NULL, 0);
    oldDst = SelectObject(hdcDst, bmpDst);

    memset( expected, 0, get_dib_image_size( &biDst ) );
    expected[0] = 0xFEDCBA98, expected[1] = 0x76543210;
    expected[16] = 0xCAFED00D, expected[17] = 0xFEEDFACE;
    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 0, 0, 2, 2, expected, __LINE__);

    SelectObject(hdcSrc, oldSrc);
    DeleteObject(bmpSrc);

    biSrc.bmiHeader.biHeight = -2;
    biSrc.bmiHeader.biBitCount = 24;
    bmpSrc = CreateDIBSection(hdcScreen, &biSrc, DIB_RGB_COLORS, (void**)&srcBuffer, NULL, 0);
    oldSrc = SelectObject(hdcSrc, bmpSrc);

    memset( expected, 0, get_dib_image_size( &biDst ) );
    expected[0] = 0xFEEDFACE, expected[1] = 0xCAFED00D;
    expected[2] = 0x76543210, expected[3] = 0xFEDCBA98;
    memcpy(dstBuffer, expected, 4 * sizeof(*dstBuffer));
    StretchBlt(hdcSrc, 0, 0, 4, 1, hdcDst, 0, 0, 4, 1, SRCCOPY );
    memset(dstBuffer, 0x55, 4 * sizeof(*dstBuffer));
    StretchBlt(hdcDst, 0, 0, 4, 1, hdcSrc, 0, 0, 4, 1, SRCCOPY );
    expected[0] = 0x00EDFACE, expected[1] = 0x00FED00D;
    expected[2] = 0x00543210, expected[3] = 0x00DCBA98;
    ok(!memcmp(dstBuffer, expected, 16),
       "StretchBlt expected { %08X, %08X, %08X, %08X } got { %08X, %08X, %08X, %08X }\n",
        expected[0], expected[1], expected[2], expected[3],
        dstBuffer[0], dstBuffer[1], dstBuffer[2], dstBuffer[3] );

    expected[0] = 0xFEEDFACE, expected[1] = 0xCAFED00D;
    expected[2] = 0x76543210, expected[3] = 0xFEDCBA98;
    memcpy(srcBuffer, expected, 4 * sizeof(*dstBuffer));
    memset(dstBuffer, 0x55, 4 * sizeof(*dstBuffer));
    StretchBlt(hdcDst, 0, 0, 4, 1, hdcSrc, 0, 0, 4, 1, SRCCOPY );
    expected[0] = 0x00EDFACE, expected[1] = 0x00D00DFE;
    expected[2] = 0x0010CAFE, expected[3] = 0x00765432;
    ok(!memcmp(dstBuffer, expected, 16),
       "StretchBlt expected { %08X, %08X, %08X, %08X } got { %08X, %08X, %08X, %08X }\n",
        expected[0], expected[1], expected[2], expected[3],
        dstBuffer[0], dstBuffer[1], dstBuffer[2], dstBuffer[3] );

    SelectObject(hdcSrc, oldSrc);
    DeleteObject(bmpSrc);

    biSrc.bmiHeader.biBitCount = 1;
    bmpSrc = CreateDIBSection(hdcScreen, &biSrc, DIB_RGB_COLORS, (void**)&srcBuffer, NULL, 0);
    oldSrc = SelectObject(hdcSrc, bmpSrc);
    *((DWORD *)colors + 0) = 0x123456;
    *((DWORD *)colors + 1) = 0x335577;
    SetDIBColorTable( hdcSrc, 0, 2, colors );
    srcBuffer[0] = 0x55555555;
    memset(dstBuffer, 0xcc, 4 * sizeof(*dstBuffer));
    SetTextColor( hdcDst, 0 );
    SetBkColor( hdcDst, 0 );
    StretchBlt(hdcDst, 0, 0, 4, 1, hdcSrc, 0, 0, 4, 1, SRCCOPY );
    expected[0] = expected[2] = 0x00123456;
    expected[1] = expected[3] = 0x00335577;
    ok(!memcmp(dstBuffer, expected, 16),
       "StretchBlt expected { %08X, %08X, %08X, %08X } got { %08X, %08X, %08X, %08X }\n",
        expected[0], expected[1], expected[2], expected[3],
        dstBuffer[0], dstBuffer[1], dstBuffer[2], dstBuffer[3] );

    SelectObject(hdcSrc, oldSrc);
    DeleteObject(bmpSrc);

    bmpSrc = CreateBitmap( 16, 16, 1, 1, 0 );
    oldSrc = SelectObject(hdcSrc, bmpSrc);
    SetPixel( hdcSrc, 0, 0, 0 );
    SetPixel( hdcSrc, 1, 0, 0xffffff );
    SetPixel( hdcSrc, 2, 0, 0xffffff );
    SetPixel( hdcSrc, 3, 0, 0 );
    memset(dstBuffer, 0xcc, 4 * sizeof(*dstBuffer));
    SetTextColor(hdcDst, RGB(0x22, 0x44, 0x66));
    SetBkColor(hdcDst, RGB(0x65, 0x43, 0x21));
    StretchBlt(hdcDst, 0, 0, 4, 1, hdcSrc, 0, 0, 4, 1, SRCCOPY );
    expected[0] = expected[3] = 0x00224466;
    expected[1] = expected[2] = 0x00654321;
    ok(!memcmp(dstBuffer, expected, 16),
       "StretchBlt expected { %08X, %08X, %08X, %08X } got { %08X, %08X, %08X, %08X }\n",
        expected[0], expected[1], expected[2], expected[3],
        dstBuffer[0], dstBuffer[1], dstBuffer[2], dstBuffer[3] );

    SelectObject(hdcSrc, oldSrc);
    DeleteObject(bmpSrc);

    SelectObject(hdcSrc, oldSrc);
    DeleteObject(bmpSrc);
    SelectObject(hdcDst, oldDst);
    DeleteObject(bmpDst);

    memset(&biDst, 0, sizeof(BITMAPINFO));              // Clear our Bitmap to to all zeroes

    biDst.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);  // Set our Size to the header size
    biDst.bmiHeader.biWidth = 2;                        // Set our Width to 2 (left-to-right)
    biDst.bmiHeader.biHeight = -2;                      // Set our Height to -2 that's negative for top-down
    biDst.bmiHeader.biPlanes = 1;                       // Set out planes to 1 (1 required by Windows)
    biDst.bmiHeader.biBitCount = 32;                    // Set out BitCount to 32 (Full Color)
    biDst.bmiHeader.biCompression = BI_RGB;             // Set our Compression to BI_RBG (uncompressed)

    memcpy(&biSrc, &biDst, sizeof(BITMAPINFO));         // Put same Destination params into the Source

    bmpSrc = CreateDIBSection(hdcSrc, &biSrc, DIB_RGB_COLORS, (void**)&srcBuffer, NULL, 0);
    oldSrc = SelectObject(hdcSrc, bmpSrc);
    bmpDst = CreateDIBSection(hdcDst, &biDst, DIB_RGB_COLORS, (void**)&dstBuffer, NULL, 0);
    oldDst = SelectObject(hdcDst, bmpDst);

    srcBuffer[0] = 0x000000FF;   // BLUE
    srcBuffer[1] = 0x0000FF00;   // GREEN
    srcBuffer[2] = 0x00FF0000;   // RED
    srcBuffer[3] = 0xFF000000;   // ALPHA (Opacity)

    expected[0] = 0x000000FF;
    expected[1] = 0x0000FF00;
    expected[2] = 0x00FF0000;
    expected[3] = 0xFF000000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 0, 0, 2, 2, expected, __LINE__); // Case 1111 (15) - No flip. Just copy.

    expected[0] = 0x00FF0000;
    expected[1] = 0xFF000000;
    expected[2] = 0x000000FF;
    expected[3] = 0x0000FF00;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 0, 1, 2, -2, expected, __LINE__); // Case 1110 (14) - Vertical flip.

    expected[0] = 0x0000FF00;
    expected[1] = 0x000000FF;
    expected[2] = 0xFF000000;
    expected[3] = 0x00FF0000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 1, 0, -2, 2, expected, __LINE__); // Case 1101 (13) - Horizontal flip.

    expected[0] = 0xFF000000;
    expected[1] = 0x00FF0000;
    expected[2] = 0x0000FF00;
    expected[3] = 0x000000FF;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 0, 2, 2, 1, 1, -2, -2, expected, __LINE__); // Case 1100 (12) - Both flip.

    expected[0] = 0x00FF0000;
    expected[1] = 0xFF000000;
    expected[2] = 0x000000FF;
    expected[3] = 0x0000FF00;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 1, 2, -2, 0, 0, 2, 2, expected, __LINE__); // Case 1011 (11) - Vertical Flip.

    expected[0] = 0x000000FF;
    expected[1] = 0x0000FF00;
    expected[2] = 0x00FF0000;
    expected[3] = 0xFF000000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 2, 2, -2, 0, 2, 2, -2, expected, __LINE__); // Case 1010 (10) - No Flip. Special Case.

    expected[0] = 0xFF000000;
    expected[1] = 0x00FF0000;
    expected[2] = 0x0000FF00;
    expected[3] = 0x000000FF;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 1, 2, -2, 1, 0, -2, 2, expected, __LINE__); // Case 1001 (9) - Both Flip.

    expected[0] = 0x0000FF00;
    expected[1] = 0x000000FF;
    expected[2] = 0xFF000000;
    expected[3] = 0x00FF0000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             0, 1, 2, -2, 1, 1, -2, -2, expected, __LINE__); // Case 1000 (8) - Horizontal Flip.

    expected[0] = 0x0000FF00;
    expected[1] = 0x000000FF;
    expected[2] = 0xFF000000;
    expected[3] = 0x00FF0000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 0, -2, 2, 0, 0, 2, 2, expected, __LINE__); // Case 0111 (7) - Horizontal Flip.

    expected[0] = 0xFF000000;
    expected[1] = 0x00FF0000;
    expected[2] = 0x0000FF00;
    expected[3] = 0x000000FF;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 0, -2, 2, 0, 1, 2, -2, expected, __LINE__); // Case 0110 (6) - Both Flip.

    expected[0] = 0x000000FF;
    expected[1] = 0x0000FF00;
    expected[2] = 0x00FF0000;
    expected[3] = 0xFF000000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             2, 0, -2, 2, 2, 0, -2, 2, expected, __LINE__); // Case 0101 (5) - No Flip. Special Case.

    expected[0] = 0x00FF0000;
    expected[1] = 0xFF000000;
    expected[2] = 0x000000FF;
    expected[3] = 0x0000FF00;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 0, -2, 2, 1, 1, -2, -2, expected, __LINE__); // Case 0100 (4) - Vertical Flip.

    expected[0] = 0xFF000000;
    expected[1] = 0x00FF0000;
    expected[2] = 0x0000FF00;
    expected[3] = 0x000000FF;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 1, -2, -2, 0, 0, 2, 2, expected, __LINE__); // Case 0011 (3) - Both Flip.

    expected[0] = 0x0000FF00;
    expected[1] = 0x000000FF;
    expected[2] = 0xFF000000;
    expected[3] = 0x00FF0000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 1, -2, -2, 0, 1, 2, -2, expected, __LINE__); // Case 0010 (2) - Horizontal Flip.

    expected[0] = 0x00FF0000;
    expected[1] = 0xFF000000;
    expected[2] = 0x000000FF;
    expected[3] = 0x0000FF00;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             1, 1, -2, -2, 1, 0, -2, 2, expected, __LINE__); // Case 0001 (1) - Vertical Flip.

    expected[0] = 0x000000FF;
    expected[1] = 0x0000FF00;
    expected[2] = 0x00FF0000;
    expected[3] = 0xFF000000;

    check_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                             2, 2, -2, -2, 2, 2, -2, -2, expected, __LINE__); // Case 0000 (0) - No Flip. Special Case.

    DeleteDC(hdcSrc);

    SelectObject(hdcDst, oldDst);
    DeleteObject(bmpDst);
    DeleteDC(hdcDst);

    DeleteDC(hdcScreen);
}

static void test_StretchBlt_TopDownOptions(BOOL SrcTopDown, BOOL DstTopDown)
{
    HBITMAP bmpDst, bmpSrc;
    HBITMAP oldDst, oldSrc;
    HDC hdcScreen, hdcDst, hdcSrc;
    UINT32 *dstBuffer, *srcBuffer;
    BITMAPINFO biDst, biSrc;
    UINT32 expected[256];

    memset(&biDst, 0, sizeof(BITMAPINFO));

    biDst.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    biDst.bmiHeader.biWidth = 2;
    biDst.bmiHeader.biHeight = 2;
    biDst.bmiHeader.biPlanes = 1;
    biDst.bmiHeader.biBitCount = 32;
    biDst.bmiHeader.biCompression = BI_RGB;
    memcpy(&biSrc, &biDst, sizeof(BITMAPINFO));

    if (!SrcTopDown && !DstTopDown)
    {
        biSrc.bmiHeader.biHeight = -2;                // Converts Source bitmap to top down
        biDst.bmiHeader.biHeight = -2;                // Converts Destination bitmap to top down
    }

    if (SrcTopDown)
    {
        biSrc.bmiHeader.biHeight = -2;                // Converts Source bitmap to top down
    }

    if (DstTopDown)
    {
        biDst.bmiHeader.biHeight = -2;                // Converts Destination bitmap to top down
    }

    hdcScreen = CreateCompatibleDC(NULL);
    hdcDst = CreateCompatibleDC(hdcScreen);
    hdcSrc = CreateCompatibleDC(hdcDst);

    bmpSrc = CreateDIBSection(hdcScreen, &biSrc, DIB_RGB_COLORS, (void**)&srcBuffer, NULL, 0);
    oldSrc = SelectObject(hdcSrc, bmpSrc);
    bmpDst = CreateDIBSection(hdcScreen, &biDst, DIB_RGB_COLORS, (void**)&dstBuffer, NULL, 0);
    oldDst = SelectObject(hdcDst, bmpDst);

    srcBuffer[0] = 0x000000FF;   // BLUE
    srcBuffer[1] = 0x0000FF00;   // GREEN
    srcBuffer[2] = 0x00FF0000;   // RED
    srcBuffer[3] = 0xFF000000;   // ALPHA (Opacity)

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;
    }
    else
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 0, 2, 2, 0, 0, 2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1111 (15) - No flip. Just copy.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }
    else
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 0, 2, 2, 0, 1, 2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1110 (14) - Vertical flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }
    else
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 0, 2, 2, 1, 0, -2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1101 (13) - Horizontal flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }
    else
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 0, 2, 2, 1, 1, -2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1100 (12) - Both flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }
    else
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 1, 2, -2, 0, 0, 2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1011 (11) - Vertical Flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;
    }
    else
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 2, 2, -2, 0, 2, 2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1010 (10) - No Flip. Special Case.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }
    else
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 1, 2, -2, 1, 0, -2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1001 (9) - Both Flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }
    else
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            0, 1, 2, -2, 1, 1, -2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 1000 (8) - Horizontal Flip

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }
    else
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            1, 0, -2, 2, 0, 0, 2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0111 (7) - Horizontal Flip

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }
    else
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            1, 0, -2, 2, 0, 1, 2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0110 (6) - Both Flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;
    }
    else
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            2, 0, -2, 2, 2, 0, -2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0101 (5) - No Flip. Special Case.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }
    else
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            1, 0, -2, 2, 1, 1, -2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0100 (4) - Vertical Flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }
    else
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            1, 1, -2, -2, 0, 0, 2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0011 (3) - Both Flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0xFF000000;
        expected[1] = 0x00FF0000;
        expected[2] = 0x0000FF00;
        expected[3] = 0x000000FF;
    }
    else
    {
        expected[0] = 0x0000FF00;
        expected[1] = 0x000000FF;
        expected[2] = 0xFF000000;
        expected[3] = 0x00FF0000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            1, 1, -2, -2, 0, 1, 2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0010 (2) - Horizontal Flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }
    else
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            1, 1, -2, -2, 1, 0, -2, 2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0001 (1) - Vertical Flip.

    if ((SrcTopDown || DstTopDown) && !(SrcTopDown && DstTopDown))
    {
        expected[0] = 0x00FF0000;
        expected[1] = 0xFF000000;
        expected[2] = 0x000000FF;
        expected[3] = 0x0000FF00;

    }
    else
    {
        expected[0] = 0x000000FF;
        expected[1] = 0x0000FF00;
        expected[2] = 0x00FF0000;
        expected[3] = 0xFF000000;
    }

    test_StretchBlt_stretch(hdcDst, hdcSrc, &biDst, dstBuffer, srcBuffer,
                            2, 2, -2, -2, 2, 2, -2, -2, expected, __LINE__, SrcTopDown, DstTopDown); // Case 0000 (0) - No Flip. Special Case.

    SelectObject(hdcSrc, oldSrc);
    DeleteObject(bmpSrc);
    DeleteDC(hdcSrc);

    SelectObject(hdcDst, oldDst);
    DeleteObject(bmpDst);
    DeleteDC(hdcDst);

    DeleteDC(hdcScreen);
}

static LRESULT CALLBACK
UF30_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    if (msg == WM_PAINT) { BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void UF30_Pump(void)
{
    MSG msg;
    DWORD end = GetTickCount() + 60;
    do
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    } while ((LONG)(GetTickCount() - end) < 0);
}

static void test_StretchBlt_NegativeHeight(void)
{
    /* UF-30: a user-mode StretchBlt with a NEGATIVE destination height, the source and
     * destination rectangles the same size (no scaling), used to bugcheck the kernel
     * (PAGE_FAULT_IN_NONPAGED_AREA, memcpy <- DIB_32BPP_BitBltSrcCopy <- IntEngBitBlt <-
     * IntEngStretchBlt). Reproduced on a real, visible top-level window with the mouse
     * pointer over the blit area, matching how it was first found (scursor round 2);
     * see modules/rostests/apitests/gdi32/BitBltPointer.c's removed DrawStretchFlipped. */
    WNDCLASSW wc = {0};
    HWND hwnd;
    HDC hdcWnd, hdcSrc, hdcScreen;
    HBITMAP hbmSrc, hbmOld;
    BITMAPINFO bmi = {{sizeof(BITMAPINFOHEADER), 160, -120, 1, 32, BI_RGB}};
    ULONG *pulSrc;
    int x, y;

    hdcScreen = GetDC(NULL);
    if (GetDeviceCaps(hdcScreen, BITSPIXEL) < 24)
    {
        ReleaseDC(NULL, hdcScreen);
        skip("UF-30: needs at least 24 bits per pixel\n");
        return;
    }
    ReleaseDC(NULL, hdcScreen);

    wc.lpfnWndProc = UF30_WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"UF30StretchBltNegHeightTest";
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(WS_EX_TOPMOST, L"UF30StretchBltNegHeightTest", L"UF-30",
                           WS_POPUP | WS_VISIBLE, 120, 120, 160, 120, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
    {
        skip("UF-30: could not create a window\n");
        return;
    }
    UF30_Pump();

    /* Put the mouse pointer over the blit area, as in the original reproducer, and let the
     * cursor subsystem actually register the move before drawing under it. */
    SetCursorPos(120 + 80, 120 + 60);
    UF30_Pump();
    {
        CURSORINFO ci = {sizeof(ci)};
        GetCursorInfo(&ci);
        ok((ci.flags & CURSOR_SHOWING) != 0, "UF-30: cursor is not showing (flags=%lx)\n", (unsigned long)ci.flags);
    }

    hdcWnd = GetDC(hwnd);
    hdcSrc = CreateCompatibleDC(hdcWnd);
    hbmSrc = CreateDIBSection(hdcWnd, &bmi, DIB_RGB_COLORS, (PVOID*)&pulSrc, NULL, 0);
    hbmOld = SelectObject(hdcSrc, hbmSrc);

    for (y = 0; y < 120; y++)
        for (x = 0; x < 160; x++)
            pulSrc[y * 160 + x] = 0xFF000000 | (y << 8) | x;

    /* A plain blit over the pointer first: this is what lazily allocates win32k's mouse-pointer
     * save/compose bitmaps (MouseSafetyOnDrawStart / IntShowMousePointer), which is what the
     * flipped stretch below needs in place to reach the bugchecking path. */
    BitBlt(hdcWnd, 0, 0, 160, 120, hdcSrc, 0, 0, SRCCOPY);

    /* UF-30 reproducer, verbatim from the bug report. If the kernel bugchecks we never return;
     * surviving with a successful blit is the screen-path regression test. */
    ok(StretchBlt(hdcWnd, 10, 110, 140, -100, hdcSrc, 10, 10, 140, 100, SRCCOPY),
       "UF-30: screen-path StretchBlt with a negative destination height failed\n");

    /* P3 (round 2): surviving the screen blit cannot see silent flip corruption. Drive the fixed
     * 32bpp vertical-flip copy into a real 32bpp destination DIB and read the pixels back. */
    {
        HDC hdcDst = CreateCompatibleDC(hdcWnd);
        BITMAPINFO bmiDst = {{sizeof(BITMAPINFOHEADER), 160, -120, 1, 32, BI_RGB}};
        ULONG *pulDst = NULL;
        HBITMAP hbmDst = CreateDIBSection(hdcWnd, &bmiDst, DIB_RGB_COLORS, (PVOID*)&pulDst, NULL, 0);
        HBITMAP hbmDstOld = SelectObject(hdcDst, hbmDst);

        /* Tag every source pixel with its row so a vertical flip is visible in the pixels. */
        for (y = 0; y < 120; y++)
            for (x = 0; x < 160; x++)
                pulSrc[y * 160 + x] = 0xFF000000 | (ULONG)y;

        /* A same-size, unflipped 32bpp DIB -> 32bpp DIB copy must be byte-exact. */
        memset(pulDst, 0, 160 * 120 * sizeof(ULONG));
        ok(BitBlt(hdcDst, 0, 0, 160, 120, hdcSrc, 0, 0, SRCCOPY), "UF-30: 32bpp BitBlt failed\n");
        GdiFlush();
        ok(pulDst[0] == 0xFF000000 && pulDst[119 * 160] == (0xFF000000 | 119),
           "UF-30: 32bpp straight copy wrong: top=%08lx bottom=%08lx\n",
           (unsigned long)pulDst[0], (unsigned long)pulDst[119 * 160]);

        /* A negative-height StretchBlt flips vertically through the fixed DIB_32BPP path; the row
         * ordering must reverse (top dest row holds a high source row, bottom a low one). */
        memset(pulDst, 0, 160 * 120 * sizeof(ULONG));
        ok(StretchBlt(hdcDst, 0, 120, 160, -120, hdcSrc, 0, 0, 160, 120, SRCCOPY),
           "UF-30: 32bpp negative-height StretchBlt failed\n");
        GdiFlush();
        /* Each source row carries its own index, so a correct vertical flip maps dest row r to
         * source row 119 - r. Check interior rows on both sides of the midline by full value (alpha
         * included): a non-flip, an off-by-one or a degenerate fill that writes one source row into
         * every dest row all fail. The negative-height destination rectangle rounds its top edge the
         * same way unpatched GDI does, so the row-0 edge is outside the check: the crash fix does not
         * change which rows the blit covers, only that the covered rows no longer fault the kernel. */
        ok(pulDst[30 * 160]  == (0xFF000000 | 89) &&
           pulDst[59 * 160]  == (0xFF000000 | 60) &&
           pulDst[60 * 160]  == (0xFF000000 | 59) &&
           pulDst[90 * 160]  == (0xFF000000 | 29),
           "UF-30: 32bpp vertical flip did not reverse interior rows: "
           "r30=%08lx r59=%08lx r60=%08lx r90=%08lx (want FF000059 FF00003C FF00003B FF00001D)\n",
           (unsigned long)pulDst[30 * 160], (unsigned long)pulDst[59 * 160],
           (unsigned long)pulDst[60 * 160], (unsigned long)pulDst[90 * 160]);

        SelectObject(hdcDst, hbmDstOld);
        DeleteObject(hbmDst);
        DeleteDC(hdcDst);
    }

    SelectObject(hdcSrc, hbmOld);
    DeleteObject(hbmSrc);
    DeleteDC(hdcSrc);
    ReleaseDC(hwnd, hdcWnd);
    DestroyWindow(hwnd);
}

START_TEST(StretchBlt)
{
    trace("\n\n## Start of generalized StretchBlt tests.\n\n");
    test_StretchBlt();

    trace("\n\n## Start of UF-30 negative destination height test.\n\n");
    test_StretchBlt_NegativeHeight();

    trace("\n\n## Start of source top-down and destination top-down tests.\n\n");
    test_StretchBlt_TopDownOptions(TRUE, TRUE);

    trace("\n\n## Start of source top-down and destination bottom-up tests.\n\n");
    test_StretchBlt_TopDownOptions(TRUE, FALSE);

    trace("\n\n## Start of source bottom-up and destination top-down tests.\n\n");
    test_StretchBlt_TopDownOptions(FALSE, TRUE);

    trace("\n\n## Start of source bottom-up and destination bottom-up tests.\n\n");
    test_StretchBlt_TopDownOptions(FALSE, FALSE);
}
