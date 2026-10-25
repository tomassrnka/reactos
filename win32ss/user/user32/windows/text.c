/*
 * PROJECT:     ReactOS user32.dll
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Text processing functions
 * COPYRIGHT:   Copyright 2001-2004 Casper S. Hornstrup <chorns@users.sourceforge.net>
 */

#include <user32.h>

static WORD
GetC1Type(WCHAR Ch)
{
    WORD CharType;

    if (! GetStringTypeW(CT_CTYPE1, &Ch, 1, &CharType))
    {
        return 0;
    }

    return CharType;
}

/*
 * @implemented
 */
LPSTR
WINAPI
CharLowerA(LPSTR str)
{
    if (!HIWORD(str))
    {
        char ch = LOWORD(str);
        CharLowerBuffA( &ch, 1 );
        return (LPSTR)(UINT_PTR)(BYTE)ch;
    }

    _SEH2_TRY
    {
        CharLowerBuffA( str, strlen(str) );
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        _SEH2_YIELD(return NULL);
    }
    _SEH2_END;

    return str;
}

/*
 * @implemented
 */
DWORD
WINAPI
CharLowerBuffA(LPSTR str, DWORD len)
{
    DWORD lenW;
    WCHAR *strW;
    if (!str) return 0; /* YES */

    lenW = MultiByteToWideChar(CP_ACP, 0, str, len, NULL, 0);
    strW = HeapAlloc(GetProcessHeap(), 0, lenW * sizeof(WCHAR));
    if (strW) {
        MultiByteToWideChar(CP_ACP, 0, str, len, strW, lenW);
        CharLowerBuffW(strW, lenW);
        len = WideCharToMultiByte(CP_ACP, 0, strW, lenW, str, len, NULL, NULL);
        HeapFree(GetProcessHeap(), 0, strW);
        return len;
    }
    return 0;
}

/*
 * @implemented
 */
DWORD
WINAPI
CharLowerBuffW(LPWSTR str, DWORD len)
{
    DWORD ret = len;
    if (!str) return 0; /* YES */
    for (; len; len--, str++) *str = towlower(*str);
    return ret;
}

/*
 * @implemented
 */
LPWSTR
WINAPI
CharLowerW(LPWSTR x)
{
    if (HIWORD(x)) return strlwrW(x);
    else return (LPWSTR)((UINT_PTR)tolowerW(LOWORD(x)));
}

/*
 * Combining (non-spacing) marks of the BMP, as used by Vista+ CharPrevW/CharNextW.
 * kernel32's GetStringTypeW(CT_CTYPE3) is only a semi-stub and does not report C3_NONSPACING,
 * so a compact range table is used instead.
 */
