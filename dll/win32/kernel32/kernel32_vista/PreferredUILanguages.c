/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The preferred user interface languages of the process (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The list is kept for the process and handed back by GetProcessPreferredUILanguages, and GetThreadPreferredUILanguages
 * puts it first in the list of the thread. Nothing loads resources by it yet: the resource loader does not look
 * at it. The rules are the ones measured on
 * Windows Server 2008 R2: three kinds of flags, names or four-digit hexadecimal identifiers, neutral languages
 * allowed, at most five languages, duplicates dropped, entries that are not languages ignored as long as one
 * is left.
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

#ifndef MUI_LANGUAGE_ID
#define MUI_LANGUAGE_ID   0x4
#define MUI_LANGUAGE_NAME 0x8
#endif

static LONG ProcessUiLock;
static ULONG ProcessUiCount;
static WCHAR ProcessUiNames[MAX_PROCESS_UI_LANGUAGES][LOCALE_NAME_MAX_LENGTH];

static
VOID
LockProcessUi(VOID)
{
    while (InterlockedCompareExchange(&ProcessUiLock, 1, 0) != 0)
        Sleep(0);
}

static
VOID
UnlockProcessUi(VOID)
{
    InterlockedExchange(&ProcessUiLock, 0);
}

static
BOOL
AddUiLanguage(
    _Inout_ WCHAR Names[][LOCALE_NAME_MAX_LENGTH],
    _Inout_ PULONG Count,
    _In_ LCID Lcid)
{
    WCHAR Name[LOCALE_NAME_MAX_LENGTH];
    ULONG i;

    if (!LCIDToLocaleName(Lcid, Name, RTL_NUMBER_OF(Name), LOCALE_ALLOW_NEUTRAL_NAMES))
        return FALSE;

    /* An entry that is there already is not added again; a sixth language is dropped */
    for (i = 0; i < *Count; i++)
    {
        if (!_wcsicmp(Names[i], Name))
            return TRUE;
    }
    if (*Count < MAX_PROCESS_UI_LANGUAGES)
    {
        wcscpy(Names[*Count], Name);
        (*Count)++;
    }
    return TRUE;
}

/* The list lives in kernel32 only: this file is linked into kernel32 and into kernel32_vista, and the copy of the latter is never set */
static
BOOL
IsInKernel32(VOID)
{
    HMODULE Self;

    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)IsInKernel32, &Self) && Self == GetModuleHandleW(L"kernel32.dll");
}

