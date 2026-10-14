/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetTimeZoneInformationForYear
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values were measured on Windows Server 2008 R2 (x86 and x64).
 * Windows 10 differs: it fails for an unknown zone and reads the names from the registry.
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_GetTimeZoneInformationForYear)(USHORT, PDYNAMIC_TIME_ZONE_INFORMATION, LPTIME_ZONE_INFORMATION);
typedef DWORD (WINAPI *FN_GetDynamicTimeZoneInformation)(PDYNAMIC_TIME_ZONE_INFORMATION);

#define ZONES_KEY L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones"
#define TEST_ZONE_PREFIX L"ReactOS Test Zone "

typedef struct _TZI_FORMAT
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    SYSTEMTIME StandardDate;
    SYSTEMTIME DaylightDate;
} TZI_FORMAT;

static FN_GetTimeZoneInformationForYear pGetTimeZoneInformationForYear;

/* A name of this process's own, so that runs at the same time cannot meet */
static WCHAR TEST_ZONE[64];

/* A name as printable text, however the array was left (it may be unterminated after a failed call) */
static PCWSTR NameOf(const WCHAR *Name)
{
    static WCHAR Ring[4][33];
    static unsigned Next;
    WCHAR *Copy = Ring[Next++ % ARRAYSIZE(Ring)];

    wcsncpy(Copy, Name, 32);
    Copy[32] = UNICODE_NULL;
    return Copy;
}

/* The tests with a synthetic zone expect what Windows 7 does; Windows 10 reads the names from the registry and fails for an unknown zone */
static BOOL Windows7Behavior(void)
{
    return is_reactos() || GetNTVersion() <= _WIN32_WINNT_WIN7;
}

static void FillInfo(DYNAMIC_TIME_ZONE_INFORMATION *Dynamic, PCWSTR KeyName)
{
    ZeroMemory(Dynamic, sizeof(*Dynamic));
    Dynamic->Bias = -5;
    Dynamic->StandardBias = -6;
    Dynamic->DaylightBias = -7;
    Dynamic->StandardDate.wMonth = 11;
    Dynamic->StandardDate.wDay = 2;
    Dynamic->DaylightDate.wMonth = 3;
    Dynamic->DaylightDate.wDay = 4;
    StringCchCopyW(Dynamic->StandardName, ARRAYSIZE(Dynamic->StandardName), L"Test Standard");
    StringCchCopyW(Dynamic->DaylightName, ARRAYSIZE(Dynamic->DaylightName), L"Test Daylight");
    StringCchCopyW(Dynamic->TimeZoneKeyName, ARRAYSIZE(Dynamic->TimeZoneKeyName), KeyName);
}

/* The year entries of the test zone have these values, derived from the year */
static void MakeYearData(TZI_FORMAT *Data, LONG Year)
{
    ZeroMemory(Data, sizeof(*Data));
    Data->Bias = 1000 + Year;
    Data->StandardBias = 2000 + Year;
    Data->DaylightBias = 3000 + Year;
    Data->StandardDate.wMonth = 10;
    Data->StandardDate.wDay = 5;
    Data->StandardDate.wHour = 2;
    Data->DaylightDate.wMonth = 4;
    Data->DaylightDate.wDay = 1;
    Data->DaylightDate.wHour = 3;
}

