/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the TokenLogonSid information class and the TOKEN_GROUPS classes
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/sefuncs.h>

#define LOGON_SID_CLASS ((TOKEN_INFORMATION_CLASS)28) /* TokenLogonSid */

static BOOL IsWow64(void)
{
    BOOL Wow64 = FALSE;
    return IsWow64Process(GetCurrentProcess(), &Wow64) && Wow64;
}

/* Returns the token's groups (caller frees) */
static PTOKEN_GROUPS QueryGroups(HANDLE hToken, TOKEN_INFORMATION_CLASS Class, PDWORD ReturnedLength)
{
    DWORD Length = 0, Length2 = 0;
    PTOKEN_GROUPS Groups;

    SetLastError(0xdeadbeef);
    ok(!GetTokenInformation(hToken, Class, NULL, 0, &Length), "Class %d: size query succeeded\n", Class);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Class %d: error %lu\n", Class, GetLastError());
    if (!Length)
        return NULL;
    Groups = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Length);
    if (!Groups)
        return NULL;
    if (!GetTokenInformation(hToken, Class, Groups, Length, &Length2))
    {
        ok(0, "Class %d: query failed with %lu\n", Class, GetLastError());
        HeapFree(GetProcessHeap(), 0, Groups);
        return NULL;
    }
    ok(Length2 == Length, "Class %d: returned length %lu, size query said %lu\n", Class, Length2, Length);
    if (ReturnedLength)
        *ReturnedLength = Length;
    return Groups;
}

/* Every SID of a TOKEN_GROUPS result lies in the buffer after the array */
static void CheckGroupsLayout(PTOKEN_GROUPS Groups, DWORD Length, TOKEN_INFORMATION_CLASS Class)
{
    ULONG_PTR Start = (ULONG_PTR)&Groups->Groups[Groups->GroupCount], End = (ULONG_PTR)Groups + Length;
    DWORD i;

    for (i = 0; i < Groups->GroupCount; i++)
    {
        ULONG_PTR Sid = (ULONG_PTR)Groups->Groups[i].Sid;
        ok(Sid >= Start && Sid < End, "Class %d: SID %lu at %p outside the buffer %p+%lu\n",
           Class, i, Groups->Groups[i].Sid, Groups, Length);
        if (Sid >= Start && Sid < End)
        {
            ok(IsValidSid(Groups->Groups[i].Sid), "Class %d: SID %lu invalid\n", Class, i);
            ok(Sid + GetLengthSid(Groups->Groups[i].Sid) <= End, "Class %d: SID %lu runs past the buffer\n", Class, i);
        }
    }
}

