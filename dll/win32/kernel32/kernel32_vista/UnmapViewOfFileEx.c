/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     UnmapViewOfFileEx (Windows 10 version 1703 and later; the flags came with UnmapViewOfFile2)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2: the flags are checked before the address, a flag outside the two known ones is
 * ERROR_INVALID_PARAMETER, and the flag that keeps a placeholder is ERROR_INVALID_ADDRESS for a view that is
 * not one (every view here), which then stays mapped.
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

#define UNMAP_WITH_TRANSIENT_BOOST 0x1
#define UNMAP_PRESERVE_PLACEHOLDER 0x2

BOOL
WINAPI
UnmapViewOfFileEx(
    _In_ PVOID BaseAddress,
    _In_ ULONG UnmapFlags)
{
    if (UnmapFlags & ~(UNMAP_WITH_TRANSIENT_BOOST | UNMAP_PRESERVE_PLACEHOLDER))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* There are no placeholders, so there is none to preserve */
    if (UnmapFlags & UNMAP_PRESERVE_PLACEHOLDER)
    {
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }

    return UnmapViewOfFile(BaseAddress);
}
