/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for TokenElevationType, TokenElevation, TokenLinkedToken and the other NT 6 token classes
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * ReactOS has no UAC split token, so it answers as Windows does with UAC off (EnableLUA = 0, measured on
 * Windows 10 22H2, x64 and WoW64): TokenElevationTypeDefault, no linked token (ERROR_NO_SUCH_LOGON_SESSION),
 * UI access and virtualization off. With UAC on, Windows reports Full or Limited and a linked token; those
 * expectations are checked for consistency only.
 */

#include "precomp.h"

#define CLASS_VIRTUALIZATION_ALLOWED ((TOKEN_INFORMATION_CLASS)23)
#define CLASS_VIRTUALIZATION_ENABLED ((TOKEN_INFORMATION_CLASS)24)
#define CLASS_UIACCESS               ((TOKEN_INFORMATION_CLASS)26)
#define CLASS_MANDATORY_POLICY       ((TOKEN_INFORMATION_CLASS)27)
#define CLASS_IS_APPCONTAINER        ((TOKEN_INFORMATION_CLASS)29)

#define ELEVATION_TYPE_DEFAULT 1
#define ELEVATION_TYPE_FULL    2
#define ELEVATION_TYPE_LIMITED 3

/* UAC is on when the policy says so; a system without the policy key (ReactOS) has no UAC */
static BOOL IsUacEnabled(void)
{
    HKEY Key;
    DWORD Value = 0, Size = sizeof(Value), Type;
    LONG Error;
    BOOL Enabled = FALSE;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                          0, KEY_READ | KEY_WOW64_64KEY, &Key);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    Error = RegQueryValueExW(Key, L"EnableLUA", NULL, &Type, (PBYTE)&Value, &Size);
    if (Error == ERROR_SUCCESS && Type == REG_DWORD)
        Enabled = (Value != 0);
    RegCloseKey(Key);
    return Enabled;
}

static BOOL HasAdminGroup(void)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID AdminSid;
    BOOL IsMember = FALSE;

    if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &AdminSid))
        return FALSE;
    if (!CheckTokenMembership(NULL, AdminSid, &IsMember))
        IsMember = FALSE;
    FreeSid(AdminSid);
    return IsMember;
}

/* The class reports one DWORD, and takes any buffer of at least that size */
static void CheckDword(HANDLE Token, TOKEN_INFORMATION_CLASS Class, PCSTR Name, DWORD *Value)
{
    DWORD Buffer[2], Length;
    BOOL Ret;

    Buffer[0] = Buffer[1] = 0xdeadbeef;
    Length = 0;
    Ret = GetTokenInformation(Token, Class, Buffer, sizeof(DWORD), &Length);
    ok(Ret, "%s: failed with %lu\n", Name, GetLastError());
    ok(Length == sizeof(DWORD), "%s: length %lu\n", Name, Length);
    *Value = Ret ? Buffer[0] : 0xdeadbeef;

    Buffer[0] = Buffer[1] = 0xdeadbeef;
    Length = 0;
    Ret = GetTokenInformation(Token, Class, Buffer, sizeof(Buffer), &Length);
    ok(Ret, "%s: large buffer failed with %lu\n", Name, GetLastError());
    ok(Length == sizeof(DWORD), "%s: length %lu with a large buffer\n", Name, Length);
    ok(Buffer[1] == 0xdeadbeef, "%s: wrote past the DWORD\n", Name);

    SetLastError(0xdeadbeef);
    Length = 0;
    Ret = GetTokenInformation(Token, Class, Buffer, sizeof(DWORD) - 1, &Length);
    ok(!Ret, "%s: succeeded with a 3 byte buffer\n", Name);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "%s: error %lu\n", Name, GetLastError());
    ok(Length == sizeof(DWORD), "%s: length %lu\n", Name, Length);
}