/* The logon SID of TokenGroups, or NULL */
static PSID_AND_ATTRIBUTES FindLogonGroup(PTOKEN_GROUPS Groups, PDWORD Count)
{
    PSID_AND_ATTRIBUTES Found = NULL;
    DWORD i;

    *Count = 0;
    for (i = 0; i < Groups->GroupCount; i++)
    {
        if ((Groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
        {
            if (!Found)
                Found = &Groups->Groups[i];
            (*Count)++;
        }
    }
    return Found;
}

static void TestLogonSid(HANDLE hToken, const char *What)
{
    PTOKEN_GROUPS Groups, Logon;
    PSID_AND_ATTRIBUTES Expected;
    DWORD Count, Length = 0, Length2 = 0, GroupsLength;
    DECLSPEC_ALIGN(8) BYTE Small[64];
    NTSTATUS Status;
    ULONG NtLength;

    Groups = QueryGroups(hToken, TokenGroups, &GroupsLength);
    ok(Groups != NULL, "%s: no TokenGroups\n", What);
    if (!Groups)
        return;
    CheckGroupsLayout(Groups, GroupsLength, TokenGroups);
    Expected = FindLogonGroup(Groups, &Count);
    ok(Count <= 1, "%s: %lu logon SIDs\n", What, Count);

    if (!Expected)
    {
        SetLastError(0xdeadbeef);
        Length = 0x55;
        ok(!GetTokenInformation(hToken, LOGON_SID_CLASS, NULL, 0, &Length), "%s: query succeeded\n", What);
        ok(GetLastError() == ERROR_NOT_FOUND, "%s: error %lu\n", What, GetLastError());
        SetLastError(0xdeadbeef);
        ok(!GetTokenInformation(hToken, LOGON_SID_CLASS, Small, sizeof(Small), &Length), "%s: query succeeded\n", What);
        ok(GetLastError() == ERROR_NOT_FOUND, "%s: error %lu\n", What, GetLastError());
        Status = NtQueryInformationToken(hToken, LOGON_SID_CLASS, Small, sizeof(Small), &NtLength);
        ok(Status == STATUS_NOT_FOUND, "%s: status 0x%lx\n", What, Status);
        HeapFree(GetProcessHeap(), 0, Groups);
        return;
    }

    /* Logon SIDs are S-1-5-5-X-Y */
    ok(GetSidIdentifierAuthority(Expected->Sid)->Value[5] == 5, "%s: authority %u\n", What,
       GetSidIdentifierAuthority(Expected->Sid)->Value[5]);
    ok(*GetSidSubAuthorityCount(Expected->Sid) == SECURITY_LOGON_IDS_RID_COUNT, "%s: %u sub authorities\n", What,
       *GetSidSubAuthorityCount(Expected->Sid));
    ok(*GetSidSubAuthority(Expected->Sid, 0) == SECURITY_LOGON_IDS_RID, "%s: first RID %lu\n", What,
       *GetSidSubAuthority(Expected->Sid, 0));

    /* The size query and a buffer one byte short */
    SetLastError(0xdeadbeef);
    ok(!GetTokenInformation(hToken, LOGON_SID_CLASS, NULL, 0, &Length), "%s: size query succeeded\n", What);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "%s: error %lu\n", What, GetLastError());
    ok(Length == FIELD_OFFSET(TOKEN_GROUPS, Groups[1]) + GetLengthSid(Expected->Sid),
       "%s: length %lu, SID %lu\n", What, Length, GetLengthSid(Expected->Sid));
    if (Length > 1 && Length <= sizeof(Small))
    {
        SetLastError(0xdeadbeef);
        ok(!GetTokenInformation(hToken, LOGON_SID_CLASS, Small, Length - 1, &Length2), "%s: short query succeeded\n", What);
        ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "%s: error %lu\n", What, GetLastError());
        ok(Length2 == Length, "%s: short query length %lu, expected %lu\n", What, Length2, Length);
        Status = NtQueryInformationToken(hToken, LOGON_SID_CLASS, Small, Length - 1, &NtLength);
        ok(Status == STATUS_BUFFER_TOO_SMALL, "%s: status 0x%lx\n", What, Status);
        ok(NtLength == Length, "%s: NT length %lu, expected %lu\n", What, NtLength, Length);
    }

    /* Windows stores the length before the result, so a result sharing its storage wins */
    if (Length <= sizeof(Small))
    {
        Status = NtQueryInformationToken(hToken, LOGON_SID_CLASS, Small, sizeof(Small), (PULONG)Small);
        ok(Status == STATUS_SUCCESS, "%s: aliased query status 0x%lx\n", What, Status);
        ok(*(PULONG)Small == 1, "%s: aliased first field %lu, expected the group count 1\n", What, *(PULONG)Small);
    }
    /* Natively a misaligned or inaccessible buffer is refused before the token is looked at;
     * WOW64 converts the result, does not require alignment and checks the handle first */
    Status = NtQueryInformationToken(hToken, LOGON_SID_CLASS, Small + 1, sizeof(Small) - 4, &NtLength);
    ok(Status == (IsWow64() ? STATUS_SUCCESS : STATUS_DATATYPE_MISALIGNMENT), "%s: misaligned status 0x%lx\n", What, Status);
    Status = NtQueryInformationToken(NULL, LOGON_SID_CLASS, (PVOID)(ULONG_PTR)0x10, sizeof(Small), &NtLength);
    ok(Status == (IsWow64() ? STATUS_INVALID_HANDLE : STATUS_ACCESS_VIOLATION), "%s: bad buffer status 0x%lx\n", What, Status);
    Status = NtQueryInformationToken(hToken, LOGON_SID_CLASS, Small, sizeof(Small), (PULONG)(ULONG_PTR)0x10);
    ok(Status == STATUS_ACCESS_VIOLATION, "%s: bad length pointer status 0x%lx\n", What, Status);

    Logon = QueryGroups(hToken, LOGON_SID_CLASS, &Length2);
    ok(Logon != NULL, "%s: no TokenLogonSid\n", What);
    if (Logon)
    {
        ok(Logon->GroupCount == 1, "%s: %lu groups\n", What, Logon->GroupCount);
        CheckGroupsLayout(Logon, Length2, LOGON_SID_CLASS);
        if (Logon->GroupCount == 1)
        {
            ok(EqualSid(Logon->Groups[0].Sid, Expected->Sid), "%s: not the logon SID of TokenGroups\n", What);
            ok(Logon->Groups[0].Attributes == Expected->Attributes, "%s: attributes 0x%lx, TokenGroups 0x%lx\n",
               What, Logon->Groups[0].Attributes, Expected->Attributes);
        }
        HeapFree(GetProcessHeap(), 0, Logon);
    }
    HeapFree(GetProcessHeap(), 0, Groups);
}