static void ExpectYearData(PCSTR What, USHORT QueryYear, LONG Year, const TIME_ZONE_INFORMATION *Info)
{
    TZI_FORMAT Expected;

    MakeYearData(&Expected, Year);
    ok(Info->Bias == Expected.Bias, "%s, year %u: bias %ld, expected %ld\n", What, QueryYear, Info->Bias, Expected.Bias);
    ok(Info->StandardBias == Expected.StandardBias, "%s, year %u: standard bias %ld, expected %ld\n",
       What, QueryYear, Info->StandardBias, Expected.StandardBias);
    ok(Info->DaylightBias == Expected.DaylightBias, "%s, year %u: daylight bias %ld, expected %ld\n",
       What, QueryYear, Info->DaylightBias, Expected.DaylightBias);
    ok(!memcmp(&Info->StandardDate, &Expected.StandardDate, sizeof(SYSTEMTIME)), "%s, year %u: standard date differs\n", What, QueryYear);
    ok(!memcmp(&Info->DaylightDate, &Expected.DaylightDate, sizeof(SYSTEMTIME)), "%s, year %u: daylight date differs\n", What, QueryYear);
    ok(!wcsncmp(Info->StandardName, L"Test Standard", 32), "%s, year %u: standard name '%S'\n", What, QueryYear, NameOf(Info->StandardName));
    ok(!wcsncmp(Info->DaylightName, L"Test Daylight", 32), "%s, year %u: daylight name '%S'\n", What, QueryYear, NameOf(Info->DaylightName));
}

/* The caller's own values, untouched (the names are copied as strings, not as whole arrays) */
static void ExpectOwnData(PCSTR What, USHORT QueryYear, const DYNAMIC_TIME_ZONE_INFORMATION *Own, const TIME_ZONE_INFORMATION *Info)
{
    ok(Info->Bias == Own->Bias, "%s, year %u: bias %ld, expected %ld\n", What, QueryYear, Info->Bias, Own->Bias);
    ok(Info->StandardBias == Own->StandardBias, "%s, year %u: standard bias %ld, expected %ld\n",
       What, QueryYear, Info->StandardBias, Own->StandardBias);
    ok(Info->DaylightBias == Own->DaylightBias, "%s, year %u: daylight bias %ld, expected %ld\n",
       What, QueryYear, Info->DaylightBias, Own->DaylightBias);
    ok(!memcmp(&Info->StandardDate, &Own->StandardDate, sizeof(SYSTEMTIME)), "%s, year %u: standard date differs\n", What, QueryYear);
    ok(!memcmp(&Info->DaylightDate, &Own->DaylightDate, sizeof(SYSTEMTIME)), "%s, year %u: daylight date differs\n", What, QueryYear);
    ok(!wcsncmp(Info->StandardName, Own->StandardName, 32), "%s, year %u: standard name '%S'\n", What, QueryYear, NameOf(Info->StandardName));
    ok(!wcsncmp(Info->DaylightName, Own->DaylightName, 32), "%s, year %u: daylight name '%S'\n", What, QueryYear, NameOf(Info->DaylightName));
}

static BOOL Query(USHORT Year, PCWSTR KeyName, BOOL Disabled, TIME_ZONE_INFORMATION *Info, DYNAMIC_TIME_ZONE_INFORMATION *Own)
{
    DYNAMIC_TIME_ZONE_INFORMATION Dynamic;
    BOOL Ret;

    FillInfo(&Dynamic, KeyName);
    Dynamic.DynamicDaylightTimeDisabled = Disabled;
    *Own = Dynamic;
    RtlFillMemory(Info, sizeof(*Info), 0xBB);
    SetLastError(0xDEADBEEF);
    Ret = pGetTimeZoneInformationForYear(Year, &Dynamic, Info);
    ok(!memcmp(&Dynamic, Own, sizeof(Dynamic)), "The dynamic information was modified\n");
    return Ret;
}

static BOOL SetYear(HKEY hKey, LONG Year)
{
    WCHAR Name[16];
    TZI_FORMAT Data;

    StringCchPrintfW(Name, ARRAYSIZE(Name), L"%ld", Year);
    MakeYearData(&Data, Year);
    return !RegSetValueExW(hKey, Name, 0, REG_BINARY, (const BYTE *)&Data, sizeof(Data));
}

static void SetEntries(HKEY hKey, DWORD First, DWORD Last)
{
    ok(!RegSetValueExW(hKey, L"FirstEntry", 0, REG_DWORD, (const BYTE *)&First, sizeof(First)), "FirstEntry\n");
    ok(!RegSetValueExW(hKey, L"LastEntry", 0, REG_DWORD, (const BYTE *)&Last, sizeof(Last)), "LastEntry\n");
}

