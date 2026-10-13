/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that changing the time zone requires SeTimeZonePrivilege
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <lm.h>
#include <ndk/setypes.h>

#define TEST_PASSWORD L"Tz!Priv-2026x"

static const WCHAR TimeZoneKey[] = L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation";

static BOOL
SetPrivilege(HANDLE hToken, ULONG Privilege, BOOL Enable)
{
    TOKEN_PRIVILEGES Tp;

    Tp.PrivilegeCount = 1;
    Tp.Privileges[0].Luid.LowPart = Privilege;
    Tp.Privileges[0].Luid.HighPart = 0;
    Tp.Privileges[0].Attributes = Enable ? SE_PRIVILEGE_ENABLED : 0;
    return AdjustTokenPrivileges(hToken, FALSE, &Tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
}

static LONG
RegistryBias(VOID)
{
    HKEY hKey;
    DWORD Bias = 0xDEADBEEF, Size = sizeof(Bias);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TimeZoneKey, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        RegQueryValueExW(hKey, L"Bias", NULL, NULL, (LPBYTE)&Bias, &Size);
        RegCloseKey(hKey);
    }
    return (LONG)Bias;
}

static LONG
KernelBias(VOID)
{
    RTL_TIME_ZONE_INFORMATION Tzi;
    ULONG Length;

    if (!NT_SUCCESS(NtQuerySystemInformation(SystemCurrentTimeZoneInformation, &Tzi, sizeof(Tzi), &Length)))
        return 0xDEADBEEF;
    return Tzi.Bias;
}

/* Windows 10 lets a caller that holds the privilege change the zone without enabling it; NT 6.1 does not */
static BOOL
HolderNeedsNoEnable(VOID)
{
    DWORD Version = GetVersion();
    BYTE Major = LOBYTE(LOWORD(Version)), Minor = HIBYTE(LOWORD(Version));

    return (Major > 6 || (Major == 6 && Minor > 1));
}

/* Without the privilege nothing changes: not the kernel's zone, not the registry */
static VOID
TestDenied(PCSTR Who, const TIME_ZONE_INFORMATION *Other, const TIME_ZONE_INFORMATION *Orig, BOOL HoldsPrivilege)
{
    LONG OrigBias = Orig->Bias;
    RTL_TIME_ZONE_INFORMATION Current;
    ULONG Length;
    NTSTATUS Status;
    BOOL Ret;

    Status = NtQuerySystemInformation(SystemCurrentTimeZoneInformation, &Current, sizeof(Current), &Length);
    ok_ntstatus(Status, STATUS_SUCCESS);
    /* Windows 6.1 no longer takes this class from user mode at all */
    Status = NtSetSystemInformation(SystemCurrentTimeZoneInformation, &Current, sizeof(Current));
    ok(Status == STATUS_PRIVILEGE_NOT_HELD || Status == STATUS_INVALID_INFO_CLASS,
       "%s: NtSetSystemInformation returned 0x%lx\n", Who, Status);

    if (HoldsPrivilege && HolderNeedsNoEnable())
    {
        trace("%s: this version does not require the held privilege to be enabled\n", Who);
        return;
    }

    SetLastError(0xDEADBEEF);
    Ret = SetTimeZoneInformation(Other);
    ok(!Ret, "%s: SetTimeZoneInformation succeeded\n", Who);
    ok(GetLastError() == ERROR_PRIVILEGE_NOT_HELD, "%s: error %lu\n", Who, GetLastError());
    ok(KernelBias() == OrigBias, "%s: kernel bias %ld, expected %ld\n", Who, KernelBias(), OrigBias);
    ok(RegistryBias() == OrigBias, "%s: registry bias %ld, expected %ld\n", Who, RegistryBias(), OrigBias);
    if (Ret)
        SetTimeZoneInformation(Orig);
}

static VOID
TestAllowed(PCSTR Who, const TIME_ZONE_INFORMATION *Other, const TIME_ZONE_INFORMATION *Orig)
{
    BOOL Ret;

    Ret = SetTimeZoneInformation(Other);
    ok(Ret, "%s: SetTimeZoneInformation failed with %lu\n", Who, GetLastError());
    ok(KernelBias() == Other->Bias, "%s: kernel bias %ld, expected %ld\n", Who, KernelBias(), Other->Bias);
    ok(RegistryBias() == Other->Bias, "%s: registry bias %ld, expected %ld\n", Who, RegistryBias(), Other->Bias);
    Ret = SetTimeZoneInformation(Orig);
    ok(Ret, "%s: restoring the zone failed with %lu\n", Who, GetLastError());
}

static BOOL
TokenHasPrivilege(HANDLE hToken, ULONG Privilege)
{
    BYTE Buffer[2048];
    PTOKEN_PRIVILEGES Privileges = (PTOKEN_PRIVILEGES)Buffer;
    DWORD Length, i;

    if (!GetTokenInformation(hToken, TokenPrivileges, Buffer, sizeof(Buffer), &Length))
        return FALSE;
    for (i = 0; i < Privileges->PrivilegeCount; i++)
    {
        if (Privileges->Privileges[i].Luid.LowPart == Privilege && Privileges->Privileges[i].Luid.HighPart == 0)
            return TRUE;
    }
    return FALSE;
}

