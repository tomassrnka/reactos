/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     LookupAccountSid names the account of the caller a user account even when it is a
 *              well-known one (SYSTEM looking up S-1-5-18), and the other well-known SIDs groups.
 *              OpenSSH for Windows checks that its own account is a user account.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does, as an administrator and as SYSTEM.
 */

#include "precomp.h"
#include <sddl.h>

static PSID CallerSid(PUCHAR Buffer, ULONG Size)
{
    HANDLE Token;
    ULONG Length;
    PSID Sid = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return NULL;
    if (GetTokenInformation(Token, TokenUser, Buffer, Size, &Length))
        Sid = ((PTOKEN_USER)Buffer)->User.Sid;
    CloseHandle(Token);
    return Sid;
}

static void CheckSid(PCSTR Text, PSID Caller, SID_NAME_USE WellKnown)
{
    WCHAR Name[256], Domain[256];
    DWORD NameLength = RTL_NUMBER_OF(Name), DomainLength = RTL_NUMBER_OF(Domain);
    SID_NAME_USE Use = (SID_NAME_USE)0;
    PSID Sid = NULL;
    SID_NAME_USE Expected;
    BOOL Ok;

    ok(ConvertStringSidToSidA(Text, &Sid), "%s: ConvertStringSidToSid failed: %lu\n", Text, GetLastError());
    if (!Sid)
        return;
    Ok = LookupAccountSidW(NULL, Sid, Name, &NameLength, Domain, &DomainLength, &Use);
    ok(Ok, "%s: LookupAccountSid failed: %lu\n", Text, GetLastError());
    Expected = EqualSid(Sid, Caller) ? SidTypeUser : WellKnown;
    ok(Use == Expected, "%s: use %d, expected %d\n", Text, Use, Expected);
    LocalFree(Sid);
}

START_TEST(LookupAccountSidCaller)
{
    UCHAR Buffer[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
    PSID Caller = CallerSid(Buffer, sizeof(Buffer));
    WCHAR Name[256], Domain[256];
    DWORD NameLength = RTL_NUMBER_OF(Name), DomainLength = RTL_NUMBER_OF(Domain);
    SID_NAME_USE Use = (SID_NAME_USE)0;

    ok(Caller != NULL, "no token user\n");
    if (!Caller)
        return;

    /* The caller's own account is a user, whatever it is */
    ok(LookupAccountSidW(NULL, Caller, Name, &NameLength, Domain, &DomainLength, &Use),
       "LookupAccountSid of the caller failed: %lu\n", GetLastError());
    ok(Use == SidTypeUser, "the caller's account is of use %d\n", Use);

    /* The well-known accounts: a group, unless the caller is that account */
    CheckSid("S-1-5-18", Caller, SidTypeWellKnownGroup);
    CheckSid("S-1-5-19", Caller, SidTypeWellKnownGroup);
    CheckSid("S-1-5-20", Caller, SidTypeWellKnownGroup);
    CheckSid("S-1-1-0", Caller, SidTypeWellKnownGroup);
    CheckSid("S-1-5-11", Caller, SidTypeWellKnownGroup);
}
