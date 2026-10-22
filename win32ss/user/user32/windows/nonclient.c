/*
 * ReactOS User32 Library
 * - Window non-client area management
 *
 * Copyright (C) 2003 ReactOS Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <user32.h>

WINE_DEFAULT_DEBUG_CHANNEL(user32);

#define HASSIZEGRIP(Style, ExStyle, ParentStyle, WindowRect, ParentClientRect) \
            ((!(Style & WS_CHILD) && (Style & WS_THICKFRAME) && !(Style & WS_MAXIMIZE))  || \
             ((Style & WS_CHILD) && (ParentStyle & WS_THICKFRAME) && !(ParentStyle & WS_MAXIMIZE) && \
             (WindowRect.right - WindowRect.left == ParentClientRect.right) && \
             (WindowRect.bottom - WindowRect.top == ParentClientRect.bottom)))


/* PRIVATE FUNCTIONS **********************************************************/

BOOL
UserHasWindowEdge(DWORD Style, DWORD ExStyle)
{
   if (Style & WS_MINIMIZE)
      return TRUE;
   if (ExStyle & WS_EX_DLGMODALFRAME)
      return TRUE;
   if (ExStyle & WS_EX_STATICEDGE)
      return FALSE;
   if (Style & WS_THICKFRAME)
      return TRUE;
   Style &= WS_CAPTION;
   if (Style == WS_DLGFRAME || Style == WS_CAPTION)
      return TRUE;
   return FALSE;
}

/* A metric at the given DPI, or at the DPI of the system for 0 */
static int
UserGetMetric(int Index, UINT Dpi)
{
   return Dpi ? GetSystemMetricsForDpi(Index, Dpi) : GetSystemMetrics(Index);
}

static VOID
UserGetWindowBordersForDpi(DWORD Style, DWORD ExStyle, SIZE *Size, BOOL WithClient, UINT Dpi)
{
   DWORD Border = 0;

   if (UserHasWindowEdge(Style, ExStyle))
      Border += 2;
   else if (ExStyle & WS_EX_STATICEDGE)
      Border += 1;
   if ((ExStyle & WS_EX_CLIENTEDGE) && WithClient)
      Border += 2;
   if (Style & WS_CAPTION || ExStyle & WS_EX_DLGMODALFRAME)
      Border ++;
   Size->cx = Size->cy = Border;
   if ((Style & WS_THICKFRAME) && !(Style & WS_MINIMIZE))
   {
      Size->cx += UserGetMetric(SM_CXFRAME, Dpi) - UserGetMetric(SM_CXDLGFRAME, Dpi);
      Size->cy += UserGetMetric(SM_CYFRAME, Dpi) - UserGetMetric(SM_CYDLGFRAME, Dpi);
   }
   Size->cx *= UserGetMetric(SM_CXBORDER, Dpi);
   Size->cy *= UserGetMetric(SM_CYBORDER, Dpi);
}

VOID
UserGetWindowBorders(DWORD Style, DWORD ExStyle, SIZE *Size, BOOL WithClient)
{
   UserGetWindowBordersForDpi(Style, ExStyle, Size, WithClient, 0);
}

/*
 RealUserDrawCaption: This function is passed through RegisterUserApiHook to uxtheme
                      to call it when the classic caption is needed to be drawn.
 */
BOOL WINAPI
RealUserDrawCaption(HWND hWnd, HDC hDC, LPCRECT lpRc, UINT uFlags)
{
    ERR("Real DC flags %08x\n",uFlags);
    return NtUserDrawCaption(hWnd, hDC, lpRc, uFlags);
}


/* PUBLIC FUNCTIONS ***********************************************************/

static BOOL
UserAdjustWindowRectForDpi(LPRECT lpRect,
                           DWORD dwStyle,
                           BOOL bMenu,
                           DWORD dwExStyle,
                           UINT Dpi)
{
   SIZE BorderSize;

   if (bMenu)
   {
      lpRect->top -= UserGetMetric(SM_CYMENU, Dpi);
   }
   if ((dwStyle & WS_CAPTION) == WS_CAPTION)
   {
      if (dwExStyle & WS_EX_TOOLWINDOW)
         lpRect->top -= UserGetMetric(SM_CYSMCAPTION, Dpi);
      else
         lpRect->top -= UserGetMetric(SM_CYCAPTION, Dpi);
   }
   UserGetWindowBordersForDpi(dwStyle, dwExStyle, &BorderSize, TRUE, Dpi);
   InflateRect(
      lpRect,
      BorderSize.cx,
      BorderSize.cy);

   return TRUE;
}

