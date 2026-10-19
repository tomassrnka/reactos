/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Stability test: the restricting SIDs of a restricted token stay valid after creation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SID_COUNT 4
#define ROUNDS 1000

/* Reuse the pool the kernel used to capture the restricting SIDs: filter again with other
 * SIDs of the same lengths and count, so a freed capture block is refilled with different SIDs.
 * Named events vary the other allocations. */
static void Churn(HANDLE hSource, ULONG Round)
{
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    BYTE Sids[SID_COUNT][SECURITY_MAX_SID_SIZE];
    SID_AND_ATTRIBUTES Other[SID_COUNT];
    WCHAR Name[96];
    HANDLE Events[8], hOther;
    ULONG i;

    /* Same sub-authority counts as the expected SIDs (1, 2, 1, 5), different values */
    static const BYTE Counts[SID_COUNT] = {1, 2, 1, 5};
    for (i = 0; i < SID_COUNT; i++)
    {
        ULONG j;
        InitializeSid(Sids[i], &NtAuthority, Counts[i]);
        for (j = 0; j < Counts[i]; j++)
            *GetSidSubAuthority(Sids[i], j) = 0x5a000000 | (Round << 8) | (i << 4) | j;
        Other[i].Sid = Sids[i];
        Other[i].Attributes = 0;
    }
    if (CreateRestrictedToken(hSource, 0, 0, NULL, 0, NULL, SID_COUNT, Other, &hOther))
        CloseHandle(hOther);
    else
        ok(0, "Round %lu: churn CreateRestrictedToken failed: %lu\n", Round, GetLastError());

    for (i = 0; i < 8; i++)
    {
        StringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"RestrictedSidsCopy-%lu-%lu-%.*s",
                         Round, i, (int)((Round + i) % 48), L"................................................");
        Events[i] = CreateEventW(NULL, FALSE, FALSE, Name);
        ok(Events[i] != NULL, "Round %lu: CreateEventW failed: %lu\n", Round, GetLastError());
    }
    for (i = 0; i < 8; i++)
    {
        if (Events[i])
            CloseHandle(Events[i]);
    }
}

/* The restricting SIDs of a token match the originals, in order */
static BOOL CheckRestrictedSids(HANDLE hToken, PSID *Expected, ULONG Round)
{
    BYTE Buffer[1024];
    PTOKEN_GROUPS Groups = (PTOKEN_GROUPS)Buffer;
    DWORD Length = 0, i;

    if (!GetTokenInformation(hToken, TokenRestrictedSids, Buffer, sizeof(Buffer), &Length))
    {
        ok(0, "Round %lu: TokenRestrictedSids failed: %lu\n", Round, GetLastError());
        return FALSE;
    }
    if (Groups->GroupCount != SID_COUNT)
    {
        ok(0, "Round %lu: %lu restricting SIDs\n", Round, Groups->GroupCount);
        return FALSE;
    }
    for (i = 0; i < SID_COUNT; i++)
    {
        if (!IsValidSid(Groups->Groups[i].Sid) || !EqualSid(Groups->Groups[i].Sid, Expected[i]))
        {
            ok(0, "Round %lu: restricting SID %lu differs\n", Round, i);
            return FALSE;
        }
    }
    return TRUE;
}

START_TEST(RestrictedSidsCopy)
{
    static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID Expected[SID_COUNT] = {NULL};
    SID_AND_ATTRIBUTES Restrict[SID_COUNT];
    HANDLE hProcessToken, hRestricted, hDuplicate;
    ULONG Round, Failures = 0, i;

    ok(AllocateAndInitializeSid(&WorldAuthority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &Expected[0]), "Sid 0\n");
    ok(AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &Expected[1]), "Sid 1\n");
    ok(AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_RESTRICTED_CODE_RID, 0, 0, 0, 0, 0, 0, 0, &Expected[2]), "Sid 2\n");
    ok(AllocateAndInitializeSid(&NtAuthority, 5, SECURITY_NT_NON_UNIQUE, 0x12345678, 0x9abcdef0, 0x0fedcba9, 0x87654321, 0, 0, 0, &Expected[3]), "Sid 3\n");
    for (i = 0; i < SID_COUNT; i++)
    {
        if (!Expected[i])
        {
            skip("SID setup failed\n");
            return;
        }
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hProcessToken))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }

    /* The token is made from separate copies of the SIDs, which are then destroyed */
    for (i = 0; i < SID_COUNT; i++)
    {
        DWORD Size = GetLengthSid(Expected[i]);
        Restrict[i].Sid = HeapAlloc(GetProcessHeap(), 0, Size);
        Restrict[i].Attributes = 0;
        CopySid(Size, Restrict[i].Sid, Expected[i]);
    }
    if (!CreateRestrictedToken(hProcessToken, 0, 0, NULL, 0, NULL, SID_COUNT, Restrict, &hRestricted))
    {
        ok(0, "CreateRestrictedToken failed: %lu\n", GetLastError());
        CloseHandle(hProcessToken);
        return;
    }
    for (i = 0; i < SID_COUNT; i++)
    {
        memset(Restrict[i].Sid, 0xcc, GetLengthSid(Expected[i]));
        HeapFree(GetProcessHeap(), 0, Restrict[i].Sid);
    }

    for (Round = 0; Round < ROUNDS && Failures < 5; Round++)
    {
        Churn(hProcessToken, Round);
        if (!CheckRestrictedSids(hRestricted, Expected, Round))
            Failures++;
        if (!DuplicateTokenEx(hRestricted, TOKEN_QUERY | TOKEN_DUPLICATE, NULL, SecurityImpersonation,
                              (Round & 1) ? TokenPrimary : TokenImpersonation, &hDuplicate))
        {
            ok(0, "Round %lu: DuplicateTokenEx failed: %lu\n", Round, GetLastError());
            Failures++;
            continue;
        }
        if (!CheckRestrictedSids(hDuplicate, Expected, Round))
            Failures++;
        CloseHandle(hDuplicate);
    }
    ok(Failures == 0, "%lu failures in %lu rounds\n", Failures, Round);

    CloseHandle(hRestricted);
    CloseHandle(hProcessToken);
    for (i = 0; i < SID_COUNT; i++)
        FreeSid(Expected[i]);
}