/* The identifiers of the languages of the process, for the list of a thread: it starts with them */
ULONG
BasepGetProcessUILanguageIds(
    _Out_writes_(MAX_PROCESS_UI_LANGUAGES) LANGID *Ids)
{
    WCHAR Names[MAX_PROCESS_UI_LANGUAGES][LOCALE_NAME_MAX_LENGTH];
    ULONG Count, i;

    if (!IsInKernel32())
    {
        /* four hexadecimal digits and a zero for each language, one more zero at the end */
        WCHAR Text[MAX_PROCESS_UI_LANGUAGES * 5 + 1];
        BOOL (WINAPI *GetProcessLanguages)(DWORD, PULONG, PZZWSTR, PULONG);
        ULONG Size = RTL_NUMBER_OF(Text), Number = 0;
        PCWSTR Entry;

        GetProcessLanguages = (PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessPreferredUILanguages");
        if (!GetProcessLanguages || !GetProcessLanguages(MUI_LANGUAGE_ID, &Number, Text, &Size))
            return 0;

        Count = 0;
        for (Entry = Text; *Entry && Count < MAX_PROCESS_UI_LANGUAGES; Entry += wcslen(Entry) + 1)
            Ids[Count++] = (LANGID)wcstoul(Entry, NULL, 16);
        return Count;
    }

    LockProcessUi();
    Count = ProcessUiCount;
    for (i = 0; i < Count; i++)
        wcscpy(Names[i], ProcessUiNames[i]);
    UnlockProcessUi();

    for (i = 0; i < Count; i++)
        Ids[i] = LANGIDFROMLCID(LocaleNameToLCID(Names[i], LOCALE_ALLOW_NEUTRAL_NAMES));
    return Count;
}

BOOL
WINAPI
SetProcessPreferredUILanguages(
    _In_ DWORD dwFlags,
    _In_opt_ PCZZWSTR pwszLanguagesBuffer,
    _Out_opt_ PULONG pulNumLanguages)
{
    WCHAR Names[MAX_PROCESS_UI_LANGUAGES][LOCALE_NAME_MAX_LENGTH];
    ULONG Count = 0, Entries = 0, i;
    DWORD OldError = GetLastError();
    PCWSTR Entry;

    if (dwFlags != 0 && dwFlags != MUI_LANGUAGE_ID && dwFlags != MUI_LANGUAGE_NAME)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* No list, or an empty one, removes the languages of the process */
    if (!pwszLanguagesBuffer || !*pwszLanguagesBuffer)
    {
        LockProcessUi();
        ProcessUiCount = 0;
        UnlockProcessUi();
        return TRUE;
    }

    for (Entry = pwszLanguagesBuffer; *Entry; Entry += wcslen(Entry) + 1)
    {
        LCID Lcid;

        Entries++;
        if (dwFlags == MUI_LANGUAGE_ID)
        {
            WCHAR Digits[5];
            PWCHAR End;

            /* the first four characters are read as a hexadecimal number as far as they go */
            wcsncpy(Digits, Entry, 4);
            Digits[4] = 0;
            Lcid = (LANGID)wcstoul(Digits, &End, 16);
            if (End == Digits)
                continue;
        }
        else
        {
            Lcid = LocaleNameToLCID(Entry, LOCALE_ALLOW_NEUTRAL_NAMES);
            if (!Lcid)
                continue;
        }
        AddUiLanguage(Names, &Count, Lcid);
    }

    if (Count == 0)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    LockProcessUi();
    ProcessUiCount = Count;
    for (i = 0; i < Count; i++)
        wcscpy(ProcessUiNames[i], Names[i]);
    UnlockProcessUi();

    if (pulNumLanguages)
        *pulNumLanguages = Count;

    /* The lookups of the names set the last error for the entries that are not languages; the call does not */
    SetLastError(OldError);
    return TRUE;
}

BOOL
WINAPI
GetProcessPreferredUILanguages(
    _In_ DWORD dwFlags,
    _Out_ PULONG pulNumLanguages,
    _Out_writes_opt_(*pcchLanguagesBuffer) PZZWSTR pwszLanguagesBuffer,
    _Inout_ PULONG pcchLanguagesBuffer)
{
    WCHAR Names[MAX_PROCESS_UI_LANGUAGES][LOCALE_NAME_MAX_LENGTH];
    WCHAR Text[MAX_PROCESS_UI_LANGUAGES][LOCALE_NAME_MAX_LENGTH];
    ULONG Count, i, Needed = 1;
    PWCHAR Out;

    if ((dwFlags != 0 && dwFlags != MUI_LANGUAGE_ID && dwFlags != MUI_LANGUAGE_NAME) ||
        !pulNumLanguages || !pcchLanguagesBuffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    LockProcessUi();
    Count = ProcessUiCount;
    for (i = 0; i < Count; i++)
        wcscpy(Names[i], ProcessUiNames[i]);
    UnlockProcessUi();

    for (i = 0; i < Count; i++)
    {
        if (dwFlags == MUI_LANGUAGE_ID)
        {
            static const WCHAR HexDigits[] = L"0123456789ABCDEF";
            LANGID Id = LANGIDFROMLCID(LocaleNameToLCID(Names[i], LOCALE_ALLOW_NEUTRAL_NAMES));

            Text[i][0] = HexDigits[(Id >> 12) & 0xf];
            Text[i][1] = HexDigits[(Id >> 8) & 0xf];
            Text[i][2] = HexDigits[(Id >> 4) & 0xf];
            Text[i][3] = HexDigits[Id & 0xf];
            Text[i][4] = 0;
        }
        else
        {
            wcscpy(Text[i], Names[i]);
        }
        Needed += (ULONG)wcslen(Text[i]) + 1;
    }
    if (Count == 0)
        Needed = 2;

    /* The number of languages is not written when there are none (measured) */
    if (pwszLanguagesBuffer)
    {
        if (*pcchLanguagesBuffer < Needed)
        {
            if (Count)
                *pulNumLanguages = Count;
            *pcchLanguagesBuffer = Needed;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }

        Out = pwszLanguagesBuffer;
        for (i = 0; i < Count; i++)
        {
            wcscpy(Out, Text[i]);
            Out += wcslen(Text[i]) + 1;
        }
        *Out++ = 0;
        if (Count == 0)
            *Out = 0;
    }

    if (Count)
        *pulNumLanguages = Count;
    *pcchLanguagesBuffer = Needed;
    return TRUE;
}