static void TestLinkedToken(HANDLE Token, DWORD Type)
{
    HANDLE Linked[2];
    DWORD Length;
    BOOL Ret;

    Linked[0] = Linked[1] = (HANDLE)(ULONG_PTR)0xdeadbeef;
    SetLastError(0xdeadbeef);
    Length = 0;
    Ret = GetTokenInformation(Token, (TOKEN_INFORMATION_CLASS)19 /* TokenLinkedToken */, Linked, sizeof(HANDLE), &Length);

    if (Type == ELEVATION_TYPE_DEFAULT)
    {
        /* Not a split token: Windows answers STATUS_NO_SUCH_LOGON_SESSION, the buffer is untouched */
        ok(!Ret, "TokenLinkedToken succeeded for a token without a linked one\n");
        ok(GetLastError() == ERROR_NO_SUCH_LOGON_SESSION, "TokenLinkedToken: error %lu\n", GetLastError());
        ok(Length == sizeof(HANDLE), "TokenLinkedToken: length %lu\n", Length);
        ok(Linked[0] == (HANDLE)(ULONG_PTR)0xdeadbeef, "TokenLinkedToken: wrote %p\n", Linked[0]);
    }
    else
    {
        ok(Ret, "TokenLinkedToken failed with %lu for a split token\n", GetLastError());
        if (Ret)
            CloseHandle(Linked[0]);
    }

    /* The size has to be exactly that of a handle, whatever the token */
    SetLastError(0xdeadbeef);
    Length = 0;
    Ret = GetTokenInformation(Token, (TOKEN_INFORMATION_CLASS)19, Linked, sizeof(HANDLE) * 2, &Length);
    ok(!Ret, "TokenLinkedToken succeeded with a double size buffer\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "TokenLinkedToken: error %lu\n", GetLastError());
    ok(Length == sizeof(HANDLE), "TokenLinkedToken: length %lu\n", Length);

    SetLastError(0xdeadbeef);
    Length = 0;
    Ret = GetTokenInformation(Token, (TOKEN_INFORMATION_CLASS)19, NULL, 0, &Length);
    ok(!Ret, "TokenLinkedToken succeeded without a buffer\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "TokenLinkedToken: error %lu\n", GetLastError());
    ok(Length == sizeof(HANDLE), "TokenLinkedToken: length %lu\n", Length);
}

START_TEST(TokenElevation)
{
    HANDLE Token;
    DWORD Type, Elevated, Value, Length;
    BOOL Uac = IsUacEnabled(), Ret;
    DWORD Buffer[2];

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        skip("OpenProcessToken failed with %lu\n", GetLastError());
        return;
    }
    trace("UAC is %s\n", Uac ? "on" : "off");

    CheckDword(Token, TokenElevationType, "TokenElevationType", &Type);
    ok(Type >= ELEVATION_TYPE_DEFAULT && Type <= ELEVATION_TYPE_LIMITED, "TokenElevationType = %lu\n", Type);

    /* TokenElevation takes exactly a DWORD, unlike TokenElevationType */
    Buffer[0] = Buffer[1] = 0xdeadbeef;
    Length = 0;
    Ret = GetTokenInformation(Token, TokenElevation, Buffer, sizeof(DWORD), &Length);
    ok(Ret, "TokenElevation failed with %lu\n", GetLastError());
    ok(Length == sizeof(DWORD), "TokenElevation: length %lu\n", Length);
    Elevated = Ret ? Buffer[0] : 0xdeadbeef;

    SetLastError(0xdeadbeef);
    Length = 0;
    Ret = GetTokenInformation(Token, TokenElevation, Buffer, sizeof(Buffer), &Length);
    ok(!Ret, "TokenElevation succeeded with a large buffer\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "TokenElevation: error %lu\n", GetLastError());
    ok(Length == sizeof(DWORD), "TokenElevation: length %lu\n", Length);

    SetLastError(0xdeadbeef);
    Length = 0;
    Ret = GetTokenInformation(Token, TokenElevation, NULL, 0, &Length);
    ok(!Ret, "TokenElevation succeeded without a buffer\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "TokenElevation: error %lu\n", GetLastError());

    if (!Uac)
    {
        ok(Type == ELEVATION_TYPE_DEFAULT, "TokenElevationType = %lu with UAC off\n", Type);
        /* An administrator's token is elevated without UAC (the shell the tests run in is one) */
        if (HasAdminGroup())
            ok(Elevated == 1, "TokenElevation = %lu for an administrator with UAC off\n", Elevated);
        else
            trace("not an administrator: TokenElevation = %lu\n", Elevated);
    }
    else
    {
        if (Type == ELEVATION_TYPE_FULL)
            ok(Elevated == 1, "TokenElevation = %lu for a full token\n", Elevated);
        else if (Type == ELEVATION_TYPE_LIMITED)
            ok(Elevated == 0, "TokenElevation = %lu for a limited token\n", Elevated);
    }
    TestLinkedToken(Token, Type);

    /* The rest: off without UAC (virtualization can be on with UAC), same policy and no AppContainer everywhere */
    CheckDword(Token, CLASS_VIRTUALIZATION_ALLOWED, "TokenVirtualizationAllowed", &Value);
    if (!Uac)
        ok(Value == 0, "TokenVirtualizationAllowed = %lu\n", Value);
    CheckDword(Token, CLASS_VIRTUALIZATION_ENABLED, "TokenVirtualizationEnabled", &Value);
    if (!Uac)
        ok(Value == 0, "TokenVirtualizationEnabled = %lu\n", Value);
    CheckDword(Token, CLASS_UIACCESS, "TokenUIAccess", &Value);
    ok(Value == 0, "TokenUIAccess = %lu\n", Value);
    CheckDword(Token, CLASS_MANDATORY_POLICY, "TokenMandatoryPolicy", &Value);
    ok(Value == (TOKEN_MANDATORY_POLICY_NO_WRITE_UP | TOKEN_MANDATORY_POLICY_NEW_PROCESS_MIN),
       "TokenMandatoryPolicy = %lu\n", Value);
    CheckDword(Token, CLASS_IS_APPCONTAINER, "TokenIsAppContainer", &Value);
    ok(Value == 0, "TokenIsAppContainer = %lu\n", Value);

    CloseHandle(Token);
}
