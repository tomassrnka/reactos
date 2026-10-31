/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for DeriveCapabilitySidsFromName (kernelbase.dll, api-ms-win-security-base-l1-2-2; Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <sddl.h>

typedef BOOL (WINAPI *PFN_DeriveCapabilitySidsFromName)(LPCWSTR, PSID **, DWORD *, PSID **, DWORD *);

/* SID strings returned by Windows 10 22H2 */
static const struct
{
    PCWSTR Name;
    PCWSTR Group;
    PCWSTR Capability;
} Cases[] =
{
    { L"internetClient",
      L"S-1-5-32-2779705173-1925339129-2667939958-2414465498-3395756507-4015878651-158944808-788332705",
      L"S-1-15-3-1" },
    { L"INTERNETCLIENT",
      L"S-1-5-32-2779705173-1925339129-2667939958-2414465498-3395756507-4015878651-158944808-788332705",
      L"S-1-15-3-1" },
    { L"internetClientServer",
      L"S-1-5-32-309259276-1558928059-2828253251-1567535764-1640198682-3321378364-2265860830-2415090742",
      L"S-1-15-3-2" },
    { L"privateNetworkClientServer",
      L"S-1-5-32-3451683122-3669176405-2623206703-410298525-609685457-245256422-1312352757-4006057415",
      L"S-1-15-3-3" },
    { L"documentsLibrary",
      L"S-1-5-32-4090599227-2031128978-4197150514-3106696474-3598308373-297001435-2835591233-1745192457",
      L"S-1-15-3-7" },
    { L"contacts",
      L"S-1-5-32-3940324700-2858494370-2345038474-1357291012-3714428700-1390950899-1148638500-3083056261",
      L"S-1-15-3-12" },
    { L"location",
      L"S-1-5-32-1120341015-4059530845-270443254-1514536596-2315272569-284657971-419501928-776969430",
      L"S-1-15-3-1024-1120341015-4059530845-270443254-1514536596-2315272569-284657971-419501928-776969430" },
    { L"webcam",
      L"S-1-5-32-4131216513-4266103714-3944869821-2853506808-3373049249-4035912394-2659877950-3593780078",
      L"S-1-15-3-1024-4131216513-4266103714-3944869821-2853506808-3373049249-4035912394-2659877950-3593780078" },
    { L"runFullTrust",
      L"S-1-5-32-1365790099-2797813016-1714917928-519942599-2377126242-1094757716-3949770552-3596009590",
      L"S-1-15-3-1024-1365790099-2797813016-1714917928-519942599-2377126242-1094757716-3949770552-3596009590" },
    { L"nitro.acprof.cap",
      L"S-1-5-32-426983408-1943462641-2611705943-3988080831-1795106451-2815983898-861624665-916692136",
      L"S-1-15-3-1024-426983408-1943462641-2611705943-3988080831-1795106451-2815983898-861624665-916692136" },
    { L"a.b-c_d",
      L"S-1-5-32-1331975661-3088209591-4260694696-2068526617-1537907305-4031484526-1934374131-2916264100",
      L"S-1-15-3-1024-1331975661-3088209591-4260694696-2068526617-1537907305-4031484526-1934374131-2916264100" },
    { L"Microsoft.Foo_bar",
      L"S-1-5-32-3406884367-1886399455-1268086395-3282647488-182171701-700425603-1085254821-1046224883",
      L"S-1-15-3-1024-3406884367-1886399455-1268086395-3282647488-182171701-700425603-1085254821-1046224883" },
    { L"Z",
      L"S-1-5-32-3785242761-3600114764-441277484-368322486-22126089-1791368418-520196852-3304172936",
      L"S-1-15-3-1024-3785242761-3600114764-441277484-368322486-22126089-1791368418-520196852-3304172936" },
    { L"\x00e9" L"clair",
      L"S-1-5-32-3341768302-2972124892-687967332-882003548-2211578795-3166707516-3839128873-1326298156",
      L"S-1-15-3-1024-3341768302-2972124892-687967332-882003548-2211578795-3166707516-3839128873-1326298156" },
};

static void CheckSidString(PCWSTR Name, PCSTR What, PSID Sid, PCWSTR Expected, DWORD ExpectedLocalSize)
{
    LPWSTR String = NULL;

    ok(Sid != NULL, "%ls: no %s SID\n", Name, What);
    if (!Sid)
        return;
    ok(IsValidSid(Sid), "%ls: the %s SID is not valid\n", Name, What);
    ok(ConvertSidToStringSidW(Sid, &String), "%ls: ConvertSidToStringSid failed with %lu\n", Name, GetLastError());
    if (String)
    {
        ok(!wcscmp(String, Expected), "%ls: %s SID is %ls, expected %ls\n", Name, What, String, Expected);
        LocalFree(String);
    }
    /* each SID is a separate LocalAlloc block: 44 bytes for the group, always 48 for the capability */
    ok(LocalSize(Sid) == ExpectedLocalSize, "%ls: %s SID block is %lu bytes, expected %lu\n",
       Name, What, (ULONG)LocalSize(Sid), ExpectedLocalSize);
}

static void CheckDerive(PFN_DeriveCapabilitySidsFromName pDerive, PCSTR Where, ULONG i)
{
    PSID *Groups = (PSID *)(ULONG_PTR)0x11, *Capabilities = (PSID *)(ULONG_PTR)0x22;
    DWORD GroupCount = 0x77, CapabilityCount = 0x88;
    BOOL Ret;
    DWORD Error;

    SetLastError(0xdeadbeef);
    Ret = pDerive(Cases[i].Name, &Groups, &GroupCount, &Capabilities, &CapabilityCount);
    Error = GetLastError();
    ok(Ret, "%s: %ls: failed with %lu\n", Where, Cases[i].Name, Error);
    if (!Ret)
        return;
    ok(Error == 0xdeadbeef, "%s: %ls: last error changed to %lu\n", Where, Cases[i].Name, Error);
    ok(GroupCount == 1, "%s: %ls: %lu group SIDs\n", Where, Cases[i].Name, GroupCount);
    ok(CapabilityCount == 1, "%s: %ls: %lu capability SIDs\n", Where, Cases[i].Name, CapabilityCount);
    if (GroupCount != 1 || CapabilityCount != 1)
        return;

    /* the arrays hold one pointer each, in their own blocks */
    ok(LocalSize(Groups) == sizeof(PSID), "%s: %ls: group array is %lu bytes\n", Where, Cases[i].Name, (ULONG)LocalSize(Groups));
    ok(LocalSize(Capabilities) == sizeof(PSID), "%s: %ls: capability array is %lu bytes\n", Where, Cases[i].Name, (ULONG)LocalSize(Capabilities));
    CheckSidString(Cases[i].Name, "group", Groups[0], Cases[i].Group, 44);
    CheckSidString(Cases[i].Name, "capability", Capabilities[0], Cases[i].Capability, 48);

    LocalFree(Groups[0]);
    LocalFree(Capabilities[0]);
    LocalFree(Groups);
    LocalFree(Capabilities);
}

START_TEST(DeriveCapabilitySidsFromName)
{
    PFN_DeriveCapabilitySidsFromName pKernelbase, pApiset;
    HMODULE Kernelbase, Apiset;
    ULONG i;

    Kernelbase = LoadLibraryW(L"kernelbase.dll");
    pKernelbase = Kernelbase ? (PFN_DeriveCapabilitySidsFromName)GetProcAddress(Kernelbase, "DeriveCapabilitySidsFromName") : NULL;
    if (!pKernelbase)
    {
        ok(!is_reactos(), "DeriveCapabilitySidsFromName is not exported by kernelbase.dll\n");
        skip("DeriveCapabilitySidsFromName is not available\n");
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
        CheckDerive(pKernelbase, "kernelbase", i);

    /* Programs import it through the API set contract (VS Code's copilot-sdk runtime.node does) */
    Apiset = LoadLibraryW(L"api-ms-win-security-base-l1-2-2.dll");
    ok(Apiset != NULL, "api-ms-win-security-base-l1-2-2.dll did not load: %lu\n", GetLastError());
    if (Apiset)
    {
        pApiset = (PFN_DeriveCapabilitySidsFromName)GetProcAddress(Apiset, "DeriveCapabilitySidsFromName");
        ok(pApiset != NULL, "api-ms-win-security-base-l1-2-2.dll does not export DeriveCapabilitySidsFromName: %lu\n", GetLastError());
        if (pApiset)
        {
            CheckDerive(pApiset, "apiset", 0);
            CheckDerive(pApiset, "apiset", 6);
        }
    }
}