BOOL WINAPI
RealAdjustWindowRectEx(LPRECT lpRect,
                       DWORD dwStyle,
                       BOOL bMenu,
                       DWORD dwExStyle)
{
   return UserAdjustWindowRectForDpi(lpRect, dwStyle, bMenu, dwExStyle, 0);
}

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
AdjustWindowRectEx(LPRECT lpRect,
		   DWORD dwStyle,
		   BOOL bMenu,
		   DWORD dwExStyle)
{
   BOOL Hook, Ret = FALSE;

   LoadUserApiHook();

   Hook = BeginIfHookedUserApiHook();

     /* Bypass SEH and go direct. */
   if (!Hook) return RealAdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);

   _SEH2_TRY
   {
      Ret = guah.AdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
   }
   _SEH2_END;

   EndUserApiHook();

   return Ret;
}


/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
AdjustWindowRect(LPRECT lpRect,
		 DWORD dwStyle,
		 BOOL bMenu)
{
   return AdjustWindowRectEx(lpRect, dwStyle, bMenu, 0);
}

// Enabling this will cause captions to draw smoother, but slower:
#define DOUBLE_BUFFER_CAPTION

/*
 * @implemented
 */
BOOL WINAPI
DrawCaption(HWND hWnd, HDC hDC, LPCRECT lprc, UINT uFlags)
{
   BOOL Hook, Ret = FALSE;

   LoadUserApiHook();

   Hook = BeginIfHookedUserApiHook();

   /* Bypass SEH and go direct. */
   if (!Hook) return NtUserDrawCaption(hWnd, hDC, lprc, uFlags);

   _SEH2_TRY
   {
      Ret = guah.DrawCaption(hWnd, hDC, lprc, uFlags);
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
   }
   _SEH2_END;

   EndUserApiHook();

   return Ret;
}

/*
 * @implemented
 */
BOOL
WINAPI
DrawCaptionTempW(
		 HWND        hWnd,
		 HDC         hDC,
		 const RECT *rect,
		 HFONT       hFont,
		 HICON       hIcon,
		 LPCWSTR     str,
		 UINT        uFlags
		 )
{
   UNICODE_STRING Text = {0};
   RtlInitUnicodeString(&Text, str);
   return NtUserDrawCaptionTemp(hWnd, hDC, rect, hFont, hIcon, &Text, uFlags);
}

/*
 * @implemented
 */
BOOL
WINAPI
DrawCaptionTempA(
		 HWND        hwnd,
		 HDC         hdc,
		 const RECT *rect,
		 HFONT       hFont,
		 HICON       hIcon,
		 LPCSTR      str,
		 UINT        uFlags
		 )
{
  LPWSTR strW;
  INT len;
  BOOL ret = FALSE;

  if (!(uFlags & DC_TEXT) || !str)
    return DrawCaptionTempW(hwnd, hdc, rect, hFont, hIcon, NULL, uFlags);

  len = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, 0);
  if ((strW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR))))
  {
    MultiByteToWideChar(CP_ACP, 0, str, -1, strW, len );
    ret = DrawCaptionTempW(hwnd, hdc, rect, hFont, hIcon, strW, uFlags);
    HeapFree(GetProcessHeap(), 0, strW);
  }
  return ret;
}

BOOL
WINAPI
AdjustWindowRectExForDpi(LPRECT lpRect,
                         DWORD dwStyle,
                         BOOL bMenu,
                         DWORD dwExStyle,
                         UINT dpi)
{
   DWORD Error = GetLastError();
   BOOL Ret;

   if (!lpRect || !dpi)
   {
      SetLastError(ERROR_INVALID_PARAMETER);
      return FALSE;
   }

   /* At the DPI of the system the classic metrics are the ones that the theme hook, if any, adjusts */
   if (dpi == GetDpiForSystem())
      Ret = AdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);
   else
      Ret = UserAdjustWindowRectForDpi(lpRect, dwStyle, bMenu, dwExStyle, dpi);

   /* A call that succeeds leaves the last error as it was */
   if (Ret)
      SetLastError(Error);
   return Ret;
}
