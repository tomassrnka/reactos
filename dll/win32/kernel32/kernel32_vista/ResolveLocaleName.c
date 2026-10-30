/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Implementation of ResolveLocaleName
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "k32_vista.h"
#include <ndk/rtlfuncs.h>

#define NDEBUG
#include <debug.h>

static
BOOL
IsValidLocaleNameChar(
    _In_ WCHAR Char)
{
    return (Char >= L'a' && Char <= L'z') ||
           (Char >= L'A' && Char <= L'Z') ||
           (Char >= L'0' && Char <= L'9') ||
           Char == L'-' || Char == L'_';
}

/* The name of a specific locale, or FALSE if the LCID is neutral or unknown */
static
BOOL
GetSpecificLocaleName(
    _In_ LCID Lcid,
    _Out_writes_(LOCALE_NAME_MAX_LENGTH) PWSTR Buffer)
{
    UNICODE_STRING Name;

    Name.Buffer = Buffer;
    Name.Length = 0;
    Name.MaximumLength = LOCALE_NAME_MAX_LENGTH * sizeof(WCHAR);
    return NT_SUCCESS(RtlLcidToLocaleName(Lcid, &Name, 0, FALSE));
}

/* The specific locale that a neutral locale stands for */
static
BOOL
ResolveCandidate(
    _In_ PCWSTR Candidate,
    _Out_writes_(LOCALE_NAME_MAX_LENGTH) PWSTR Resolved)
{
    PCWSTR Alias = BasepLocaleNameAlias(Candidate);
    LCID Lcid;

    if (Alias)
        Candidate = Alias;

    if (!NT_SUCCESS(RtlLocaleNameToLcid(Candidate, &Lcid, RTL_LOCALE_ALLOW_NEUTRAL_NAMES)))
        return FALSE;

    /* An alternate sort ("de-DE_phoneb") resolves to the locale itself */
    Lcid = MAKELCID(LANGIDFROMLCID(Lcid), SORT_DEFAULT);

    if (Lcid == LOCALE_INVARIANT)
    {
        Resolved[0] = UNICODE_NULL;
        return TRUE;
    }

    if (GetSpecificLocaleName(Lcid, Resolved))
    {
        /* The traditional sort of Spanish is a sort of es-ES to Windows 10 */
        SIZE_T Length = wcslen(Resolved);

        if (Length > 7 && _wcsicmp(Resolved + Length - 7, L"_tradnl") == 0)
            Resolved[Length - 7] = UNICODE_NULL;
        return TRUE;
    }

    Lcid = BasepNeutralLcidToSpecific(Lcid);
    return Lcid && GetSpecificLocaleName(Lcid, Resolved);
}

/*
 * @implemented
 *
 * Finds the specific locale that best matches a name: the name itself when it is a specific locale
 * (without an alternate sort), the specific locale a neutral name stands for ("en" -> "en-US"), else
 * the same for the name with its last tags cut off ("en-XY" -> "en-US"). A name nothing matches
 * resolves to the invariant locale ("").
 */
INT
WINAPI
ResolveLocaleName(
    _In_opt_ LPCWSTR lpNameToResolve,
    _Out_writes_opt_(cchLocaleName) LPWSTR lpLocaleName,
    _In_ INT cchLocaleName)
{
    WCHAR Name[LOCALE_NAME_MAX_LENGTH];
    WCHAR Resolved[LOCALE_NAME_MAX_LENGTH];
    SIZE_T Length, i;
    INT ResolvedLength;

    if (cchLocaleName < 0 || (cchLocaleName > 0 && !lpLocaleName))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* Windows does not take LOCALE_NAME_SYSTEM_DEFAULT here: its '!' makes it an invalid name */
    if (!lpNameToResolve)
    {
        if (!GetSpecificLocaleName(LOCALE_USER_DEFAULT, Resolved))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    else
    {
        Length = wcslen(lpNameToResolve);
        if (Length >= LOCALE_NAME_MAX_LENGTH)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
        for (i = 0; i < Length; i++)
        {
            if (!IsValidLocaleNameChar(lpNameToResolve[i]))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return 0;
            }
        }
        RtlCopyMemory(Name, lpNameToResolve, (Length + 1) * sizeof(WCHAR));

        while (!ResolveCandidate(Name, Resolved))
        {
            /* Cut off the last tag and try the parent */
            while (Length > 0 && Name[Length] != L'-' && Name[Length] != L'_')
                Length--;
            if (Length == 0)
            {
                /* Nothing matches: the invariant locale */
                Resolved[0] = UNICODE_NULL;
                break;
            }
            Name[Length] = UNICODE_NULL;
        }
    }

    ResolvedLength = (INT)wcslen(Resolved) + 1;
    if (cchLocaleName == 0)
        return ResolvedLength;

    if (ResolvedLength > cchLocaleName)
    {
        /* Windows hands back as much of the name as fits */
        RtlCopyMemory(lpLocaleName, Resolved, (cchLocaleName - 1) * sizeof(WCHAR));
        lpLocaleName[cchLocaleName - 1] = UNICODE_NULL;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    RtlCopyMemory(lpLocaleName, Resolved, ResolvedLength * sizeof(WCHAR));
    return ResolvedLength;
}