static VOID
TestStandardUser(const TIME_ZONE_INFORMATION *Orig, const TIME_ZONE_INFORMATION *Other)
{
    USER_INFO_1 UserInfo;
    LOCALGROUP_MEMBERS_INFO_3 Member;
    BYTE UsersSid[SECURITY_MAX_SID_SIZE];
    WCHAR UsersName[256], Domain[256];
    DWORD SidSize = sizeof(UsersSid), NameSize = _countof(UsersName), DomainSize = _countof(Domain);
    SID_NAME_USE Use;
    HANDLE hLogon = NULL, hImpersonation = NULL;
    NET_API_STATUS NetStatus;
    DWORD ParmError;
    WCHAR UserName[20];

    /* A name of our own; an existing account is never touched */
    StringCbPrintfW(UserName, sizeof(UserName), L"tzp%08lx", GetTickCount() ^ GetCurrentProcessId());
    RtlZeroMemory(&UserInfo, sizeof(UserInfo));
    UserInfo.usri1_name = UserName;
    UserInfo.usri1_password = TEST_PASSWORD;
    UserInfo.usri1_priv = USER_PRIV_USER;
    UserInfo.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    NetStatus = NetUserAdd(NULL, 1, (LPBYTE)&UserInfo, &ParmError);
    if (NetStatus != NERR_Success)
    {
        skip("NetUserAdd failed with %lu\n", NetStatus);
        return;
    }
    if (CreateWellKnownSid(WinBuiltinUsersSid, NULL, UsersSid, &SidSize) &&
        LookupAccountSidW(NULL, UsersSid, UsersName, &NameSize, Domain, &DomainSize, &Use))
    {
        Member.lgrmi3_domainandname = UserName;
        NetLocalGroupAddMembers(NULL, UsersName, 3, (LPBYTE)&Member, 1);
    }

    if (!LogonUserW(UserName, L".", TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hLogon))
    {
        skip("LogonUser failed with %lu\n", GetLastError());
        goto done;
    }
    /* Server 2008 R2 grants it to Administrators and LOCAL SERVICE only; Windows 10 also to Users */
    if (TokenHasPrivilege(hLogon, SE_TIME_ZONE_PRIVILEGE))
    {
        skip("This system grants SeTimeZonePrivilege to Users\n");
        goto done;
    }
    ok(!TokenHasPrivilege(hLogon, SE_SYSTEMTIME_PRIVILEGE), "The standard user holds SeSystemtimePrivilege\n");
    if (!DuplicateTokenEx(hLogon, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &hImpersonation) ||
        !SetThreadToken(NULL, hImpersonation))
    {
        skip("Impersonation failed with %lu\n", GetLastError());
        goto done;
    }

    TestDenied("standard user", Other, Orig, FALSE);
    ok(!SetPrivilege(hImpersonation, SE_TIME_ZONE_PRIVILEGE, TRUE), "The standard user enabled SeTimeZonePrivilege\n");
    TestDenied("standard user, after trying to enable", Other, Orig, FALSE);
    RevertToSelf();

done:
    if (hImpersonation)
        CloseHandle(hImpersonation);
    if (hLogon)
        CloseHandle(hLogon);
    ok(NetUserDel(NULL, UserName) == NERR_Success, "NetUserDel failed\n");
}

START_TEST(TzSetPrivilege)
{
    TIME_ZONE_INFORMATION Orig, Other;
    HANDLE hToken;

    if (GetTimeZoneInformation(&Orig) == TIME_ZONE_ID_INVALID)
    {
        skip("GetTimeZoneInformation failed with %lu\n", GetLastError());
        return;
    }
    Other = Orig;
    Other.Bias = (Orig.Bias == 120) ? 180 : 120;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        skip("OpenProcessToken failed with %lu\n", GetLastError());
        return;
    }

    /* The administrator with the privilege disabled, and with only SeSystemtimePrivilege */
    SetPrivilege(hToken, SE_TIME_ZONE_PRIVILEGE, FALSE);
    SetPrivilege(hToken, SE_SYSTEMTIME_PRIVILEGE, FALSE);
    TestDenied("administrator, privilege disabled", &Other, &Orig, TokenHasPrivilege(hToken, SE_TIME_ZONE_PRIVILEGE));
    if (SetPrivilege(hToken, SE_SYSTEMTIME_PRIVILEGE, TRUE))
    {
        TestDenied("administrator, SeSystemtimePrivilege only", &Other, &Orig, TokenHasPrivilege(hToken, SE_TIME_ZONE_PRIVILEGE));
        SetPrivilege(hToken, SE_SYSTEMTIME_PRIVILEGE, FALSE);
    }

    TestStandardUser(&Orig, &Other);

    ok(TokenHasPrivilege(hToken, SE_TIME_ZONE_PRIVILEGE), "The administrator lacks SeTimeZonePrivilege\n");
    if (SetPrivilege(hToken, SE_TIME_ZONE_PRIVILEGE, TRUE))
        TestAllowed("administrator, privilege enabled", &Other, &Orig);
    else
        skip("Cannot enable SeTimeZonePrivilege\n");
    SetTimeZoneInformation(&Orig);
    SetPrivilege(hToken, SE_TIME_ZONE_PRIVILEGE, FALSE);
    CloseHandle(hToken);
}
