/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for LogonUserExExW with LOGON32_LOGON_NETWORK_CLEARTEXT (the logon OpenSSH uses
 *              for password authentication), and for the sspicli.dll export of the same function
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <lm.h>

#ifndef LOGON32_LOGON_NETWORK_CLEARTEXT
#define LOGON32_LOGON_NETWORK_CLEARTEXT 8
#endif

typedef BOOL (WINAPI *LOGONUSEREXEXW)(PWSTR, PWSTR, PWSTR, DWORD, DWORD, PTOKEN_GROUPS,
                                      PHANDLE, PSID *, PVOID *, PDWORD, PQUOTA_LIMITS);

static WCHAR TestPassword[] = L"Cleartext-Test-2026!";

static BOOL IsMember(HANDLE Token, WELL_KNOWN_SID_TYPE Type)
{
    UCHAR Sid[SECURITY_MAX_SID_SIZE];
    DWORD Size = sizeof(Sid);
    BOOL Member = FALSE;

    if (!CreateWellKnownSid(Type, NULL, Sid, &Size) || !CheckTokenMembership(Token, Sid, &Member))
    {
        ok(0, "membership check %d failed: %lu\n", Type, GetLastError());
        return FALSE;
    }
    return Member;
}

static BOOL CreateTestUser(PWSTR Name, SIZE_T NameLength)
{
    USER_INFO_1 Info;
    NET_API_STATUS Status = NERR_UserExists;
    DWORD Err, Try;

    for (Try = 0; Try < 5 && Status == NERR_UserExists; Try++)
    {
        StringCchPrintfW(Name, NameLength, L"clrtxt%lx", (GetTickCount() + Try * 7919) & 0xffffff);
        ZeroMemory(&Info, sizeof(Info));
        Info.usri1_name = Name;
        Info.usri1_password = TestPassword;
        Info.usri1_priv = USER_PRIV_USER;
        Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
        Status = NetUserAdd(NULL, 1, (PBYTE)&Info, &Err);
    }
    if (Status == ERROR_ACCESS_DENIED)
    {
        skip("NetUserAdd needs an elevated administrator\n");
        return FALSE;
    }
    ok(Status == NERR_Success, "NetUserAdd(%ls) failed: %lu\n", Name, Status);
    return Status == NERR_Success;
}

static VOID CheckLogon(LOGONUSEREXEXW LogonEx, PCWSTR What, PWSTR User, PWSTR Domain)
{
    HANDLE Token = NULL, Ident = NULL;
    TOKEN_TYPE Type;
    SECURITY_IMPERSONATION_LEVEL Level;
    DWORD Length;
    BOOL Ok;

    Ok = LogonEx(User, Domain, TestPassword, LOGON32_LOGON_NETWORK_CLEARTEXT,
                 LOGON32_PROVIDER_DEFAULT, NULL, &Token, NULL, NULL, NULL, NULL);
    ok(Ok, "%ls: logon of %ls failed: %lu\n", What, User, GetLastError());
    if (!Ok)
        return;

    /* Like LOGON32_LOGON_NETWORK, the logon yields an impersonation token (a server duplicates it to a primary one) */
    Ok = GetTokenInformation(Token, TokenType, &Type, sizeof(Type), &Length);
    ok(Ok, "%ls: GetTokenInformation failed: %lu\n", What, GetLastError());
    if (Ok)
        ok(Type == TokenImpersonation, "%ls: token type %d, expected an impersonation token\n", What, Type);
    Ok = GetTokenInformation(Token, TokenImpersonationLevel, &Level, sizeof(Level), &Length);
    ok(Ok, "%ls: GetTokenInformation(level) failed: %lu\n", What, GetLastError());
    if (Ok)
        ok(Level == SecurityImpersonation, "%ls: impersonation level %d\n", What, Level);

    Ok = DuplicateToken(Token, SecurityIdentification, &Ident);
    ok(Ok, "%ls: DuplicateToken failed: %lu\n", What, GetLastError());
    if (Ok)
    {
        ok(IsMember(Ident, WinNetworkSid), "%ls: the token has no NETWORK group\n", What);
        ok(!IsMember(Ident, WinInteractiveSid), "%ls: the token has an INTERACTIVE group\n", What);
        ok(IsMember(Ident, WinBuiltinUsersSid), "%ls: the user is not in the Users group\n", What);
        ok(!IsMember(Ident, WinBuiltinAdministratorsSid), "%ls: the user is an administrator\n", What);
        CloseHandle(Ident);
    }
    CloseHandle(Token);
}

START_TEST(LogonUserCleartext)
{
    HMODULE Advapi32 = GetModuleHandleW(L"advapi32.dll");
    HMODULE Sspicli = LoadLibraryW(L"sspicli.dll");
    LOGONUSEREXEXW AdvEx, SspEx;
    HANDLE Token = (HANDLE)(ULONG_PTR)0xdeadbeef;
    WCHAR User[64], Wrong[] = L"Not-The-Password-2026!";
    BOOL Ok;

    AdvEx = Advapi32 ? (LOGONUSEREXEXW)GetProcAddress(Advapi32, "LogonUserExExW") : NULL;
    SspEx = Sspicli ? (LOGONUSEREXEXW)GetProcAddress(Sspicli, "LogonUserExExW") : NULL;
    ok(AdvEx != NULL, "advapi32 does not export LogonUserExExW\n");
    ok(SspEx != NULL, "sspicli does not export LogonUserExExW\n");
    if (!AdvEx || !SspEx)
        return;

    /* No such user: refused, no token */
    SetLastError(0xdeadbeef);
    Ok = AdvEx(L"no-such-user-2026", L".", Wrong, LOGON32_LOGON_NETWORK_CLEARTEXT,
               LOGON32_PROVIDER_DEFAULT, NULL, &Token, NULL, NULL, NULL, NULL);
    ok(!Ok, "logon of a missing user succeeded\n");
    ok(GetLastError() == ERROR_LOGON_FAILURE, "logon of a missing user: error %lu\n", GetLastError());
    ok(Token == NULL, "logon of a missing user returned the token %p\n", Token);
    if (Ok && Token)
        CloseHandle(Token);

    if (!CreateTestUser(User, RTL_NUMBER_OF(User)))
        return;

    CheckLogon(AdvEx, L"advapi32 domain .", User, L".");
    CheckLogon(SspEx, L"sspicli domain .", User, L".");

    /* A wrong password */
    Token = (HANDLE)(ULONG_PTR)0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ok = AdvEx(User, L".", Wrong, LOGON32_LOGON_NETWORK_CLEARTEXT,
               LOGON32_PROVIDER_DEFAULT, NULL, &Token, NULL, NULL, NULL, NULL);
    ok(!Ok, "logon with a wrong password succeeded\n");
    ok(GetLastError() == ERROR_LOGON_FAILURE, "logon with a wrong password: error %lu\n", GetLastError());
    ok(Token == NULL, "logon with a wrong password returned the token %p\n", Token);
    if (Ok && Token)
        CloseHandle(Token);

    NetUserDel(NULL, User);
}