static void TestZoneData(HKEY hDynamic)
{
    TIME_ZONE_INFORMATION Info;
    DYNAMIC_TIME_ZONE_INFORMATION Own;
    BOOL Ret;
    static const struct
    {
        USHORT Year;
        LONG Used;
    } Years[] =
    {
        { 0, 2000 }, { 1, 2000 }, { 1999, 2000 }, { 2000, 2000 }, { 2001, 2001 }, { 2003, 2003 },
        { 2004, 2003 }, { 3000, 2003 }, { 9999, 2003 },
        /* a year as a signed 16-bit number, down to -999, is below the first entry */
        { 64537, 2000 }, { 65000, 2000 }, { 65534, 2000 }, { 65535, 2000 },
    };
    static const USHORT Untouched[] = { 10000, 20000, 30827, 32768, 40000, 64536 };
    unsigned i;

    /* Entries 2000, 2001 and 2003; years outside use the first or last one */
    SetEntries(hDynamic, 2000, 2003);
    ok(SetYear(hDynamic, 2000) && SetYear(hDynamic, 2001) && SetYear(hDynamic, 2003), "Cannot store the years\n");

    for (i = 0; i < ARRAYSIZE(Years); i++)
    {
        Ret = Query(Years[i].Year, TEST_ZONE, FALSE, &Info, &Own);
        ok(Ret, "Year %u: failed: %lu\n", Years[i].Year, GetLastError());
        ExpectYearData("entries", Years[i].Year, Years[i].Used, &Info);
    }

    /* Years that do not fit four characters as a signed number get nothing from the registry */
    for (i = 0; i < ARRAYSIZE(Untouched); i++)
    {
        Ret = Query(Untouched[i], TEST_ZONE, FALSE, &Info, &Own);
        ok(Ret, "Year %u: failed: %lu\n", Untouched[i], GetLastError());
        ExpectOwnData("too many digits", Untouched[i], &Own, &Info);
    }

    /* DynamicDaylightTimeDisabled: the caller's values */
    for (i = 0; i < 4; i++)
    {
        static const USHORT Sample[] = { 1999, 2001, 2003, 2010 };

        Ret = Query(Sample[i], TEST_ZONE, TRUE, &Info, &Own);
        ok(Ret, "Disabled, year %u: failed: %lu\n", Sample[i], GetLastError());
        ExpectOwnData("disabled", Sample[i], &Own, &Info);
    }

    /* A first entry above the last: below the first entry uses the first, otherwise above the last uses the last */
    SetEntries(hDynamic, 2003, 2001);
    Ret = Query(1999, TEST_ZONE, FALSE, &Info, &Own);
    ok(Ret, "Failed: %lu\n", GetLastError());
    ExpectYearData("first above last", 1999, 2003, &Info);
    Ret = Query(2003, TEST_ZONE, FALSE, &Info, &Own);
    ok(Ret, "Failed: %lu\n", GetLastError());
    ExpectYearData("first above last", 2003, 2001, &Info);
    Ret = Query(2010, TEST_ZONE, FALSE, &Info, &Own);
    ok(Ret, "Failed: %lu\n", GetLastError());
    ExpectYearData("first above last", 2010, 2001, &Info);

    /* Without both entries nothing is read */
    SetEntries(hDynamic, 2000, 2003);
    ok(!RegDeleteValueW(hDynamic, L"FirstEntry"), "Cannot delete FirstEntry\n");
    Ret = Query(2001, TEST_ZONE, FALSE, &Info, &Own);
    ok(Ret, "Failed: %lu\n", GetLastError());
    ExpectOwnData("no FirstEntry", 2001, &Own, &Info);
    SetEntries(hDynamic, 2000, 2003);
    ok(!RegDeleteValueW(hDynamic, L"LastEntry"), "Cannot delete LastEntry\n");
    Ret = Query(2001, TEST_ZONE, FALSE, &Info, &Own);
    ok(Ret, "Failed: %lu\n", GetLastError());
    ExpectOwnData("no LastEntry", 2001, &Own, &Info);
}

