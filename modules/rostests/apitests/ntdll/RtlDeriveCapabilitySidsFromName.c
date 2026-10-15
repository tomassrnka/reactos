/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for RtlDeriveCapabilitySidsFromName (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_RtlDeriveCapabilitySidsFromName)(PUNICODE_STRING, PSID, PSID);

/* Hashes and fixed capability numbers as returned by Windows 10 22H2 */
static const struct
{
    PCWSTR Name;
    ULONG WellKnown;
    PCSTR Hash;
} Cases[] =
{
    { L"test",                       0, "cf357a0a1528d9b4d2fd4a4ec9e498b373d7a731f99b80f02c9b81f9ba721db6" },
    { L"TEST",                       0, "cf357a0a1528d9b4d2fd4a4ec9e498b373d7a731f99b80f02c9b81f9ba721db6" },
    { L"",                           0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
    { L"a",                          0, "e61c21ca716b3b1aefb7d1198f83679c4ca4d596e5792275dd6203b49216237d" },
    { L"\x00e9t\x00e9",              0, "a9bf27cf39f69777a13bd428fdd8f02cb93aa22e4eed5954bcf4fa9178ba5565" },
    { L"location",                   0, "170cc7425d86f7f1f6a21e1094fe455a7941008a3389f71068170119d69c4f2e" },
    { L"microphone",                 0, "32ca38ee32f350d6ec8c6dcf95dfc2e8bba0ad87b78ee0b6d9f019b39ae47dfd" },
    { L"webcam",                     0, "815c3df6a29347febdef21ebf80e15aaa1a50cc9ca228ff03e848a9e6ebb34d6" },
    { L"userAccountInformation",     0, "f662abb30973fff13effa3f9a6ec765764c23a78db2148334a14599210110bd2" },
    { L"codeGeneration",             0, "c60f9fe288422cb668958f31684eb6627ee5dab347b26bd7522496066152a4b1" },
    { L"runFullTrust",               0, "934d6851183dc3a628923766c7b1fd1e6211b08d54ad404138b76ceb76c056d6" },
    { L"internetClient",             1, "55efaea5f957c2727688059fdad1e98fdb2167cafb715def284e7909a100fd2e" },
};

static const PCWSTR WellKnownNames[] =
{
    L"internetClient", L"internetClientServer", L"privateNetworkClientServer", L"picturesLibrary",
    L"videosLibrary", L"musicLibrary", L"documentsLibrary", L"enterpriseAuthentication",
    L"sharedUserCertificates", L"removableStorage", L"appointments", L"contacts"
};

static void
HexToBytes(PCSTR Hex, UCHAR *Bytes)
{
    ULONG i;

    for (i = 0; i < 32; i++)
    {
        char Pair[3] = { Hex[2 * i], Hex[2 * i + 1], 0 };
        Bytes[i] = (UCHAR)strtoul(Pair, NULL, 16);
    }
}

static void
CheckGroup(PCWSTR Name, PISID Group, const UCHAR *Hash)
{
    SID_IDENTIFIER_AUTHORITY Nt = { SECURITY_NT_AUTHORITY };

    ok(Group->Revision == 1, "%ls: group revision %u\n", Name, Group->Revision);
    ok(!memcmp(&Group->IdentifierAuthority, &Nt, sizeof(Nt)), "%ls: group authority is not NT\n", Name);
    ok(Group->SubAuthorityCount == 9, "%ls: group has %u sub-authorities\n", Name, Group->SubAuthorityCount);
    ok(Group->SubAuthority[0] == SECURITY_BUILTIN_DOMAIN_RID, "%ls: group first sub-authority %lu\n", Name, Group->SubAuthority[0]);
    ok(!memcmp(&Group->SubAuthority[1], Hash, 32), "%ls: group hash differs\n", Name);
}

START_TEST(RtlDeriveCapabilitySidsFromName)
{
    FN_RtlDeriveCapabilitySidsFromName pRtlDeriveCapabilitySidsFromName;
    SID_IDENTIFIER_AUTHORITY App = { SECURITY_APP_PACKAGE_AUTHORITY };
    ULONG i;

    pRtlDeriveCapabilitySidsFromName = (FN_RtlDeriveCapabilitySidsFromName)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDeriveCapabilitySidsFromName");
    if (!pRtlDeriveCapabilitySidsFromName)
    {
        if (is_reactos())
            ok(0, "RtlDeriveCapabilitySidsFromName is not exported\n");
        else
            skip("RtlDeriveCapabilitySidsFromName is not available\n");
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
    {
        UNICODE_STRING Name;
        UCHAR CapBuffer[SECURITY_MAX_SID_SIZE], GroupBuffer[SECURITY_MAX_SID_SIZE], Hash[32];
        PISID Cap = (PISID)CapBuffer, Group = (PISID)GroupBuffer;
        NTSTATUS Status;

        HexToBytes(Cases[i].Hash, Hash);
        memset(CapBuffer, 0xcc, sizeof(CapBuffer));
        memset(GroupBuffer, 0xcc, sizeof(GroupBuffer));
        RtlInitUnicodeString(&Name, Cases[i].Name);
        Status = pRtlDeriveCapabilitySidsFromName(&Name, Group, Cap);
        ok_hex(Status, STATUS_SUCCESS);

        ok(Cap->Revision == 1, "%ls: revision %u\n", Cases[i].Name, Cap->Revision);
        ok(!memcmp(&Cap->IdentifierAuthority, &App, sizeof(App)), "%ls: authority is not the app package authority\n", Cases[i].Name);
        ok(Cap->SubAuthority[0] == SECURITY_CAPABILITY_BASE_RID, "%ls: first sub-authority %lu\n", Cases[i].Name, Cap->SubAuthority[0]);
        if (Cases[i].WellKnown)
        {
            ok(Cap->SubAuthorityCount == 2, "%ls: %u sub-authorities\n", Cases[i].Name, Cap->SubAuthorityCount);
            ok(Cap->SubAuthority[1] == Cases[i].WellKnown, "%ls: capability %lu\n", Cases[i].Name, Cap->SubAuthority[1]);
        }
        else
        {
            ok(Cap->SubAuthorityCount == 10, "%ls: %u sub-authorities\n", Cases[i].Name, Cap->SubAuthorityCount);
            ok(Cap->SubAuthority[1] == 1024, "%ls: second sub-authority %lu\n", Cases[i].Name, Cap->SubAuthority[1]);
            ok(!memcmp(&Cap->SubAuthority[2], Hash, 32), "%ls: capability hash differs\n", Cases[i].Name);
        }
        CheckGroup(Cases[i].Name, Group, Hash);
    }

    /* The twelve Windows 8 capabilities have fixed numbers, in any case */
    for (i = 0; i < RTL_NUMBER_OF(WellKnownNames); i++)
    {
        UNICODE_STRING Name;
        UCHAR CapBuffer[SECURITY_MAX_SID_SIZE], GroupBuffer[SECURITY_MAX_SID_SIZE];
        PISID Cap = (PISID)CapBuffer;
        WCHAR Upper[64];
        NTSTATUS Status;

        RtlInitUnicodeString(&Name, WellKnownNames[i]);
        Status = pRtlDeriveCapabilitySidsFromName(&Name, GroupBuffer, CapBuffer);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Cap->SubAuthorityCount == 2 && Cap->SubAuthority[1] == i + 1, "%ls: count %u, capability %lu\n",
           WellKnownNames[i], Cap->SubAuthorityCount, Cap->SubAuthority[1]);

        wcscpy(Upper, WellKnownNames[i]);
        _wcsupr(Upper);
        RtlInitUnicodeString(&Name, Upper);
        Status = pRtlDeriveCapabilitySidsFromName(&Name, GroupBuffer, CapBuffer);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Cap->SubAuthorityCount == 2 && Cap->SubAuthority[1] == i + 1, "%ls: count %u, capability %lu\n",
           Upper, Cap->SubAuthorityCount, Cap->SubAuthority[1]);
    }
}
