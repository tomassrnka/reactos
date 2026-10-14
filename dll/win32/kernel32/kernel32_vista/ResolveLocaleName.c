/*
 * Locale support
 *
 * Copyright 1995 Martin von Loewis
 * Copyright 1998 David Lee Lambert
 * Copyright 2000 Julio César Gázquez
 * Copyright 2003 Jon Griffiths
 * Copyright 2005 Dmitry Timoshkov
 * Copyright 2002, 2019 Alexandre Julliard
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
 */

#include "k32_vista.h"

/* Wine's locale fallback algorithm, using ReactOS's LCID/name database. */
INT WINAPI
ResolveLocaleName(LPCWSTR Name, LPWSTR Buffer, INT Length)
{
    WCHAR Candidate[LOCALE_NAME_MAX_LENGTH], Resolved[LOCALE_NAME_MAX_LENGTH];
    WCHAR *Separator;
    LCID Locale;
    INT Required;
    DWORD LastError = GetLastError();

    if (Buffer && Length < 0)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (Name && !wcscmp(Name, LOCALE_NAME_SYSTEM_DEFAULT))
        Locale = GetSystemDefaultLCID();
    else
        Locale = LocaleNameToLCID(Name, 0);

    if (!Locale && Name)
    {
        lstrcpynW(Candidate, Name, ARRAYSIZE(Candidate));
        while (!Locale)
        {
            Separator = Candidate + wcslen(Candidate);
            while (Separator > Candidate && Separator[-1] != L'-' && Separator[-1] != L'_')
                --Separator;
            if (Separator <= Candidate + 1) break;
            Separator[-1] = 0;
            Locale = LocaleNameToLCID(Candidate, 0);
        }
    }
    if (!Locale) Locale = LOCALE_INVARIANT;
    Required = LCIDToLocaleName(Locale, Resolved, ARRAYSIZE(Resolved), 0);
    if (!Required) return 0;
    if (!Buffer || !Length)
    {
        SetLastError(LastError);
        return Required;
    }
    lstrcpynW(Buffer, Resolved, Length);
    if (Required > Length)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    SetLastError(LastError);
    return Required;
}