static void TestNoDynamicData(HKEY hZone)
{
    TIME_ZONE_INFORMATION Info;
    DYNAMIC_TIME_ZONE_INFORMATION Own;
    HKEY hDynamic = NULL;
    LONG Error;
    BOOL Ret;

    /* The zone exists but has no "Dynamic DST" key */
    Ret = Query(2005, TEST_ZONE, FALSE, &Info, &Own);
    ok(Ret, "Failed: %lu\n", GetLastError());
    ExpectOwnData("no Dynamic DST key", 2005, &Own, &Info);

    /* A key without entries */
    Error = RegCreateKeyExW(hZone, L"Dynamic DST", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hDynamic, NULL);
    ok(Error == ERROR_SUCCESS, "Cannot create Dynamic DST: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    Ret = Query(2005, TEST_ZONE, FALSE, &Info, &Own);
    ok(Ret, "Failed: %lu\n", GetLastError());
    ExpectOwnData("empty Dynamic DST key", 2005, &Own, &Info);

    TestZoneData(hDynamic);
    RegCloseKey(hDynamic);
    Error = RegDeleteKeyW(hZone, L"Dynamic DST");
    ok(Error == ERROR_SUCCESS, "Cannot delete Dynamic DST: %ld\n", Error);
}

static void TestUnknownZones(void)
{
    static const PCWSTR Names[] = { L"No Such Zone", L"", L"ReactOS Test Zone  " };
    TIME_ZONE_INFORMATION Info;
    DYNAMIC_TIME_ZONE_INFORMATION Own;
    unsigned i;
    BOOL Ret;

    /* Windows Server 2008 R2 and Windows 7 succeed with the caller's values for a zone they do not know */
    for (i = 0; i < ARRAYSIZE(Names); i++)
    {
        Ret = Query(2005, Names[i], FALSE, &Info, &Own);
        ok(Ret, "Zone '%S': failed: %lu\n", Names[i], GetLastError());
        ExpectOwnData("unknown zone", 2005, &Own, &Info);
    }
}

static BOOL HasDynamicDst(PCWSTR KeyName)
{
    WCHAR Path[256];
    HKEY hKey;

    if (!KeyName[0])
        return FALSE;
    StringCchPrintfW(Path, ARRAYSIZE(Path), ZONES_KEY L"\\%s\\Dynamic DST", KeyName);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_QUERY_VALUE, &hKey))
        return FALSE;
    RegCloseKey(hKey);
    return TRUE;
}

static void ExpectSameZone(PCSTR What, const TIME_ZONE_INFORMATION *Info, const TIME_ZONE_INFORMATION *Expected, BOOL All)
{
    ok(Info->Bias == Expected->Bias, "%s: bias %ld, expected %ld\n", What, Info->Bias, Expected->Bias);
    ok(!wcsncmp(Info->StandardName, Expected->StandardName, 32), "%s: standard name '%S', expected '%S'\n",
       What, NameOf(Info->StandardName), NameOf(Expected->StandardName));
    ok(!wcsncmp(Info->DaylightName, Expected->DaylightName, 32), "%s: daylight name '%S', expected '%S'\n",
       What, NameOf(Info->DaylightName), NameOf(Expected->DaylightName));
    if (!All)
        return;
    ok(Info->StandardBias == Expected->StandardBias, "%s: standard bias %ld, expected %ld\n", What, Info->StandardBias, Expected->StandardBias);
    ok(Info->DaylightBias == Expected->DaylightBias, "%s: daylight bias %ld, expected %ld\n", What, Info->DaylightBias, Expected->DaylightBias);
    ok(!memcmp(&Info->StandardDate, &Expected->StandardDate, sizeof(SYSTEMTIME)), "%s: standard date differs\n", What);
    ok(!memcmp(&Info->DaylightDate, &Expected->DaylightDate, sizeof(SYSTEMTIME)), "%s: daylight date differs\n", What);
}

