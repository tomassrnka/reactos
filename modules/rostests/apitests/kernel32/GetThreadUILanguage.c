/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetThreadUILanguage
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expectations were measured on Windows 10 22H2.
 */

#include "precomp.h"

#ifndef MUI_LANGUAGE_NAME
#define MUI_LANGUAGE_NAME 0x8
#endif

typedef LANGID (WINAPI *FN_GetThreadUILanguage)(VOID);
typedef BOOL (WINAPI *FN_SetProcessPreferredUILanguages)(DWORD, PCZZWSTR, PULONG);

static FN_GetThreadUILanguage pGetThreadUILanguage;

static DWORD WINAPI
LanguageThread(PVOID Context)
{
    *(LANGID *)Context = pGetThreadUILanguage();
    return 0;
}

static LANGID
LanguageInNewThread(VOID)
{
    LANGID Language = 0xDEAD;
    HANDLE Thread = CreateThread(NULL, 0, LanguageThread, &Language, 0, NULL);

    ok(Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (Thread)
    {
        WaitForSingleObject(Thread, 10000);
        CloseHandle(Thread);
    }
    return Language;
}

START_TEST(GetThreadUILanguage)
{
    FN_SetProcessPreferredUILanguages pSetProcessPreferredUILanguages;
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    LANGID Default, Language;
    LCID OldLocale;
    ULONG Count;

    pGetThreadUILanguage = (FN_GetThreadUILanguage)GetProcAddress(hKernel32, "GetThreadUILanguage");
    pSetProcessPreferredUILanguages = (FN_SetProcessPreferredUILanguages)GetProcAddress(hKernel32, "SetProcessPreferredUILanguages");
    if (!pGetThreadUILanguage)
    {
        skip("GetThreadUILanguage is not exported\n");
        return;
    }

    /* The default of a thread is the UI language of the user */
    Default = GetUserDefaultUILanguage();
    SetLastError(0xDEADBEEF);
    Language = pGetThreadUILanguage();
    ok(Language != 0, "GetThreadUILanguage returned 0\n");
    ok(Language == Default, "GetThreadUILanguage returned 0x%04x, expected 0x%04x\n", Language, Default);
    ok(GetLastError() == 0xDEADBEEF, "GetLastError returned %lu\n", GetLastError());

    /* A new thread has the same */
    Language = LanguageInNewThread();
    ok(Language == Default, "GetThreadUILanguage in a new thread returned 0x%04x, expected 0x%04x\n", Language, Default);

    /* The locale of the thread is not the UI language */
    OldLocale = GetThreadLocale();
    if (SetThreadLocale(MAKELCID(MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN), SORT_DEFAULT)))
    {
        Language = pGetThreadUILanguage();
        ok(Language == Default, "GetThreadUILanguage after SetThreadLocale returned 0x%04x, expected 0x%04x\n", Language, Default);
        SetThreadLocale(OldLocale);
    }

    /* The first language of the process is the one of its threads, also a new thread */
    if (!pSetProcessPreferredUILanguages)
    {
        skip("SetProcessPreferredUILanguages is not exported\n");
        return;
    }
    Count = 0;
    if (!pSetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, L"de-DE\0", &Count))
    {
        skip("SetProcessPreferredUILanguages failed with %lu\n", GetLastError());
        return;
    }
    Language = pGetThreadUILanguage();
    ok(Language == 0x0407, "GetThreadUILanguage with de-DE as process language returned 0x%04x\n", Language);
    Language = LanguageInNewThread();
    ok(Language == 0x0407, "GetThreadUILanguage in a new thread with de-DE as process language returned 0x%04x\n", Language);

    /* Without process languages it is the default again */
    Count = 0;
    ok(pSetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, L"\0", &Count), "clearing the process languages failed with %lu\n", GetLastError());
    Language = pGetThreadUILanguage();
    ok(Language == Default, "GetThreadUILanguage after clearing the process languages returned 0x%04x, expected 0x%04x\n", Language, Default);
}
