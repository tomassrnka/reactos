/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A logon token carries the privileges granted to the user account itself (not only the
 *              ones of its groups). OpenSSH's sshd runs under an account with SeAssignPrimaryToken,
 *              SeTcb and SeIncreaseQuota granted so.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Not checked on the Windows 10 oracle: it would change the LSA policy there.
 */

#include "precomp.h"
#include <lm.h>
#include <ntsecapi.h>

#ifndef LOGON32_LOGON_NETWORK_CLEARTEXT
#define LOGON32_LOGON_NETWORK_CLEARTEXT 8
#endif

static WCHAR TestPassword[] = L"User-Rights-Test-2026!";

static BOOL TokenHasPrivilege(HANDLE Token, PCWSTR Name)
{
    UCHAR Buffer[2048];
    PTOKEN_PRIVILEGES Privileges = (PTOKEN_PRIVILEGES)Buffer;
    DWORD Length, i;
    LUID Luid;

    if (!LookupPrivilegeValueW(NULL, Name, &Luid) ||
        !GetTokenInformation(Token, TokenPrivileges, Buffer, sizeof(Buffer), &Length))
        return FALSE;
    for (i = 0; i < Privileges->PrivilegeCount; i++)
    {
        if (Privileges->Privileges[i].Luid.LowPart == Luid.LowPart &&
            Privileges->Privileges[i].Luid.HighPart == Luid.HighPart)
            return TRUE;
    }
    return FALSE;
}

static BOOL Logon(PWSTR User, PHANDLE Token)
{
    return LogonUserW(User, L".", TestPassword, LOGON32_LOGON_NETWORK_CLEARTEXT, LOGON32_PROVIDER_DEFAULT, Token);
}

START_TEST(LogonUserUserRights)
{
    WCHAR User[32];
    USER_INFO_1 Info;
    BYTE Sid[SECURITY_MAX_SID_SIZE];
    WCHAR Domain[256];
    DWORD SidLength = sizeof(Sid), DomainLength = RTL_NUMBER_OF(Domain), Err, Try;
    SID_NAME_USE Use;
    HANDLE Token = NULL;
    LSA_OBJECT_ATTRIBUTES Attributes = {0};
    LSA_HANDLE Policy;
    LSA_UNICODE_STRING Right;
    static WCHAR RightName[] = L"SeSystemEnvironmentPrivilege";
    NET_API_STATUS Status = NERR_UserExists;
    NTSTATUS NtStatus;

    for (Try = 0; Try < 5 && Status == NERR_UserExists; Try++)
    {
        StringCchPrintfW(User, RTL_NUMBER_OF(User), L"urights%lx", (GetTickCount() + Try * 7919) & 0xffffff);
        ZeroMemory(&Info, sizeof(Info));
        Info.usri1_name = User;
        Info.usri1_password = TestPassword;
        Info.usri1_priv = USER_PRIV_USER;
        Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
        Status = NetUserAdd(NULL, 1, (PBYTE)&Info, &Err);
    }
    if (Status == ERROR_ACCESS_DENIED)
    {
        skip("NetUserAdd needs an elevated administrator\n");
        return;
    }
    ok(Status == NERR_Success, "NetUserAdd failed: %lu\n", Status);
    if (Status != NERR_Success)
        return;

    ok(LookupAccountNameW(NULL, User, Sid, &SidLength, Domain, &DomainLength, &Use),
       "LookupAccountName failed: %lu\n", GetLastError());

    /* Before: no such privilege */
    ok(Logon(User, &Token), "LogonUser failed: %lu\n", GetLastError());
    if (Token)
    {
        ok(!TokenHasPrivilege(Token, RightName), "the token has the privilege before it was granted\n");
        CloseHandle(Token);
        Token = NULL;
    }

    NtStatus = LsaOpenPolicy(NULL, &Attributes, POLICY_ALL_ACCESS, &Policy);
    ok(NT_SUCCESS(NtStatus), "LsaOpenPolicy failed: 0x%08lx\n", NtStatus);
    if (NT_SUCCESS(NtStatus))
    {
        Right.Buffer = RightName;
        Right.Length = (USHORT)(wcslen(RightName) * sizeof(WCHAR));
        Right.MaximumLength = Right.Length + sizeof(WCHAR);
        NtStatus = LsaAddAccountRights(Policy, Sid, &Right, 1);
        ok(NT_SUCCESS(NtStatus), "LsaAddAccountRights failed: 0x%08lx\n", NtStatus);

        /* After: the token has it */
        ok(Logon(User, &Token), "LogonUser failed: %lu\n", GetLastError());
        if (Token)
        {
            ok(TokenHasPrivilege(Token, RightName), "the token lacks the privilege granted to the user\n");
            CloseHandle(Token);
            Token = NULL;
        }

        /* Leave no account behind (removing a right is not what this test is about) */
        LsaRemoveAccountRights(Policy, Sid, TRUE, NULL, 0);
        LsaClose(Policy);
    }

    NetUserDel(NULL, User);
}
