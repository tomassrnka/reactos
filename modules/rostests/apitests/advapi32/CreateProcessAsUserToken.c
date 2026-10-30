/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CreateProcessAsUser with an impersonation token, as OpenSSH's sshd calls it with the
 *              token of a LOGON32_LOGON_NETWORK_CLEARTEXT logon: Windows makes the process token
 *              from it when its level is impersonation or above, and fails with
 *              ERROR_BAD_IMPERSONATION_LEVEL below
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does. The tokens are copies of the caller's own, which
 * need no assignment privilege.
 */

#include "precomp.h"
#include <lm.h>

#define CHILD_EXIT 7

static void Run(PCSTR What, HANDLE Token, BOOL ExpectSuccess, DWORD ExpectedError)
{
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    WCHAR Command[] = L"cmd.exe /c exit 7";
    DWORD Exit = 0, Error;
    BOOL Ok;

    ZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    SetLastError(0xdeadbeef);
    Ok = CreateProcessAsUserW(Token, NULL, Command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &Si, &Pi);
    Error = GetLastError();
    if (ExpectSuccess)
    {
        ok(Ok, "%s: CreateProcessAsUser failed: %lu\n", What, Error);
        if (Ok)
        {
            ok(WaitForSingleObject(Pi.hProcess, 30000) == WAIT_OBJECT_0, "%s: the child did not exit\n", What);
            GetExitCodeProcess(Pi.hProcess, &Exit);
            ok(Exit == CHILD_EXIT, "%s: exit code %lu\n", What, Exit);
            CloseHandle(Pi.hProcess);
            CloseHandle(Pi.hThread);
        }
    }
    else
    {
        ok(!Ok, "%s: CreateProcessAsUser succeeded\n", What);
        ok(Error == ExpectedError, "%s: error %lu, expected %lu\n", What, Error, ExpectedError);
        if (Ok)
        {
            TerminateProcess(Pi.hProcess, 0);
            CloseHandle(Pi.hProcess);
            CloseHandle(Pi.hThread);
        }
    }
}

static void Case(PCSTR What, HANDLE Self, TOKEN_TYPE Type, SECURITY_IMPERSONATION_LEVEL Level, BOOL ExpectSuccess)
{
    HANDLE Token = NULL;

    ok(DuplicateTokenEx(Self, MAXIMUM_ALLOWED, NULL, Level, Type, &Token), "%s: DuplicateTokenEx failed: %lu\n",
       What, GetLastError());
    if (Token)
    {
        Run(What, Token, ExpectSuccess, ERROR_BAD_IMPERSONATION_LEVEL);
        CloseHandle(Token);
    }
}


static BOOL HasPrivilege(HANDLE Token, PCWSTR Name)
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

/*
 * A token of ANOTHER user, impersonation or primary, needs SeAssignPrimaryToken whatever its type:
 * accepting impersonation tokens must not open a way around the privilege.
 */
static void ForeignToken(HANDLE Self)
{
    static WCHAR Password[] = L"Foreign-Token-2026!";
    WCHAR User[32];
    USER_INFO_1 Info;
    NET_API_STATUS Status = NERR_UserExists;
    DWORD Err, Try;
    HANDLE Impersonation = NULL, Primary = NULL;

    if (HasPrivilege(Self, L"SeAssignPrimaryTokenPrivilege"))
    {
        skip("The caller holds SeAssignPrimaryTokenPrivilege\n");
        return;
    }
    for (Try = 0; Try < 5 && Status == NERR_UserExists; Try++)
    {
        StringCchPrintfW(User, RTL_NUMBER_OF(User), L"cpafor%lx", (GetTickCount() + Try * 7919) & 0xffffff);
        ZeroMemory(&Info, sizeof(Info));
        Info.usri1_name = User;
        Info.usri1_password = Password;
        Info.usri1_priv = USER_PRIV_USER;
        Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
        Status = NetUserAdd(NULL, 1, (PBYTE)&Info, &Err);
    }
    if (Status != NERR_Success)
    {
        skip("NetUserAdd failed: %lu\n", Status);
        return;
    }
    if (LogonUserW(User, L".", Password, 8 /* LOGON32_LOGON_NETWORK_CLEARTEXT */, LOGON32_PROVIDER_DEFAULT, &Impersonation))
    {
        Run("another user's impersonation token", Impersonation, FALSE, ERROR_PRIVILEGE_NOT_HELD);
        if (DuplicateTokenEx(Impersonation, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &Primary))
        {
            Run("another user's primary token", Primary, FALSE, ERROR_PRIVILEGE_NOT_HELD);
            CloseHandle(Primary);
        }
        CloseHandle(Impersonation);
    }
    else
        ok(0, "LogonUser failed: %lu\n", GetLastError());
    NetUserDel(NULL, User);
}

START_TEST(CreateProcessAsUserToken)
{
    HANDLE Self = NULL;

    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &Self), "OpenProcessToken failed: %lu\n",
       GetLastError());
    if (!Self)
        return;

    Case("primary", Self, TokenPrimary, SecurityImpersonation, TRUE);
    Case("impersonation, Impersonation level", Self, TokenImpersonation, SecurityImpersonation, TRUE);
    Case("impersonation, Delegation level", Self, TokenImpersonation, SecurityDelegation, TRUE);
    Case("impersonation, Identification level", Self, TokenImpersonation, SecurityIdentification, FALSE);
    Case("impersonation, Anonymous level", Self, TokenImpersonation, SecurityAnonymous, FALSE);
    ForeignToken(Self);
    CloseHandle(Self);
}