static void TestCurrentZone(void)
{
    FN_GetDynamicTimeZoneInformation pGetDynamicTimeZoneInformation;
    DYNAMIC_TIME_ZONE_INFORMATION Dynamic;
    TIME_ZONE_INFORMATION Info, Current, Explicit;
    SYSTEMTIME Now;
    BOOL Ret, All;

    pGetDynamicTimeZoneInformation = (FN_GetDynamicTimeZoneInformation)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetDynamicTimeZoneInformation");
    if (!pGetDynamicTimeZoneInformation)
        return;

    GetLocalTime(&Now);
    if (GetTimeZoneInformation(&Current) == TIME_ZONE_ID_INVALID || pGetDynamicTimeZoneInformation(&Dynamic) == TIME_ZONE_ID_INVALID)
    {
        skip("Cannot read the current zone\n");
        return;
    }
    Dynamic.TimeZoneKeyName[ARRAYSIZE(Dynamic.TimeZoneKeyName) - 1] = UNICODE_NULL;

    /* A zone with yearly data has the dates of that year, the others the ones in effect */
    All = Dynamic.DynamicDaylightTimeDisabled || !HasDynamicDst(Dynamic.TimeZoneKeyName);

    /* No dynamic information: the zone in effect */
    RtlFillMemory(&Info, sizeof(Info), 0xBB);
    SetLastError(0xDEADBEEF);
    Ret = pGetTimeZoneInformationForYear(Now.wYear, NULL, &Info);
    ok(Ret, "Failed: %lu\n", GetLastError());
    if (Ret)
        ExpectSameZone("no dynamic information", &Info, &Current, All);

    /* The information of GetDynamicTimeZoneInformation gives the same, all of it */
    RtlFillMemory(&Explicit, sizeof(Explicit), 0xBB);
    Ret = pGetTimeZoneInformationForYear(Now.wYear, &Dynamic, &Explicit);
    ok(Ret, "Failed: %lu\n", GetLastError());
    if (Ret)
    {
        ExpectSameZone("explicit information", &Explicit, &Current, All);
        if (Info.Bias != 0xBBBBBBBB)
            ExpectSameZone("explicit and implicit", &Explicit, &Info, TRUE);
    }
}

START_TEST(GetTimeZoneInformationForYear)
{
    HKEY hZones, hZone;
    LONG Error;
    DWORD Disposition;

    pGetTimeZoneInformationForYear = (FN_GetTimeZoneInformationForYear)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTimeZoneInformationForYear");
    if (!pGetTimeZoneInformationForYear && ((is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64")) ||
                                            (!is_reactos() && GetNTVersion() < _WIN32_WINNT_VISTA)))
    {
        skip("No NT 6 exports\n");
        return;
    }
    ok(pGetTimeZoneInformationForYear != NULL, "GetTimeZoneInformationForYear is not exported\n");
    if (!pGetTimeZoneInformationForYear)
        return;

    StringCchPrintfW(TEST_ZONE, ARRAYSIZE(TEST_ZONE), TEST_ZONE_PREFIX L"%lu", GetCurrentProcessId());

    TestCurrentZone();
    if (!Windows7Behavior())
    {
        skip("Newer Windows versions read the names from the registry and fail for an unknown zone\n");
        return;
    }
    TestUnknownZones();

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ZONES_KEY, 0, KEY_CREATE_SUB_KEY, &hZones);
    if (Error)
    {
        skip("Cannot open the time zones key: %ld\n", Error);
        return;
    }
    Error = RegCreateKeyExW(hZones, TEST_ZONE, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hZone, &Disposition);
    if (Error)
    {
        skip("Cannot create a time zone: %ld\n", Error);
        RegCloseKey(hZones);
        return;
    }
    if (Disposition != REG_CREATED_NEW_KEY)
    {
        skip("The test zone exists already\n");
        RegCloseKey(hZone);
        RegCloseKey(hZones);
        return;
    }

    TestNoDynamicData(hZone);

    RegCloseKey(hZone);
    Error = RegDeleteKeyW(hZones, TEST_ZONE);
    ok(Error == ERROR_SUCCESS, "Cannot delete the test zone: %ld\n", Error);
    RegCloseKey(hZones);
}
