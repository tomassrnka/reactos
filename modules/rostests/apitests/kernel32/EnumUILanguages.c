/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for EnumUILanguagesA/W: only installed UI languages are enumerated
 */

#include "precomp.h"

#define MAX_ALLOWED 256

static ULONG g_Allowed[MAX_ALLOWED];
static ULONG g_AllowedCount;
static ULONG g_Count, g_Unexpected;
static BOOL g_SawSystem, g_SawUser;
static LANGID g_SystemLang, g_UserLang;

static void
Allow(ULONG Id)
{
    if (g_AllowedCount < MAX_ALLOWED)
        g_Allowed[g_AllowedCount++] = Id;
}

static BOOL
IsAllowed(ULONG Id)
{
    ULONG i;

    for (i = 0; i < g_AllowedCount; i++)
    {
        if (g_Allowed[i] == Id)
            return TRUE;
    }
    return FALSE;
}

/* The installed UI languages: the system and the user language, and the MUI list when the system has one. */
static void
BuildAllowedSet(void)
{
    typedef LCID (WINAPI *LOCALENAMETOLCID)(LPCWSTR, DWORD);
    LOCALENAMETOLCID pLocaleNameToLCID =
        (LOCALENAMETOLCID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LocaleNameToLCID");
    HKEY Key;
    DWORD i;
    WCHAR Name[LOCALE_NAME_MAX_LENGTH];

    Allow(g_SystemLang);
    Allow(g_UserLang);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\MUI\\UILanguages", 0,
                      KEY_ENUMERATE_SUB_KEYS, &Key) != ERROR_SUCCESS)
        return;

    for (i = 0; RegEnumKeyW(Key, i, Name, ARRAY_SIZE(Name)) == ERROR_SUCCESS; i++)
    {
        WCHAR *End;
        ULONG Id = wcstoul(Name, &End, 16);

        if (*End == 0 && End != Name)
            Allow(Id);
        else if (pLocaleNameToLCID)
            Allow(pLocaleNameToLCID(Name, 0));
    }
    RegCloseKey(Key);
}

static void
Note(ULONG Id)
{
    g_Count++;
    if (Id == g_SystemLang)
        g_SawSystem = TRUE;
    if (Id == g_UserLang)
        g_SawUser = TRUE;
    if (!IsAllowed(Id))
        g_Unexpected++;
}

static BOOL CALLBACK
ProcA(LPSTR Value, LONG_PTR Param)
{
    char *End;
    ULONG Id = strtoul(Value, &End, 16);

    ok(*End == 0 && End != Value, "A: '%s' is not a hexadecimal language id\n", Value);
    Note(Id);
    return TRUE;
}

static BOOL CALLBACK
ProcW(LPWSTR Value, LONG_PTR Param)
{
    WCHAR *End;
    ULONG Id = wcstoul(Value, &End, 16);

    ok(*End == 0 && End != Value, "W: '%S' is not a hexadecimal language id\n", Value);
    Note(Id);
    return TRUE;
}

static ULONG StopCount;

static BOOL CALLBACK
StopProcW(LPWSTR Value, LONG_PTR Param)
{
    StopCount++;
    return FALSE;
}

static void
Check(const char *Func)
{
    ok(g_SawSystem, "%s did not list the system UI language %04x\n", Func, g_SystemLang);
    ok(g_SawUser, "%s did not list the user UI language %04x\n", Func, g_UserLang);
    ok(g_Unexpected == 0, "%s listed %lu of %lu languages that are not installed\n", Func, g_Unexpected, g_Count);
}

START_TEST(EnumUILanguages)
{
    BOOL Ret;

    g_SystemLang = GetSystemDefaultUILanguage();
    g_UserLang = GetUserDefaultUILanguage();
    BuildAllowedSet();

    g_Count = g_Unexpected = 0;
    g_SawSystem = g_SawUser = FALSE;
    SetLastError(0xdeadbeef);
    Ret = EnumUILanguagesA(ProcA, 0, 0);
    ok(Ret, "EnumUILanguagesA failed, error %lu\n", GetLastError());
    Check("EnumUILanguagesA");

    g_Count = g_Unexpected = 0;
    g_SawSystem = g_SawUser = FALSE;
    SetLastError(0xdeadbeef);
    Ret = EnumUILanguagesW(ProcW, 0, 0);
    ok(Ret, "EnumUILanguagesW failed, error %lu\n", GetLastError());
    Check("EnumUILanguagesW");

    StopCount = 0;
    Ret = EnumUILanguagesW(StopProcW, 0, 0);
    ok(Ret, "EnumUILanguagesW with a stopping callback failed, error %lu\n", GetLastError());
    ok(StopCount == 1, "the callback ran %lu times after returning FALSE\n", StopCount);
}
