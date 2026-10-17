/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     LoadStringA and LoadStringW of kernel32, the home of the api-ms-win-core-libraryloader contract
 * COPYRIGHT:   Copyright 1993 Robert J. Amstadt
 *              Copyright 1996 Marcus Meissner
 *              Copyright 2004 Jon Griffiths
 *              Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The functions follow LoadStringW and LoadStringA of Wine's kernelbase (dlls/kernelbase/string.c).
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

/* winuser.h is not part of the kernel32 headers */
#define STRING_BLOCK_NAME(uID) ((LPCWSTR)(ULONG_PTR)((LOWORD(uID) >> 4) + 1))
#define RESOURCE_TYPE_STRING ((LPCWSTR)(ULONG_PTR)6)

INT
WINAPI
LoadStringW(
    _In_opt_ HINSTANCE hInstance,
    _In_ UINT uID,
    _Out_writes_to_opt_(nBufferMax, return + 1) LPWSTR lpBuffer,
    _In_ INT nBufferMax)
{
    HGLOBAL hMem;
    HRSRC hRsrc;
    WCHAR *p;
    UINT i;

    if (!lpBuffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* The strings of a table share a block of 16, named by the identifier divided by 16, plus one */
    hRsrc = FindResourceW(hInstance, STRING_BLOCK_NAME(uID), RESOURCE_TYPE_STRING);
    if (!hRsrc || !(hMem = LoadResource(hInstance, hRsrc)))
    {
        if (nBufferMax > 0)
            lpBuffer[0] = 0;
        return 0;
    }

    p = LockResource(hMem);
    for (i = 0; i < (uID & 0x000f); i++)
        p += *p + 1;

    /* A length of zero asks for a pointer to the string in the resource, passed through lpBuffer */
    if (nBufferMax == 0)
    {
        *((LPWSTR *)lpBuffer) = p + 1;
        return *p;
    }

    i = min(nBufferMax - 1, *p);
    memcpy(lpBuffer, p + 1, i * sizeof(WCHAR));
    lpBuffer[i] = 0;
    return i;
}

INT
WINAPI
LoadStringA(
    _In_opt_ HINSTANCE hInstance,
    _In_ UINT uID,
    _Out_writes_to_opt_(nBufferMax, return + 1) LPSTR lpBuffer,
    _In_ INT nBufferMax)
{
    ULONG Length = 0;
    const WCHAR *p = NULL;
    HGLOBAL hMem;
    HRSRC hRsrc;

    hRsrc = FindResourceW(hInstance, STRING_BLOCK_NAME(uID), RESOURCE_TYPE_STRING);
    if (hRsrc && (hMem = LoadResource(hInstance, hRsrc)))
    {
        UINT i = uID & 0x000f;

        p = LockResource(hMem);
        while (i--)
            p += *p + 1;
    }

    /*
     * A size of zero is -1 for a string that has characters (measured on Windows 10); a string that is
     * empty or not there is zero, and the buffer gets its terminator all the same.
     */
    if (!nBufferMax && p && *p)
        return -1;

    if (!lpBuffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!nBufferMax)
    {
        lpBuffer[0] = 0;
        return 0;
    }

    if (p)
        RtlUnicodeToMultiByteN(lpBuffer, nBufferMax - 1, &Length, p + 1, *p * sizeof(WCHAR));
    lpBuffer[Length] = 0;
    return Length;
}