static BOOL
IsNonSpacingMarkW(WCHAR ch)
{
    static const WCHAR Ranges[][2] =
    {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
        {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
        {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
        {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0902}, {0x093C, 0x093C},
        {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957}, {0x0E31, 0x0E31},
        {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
        {0x20D0, 0x20FF}, {0x3099, 0x309A}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    };
    UINT i;

    for (i = 0; i < _countof(Ranges); i++)
    {
        if (ch >= Ranges[i][0] && ch <= Ranges[i][1])
            return TRUE;
    }
    return FALSE;
}

/*
 * @implemented
 */
LPWSTR
WINAPI
CharPrevW(LPCWSTR start, LPCWSTR x)
{
    if (x > start)
    {
        --x;
        /* Vista+ treats combining (non-spacing) marks as part of the preceding character */
        if (NtCurrentPeb()->OSMajorVersion >= 6)
        {
            /* the character before x is always read, even if it is the first one */
            while (IsNonSpacingMarkW(*x) && x > start)
                --x;
        }
        return (LPWSTR)x;
    }
    else return (LPWSTR)x;
}

/*
 * @implemented
 */
LPSTR
WINAPI
CharNextA(LPCSTR ptr)
{
    if (!*ptr) return (LPSTR)ptr;
    if (IsDBCSLeadByte(ptr[0]) && ptr[1]) return (LPSTR)(ptr + 2);
    return (LPSTR)(ptr + 1);
}

/*
 * @implemented
 */
LPSTR
WINAPI
CharNextExA(WORD codepage, LPCSTR ptr, DWORD flags)
{
    if (!*ptr) return (LPSTR)ptr;
    if (IsDBCSLeadByteEx(codepage, ptr[0]) && ptr[1]) return (LPSTR)(ptr + 2);
    return (LPSTR)(ptr + 1);
}

/*
 * @implemented
 */
LPWSTR
WINAPI
CharNextW(LPCWSTR x)
{
    if (*x)
    {
        x++;
        /* Vista+ skips the combining (non-spacing) marks following a character */
        if (NtCurrentPeb()->OSMajorVersion >= 6)
        {
            while (*x && IsNonSpacingMarkW(*x))
                x++;
        }
    }
    return (LPWSTR)x;
}

/*
 * @implemented
 */
LPSTR
WINAPI
CharPrevA(LPCSTR start, LPCSTR ptr)
{
    if (ptr > start)
    {
        --ptr;
        if (gpsi->dwSRVIFlags & SRVINFO_DBCSENABLED)
        {
            LPCSTR ch;
            BOOL dbl = FALSE;

            for (ch = ptr - 1; ch >= start; --ch)
            {
                if (!IsDBCSLeadByte(*ch))
                    break;

                dbl = !dbl;
            }
            if (dbl) --ptr;
        }
    }
    return (LPSTR)ptr;
}

/*
 * @implemented
 */
LPSTR
WINAPI
CharPrevExA(WORD codepage, LPCSTR start, LPCSTR ptr, DWORD flags)
{
    if (ptr > start)
    {
        LPCSTR ch;
        BOOL dbl = FALSE;

        --ptr;
        for (ch = ptr - 1; ch >= start; --ch)
        {
            if (!IsDBCSLeadByteEx(codepage, *ch))
                break;

            dbl = !dbl;
        }
        if (dbl) --ptr;
    }
    return (LPSTR)ptr;
}

/*
 * @implemented
 */
BOOL
WINAPI
CharToOemA(LPCSTR s, LPSTR d)
{
    if (!s || !d) return FALSE;
    return CharToOemBuffA(s, d, strlen(s) + 1);
}

/*
 * @implemented
 */
BOOL
WINAPI
CharToOemBuffA(LPCSTR s, LPSTR d, DWORD len)
{
    WCHAR* bufW;

    if ( !s || !d ) return FALSE;

    bufW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (bufW) {
	    MultiByteToWideChar(CP_ACP, 0, s, len, bufW, len);
        WideCharToMultiByte(CP_OEMCP, 0, bufW, len, d, len, NULL, NULL);
	    HeapFree(GetProcessHeap(), 0, bufW);
    }
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
CharToOemBuffW(LPCWSTR s, LPSTR d, DWORD len)
{
    if (!s || !d)
        return FALSE;
    WideCharToMultiByte(CP_OEMCP, 0, s, len, d, len, NULL, NULL);
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
CharToOemW(LPCWSTR s, LPSTR d)
{
    if ( !s || !d ) return FALSE;
    return CharToOemBuffW(s, d, wcslen(s) + 1);
}

/*
 * @implemented
 */
LPSTR WINAPI CharUpperA(LPSTR str)
{
    if (!HIWORD(str))
    {
        char ch = LOWORD(str);
        CharUpperBuffA( &ch, 1 );
        return (LPSTR)(UINT_PTR)(BYTE)ch;
    }

    _SEH2_TRY
    {
        CharUpperBuffA( str, strlen(str) );
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        _SEH2_YIELD(return NULL);
    }
    _SEH2_END;

    return str;
}

/*
 * @implemented
 */
DWORD
WINAPI
CharUpperBuffA(LPSTR str, DWORD len)
{
    DWORD lenW;
    WCHAR* strW;
    if (!str) return 0; /* YES */

    lenW = MultiByteToWideChar(CP_ACP, 0, str, len, NULL, 0);
    strW = HeapAlloc(GetProcessHeap(), 0, lenW * sizeof(WCHAR));
    if (strW) {
        MultiByteToWideChar(CP_ACP, 0, str, len, strW, lenW);
        CharUpperBuffW(strW, lenW);
        len = WideCharToMultiByte(CP_ACP, 0, strW, lenW, str, len, NULL, NULL);
        HeapFree(GetProcessHeap(), 0, strW);
        return len;
    }
    return 0;
}

/*
 * @implemented
 */
DWORD
WINAPI
CharUpperBuffW(LPWSTR str, DWORD len)
{
    DWORD ret = len;
    if (!str) return 0; /* YES */
    for (; len; len--, str++) *str = towupper(*str);
    return ret;
}

/*
 * @implemented
 */
LPWSTR
WINAPI
CharUpperW(LPWSTR x)
{
    if (HIWORD(x)) return struprW(x);
    else return (LPWSTR)((UINT_PTR)toupperW(LOWORD(x)));
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharAlphaA(CHAR Ch)
{
    WCHAR WCh;

    MultiByteToWideChar(CP_ACP, 0, &Ch, 1, &WCh, 1);
    return IsCharAlphaW(WCh);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharAlphaNumericA(CHAR Ch)
{
    WCHAR WCh;

    MultiByteToWideChar(CP_ACP, 0, &Ch, 1, &WCh, 1);
    return IsCharAlphaNumericW(WCh);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharAlphaNumericW(WCHAR Ch)
{
    return (GetC1Type(Ch) & (C1_ALPHA|C1_DIGIT)) != 0;
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharAlphaW(WCHAR Ch)
{
    return (GetC1Type(Ch) & C1_ALPHA) != 0;
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharLowerA(CHAR Ch)
{
    WCHAR WCh;

    MultiByteToWideChar(CP_ACP, 0, &Ch, 1, &WCh, 1);
    return IsCharLowerW(WCh);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharLowerW(WCHAR Ch)
{
    return (GetC1Type(Ch) & C1_LOWER) != 0;
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharUpperA(CHAR Ch)
{
    WCHAR WCh;

    MultiByteToWideChar(CP_ACP, 0, &Ch, 1, &WCh, 1);
    return IsCharUpperW(WCh);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsCharUpperW(WCHAR Ch)
{
    return (GetC1Type(Ch) & C1_UPPER) != 0;
}

/*
 * @implemented
 */
BOOL
WINAPI
OemToCharA(LPCSTR s, LPSTR d)
{
    if ( !s || !d ) return FALSE;
    return OemToCharBuffA(s, d, strlen(s) + 1);
}

/*
 * @implemented
 */
BOOL WINAPI OemToCharBuffA(LPCSTR s, LPSTR d, DWORD len)
{
    WCHAR* bufW;

    if ( !s || !d ) return FALSE;

    bufW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (bufW) {
        MultiByteToWideChar(CP_OEMCP, 0, s, len, bufW, len);
	    WideCharToMultiByte(CP_ACP, 0, bufW, len, d, len, NULL, NULL);
	    HeapFree(GetProcessHeap(), 0, bufW);
    }
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
OemToCharBuffW(LPCSTR s, LPWSTR d, DWORD len)
{
    if ( !s || !d ) return FALSE;
    MultiByteToWideChar(CP_OEMCP, 0, s, len, d, len);
    return TRUE;
}

/*
 * @implemented
 */
BOOL WINAPI OemToCharW(LPCSTR s, LPWSTR d)
{
    if ( !s || !d ) return FALSE;
    return OemToCharBuffW(s, d, strlen(s) + 1);
}

/* EOF */
