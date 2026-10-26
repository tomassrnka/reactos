/*
 * wsprintf functions
 *
 * Copyright 1996 Alexandre Julliard
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * NOTE: On Windows (NT 6+) the shlwapi wnsprintf family is a thin wrapper
 * around ntdll's _vsnprintf/_vsnwprintf. We match that here. The previous
 * hand-rolled format parser (synced from Wine Staging 3.3) did not consume
 * repeated length modifiers (e.g. "%ll", "%I64ll") and therefore mis-aligned
 * the following varargs, which crashed on specs such as "%llu|%s" once the
 * NT6-gated shlwapi:string tests started exercising them (CORE / VERBUMP B7).
 */

#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "shlwapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(string);

static int (__cdecl *ntdll__vsnprintf)(char *str, size_t len, const char *format, __ms_va_list args);
static int (__cdecl *ntdll__vsnwprintf)(WCHAR *str, size_t len, const WCHAR *format, __ms_va_list args);

static void SHLWAPI_LoadNtdllPrintf(void)
{
    if (!ntdll__vsnprintf || !ntdll__vsnwprintf)
    {
        HMODULE hntdll = GetModuleHandleW(L"ntdll.dll");
        if (hntdll)
        {
            ntdll__vsnprintf = (void *)GetProcAddress(hntdll, "_vsnprintf");
            ntdll__vsnwprintf = (void *)GetProcAddress(hntdll, "_vsnwprintf");
        }
    }
}

/***********************************************************************
 *           wvnsprintfA   (SHLWAPI.@)
 *
 * Print formatted output to a string, up to a maximum number of chars.
 *
 * PARAMS
 * buffer [O] Destination for output string
 * maxlen [I] Maximum number of characters to write
 * spec   [I] Format string
 *
 * RETURNS
 *  Success: The number of characters written.
 *  Failure: -1.
 */
INT WINAPI wvnsprintfA( LPSTR buffer, INT maxlen, LPCSTR spec, __ms_va_list args )
{
    INT ret;

    TRACE("%p %u %s\n", buffer, maxlen, debugstr_a(spec));

    if (!maxlen) return -1;
    if (maxlen < 0)
    {
        buffer[0] = 0;
        return -1;
    }

    SHLWAPI_LoadNtdllPrintf();
    ret = ntdll__vsnprintf( buffer, maxlen, spec, args );

    /* wnsprintf always NUL-terminates its output. ntdll's _vsnprintf returns
     * maxlen (not -1) and writes no terminator when the output exactly fills
     * the buffer, so treat both the exact-fit and overflow cases as truncation:
     * terminate the buffer and return -1, matching the historical parser. */
    if (ret < 0 || ret >= maxlen)
    {
        buffer[maxlen - 1] = 0;
        ret = -1;
    }
    return ret;
}


/***********************************************************************
 *           wvnsprintfW   (SHLWAPI.@)
 *
 * See wvnsprintfA.
 */
INT WINAPI wvnsprintfW( LPWSTR buffer, INT maxlen, LPCWSTR spec, __ms_va_list args )
{
    INT ret;

    TRACE("%p %u %s\n", buffer, maxlen, debugstr_w(spec));

    if (!maxlen) return -1;
    if (maxlen < 0)
    {
        buffer[0] = 0;
        return -1;
    }

    SHLWAPI_LoadNtdllPrintf();
    ret = ntdll__vsnwprintf( buffer, maxlen, spec, args );

    /* See wvnsprintfA: always NUL-terminate, report truncation on exact-fit. */
    if (ret < 0 || ret >= maxlen)
    {
        buffer[maxlen - 1] = 0;
        ret = -1;
    }
    return ret;
}


/*************************************************************************
 *           wnsprintfA   (SHLWAPI.@)
 *
 * Print formatted output to a string, up to a maximum number of chars.
 *
 * PARAMS
 * lpOut      [O] Destination for output string
 * cchLimitIn [I] Maximum number of characters to write
 * lpFmt      [I] Format string
 *
 * RETURNS
 *  Success: The number of characters written.
 *  Failure: -1.
 */
int WINAPIV wnsprintfA(LPSTR lpOut, int cchLimitIn, LPCSTR lpFmt, ...)
{
    __ms_va_list valist;
    INT res;

    __ms_va_start( valist, lpFmt );
    res = wvnsprintfA( lpOut, cchLimitIn, lpFmt, valist );
    __ms_va_end( valist );
    return res;
}


/*************************************************************************
 *           wnsprintfW   (SHLWAPI.@)
 *
 * See wnsprintfA.
 */
int WINAPIV wnsprintfW(LPWSTR lpOut, int cchLimitIn, LPCWSTR lpFmt, ...)
{
    __ms_va_list valist;
    INT res;

    __ms_va_start( valist, lpFmt );
    res = wvnsprintfW( lpOut, cchLimitIn, lpFmt, valist );
    __ms_va_end( valist );
    return res;
}
