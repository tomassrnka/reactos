/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Tests for the NtFilterToken API
 * COPYRIGHT:       Copyright 2021 George Bișoc <george.bisoc@reactos.org>
 */

#include "precomp.h"

#ifndef WRITE_RESTRICTED
#define WRITE_RESTRICTED 0x8
#endif

static
HANDLE
GetTokenProcess(VOID)
{
    BOOL Success;
    HANDLE Token;

    Success = OpenProcessToken(GetCurrentProcess(),
                               TOKEN_DUPLICATE | TOKEN_QUERY,
                               &Token);
    if (!Success)
    {
        skip("GetTokenProcess() has failed to get the process' token (error code: %lu)!\n", GetLastError());
        return NULL;
    }

    return Token;
}

static ULONG RestrictedCount(HANDLE Token)
{
    BYTE Buffer[512];
    ULONG Size;
    NTSTATUS Status = NtQueryInformationToken(Token, TokenRestrictedSids, Buffer, sizeof(Buffer), &Size);
    return NT_SUCCESS(Status) ? ((PTOKEN_GROUPS)Buffer)->GroupCount : ~0UL;
}

/* Parameter rules of NtFilterToken: flags, attributes, empty lists, restricted sources */
static void TestFilterParameters(HANDLE Token)
{
    static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    static const ULONG BadFlags[] = { 0x10, 0x20, 0x80000000 };
    BYTE WorldBuffer[16], RestrictedBuffer[16];
    PSID World = (PSID)WorldBuffer, RestrictedCode = (PSID)RestrictedBuffer;
    TOKEN_GROUPS Groups, Empty;
    HANDLE Filtered, Filtered2;
    NTSTATUS Status;
    ULONG i;

    RtlInitializeSid(World, &WorldAuthority, 1);
    *RtlSubAuthoritySid(World, 0) = SECURITY_WORLD_RID;
    RtlInitializeSid(RestrictedCode, &NtAuthority, 1);
    *RtlSubAuthoritySid(RestrictedCode, 0) = SECURITY_RESTRICTED_CODE_RID;

    for (i = 0; i < ARRAYSIZE(BadFlags); i++)
    {
        Filtered = NULL;
        Status = NtFilterToken(Token, BadFlags[i], NULL, NULL, NULL, &Filtered);
        ok(Status == STATUS_SUCCESS, "Flags 0x%lx: status 0x%lx\n", BadFlags[i], Status);
        if (NT_SUCCESS(Status))
            NtClose(Filtered);
    }

    /* Attributes on restricting SIDs and on SIDs to disable */
    Groups.GroupCount = 1;
    Groups.Groups[0].Sid = RestrictedCode;
    Groups.Groups[0].Attributes = SE_GROUP_ENABLED;
    Filtered = NULL;
    Status = NtFilterToken(Token, 0, NULL, NULL, &Groups, &Filtered);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    if (NT_SUCCESS(Status))
        NtClose(Filtered);
    Groups.Groups[0].Sid = World;
    Filtered = NULL;
    Status = NtFilterToken(Token, 0, &Groups, NULL, NULL, &Filtered);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        NtClose(Filtered);

    /* An empty restricting list does not make the token restricted */
    Empty.GroupCount = 0;
    Filtered = NULL;
    Status = NtFilterToken(Token, 0, &Empty, NULL, &Empty, &Filtered);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(RestrictedCount(Filtered) == 0, "Restricted SID count %lu\n", RestrictedCount(Filtered));
        NtClose(Filtered);
    }

    /* WRITE_RESTRICTED without restricting SIDs */
    Filtered = NULL;
    Status = NtFilterToken(Token, WRITE_RESTRICTED, NULL, NULL, NULL, &Filtered);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(RestrictedCount(Filtered) == 0, "Restricted SID count %lu\n", RestrictedCount(Filtered));
        NtClose(Filtered);
    }

    /* A restricted source token filtered again */
    Groups.Groups[0].Sid = RestrictedCode;
    Groups.Groups[0].Attributes = 0;
    Filtered = NULL;
    Status = NtFilterToken(Token, 0, NULL, NULL, &Groups, &Filtered);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(RestrictedCount(Filtered) == 1, "Restricted SID count %lu\n", RestrictedCount(Filtered));
        Filtered2 = NULL;
        Status = NtFilterToken(Filtered, 0, NULL, NULL, NULL, &Filtered2);
        ok(Status == STATUS_INVALID_PARAMETER, "Restricted source, nothing to add: status 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
            NtClose(Filtered2);
        Filtered2 = NULL;
        Status = NtFilterToken(Filtered, DISABLE_MAX_PRIVILEGE, NULL, NULL, NULL, &Filtered2);
        ok(Status == STATUS_INVALID_PARAMETER, "Restricted source, disable_max_privilege: status 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
            NtClose(Filtered2);
        Filtered2 = NULL;
        Status = NtFilterToken(Filtered, DISABLE_MAX_PRIVILEGE, NULL, NULL, &Groups, &Filtered2);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok(RestrictedCount(Filtered2) == 1, "Restricted SID count %lu\n", RestrictedCount(Filtered2));
            NtClose(Filtered2);
        }
        Groups.Groups[0].Sid = World;
        Filtered2 = NULL;
        Status = NtFilterToken(Filtered, 0, NULL, NULL, &Groups, &Filtered2);
        ok(Status == STATUS_INVALID_PARAMETER, "Restricted source, disjoint list: status 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
            NtClose(Filtered2);
        NtClose(Filtered);
    }
}

