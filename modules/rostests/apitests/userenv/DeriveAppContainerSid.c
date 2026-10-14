/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for userenv!DeriveAppContainerSidFromAppContainerName (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <sddl.h>

typedef HRESULT (WINAPI *PFN_Derive)(PCWSTR, PSID *);

static PFN_Derive pDerive;

static void CheckSid(PCWSTR Name, PCWSTR Expected)
{
    PSID Sid = (PSID)(ULONG_PTR)0x55;
    LPWSTR String = NULL;
    HRESULT hr;
    BOOL Converted;
    DWORD Error;

    hr = pDerive(Name, &Sid);
    ok(hr == S_OK, "%ls: hr %#lx\n", Name, hr);
    if (hr != S_OK)
        return;

    ok(IsValidSid(Sid), "%ls: the SID is not valid\n", Name);
    ok(GetLengthSid(Sid) == 40, "%ls: SID length %lu\n", Name, GetLengthSid(Sid));
    ok(*GetSidSubAuthorityCount(Sid) == 8, "%ls: %u sub-authorities\n", Name, *GetSidSubAuthorityCount(Sid));
    Converted = ConvertSidToStringSidW(Sid, &String);
    Error = GetLastError();
    ok(Converted, "%ls: ConvertSidToStringSid failed with %lu\n", Name, Error);
    if (String)
    {
        ok(!wcscmp(String, Expected), "%ls: got %ls, expected %ls\n", Name, String, Expected);
        LocalFree(String);
    }
    FreeSid(Sid);
}

START_TEST(DeriveAppContainerSid)
{
    HMODULE Userenv = LoadLibraryW(L"userenv.dll");
    PSID Sid, Sid2;
    WCHAR Name[100];
    HRESULT hr;
    ULONG i;

    pDerive = (PFN_Derive)GetProcAddress(Userenv, "DeriveAppContainerSidFromAppContainerName");
    /*
     * Windows 7 has none: the function arrived with Windows 8. ReactOS exports it from export level 0x601 on,
     * which a kernel32 with the Windows 7 functions shows.
     */
    if (!pDerive)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "DeriveAppContainerSidFromAppContainerName is not exported\n");
        skip("DeriveAppContainerSidFromAppContainerName is not exported\n");
        return;
    }

    /* SHA-256 of the lower case name in UTF-16, the first seven words after S-1-15-2 (values measured) */
    CheckSid(L"a", L"S-1-15-2-3937069567-81109666-199193729-4036909440-616594969-3159470276-4124008600");
    CheckSid(L"A", L"S-1-15-2-3937069567-81109666-199193729-4036909440-616594969-3159470276-4124008600");
    CheckSid(L"test", L"S-1-15-2-1980125950-1037672881-421768107-1949737198-2922275827-507320043-1582245000");
    CheckSid(L"TEST", L"S-1-15-2-1980125950-1037672881-421768107-1949737198-2922275827-507320043-1582245000");
    CheckSid(L"Test", L"S-1-15-2-1980125950-1037672881-421768107-1949737198-2922275827-507320043-1582245000");
    CheckSid(L"test.app", L"S-1-15-2-362878394-2413698015-1748633419-853110514-1881931786-344398737-3964070663");
    CheckSid(L"Microsoft.Windows.Cortana_cw5n1h2txyewy",
             L"S-1-15-2-1861897761-1695161497-2927542615-642690995-327840285-2659745135-2630312742");
    CheckSid(L"microsoft.windows.cortana_cw5n1h2txyewy",
             L"S-1-15-2-1861897761-1695161497-2927542615-642690995-327840285-2659745135-2630312742");
    CheckSid(L" ", L"S-1-15-2-4213022598-793090713-4148984393-3602009588-1948215022-1390471081-538229841");
    CheckSid(L"a b", L"S-1-15-2-2647426951-3122004733-2420991111-592833065-957374437-2057985451-1399199878");
    CheckSid(L"\x00e4\x00c4", L"S-1-15-2-297628925-3929566818-2089327425-651482514-1113726981-3289096625-1596543063");
    CheckSid(L"\x00c4", L"S-1-15-2-1180528735-971313865-473053020-43939126-3937226589-835963688-1208880440");
    CheckSid(L"\x00e4", L"S-1-15-2-1180528735-971313865-473053020-43939126-3937226589-835963688-1208880440");

    /* names around the padding limits of the hash: 55 and 56 bytes, 63 and 64 bytes, and the same plus 64 bytes */
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbA",
             L"S-1-15-2-2383336824-103878090-148571191-1810237367-1257544369-3452035857-2314129229");
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbAb",
             L"S-1-15-2-2988140197-2504585876-309928769-3122557067-1719310861-2971299860-2963535077");
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbAbAbA",
             L"S-1-15-2-2012558806-48246290-3643997954-137182233-2819636818-1233830670-3926477864");
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAb",
             L"S-1-15-2-4148675217-1703392543-2637484000-2849704302-3160562491-1568474383-1451553202");
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbA",
             L"S-1-15-2-746820646-3712017789-454735835-1848722995-630847543-2974573189-1611200027");
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAb",
             L"S-1-15-2-2075153259-2086961297-3539082815-3759905439-3156658434-2472754242-593592412");
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbA",
             L"S-1-15-2-1487077160-1543981943-2175825566-64767609-2177487474-751487326-852135361");
    CheckSid(L"AbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAb",
             L"S-1-15-2-3878587808-3641748430-4201452425-324436599-4027560868-2910771926-2483406025");

    /* names of 64 characters are fine, longer ones and empty ones are not */
    for (i = 0; i < 99; i++)
        Name[i] = L'a';
    Name[99] = 0;
    Name[64] = 0;
    Sid = (PSID)(ULONG_PTR)0x55;
    hr = pDerive(Name, &Sid);
    ok(hr == S_OK, "64 characters: hr %#lx\n", hr);
    if (hr == S_OK)
    {
        Sid2 = (PSID)(ULONG_PTR)0x55;
        Name[64] = L'a';
        Name[65] = 0;
        hr = pDerive(Name, &Sid2);
        ok(hr == E_INVALIDARG, "65 characters: hr %#lx\n", hr);
        ok(Sid2 == (PSID)(ULONG_PTR)0x55, "65 characters: the result was written\n");
        FreeSid(Sid);
    }
    Name[65] = L'a';
    Name[99] = 0;
    Sid = (PSID)(ULONG_PTR)0x55;
    hr = pDerive(Name, &Sid);
    ok(hr == E_INVALIDARG, "99 characters: hr %#lx\n", hr);

    Sid = (PSID)(ULONG_PTR)0x55;
    hr = pDerive(L"", &Sid);
    ok(hr == E_INVALIDARG, "empty name: hr %#lx\n", hr);
    ok(Sid == (PSID)(ULONG_PTR)0x55, "empty name: the result was written\n");
    hr = pDerive(NULL, &Sid);
    ok(hr == E_INVALIDARG, "NULL name: hr %#lx\n", hr);
    ok(Sid == (PSID)(ULONG_PTR)0x55, "NULL name: the result was written\n");
    hr = pDerive(L"test", NULL);
    ok(hr == E_INVALIDARG, "NULL result: hr %#lx\n", hr);
}
