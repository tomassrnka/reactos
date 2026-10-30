/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CreateEnvironmentBlock for the token of a user whose profile is not loaded: it succeeds
 *              with the system variables, USERNAME, USERDOMAIN and the default profile as USERPROFILE.
 *              OpenSSH for Windows (sshd) builds its sessions' environment so before it loads any profile.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does.
 */

#include "precomp.h"
#include <lm.h>
#include <userenv.h>

#ifndef LOGON32_LOGON_NETWORK_CLEARTEXT
#define LOGON32_LOGON_NETWORK_CLEARTEXT 8
#endif

static WCHAR TestPassword[] = L"Env-Block-Test-2026!";

/* The value of a variable in an environment block, NULL if there is none */
static PCWSTR FindVariable(PCWSTR Env, PCWSTR Name)
{
    SIZE_T Length = wcslen(Name);

    for (; *Env; Env += wcslen(Env) + 1)
    {
        if (!_wcsnicmp(Env, Name, Length) && Env[Length] == L'=')
            return Env + Length + 1;
    }
    return NULL;
}

START_TEST(CreateEnvironmentBlockProfile)
{
    WCHAR User[32];
    USER_INFO_1 Info;
    HANDLE Logon = NULL, Token = NULL;
    LPVOID Env;
    PCWSTR Value;
    NET_API_STATUS Status = NERR_UserExists;
    DWORD Err, Try;
    BOOL Ok;

    for (Try = 0; Try < 5 && Status == NERR_UserExists; Try++)
    {
        StringCchPrintfW(User, RTL_NUMBER_OF(User), L"envblk%lx", (GetTickCount() + Try * 7919) & 0xffffff);
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

    /* A logon does not load the profile */
    Ok = LogonUserW(User, L".", TestPassword, LOGON32_LOGON_NETWORK_CLEARTEXT, LOGON32_PROVIDER_DEFAULT, &Logon);
    ok(Ok, "LogonUser failed: %lu\n", GetLastError());
    if (Ok)
    {
        ok(DuplicateTokenEx(Logon, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &Token),
           "DuplicateTokenEx failed: %lu\n", GetLastError());

        Env = (LPVOID)(ULONG_PTR)0x1234;
        SetLastError(0xdeadbeef);
        Ok = CreateEnvironmentBlock(&Env, Token, FALSE);
        ok(Ok, "CreateEnvironmentBlock failed: %lu\n", GetLastError());
        ok(Env != NULL && Env != (LPVOID)(ULONG_PTR)0x1234, "the environment is %p\n", Env);
        if (Ok && Env)
        {
            Value = FindVariable(Env, L"USERNAME");
            ok(Value && !_wcsicmp(Value, User), "USERNAME is %ls\n", Value ? Value : L"(none)");
            Value = FindVariable(Env, L"USERDOMAIN");
            ok(Value != NULL, "no USERDOMAIN\n");
            Value = FindVariable(Env, L"USERPROFILE");
            ok(Value != NULL, "no USERPROFILE\n");
            if (Value)
            {
                /* the default profile: <profiles>\Default or <profiles>\Default User */
                ok(wcsstr(Value, L"\\Default") != NULL, "USERPROFILE is %ls\n", Value);
            }
            ok(FindVariable(Env, L"SystemRoot") != NULL, "no SystemRoot\n");
            DestroyEnvironmentBlock(Env);
        }
        if (Token)
            CloseHandle(Token);
        CloseHandle(Logon);
    }

    Status = NetUserDel(NULL, User);
    ok(Status == NERR_Success, "NetUserDel failed: %lu\n", Status);
}