START_TEST(NtFilterToken)
{
    NTSTATUS Status;
    HANDLE FilteredToken, Token;
    TOKEN_PRIVILEGES Priv;
    ULONG Size;
    PTOKEN_STATISTICS TokenStats;

    /* We don't give a token */
    Status = NtFilterToken(NULL,
                           0,
                           NULL,
                           NULL,
                           NULL,
                           &FilteredToken);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    /* Get the token from process now */
    Token = GetTokenProcess();

    /* We don't give any privileges to delete */
    Status = NtFilterToken(Token,
                           0,
                           NULL,
                           NULL,
                           NULL,
                           &FilteredToken);
    ok_hex(Status, STATUS_SUCCESS);

    /* Query the total size to hold the statistics */
    Status = NtQueryInformationToken(Token, TokenStatistics, NULL, 0, &Size);
    if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_TOO_SMALL)
    {
        skip("Failed to query the total size for token statistics structure! (Status -> 0x%lx)\n", Status);
        return;
    }

    /* Total size queried, time to allocate our buffer based on that size */
    TokenStats = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
    if (TokenStats == NULL)
    {
        skip("Failed to allocate our token statistics buffer!\n");
        return;
    }

    /* Time to query our token statistics, prior disabling token's privileges */
    Status = NtQueryInformationToken(Token, TokenStatistics, TokenStats, Size, &Size);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to query the token statistics! (Status -> 0x%lx)\n", Status);
        return;
    }

    trace("Number of privileges before token filtering -- %lu\n\n", TokenStats->PrivilegeCount);

    /* Disable the privileges and make the token a safer inert one */
    Status = NtFilterToken(Token,
                           DISABLE_MAX_PRIVILEGE | SANDBOX_INERT,
                           NULL,
                           NULL,
                           NULL,
                           &FilteredToken);
    ok_hex(Status, STATUS_SUCCESS);

    /* We've disabled privileges, query the stats again */
    Status = NtQueryInformationToken(FilteredToken, TokenStatistics, TokenStats, Size, &Size);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to query the token statistics! (Status -> 0x%lx)\n", Status);
        return;
    }

    trace("Number of privileges after token filtering (privileges disabled with DISABLE_MAX_PRIVILEGE) -- %lu\n\n", TokenStats->PrivilegeCount);

    /* Close the filtered token and do another test */
    CloseHandle(FilteredToken);

    /* Fill in a privilege to delete */
    Priv.PrivilegeCount = 1;

    Priv.Privileges[0].Luid = RtlConvertUlongToLuid(SE_BACKUP_PRIVILEGE);
    Priv.Privileges[0].Attributes = 0;

    /* Delete the privileges */
    Status = NtFilterToken(Token,
                           0,
                           NULL,
                           &Priv,
                           NULL,
                           &FilteredToken);
    ok_hex(Status, STATUS_SUCCESS);

    /* We've deleted a privilege, query the stats again */
    Status = NtQueryInformationToken(FilteredToken, TokenStatistics, TokenStats, Size, &Size);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to query the token statistics! (Status -> 0x%lx)\n", Status);
        return;
    }

    trace("Number of privileges after token filtering (manually deleted privilege) -- %lu\n\n", TokenStats->PrivilegeCount);

    TestFilterParameters(Token);

    /* We're done */
    RtlFreeHeap(RtlGetProcessHeap(), 0, TokenStats);
    CloseHandle(Token);
    CloseHandle(FilteredToken);
}
