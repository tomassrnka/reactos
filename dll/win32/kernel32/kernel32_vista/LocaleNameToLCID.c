/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of LocaleNameToLCID
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

#include "k32_vista.h"
#include <ndk/rtlfuncs.h>

#define NDEBUG
#include <debug.h>

/* The specific locale of a neutral locale whose sublanguage is not SUBLANG_NEUTRAL, or 0 if unknown */
LCID
BasepSpecialNeutralLcidToSpecific(
    _In_ LCID Lcid)
{
    switch (Lcid)
    {
        case 0x0742C: return 0x0082C; // "az-Cyrl" -> "az-Cyrl-AZ"
        case 0x0782C: return 0x0042C; // "az-Latn" -> "az-Latn-AZ"
        case 0x07C67: return 0x00867; // "ff-Latn" -> "ff-Latn-SN"
        case 0x0781A: return 0x0141A; // "bs" -> "bs-Latn-BA"
        case 0x0641A: return 0x0201A; // "bs-Cyrl" -> "bs-Cyrl-BA"
        case 0x0681A: return 0x0141A; // "bs-Latn" -> "bs-Latn-BA"
        case 0x07C5C: return 0x0045C; // "chr-Cher" -> "chr-Cher-US"
        case 0x07C2E: return 0x0082E; // "dsb" -> "dsb-DE"
        case 0x07C68: return 0x00468; // "ha-Latn" -> "ha-Latn-NG"
        case 0x0785D: return 0x0045D; // "iu-Cans" -> "iu-Cans-CA"
        case 0x07C5D: return 0x0085D; // "iu-Latn" -> "iu-Latn-CA"
        case 0x07C92: return 0x00492; // "ku-Arab" -> "ku-Arab-IQ"
        case 0x07850: return 0x00450; // "mn-Cyrl" -> "mn-MN"
        case 0x07C50: return 0x00850; // "mn-Mong" -> "mn-Mong-CN"
        case 0x07C14: return 0x00414; // "nb" -> "nb-NO"
        case 0x07814: return 0x00814; // "nn" -> "nn-NO"
        case 0x07C46: return 0x00846; // "pa-Arab" -> "pa-Arab-PK"
        case 0x0703B: return 0x0243B; // "smn" -> "smn-FI"
        case 0x07C59: return 0x00859; // "sd-Arab" -> "sd-Arab-PK"
        case 0x0783B: return 0x01C3B; // "sma" -> "sma-SE"
        case 0x07C3B: return 0x0143B; // "smj" -> "smj-SE"
        case 0x0743B: return 0x0203B; // "sms" -> "sms-FI"
        case 0x06C1A: return 0x0281A; // "sr-Cyrl" -> "sr-Cyrl-RS"
        case 0x0701A: return 0x0241A; // "sr-Latn" -> "sr-Latn-RS"
        case 0x07C28: return 0x00428; // "tg-Cyrl" -> "tg-Cyrl-TJ"
        case 0x07C5F: return 0x0085F; // "tzm-Latn" -> "tzm-Latn-DZ"
        case 0x0785F: return 0x0105F; // "tzm-Tfng" -> "tzm-Tfng-MA"
        case 0x07843: return 0x00843; // "uz-Cyrl" -> "uz-Cyrl-UZ"
        case 0x07C43: return 0x00443; // "uz-Latn" -> "uz-Latn-UZ"
        case 0x07804: return 0x00804; // "zh" -> "zh-CN"
        case 0x07C04: return 0x00C04; // "zh-Hant" -> "zh-HK"
    }

    return 0;
}

LCID
BasepNeutralLcidToSpecific(
    _In_ LCID Lcid)
{
    switch (Lcid)
    {
        /* Neutral locales whose SUBLANG_DEFAULT locale is not the one Windows picks, or does not exist */
        case 0x00004: return 0x00804; // "zh-Hans" -> "zh-CN" (SUBLANG_DEFAULT is zh-TW)
        case 0x0000A: return 0x00C0A; // "es" -> "es-ES" (SUBLANG_DEFAULT is es-ES_tradnl)
        case 0x0003C: return 0x0083C; // "ga" -> "ga-IE"
        case 0x00045: return 0x00845; // "bn" -> "bn-BD"
        case 0x00059: return 0x00859; // "sd" -> "sd-Arab-PK"
        case 0x0005D: return 0x0085D; // "iu" -> "iu-Latn-CA"
        case 0x0005F: return 0x0085F; // "tzm" -> "tzm-Latn-DZ"
        case 0x00067: return 0x00867; // "ff" -> "ff-Latn-SN"
        case 0x00073: return 0x00873; // "ti" -> "ti-ER"
        case 0x07C1A: return 0x0241A; // "sr" -> "sr-Latn-RS"
    }

    if (SUBLANGID(LANGIDFROMLCID(Lcid)) == SUBLANG_NEUTRAL)
        return MAKELCID(MAKELANGID(PRIMARYLANGID(LANGIDFROMLCID(Lcid)), SUBLANG_DEFAULT), SORT_DEFAULT);

    return BasepSpecialNeutralLcidToSpecific(Lcid);
}

PCWSTR
BasepLocaleNameAlias(
    _In_ PCWSTR Name)
{
    static const struct { PCWSTR Alias; PCWSTR Name; } Aliases[] =
    {
        { L"zh-Hans-CN", L"zh-CN" },
        { L"zh-Hans-SG", L"zh-SG" },
        { L"zh-Hant-TW", L"zh-TW" },
        { L"zh-Hant-HK", L"zh-HK" },
        { L"zh-Hant-MO", L"zh-MO" },
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Aliases); i++)
    {
        if (_wcsicmp(Name, Aliases[i].Alias) == 0)
            return Aliases[i].Name;
    }
    return NULL;
}

LCID
WINAPI
LocaleNameToLCID(
    _In_ LPCWSTR lpName,
    _In_ DWORD dwFlags)
{
    NTSTATUS Status;
    PCWSTR Alias;
    LCID Lcid;

    /* Validate flags */
    if (dwFlags & ~LOCALE_ALLOW_NEUTRAL_NAMES)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* Check for LOCALE_NAME_USER_DEFAULT (NULL) */
    if (lpName == NULL)
    {
        return GetUserDefaultLCID();
    }

    /* Check for LOCALE_NAME_INVARIANT (L"") */
    if (lpName[0] == L'\0')
    {
        return MAKELCID(MAKELANGID(LANG_INVARIANT, SUBLANG_NEUTRAL), SORT_DEFAULT);
    }

    /* Script-tagged names of the Chinese locales are not in the RTL table */
    Alias = BasepLocaleNameAlias(lpName);
    if (Alias)
        lpName = Alias;

    /* Call the RTL function (include neutral names) */
    Status = RtlLocaleNameToLcid(lpName, &Lcid, RTL_LOCALE_ALLOW_NEUTRAL_NAMES);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* Are neutral locales allowed? */
    if ((dwFlags & LOCALE_ALLOW_NEUTRAL_NAMES) == 0)
    {
        /* Check if we got a neutral locale */
        LCID SpecificLcid = BasepNeutralLcidToSpecific(Lcid);

        if (SpecificLcid)
            return SpecificLcid;
        if (SUBLANGID(LANGIDFROMLCID(Lcid)) == SUBLANG_NEUTRAL ||
            SUBLANGID(LANGIDFROMLCID(Lcid)) >= 0x18)
        {
            /* Should not happen */
            DPRINT1("Unandled neutral LCID %x\n", Lcid);
            return 0;
        }
    }

    return Lcid;
}
