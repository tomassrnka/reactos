/*
 * PROJECT:     ReactOS Console Server
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Bounded length of clipboard text (header-only, shared with apitests)
 */

#pragma once

/*
 * Clipboard text is supplied by an arbitrary process and is not guaranteed to be
 * NUL-terminated. Returns the number of WCHARs in the string, never reading more
 * than cbAvailable bytes (the GlobalSize of the block) from pszText.
 */
static inline SIZE_T
ConGetBoundedTextLength(const WCHAR *pszText, SIZE_T cbAvailable)
{
    SIZE_T cchMax = cbAvailable / sizeof(WCHAR);
    SIZE_T Length = 0;

    while (Length < cchMax && pszText[Length] != UNICODE_NULL)
        ++Length;

    return Length;
}
