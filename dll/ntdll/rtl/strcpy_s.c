/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     strcpy_s of ntdll
 * COPYRIGHT:   Copyright 2000 Francois Gouget
 *              Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * After strcpy_s of Wine's ntdll (dlls/ntdll/string.c). The results, including the byte of the destination
 * that is zeroed when the string does not fit, are the ones of ntdll on Windows Server 2008 R2 (measured).
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

#define STRCPY_S_EINVAL 22
#define STRCPY_S_ERANGE 34

int
__cdecl
strcpy_s(
    _Out_writes_z_(len) char *dst,
    _In_ size_t len,
    _In_z_ const char *src)
{
    size_t i;

    if (!dst || !len)
        return STRCPY_S_EINVAL;

    if (!src)
    {
        *dst = 0;
        return STRCPY_S_EINVAL;
    }

    for (i = 0; i < len; i++)
    {
        if (!(dst[i] = src[i]))
            return 0;
    }

    *dst = 0;
    return STRCPY_S_ERANGE;
}