/* The SID of a group in TokenGroups, or NULL */
static PSID_AND_ATTRIBUTES FindGroup(PTOKEN_GROUPS Groups, PSID Sid)
{
    DWORD i;

    for (i = 0; i < Groups->GroupCount; i++)
    {
        if (EqualSid(Groups->Groups[i].Sid, Sid))
            return &Groups->Groups[i];
    }
    return NULL;
}

/* SidsToDisable of NtFilterToken: the group stays, as deny-only, and the logon SID is kept */
static void TestDisabledSid(HANDLE hToken, PSID Everyone)
{
    SID_AND_ATTRIBUTES Disable;
    PTOKEN_GROUPS Before, After;
    PSID_AND_ATTRIBUTES Group;
    HANDLE hDisabled;
    DWORD Count;
    BOOL bEnabled;

    Before = QueryGroups(hToken, TokenGroups, NULL);
    if (!Before)
        return;
    Group = FindGroup(Before, Everyone);
    ok(Group != NULL, "Everyone is not in the token\n");
    bEnabled = Group && (Group->Attributes & SE_GROUP_ENABLED);
    HeapFree(GetProcessHeap(), 0, Before);
    if (!bEnabled)
    {
        skip("Everyone is not enabled in the token\n");
        return;
    }

    Disable.Sid = Everyone;
    Disable.Attributes = 0;
    if (!CreateRestrictedToken(hToken, 0, 1, &Disable, 0, NULL, 0, NULL, &hDisabled))
    {
        ok(0, "CreateRestrictedToken with a disabled SID failed: %lu\n", GetLastError());
        return;
    }
    After = QueryGroups(hDisabled, TokenGroups, NULL);
    if (After)
    {
        Group = FindGroup(After, Everyone);
        ok(Group != NULL, "Everyone is gone from the filtered token\n");
        if (Group)
        {
            /* The group stays as deny-only, like on Windows */
            ok(Group->Attributes & SE_GROUP_USE_FOR_DENY_ONLY, "Everyone is not deny-only: 0x%lx\n", Group->Attributes);
            ok(!(Group->Attributes & SE_GROUP_ENABLED), "Everyone is still enabled: 0x%lx\n", Group->Attributes);
        }
        ok(FindLogonGroup(After, &Count) != NULL, "The filtered token lost the logon SID\n");
        HeapFree(GetProcessHeap(), 0, After);
    }
    TestLogonSid(hDisabled, "disabled SIDs");
    CloseHandle(hDisabled);
}

/* A SID above the highest address the caller may use is refused by NtFilterToken, as an access violation,
 * for the disabled and for the restricting SIDs */
static void TestFilterBadSid(HANDLE hToken)
{
    TOKEN_GROUPS Bad;
    HANDLE hNew = NULL;
    NTSTATUS Status;

    Bad.GroupCount = 1;
    Bad.Groups[0].Sid = (PSID)(ULONG_PTR)-16;
    Bad.Groups[0].Attributes = 0;

    Status = NtFilterToken(hToken, 0, &Bad, NULL, NULL, &hNew);
    ok(Status == STATUS_ACCESS_VIOLATION, "Disabled SID out of range: status 0x%lx\n", Status);
    if (NT_SUCCESS(Status))
        CloseHandle(hNew);

    hNew = NULL;
    Status = NtFilterToken(hToken, 0, NULL, NULL, &Bad, &hNew);
    ok(Status == STATUS_ACCESS_VIOLATION, "Restricting SID out of range: status 0x%lx\n", Status);
    if (NT_SUCCESS(Status))
        CloseHandle(hNew);
}

/* An unrestricted token has no restricting SIDs: an empty TOKEN_GROUPS: 4 bytes in the 32-bit layout (also under WoW64), 8 in the 64-bit one */
static void TestNoRestrictedSids(HANDLE hToken)
{
    PTOKEN_GROUPS Restricted;
    DWORD Length = 0, bIsRestricted = 0;

    /* TokenIsRestricted (40) is Vista and later; where it is unknown the token is taken as unrestricted */
    if (GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS)40, &bIsRestricted, sizeof(bIsRestricted), &Length) &&
        bIsRestricted)
    {
        skip("The process token is restricted\n");
        return;
    }
    Restricted = QueryGroups(hToken, TokenRestrictedSids, &Length);
    ok(Restricted != NULL, "No TokenRestrictedSids on an unrestricted token\n");
    if (Restricted)
    {
        ok(Restricted->GroupCount == 0, "%lu restricting SIDs\n", Restricted->GroupCount);
        ok(Length == FIELD_OFFSET(TOKEN_GROUPS, Groups), "Empty TOKEN_GROUPS length %lu\n", Length);
        HeapFree(GetProcessHeap(), 0, Restricted);
    }
}

START_TEST(TokenLogonSid)
{
    HANDLE hToken, hNoQuery, hRestricted, hAnonymous = NULL;
    SID_AND_ATTRIBUTES Restrict;
    BYTE EveryoneBuffer[SECURITY_MAX_SID_SIZE], Small[64];
    DWORD Size = sizeof(EveryoneBuffer), Length;
    PTOKEN_GROUPS Restricted;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_QUERY_SOURCE, &hToken))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }

    /* An interactive logon has a logon SID */
    TestLogonSid(hToken, "process");
    {
        PTOKEN_GROUPS Groups = QueryGroups(hToken, TokenGroups, NULL);
        DWORD Count = 0;
        if (Groups)
        {
            ok(FindLogonGroup(Groups, &Count) != NULL, "The process token has no logon SID\n");
            HeapFree(GetProcessHeap(), 0, Groups);
        }
    }

    /* TOKEN_QUERY is required */
    if (DuplicateHandle(GetCurrentProcess(), hToken, GetCurrentProcess(), &hNoQuery, TOKEN_QUERY_SOURCE, FALSE, 0))
    {
        SetLastError(0xdeadbeef);
        ok(!GetTokenInformation(hNoQuery, LOGON_SID_CLASS, Small, sizeof(Small), &Length), "Query succeeded\n");
        ok(GetLastError() == ERROR_ACCESS_DENIED, "Error %lu\n", GetLastError());
        CloseHandle(hNoQuery);
    }

    /* A restricted token keeps the logon SID; its restricting SIDs come back as TOKEN_GROUPS too */
    ok(CreateWellKnownSid(WinWorldSid, NULL, EveryoneBuffer, &Size), "CreateWellKnownSid failed: %lu\n", GetLastError());
    Restrict.Sid = EveryoneBuffer;
    Restrict.Attributes = 0;
    if (CreateRestrictedToken(hToken, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 1, &Restrict, &hRestricted))
    {
        PTOKEN_GROUPS Groups = QueryGroups(hRestricted, TokenGroups, NULL);
        DWORD Count = 0;
        if (Groups)
        {
            ok(FindLogonGroup(Groups, &Count) != NULL, "The restricted token lost the logon SID\n");
            HeapFree(GetProcessHeap(), 0, Groups);
        }
        TestLogonSid(hRestricted, "restricted");
        Restricted = QueryGroups(hRestricted, TokenRestrictedSids, &Length);
        ok(Restricted != NULL, "No TokenRestrictedSids\n");
        if (Restricted)
        {
            CheckGroupsLayout(Restricted, Length, TokenRestrictedSids);
            ok(Restricted->GroupCount == 1, "%lu restricting SIDs\n", Restricted->GroupCount);
            if (Restricted->GroupCount == 1)
                ok(EqualSid(Restricted->Groups[0].Sid, EveryoneBuffer), "Not Everyone\n");
            HeapFree(GetProcessHeap(), 0, Restricted);
        }
        CloseHandle(hRestricted);
    }
    else
    {
        ok(0, "CreateRestrictedToken failed: %lu\n", GetLastError());
    }

    /* The anonymous token has no logon SID */
    if (ImpersonateAnonymousToken(GetCurrentThread()))
    {
        ok(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hAnonymous), "OpenThreadToken failed: %lu\n", GetLastError());
        ok(RevertToSelf(), "RevertToSelf failed: %lu\n", GetLastError());
        if (hAnonymous)
        {
            PTOKEN_GROUPS Groups = QueryGroups(hAnonymous, TokenGroups, NULL);
            DWORD Count = 0;
            if (Groups)
            {
                ok(FindLogonGroup(Groups, &Count) == NULL, "The anonymous token has a logon SID\n");
                HeapFree(GetProcessHeap(), 0, Groups);
            }
            TestLogonSid(hAnonymous, "anonymous");
            CloseHandle(hAnonymous);
        }
    }
    else
    {
        skip("ImpersonateAnonymousToken failed: %lu\n", GetLastError());
    }

    TestNoRestrictedSids(hToken);
    TestDisabledSid(hToken, EveryoneBuffer);
    TestFilterBadSid(hToken);

    CloseHandle(hToken);
}
