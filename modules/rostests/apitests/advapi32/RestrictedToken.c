/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for CreateRestrictedToken (NtFilterToken) and access checks with filtered tokens
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SE_CHANGE_NOTIFY_PRIVILEGE
#define SE_CHANGE_NOTIFY_PRIVILEGE 23
#endif
#define SE_SHUTDOWN_PRIVILEGE_ 19
#define SE_UNDOCK_PRIVILEGE_ 25
#define SE_INC_WORKING_SET_PRIVILEGE_ 33
#define SE_TIME_ZONE_PRIVILEGE_ 34
#ifndef DISABLE_MAX_PRIVILEGE
#define DISABLE_MAX_PRIVILEGE 0x1
#endif
#ifndef SANDBOX_INERT
#define SANDBOX_INERT 0x2
#endif
#ifndef LUA_TOKEN
#define LUA_TOKEN 0x4
#endif
#ifndef WRITE_RESTRICTED
#define WRITE_RESTRICTED 0x8
#endif

BOOL WINAPI IsTokenRestricted(HANDLE TokenHandle);

#define R_ 0x1
#define W_ 0x2
#define X_ 0x4
static GENERIC_MAPPING Mapping = { R_, W_, X_, R_ | W_ | X_ | 0x8 };

static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};

static BYTE EveryoneBuf[SECURITY_MAX_SID_SIZE], AuthUsersBuf[SECURITY_MAX_SID_SIZE];
static BYTE RestrictedCodeBuf[SECURITY_MAX_SID_SIZE], SystemBuf[SECURITY_MAX_SID_SIZE];
static BYTE UserBuf[SECURITY_MAX_SID_SIZE], AdminsBuf[SECURITY_MAX_SID_SIZE];
static BYTE NotInTokenBuf[SECURITY_MAX_SID_SIZE];
static PSID Everyone = EveryoneBuf, AuthUsers = AuthUsersBuf, RestrictedCode = RestrictedCodeBuf;
static PSID LocalSystem = SystemBuf, User = UserBuf, Admins = AdminsBuf, NotInToken = NotInTokenBuf;

static BOOL IsWindows8OrNewer(void)
{
    DWORD Version = GetVersion();
    BYTE Major = LOBYTE(LOWORD(Version)), Minor = HIBYTE(LOWORD(Version));
    return Major > 6 || (Major == 6 && Minor >= 2);
}

static void MakeSid(PSID Sid, PSID_IDENTIFIER_AUTHORITY Authority, BYTE Count, DWORD Sub0, DWORD Sub1)
{
    InitializeSid(Sid, Authority, Count);
    *GetSidSubAuthority(Sid, 0) = Sub0;
    if (Count > 1)
        *GetSidSubAuthority(Sid, 1) = Sub1;
}

static PVOID QueryToken(HANDLE Token, TOKEN_INFORMATION_CLASS Class)
{
    DWORD Size = 0;
    PVOID Buffer;

    GetTokenInformation(Token, Class, NULL, 0, &Size);
    if (!Size)
        return NULL;
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (Buffer && !GetTokenInformation(Token, Class, Buffer, Size, &Size))
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return NULL;
    }
    return Buffer;
}

/* Attributes of Sid in the token's TokenGroups (or TokenRestrictedSids), ~0 if absent */
static DWORD GroupAttributes(HANDLE Token, TOKEN_INFORMATION_CLASS Class, PSID Sid)
{
    PTOKEN_GROUPS Groups = QueryToken(Token, Class);
    DWORD i, Attributes = ~0UL;

    if (!Groups)
        return ~0UL;
    for (i = 0; i < Groups->GroupCount; i++)
    {
        if (EqualSid(Groups->Groups[i].Sid, Sid))
        {
            Attributes = Groups->Groups[i].Attributes;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, Groups);
    return Attributes;
}

static DWORD GroupCount(HANDLE Token, TOKEN_INFORMATION_CLASS Class)
{
    PTOKEN_GROUPS Groups = QueryToken(Token, Class);
    DWORD Count;

    if (!Groups)
        return ~0UL;
    Count = Groups->GroupCount;
    HeapFree(GetProcessHeap(), 0, Groups);
    return Count;
}

static BOOL HasPrivilege(HANDLE Token, DWORD Privilege, PDWORD Attributes)
{
    PTOKEN_PRIVILEGES Privileges = QueryToken(Token, TokenPrivileges);
    DWORD i;
    BOOL Found = FALSE;

    if (!Privileges)
        return FALSE;
    for (i = 0; i < Privileges->PrivilegeCount; i++)
    {
        if (Privileges->Privileges[i].Luid.LowPart == Privilege && !Privileges->Privileges[i].Luid.HighPart)
        {
            Found = TRUE;
            if (Attributes)
                *Attributes = Privileges->Privileges[i].Attributes;
        }
    }
    HeapFree(GetProcessHeap(), 0, Privileges);
    return Found;
}

static DWORD PrivilegeCount(HANDLE Token)
{
    PTOKEN_PRIVILEGES Privileges = QueryToken(Token, TokenPrivileges);
    DWORD Count;

    if (!Privileges)
        return ~0UL;
    Count = Privileges->PrivilegeCount;
    HeapFree(GetProcessHeap(), 0, Privileges);
    return Count;
}

static BOOL Filter(HANDLE Token, DWORD Flags, DWORD DisableCount, PSID *Disable,
                   DWORD DeleteCount, DWORD *Delete, DWORD RestrictCount, PSID *Restrict, PHANDLE NewToken)
{
    SID_AND_ATTRIBUTES DisableSids[4], RestrictSids[4];
    LUID_AND_ATTRIBUTES DeletePrivileges[4];
    DWORD i;

    for (i = 0; i < DisableCount; i++)
    {
        DisableSids[i].Sid = Disable[i];
        DisableSids[i].Attributes = 0;
    }
    for (i = 0; i < DeleteCount; i++)
    {
        DeletePrivileges[i].Luid.LowPart = Delete[i];
        DeletePrivileges[i].Luid.HighPart = 0;
        DeletePrivileges[i].Attributes = 0;
    }
    for (i = 0; i < RestrictCount; i++)
    {
        RestrictSids[i].Sid = Restrict[i];
        RestrictSids[i].Attributes = 0;
    }
    *NewToken = NULL;
    return CreateRestrictedToken(Token, Flags,
                                 DisableCount, DisableCount ? DisableSids : NULL,
                                 DeleteCount, DeleteCount ? DeletePrivileges : NULL,
                                 RestrictCount, RestrictCount ? RestrictSids : NULL,
                                 NewToken);
}

static void TestGroups(HANDLE Token)
{
    HANDLE Filtered;
    PSID Two[2] = { Everyone, AuthUsers };
    PSID Missing[1] = { NotInToken };
    PSID UserOnly[1] = { User };
    PTOKEN_USER UserInfo;
    PTOKEN_OWNER Owner;
    DWORD Attributes, Count;

    ok(GroupAttributes(Token, TokenGroups, Everyone) & SE_GROUP_ENABLED, "Everyone not enabled in the source token\n");
    ok(GroupAttributes(Token, TokenGroups, AuthUsers) & SE_GROUP_ENABLED, "Authenticated Users not enabled in the source token\n");
    Count = GroupCount(Token, TokenGroups);

    /* Two groups at once: both become deny-only, the rest stays as it was */
    ok(Filter(Token, 0, 2, Two, 0, NULL, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        Attributes = GroupAttributes(Filtered, TokenGroups, Everyone);
        ok(Attributes == SE_GROUP_USE_FOR_DENY_ONLY, "Everyone attributes 0x%lx\n", Attributes);
        ok(!(Attributes & (SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT)), "Everyone still enabled: 0x%lx\n", Attributes);
        ok(Attributes & SE_GROUP_USE_FOR_DENY_ONLY, "Everyone not deny-only: 0x%lx\n", Attributes);
        Attributes = GroupAttributes(Filtered, TokenGroups, AuthUsers);
        ok(Attributes & SE_GROUP_USE_FOR_DENY_ONLY, "Authenticated Users not deny-only: 0x%lx\n", Attributes);
        ok(!(Attributes & SE_GROUP_ENABLED), "Authenticated Users still enabled: 0x%lx\n", Attributes);
        ok(GroupCount(Filtered, TokenGroups) == Count, "Group count %lu, source %lu\n", GroupCount(Filtered, TokenGroups), Count);
        ok(GroupAttributes(Filtered, TokenGroups, Admins) == GroupAttributes(Token, TokenGroups, Admins),
           "Administrators changed: 0x%lx\n", GroupAttributes(Filtered, TokenGroups, Admins));
        ok(!IsTokenRestricted(Filtered), "Token without restricting SIDs reported restricted\n");
        ok(GroupCount(Filtered, TokenRestrictedSids) == 0, "Restricted SID count %lu\n", GroupCount(Filtered, TokenRestrictedSids));
        CloseHandle(Filtered);
    }

    /* A SID that is not in the token is ignored */
    ok(Filter(Token, 0, 1, Missing, 0, NULL, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        ok(GroupAttributes(Filtered, TokenGroups, Everyone) == GroupAttributes(Token, TokenGroups, Everyone),
           "Everyone changed: 0x%lx\n", GroupAttributes(Filtered, TokenGroups, Everyone));
        CloseHandle(Filtered);
    }

    /* The user SID can be made deny-only too */
    ok(Filter(Token, 0, 1, UserOnly, 0, NULL, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        UserInfo = QueryToken(Filtered, TokenUser);
        ok(UserInfo != NULL, "TokenUser query failed: %lu\n", GetLastError());
        if (UserInfo)
        {
            ok(UserInfo->User.Attributes == SE_GROUP_USE_FOR_DENY_ONLY, "User attributes 0x%lx\n", UserInfo->User.Attributes);
            HeapFree(GetProcessHeap(), 0, UserInfo);
        }
        CloseHandle(Filtered);
    }

    /* Administrators deny-only: the default owner falls back to the user */
    if (GroupAttributes(Token, TokenGroups, Admins) & SE_GROUP_ENABLED)
    {
        PSID AdminsOnly[1] = { Admins };
        ok(Filter(Token, 0, 1, AdminsOnly, 0, NULL, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
        if (Filtered)
        {
            ok(GroupAttributes(Filtered, TokenGroups, Admins) & SE_GROUP_USE_FOR_DENY_ONLY, "Administrators not deny-only\n");
            ok(GroupAttributes(Filtered, TokenGroups, Admins) == SE_GROUP_USE_FOR_DENY_ONLY, "Administrators 0x%lx\n", GroupAttributes(Filtered, TokenGroups, Admins));
            Owner = QueryToken(Filtered, TokenOwner);
            ok(Owner != NULL, "TokenOwner query failed: %lu\n", GetLastError());
            if (Owner)
            {
                ok(EqualSid(Owner->Owner, User), "Owner is not the user\n");
                HeapFree(GetProcessHeap(), 0, Owner);
            }
            CloseHandle(Filtered);
        }
    }
    else
    {
        skip("Not running as an administrator\n");
    }

    /* The logon SID and the integrity label */
    {
        PTOKEN_GROUPS Groups = QueryToken(Token, TokenGroups);
        DWORD i;
        for (i = 0; Groups && i < Groups->GroupCount; i++)
        {
            PSID One[1];
            if (!(Groups->Groups[i].Attributes & (SE_GROUP_LOGON_ID | SE_GROUP_INTEGRITY)))
                continue;
            One[0] = Groups->Groups[i].Sid;
            ok(Filter(Token, 0, 1, One, 0, NULL, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
            if (!Filtered)
                continue;
            Attributes = GroupAttributes(Filtered, TokenGroups, One[0]);
            if (Groups->Groups[i].Attributes & SE_GROUP_INTEGRITY)
                ok(Attributes == Groups->Groups[i].Attributes, "Integrity SID 0x%lx -> 0x%lx\n", Groups->Groups[i].Attributes, Attributes);
            else
                ok(Attributes == (SE_GROUP_LOGON_ID | SE_GROUP_USE_FOR_DENY_ONLY), "Logon SID 0x%lx -> 0x%lx\n", Groups->Groups[i].Attributes, Attributes);
            CloseHandle(Filtered);
        }
        if (Groups)
            HeapFree(GetProcessHeap(), 0, Groups);
    }
}

static void TestPrivileges(HANDLE Token)
{
    HANDLE Filtered;
    DWORD Delete[2] = { SE_CHANGE_NOTIFY_PRIVILEGE, 0xdeadbeef };
    DWORD Attributes = 0, SourceAttributes = 0, Count = PrivilegeCount(Token);

    ok(HasPrivilege(Token, SE_CHANGE_NOTIFY_PRIVILEGE, &SourceAttributes), "No SeChangeNotifyPrivilege in the source token\n");

    /* DISABLE_MAX_PRIVILEGE deletes everything except SeChangeNotifyPrivilege and ignores the delete list */
    ok(Filter(Token, DISABLE_MAX_PRIVILEGE, 0, NULL, 2, Delete, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        ok(PrivilegeCount(Filtered) == 1, "Privilege count %lu (source %lu)\n", PrivilegeCount(Filtered), Count);
        ok(HasPrivilege(Filtered, SE_CHANGE_NOTIFY_PRIVILEGE, &Attributes), "SeChangeNotifyPrivilege deleted\n");
        ok(Attributes == SourceAttributes, "SeChangeNotifyPrivilege attributes 0x%lx, source 0x%lx\n", Attributes, SourceAttributes);
        CloseHandle(Filtered);
    }

    /* An explicit list deletes what it names; unknown LUIDs are ignored */
    ok(Filter(Token, 0, 0, NULL, 2, Delete, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        ok(!HasPrivilege(Filtered, SE_CHANGE_NOTIFY_PRIVILEGE, NULL), "SeChangeNotifyPrivilege not deleted\n");
        ok(PrivilegeCount(Filtered) == Count - 1, "Privilege count %lu, source %lu\n", PrivilegeCount(Filtered), Count);
        CloseHandle(Filtered);
    }
}

static void TestFlags(HANDLE Token)
{
    HANDLE Filtered, Filtered2;
    PSID Restrict[2] = { RestrictedCode, Everyone };
    PSID Restrict2[2] = { User, Everyone };
    DWORD Inert = 0xdeadbeef, Size, Attributes, i;
    BOOL Ret;
    PTOKEN_GROUPS Sids;

    /* SANDBOX_INERT */
    ok(Filter(Token, SANDBOX_INERT, 0, NULL, 0, NULL, 0, NULL, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        ok(GetTokenInformation(Filtered, TokenSandBoxInert, &Inert, sizeof(Inert), &Size), "TokenSandBoxInert failed: %lu\n", GetLastError());
        /* Server 2008 R2 reports 1, Windows 10 reports 0 */
        ok(Inert == (IsWindows8OrNewer() ? 0 : 1), "TokenSandBoxInert %lu\n", Inert);
        CloseHandle(Filtered);
    }
    ok(GetTokenInformation(Token, TokenSandBoxInert, &Inert, sizeof(Inert), &Size), "TokenSandBoxInert failed: %lu\n", GetLastError());
    ok(Inert == 0, "TokenSandBoxInert of the source %lu\n", Inert);

    /* Restricting SIDs come back with mandatory, enabled-by-default and enabled */
    ok(Filter(Token, 0, 0, NULL, 0, NULL, 2, Restrict, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        ok(IsTokenRestricted(Filtered), "Not restricted\n");
        ok(GroupCount(Filtered, TokenRestrictedSids) == 2, "Restricted SID count %lu\n", GroupCount(Filtered, TokenRestrictedSids));
        Attributes = GroupAttributes(Filtered, TokenRestrictedSids, RestrictedCode);
        ok(Attributes == (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED), "RESTRICTED attributes 0x%lx\n", Attributes);
        Attributes = GroupAttributes(Filtered, TokenRestrictedSids, Everyone);
        ok(Attributes == (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED), "Everyone attributes 0x%lx\n", Attributes);
        ok(GroupAttributes(Filtered, TokenGroups, RestrictedCode) == ~0UL, "RESTRICTED added to the normal groups\n");

        /* Filtering again without restricting SIDs */
        /* A restricted token cannot be filtered again without naming restricting SIDs */
        SetLastError(0xdeadbeef);
        Ret = Filter(Filtered, 0, 0, NULL, 0, NULL, 0, NULL, &Filtered2);
        ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "Refilter: ret %d error %lu\n", Ret, GetLastError());
        if (Ret && Filtered2)
            CloseHandle(Filtered2);
        {
            PSID Deny1[1] = { AuthUsers };
            SetLastError(0xdeadbeef);
            Ret = Filter(Filtered, 0, 1, Deny1, 0, NULL, 0, NULL, &Filtered2);
            ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "Refilter with a SID to disable: ret %d error %lu\n", Ret, GetLastError());
            if (Ret && Filtered2)
                CloseHandle(Filtered2);
        }

        /* Filtering again with restricting SIDs */
        ok(Filter(Filtered, 0, 0, NULL, 0, NULL, 2, Restrict2, &Filtered2), "CreateRestrictedToken failed: %lu\n", GetLastError());
        if (Filtered2)
        {
            /* The new list is the intersection of the old and the requested one */
            ok(GroupCount(Filtered2, TokenRestrictedSids) == 1, "Restricted SID count %lu\n", GroupCount(Filtered2, TokenRestrictedSids));
            ok(GroupAttributes(Filtered2, TokenRestrictedSids, Everyone) == (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED),
               "Everyone attributes 0x%lx\n", GroupAttributes(Filtered2, TokenRestrictedSids, Everyone));
            CloseHandle(Filtered2);
        }
        CloseHandle(Filtered);
    }

    /* WRITE_RESTRICTED */
    ok(Filter(Token, WRITE_RESTRICTED, 0, NULL, 0, NULL, 1, Restrict, &Filtered), "CreateRestrictedToken failed: %lu\n", GetLastError());
    if (Filtered)
    {
        ok(IsTokenRestricted(Filtered), "Not restricted\n");
        ok(GroupCount(Filtered, TokenRestrictedSids) == 1, "Restricted SID count %lu\n", GroupCount(Filtered, TokenRestrictedSids));
        CloseHandle(Filtered);
    }

    /* LUA_TOKEN: record what it does to the groups and privileges */
    ok(Filter(Token, LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL, &Filtered), "CreateRestrictedToken(LUA_TOKEN) failed: %lu\n", GetLastError());
    if (Filtered)
    {
        /* The privileges of a standard user stay, with their attributes; Administrators becomes deny-only */
        {
            static const DWORD Kept[] = { SE_SHUTDOWN_PRIVILEGE_, SE_CHANGE_NOTIFY_PRIVILEGE, SE_UNDOCK_PRIVILEGE_, SE_INC_WORKING_SET_PRIVILEGE_, SE_TIME_ZONE_PRIVILEGE_ };
            PTOKEN_PRIVILEGES Privileges = QueryToken(Filtered, TokenPrivileges);
            DWORD k, Expected = 0, Before, After;
            for (k = 0; k < ARRAYSIZE(Kept); k++)
            {
                BOOL InSource = HasPrivilege(Token, Kept[k], &Before);
                BOOL InFiltered = HasPrivilege(Filtered, Kept[k], &After);
                ok(InSource == InFiltered, "Privilege %lu: source %d filtered %d\n", Kept[k], InSource, InFiltered);
                if (InSource && InFiltered)
                    ok(Before == After, "Privilege %lu attributes 0x%lx -> 0x%lx\n", Kept[k], Before, After);
                Expected += InSource;
            }
            ok(PrivilegeCount(Filtered) == Expected, "LUA_TOKEN privilege count %lu, expected %lu\n", PrivilegeCount(Filtered), Expected);
            if (Privileges)
                HeapFree(GetProcessHeap(), 0, Privileges);
        }
        if (GroupAttributes(Token, TokenGroups, Admins) != ~0UL)
            ok(GroupAttributes(Filtered, TokenGroups, Admins) == SE_GROUP_USE_FOR_DENY_ONLY, "LUA_TOKEN Administrators 0x%lx\n", GroupAttributes(Filtered, TokenGroups, Admins));
        ok(GroupAttributes(Filtered, TokenGroups, Everyone) == GroupAttributes(Token, TokenGroups, Everyone), "LUA_TOKEN changed Everyone\n");
        ok(GroupAttributes(Filtered, TokenGroups, AuthUsers) == GroupAttributes(Token, TokenGroups, AuthUsers), "LUA_TOKEN changed Authenticated Users\n");
        ok(!IsTokenRestricted(Filtered), "LUA_TOKEN made the token restricted\n");
        CloseHandle(Filtered);
    }
}

/* The access check matrix: one filtered impersonation token per row, one descriptor per column */
enum { T_BASE, T_DENYAU, T_RCODE, T_DENYAU_RCODE, T_RCODE_WR, T_REVERYONE, T_RCODE_USER, T_COUNT };
enum { S_AU, S_RC, S_AU_RC, S_U3_RC1, S_U, S_DENYAU, S_DENYRC, S_EVERYONE, S_U_RC, S_NULLDACL, S_COUNT };

static const char *TokenNames[T_COUNT] = { "base", "deny-AU", "restrict-RC", "deny-AU+restrict-RC", "restrict-RC/write", "restrict-Everyone", "restrict-RC+user" };
static const char *SdNames[S_COUNT] = { "allow AU", "allow RC", "allow AU+RC", "allow U rw RC r", "allow U", "deny AU w, allow U", "deny RC w, allow U+RC", "allow Everyone", "allow U+RC", "NULL DACL" };

/* MAXIMUM_ALLOWED result per token and descriptor; a specific request succeeds when it is a subset */
static const ACCESS_MASK Expected[T_COUNT][S_COUNT] =
{
    /*                   AU  RC  AU+RC U3RC1 U   denyAU denyRC Evry U+RC NULL */
    /* base          */ { 3,  0,  3,    3,    3,  1,     3,     3,   3,   0xF },
    /* deny AU       */ { 0,  0,  0,    3,    3,  1,     3,     3,   3,   0xF },
    /* restrict RC   */ { 0,  0,  3,    1,    0,  0,     1,     0,   3,   0xF },
    /* deny AU + RC  */ { 0,  0,  0,    1,    0,  0,     1,     0,   3,   0xF },
    /* RC, write     */ { 1,  0,  3,    1,    1,  1,     1,     1,   3,   0xF },
    /* restrict Evry */ { 0,  0,  0,    0,    0,  0,     0,     3,   0,   0xF },
    /* RC + user     */ { 0,  0,  3,    3,    3,  1,     1,     0,   3,   0xF },
};

static PSECURITY_DESCRIPTOR MakeSd(int Which, PSID Owner)
{
    PSECURITY_DESCRIPTOR Sd = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, SECURITY_DESCRIPTOR_MIN_LENGTH + 512);
    PACL Acl = (PACL)((PBYTE)Sd + SECURITY_DESCRIPTOR_MIN_LENGTH);

    InitializeSecurityDescriptor(Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorOwner(Sd, Owner, FALSE);
    SetSecurityDescriptorGroup(Sd, LocalSystem, FALSE);
    if (Which == S_NULLDACL)
    {
        SetSecurityDescriptorDacl(Sd, TRUE, NULL, FALSE);
        return Sd;
    }
    InitializeAcl(Acl, 512, ACL_REVISION);
    switch (Which)
    {
        case S_AU: AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, AuthUsers); break;
        case S_RC: AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, RestrictedCode); break;
        case S_AU_RC:
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, AuthUsers);
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, RestrictedCode);
            break;
        case S_U3_RC1:
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, User);
            AddAccessAllowedAce(Acl, ACL_REVISION, R_, RestrictedCode);
            break;
        case S_U: AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, User); break;
        case S_DENYAU:
            AddAccessDeniedAce(Acl, ACL_REVISION, W_, AuthUsers);
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, User);
            break;
        case S_DENYRC:
            AddAccessDeniedAce(Acl, ACL_REVISION, W_, RestrictedCode);
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, User);
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, RestrictedCode);
            break;
        case S_EVERYONE: AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, Everyone); break;
        case S_U_RC:
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, User);
            AddAccessAllowedAce(Acl, ACL_REVISION, R_ | W_, RestrictedCode);
            break;
    }
    SetSecurityDescriptorDacl(Sd, TRUE, Acl, FALSE);
    return Sd;
}

static void Check(HANDLE Token, PSECURITY_DESCRIPTOR Sd, ACCESS_MASK Desired, PBOOL Status, PACCESS_MASK Granted)
{
    BYTE PrivBuffer[256];
    DWORD PrivLength = sizeof(PrivBuffer);
    BOOL Ret;

    *Status = FALSE;
    *Granted = 0xdeadbeef;
    Ret = AccessCheck(Sd, Token, Desired, &Mapping, (PPRIVILEGE_SET)PrivBuffer, &PrivLength, Granted, Status);
    ok(Ret, "AccessCheck failed: %lu\n", GetLastError());
    if (!*Status)
        ok(*Granted == 0, "Granted 0x%lx on failure\n", *Granted);
}

static BOOL MakeTokens(HANDLE Impersonation, HANDLE *Tokens)
{
    PSID DenyAu[1] = { AuthUsers };
    PSID Rc[1] = { RestrictedCode };
    PSID Evry[1] = { Everyone };
    PSID RcUser[2] = { RestrictedCode, User };
    BOOL Ok = TRUE;

    Tokens[T_BASE] = Impersonation;
    Ok &= Filter(Impersonation, 0, 1, DenyAu, 0, NULL, 0, NULL, &Tokens[T_DENYAU]);
    Ok &= Filter(Impersonation, 0, 0, NULL, 0, NULL, 1, Rc, &Tokens[T_RCODE]);
    Ok &= Filter(Impersonation, 0, 1, DenyAu, 0, NULL, 1, Rc, &Tokens[T_DENYAU_RCODE]);
    Ok &= Filter(Impersonation, WRITE_RESTRICTED, 0, NULL, 0, NULL, 1, Rc, &Tokens[T_RCODE_WR]);
    Ok &= Filter(Impersonation, 0, 0, NULL, 0, NULL, 1, Evry, &Tokens[T_REVERYONE]);
    Ok &= Filter(Impersonation, 0, 0, NULL, 0, NULL, 2, RcUser, &Tokens[T_RCODE_USER]);
    return Ok;
}

static void TestAccessMatrix(HANDLE Impersonation)
{
    HANDLE Tokens[T_COUNT] = { 0 };
    PSECURITY_DESCRIPTOR Sd;
    static const ACCESS_MASK Requests[] = { R_, W_, R_ | W_, MAXIMUM_ALLOWED };
    ACCESS_MASK Granted, Want;
    BOOL Status;
    int t, s, r;

    ok(MakeTokens(Impersonation, Tokens), "CreateRestrictedToken failed: %lu\n", GetLastError());
    for (t = 0; t < T_COUNT; t++)
    {
        if (!Tokens[t])
            continue;
        for (s = 0; s < S_COUNT; s++)
        {
            Sd = MakeSd(s, LocalSystem);
            for (r = 0; r < ARRAYSIZE(Requests); r++)
            {
                Check(Tokens[t], Sd, Requests[r], &Status, &Granted);
                if (Requests[r] == MAXIMUM_ALLOWED)
                {
                    Want = Expected[t][s];
                    ok(Status == (Want != 0) && (!Status || Granted == Want),
                       "%s / %s / MAXIMUM_ALLOWED: status %d granted 0x%lx, expected 0x%lx\n",
                       TokenNames[t], SdNames[s], Status, Granted, Want);
                }
                else
                {
                    Want = ((Requests[r] & Expected[t][s]) == Requests[r]) ? Requests[r] : 0;
                    ok(Status == (Want != 0) && (!Status || Granted == Want),
                       "%s / %s / 0x%lx: status %d granted 0x%lx, expected 0x%lx\n",
                       TokenNames[t], SdNames[s], Requests[r], Status, Granted, Want);
                }
            }
            HeapFree(GetProcessHeap(), 0, Sd);
        }
    }

    /*
     * Owner rights: the owner (the user) gets READ_CONTROL and WRITE_DAC; a restricted
     * token only when the owner is a restricting SID too. Two DACLs, both allowing the
     * restricting SID only: R, and READ_CONTROL | R.
     */
    for (t = 0; t < T_COUNT; t++)
    {
        static const ACCESS_MASK OwnerRequests[] = { READ_CONTROL, WRITE_DAC, READ_CONTROL | R_, MAXIMUM_ALLOWED };
        BOOL OwnerRights = (t == T_BASE || t == T_DENYAU || t == T_RCODE_USER);
        int d;
        if (!Tokens[t])
            continue;
        for (d = 0; d < 2; d++)
        {
            Sd = MakeSd(S_RC, User);
            if (d == 1)
            {
                PACL Acl = (PACL)((PBYTE)Sd + SECURITY_DESCRIPTOR_MIN_LENGTH);
                InitializeAcl(Acl, 512, ACL_REVISION);
                AddAccessAllowedAce(Acl, ACL_REVISION, READ_CONTROL | R_, RestrictedCode);
            }
            for (r = 0; r < ARRAYSIZE(OwnerRequests); r++)
            {
                Want = (OwnerRequests[r] == READ_CONTROL || OwnerRequests[r] == WRITE_DAC) ? (OwnerRights ? OwnerRequests[r] : 0) :
                       (OwnerRequests[r] == MAXIMUM_ALLOWED) ? (OwnerRights ? READ_CONTROL | WRITE_DAC : 0) : 0;
                Check(Tokens[t], Sd, OwnerRequests[r], &Status, &Granted);
                ok(Status == (Want != 0) && (!Status || Granted == Want), "owner=user, DACL %d: %s / 0x%lx: status %d granted 0x%lx, expected 0x%lx\n",
                   d, TokenNames[t], OwnerRequests[r], Status, Granted, Want);
            }
            HeapFree(GetProcessHeap(), 0, Sd);
        }
    }

    /* Membership checks see deny-only groups and restricting SIDs */
    for (t = 0; t < T_COUNT; t++)
    {
        BOOL AuMember = -1, RcMember = -1, EveryoneMember = -1;
        if (!Tokens[t])
            continue;
        ok(CheckTokenMembership(Tokens[t], AuthUsers, &AuMember), "CheckTokenMembership failed: %lu\n", GetLastError());
        ok(CheckTokenMembership(Tokens[t], RestrictedCode, &RcMember), "CheckTokenMembership failed: %lu\n", GetLastError());
        ok(CheckTokenMembership(Tokens[t], Everyone, &EveryoneMember), "CheckTokenMembership failed: %lu\n", GetLastError());
        ok(AuMember == (t == T_BASE || t == T_RCODE_WR), "%s: AU member %d\n", TokenNames[t], AuMember);
        ok(!RcMember, "%s: RC member %d\n", TokenNames[t], RcMember);
        ok(EveryoneMember == (t != T_RCODE && t != T_DENYAU_RCODE && t != T_RCODE_USER),
           "%s: Everyone member %d\n", TokenNames[t], EveryoneMember);
    }

    for (t = T_BASE + 1; t < T_COUNT; t++)
    {
        if (Tokens[t])
            CloseHandle(Tokens[t]);
    }
}

/* Write-restricted tokens: which rights does the restricting pass cover? */
static void TestWriteRestricted(HANDLE Impersonation)
{
    PSID Rc[1] = { RestrictedCode };
    static const ACCESS_MASK Bits[] = { R_, W_, X_, 0x8, DELETE, READ_CONTROL, WRITE_DAC, WRITE_OWNER, SYNCHRONIZE, 0x100, MAXIMUM_ALLOWED };
    HANDLE Token;
    PSECURITY_DESCRIPTOR Sd;
    PACL Acl;
    ACCESS_MASK Granted;
    BOOL Status;
    int i, m;

    if (!Filter(Impersonation, WRITE_RESTRICTED, 0, NULL, 0, NULL, 1, Rc, &Token))
    {
        ok(0, "CreateRestrictedToken failed: %lu\n", GetLastError());
        return;
    }
    Sd = MakeSd(S_U, LocalSystem);
    Acl = (PACL)((PBYTE)Sd + SECURITY_DESCRIPTOR_MIN_LENGTH);
    InitializeAcl(Acl, 512, ACL_REVISION);
    AddAccessAllowedAce(Acl, ACL_REVISION, 0x1FF | DELETE | READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE, User);
    /* The restricting SIDs are checked for the generic write rights, DELETE, WRITE_DAC and WRITE_OWNER */
    for (m = 0; m < 2; m++)
    {
        GENERIC_MAPPING Saved = Mapping;
        ACCESS_MASK WriteRights = W_ | DELETE | WRITE_DAC | WRITE_OWNER, Want;
        if (m == 1)
        {
            Mapping.GenericWrite = W_ | READ_CONTROL | SYNCHRONIZE;
            WriteRights |= READ_CONTROL | SYNCHRONIZE;
        }
        for (i = 0; i < ARRAYSIZE(Bits); i++)
        {
            Check(Token, Sd, Bits[i], &Status, &Granted);
            if (Bits[i] == MAXIMUM_ALLOWED)
                Want = (0x1FF | DELETE | READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE) & ~WriteRights;
            else
                Want = (Bits[i] & WriteRights) ? 0 : Bits[i];
            ok(Status == (Want != 0) && (!Status || Granted == Want), "write-restricted, mapping %d: 0x%lx: status %d granted 0x%lx, expected 0x%lx\n",
               m, Bits[i], Status, Granted, Want);
        }
        Mapping = Saved;
    }
    Check(Token, Sd, R_, &Status, &Granted);
    ok(Status && Granted == R_, "Read: status %d granted 0x%lx\n", Status, Granted);
    Check(Token, Sd, W_, &Status, &Granted);
    ok(!Status, "Write: status %d granted 0x%lx\n", Status, Granted);
    HeapFree(GetProcessHeap(), 0, Sd);
    CloseHandle(Token);
}

/* AdjustTokenGroups must not enable a deny-only group, and a reset leaves it deny-only */
static void TestAdjustGroups(HANDLE Token)
{
    PSID DenyAu[1] = { AuthUsers };
    HANDLE Filtered;
    TOKEN_GROUPS Change;
    PTOKEN_GROUPS Before, After;
    DWORD i;
    BOOL Ret;

    if (!Filter(Token, 0, 1, DenyAu, 0, NULL, 0, NULL, &Filtered))
    {
        ok(0, "CreateRestrictedToken failed: %lu\n", GetLastError());
        return;
    }

    Change.GroupCount = 1;
    Change.Groups[0].Sid = AuthUsers;
    Change.Groups[0].Attributes = SE_GROUP_ENABLED;
    SetLastError(0xdeadbeef);
    Ret = AdjustTokenGroups(Filtered, FALSE, &Change, 0, NULL, NULL);
    ok(!Ret && GetLastError() == ERROR_CANT_ENABLE_DENY_ONLY, "Enable deny-only: ret %d error %lu\n", Ret, GetLastError());
    ok(GroupAttributes(Filtered, TokenGroups, AuthUsers) == SE_GROUP_USE_FOR_DENY_ONLY, "AU attributes 0x%lx\n", GroupAttributes(Filtered, TokenGroups, AuthUsers));

    Change.Groups[0].Sid = Everyone;
    Change.Groups[0].Attributes = 0;
    SetLastError(0xdeadbeef);
    Ret = AdjustTokenGroups(Filtered, FALSE, &Change, 0, NULL, NULL);
    ok(!Ret && GetLastError() == ERROR_CANT_DISABLE_MANDATORY, "Disable mandatory: ret %d error %lu\n", Ret, GetLastError());

    Change.Groups[0].Attributes = SE_GROUP_ENABLED;
    SetLastError(0xdeadbeef);
    Ret = AdjustTokenGroups(Filtered, FALSE, &Change, 0, NULL, NULL);
    ok(Ret && GetLastError() == ERROR_SUCCESS, "Enable enabled: ret %d error %lu\n", Ret, GetLastError());

    Change.Groups[0].Sid = NotInToken;
    SetLastError(0xdeadbeef);
    Ret = AdjustTokenGroups(Filtered, FALSE, &Change, 0, NULL, NULL);
    ok(Ret && GetLastError() == ERROR_NOT_ALL_ASSIGNED, "Not in the token: ret %d error %lu\n", Ret, GetLastError());

    Before = QueryToken(Filtered, TokenGroups);
    SetLastError(0xdeadbeef);
    Ret = AdjustTokenGroups(Filtered, TRUE, NULL, 0, NULL, NULL);
    ok(Ret, "Reset: error %lu\n", GetLastError());
    After = QueryToken(Filtered, TokenGroups);
    ok(Before && After && Before->GroupCount == After->GroupCount, "Group lists differ\n");
    for (i = 0; Before && After && i < Before->GroupCount && i < After->GroupCount; i++)
    {
        ok(EqualSid(Before->Groups[i].Sid, After->Groups[i].Sid), "Group %lu moved\n", i);
        ok(Before->Groups[i].Attributes == After->Groups[i].Attributes, "Group %lu attributes 0x%lx -> 0x%lx\n",
           i, Before->Groups[i].Attributes, After->Groups[i].Attributes);
    }
    if (Before)
        HeapFree(GetProcessHeap(), 0, Before);
    if (After)
        HeapFree(GetProcessHeap(), 0, After);
    CloseHandle(Filtered);
}

START_TEST(RestrictedToken)
{
    HANDLE Process, Impersonation;
    PTOKEN_USER UserInfo;

    MakeSid(Everyone, &WorldAuthority, 1, SECURITY_WORLD_RID, 0);
    MakeSid(AuthUsers, &NtAuthority, 1, SECURITY_AUTHENTICATED_USER_RID, 0);
    MakeSid(RestrictedCode, &NtAuthority, 1, SECURITY_RESTRICTED_CODE_RID, 0);
    MakeSid(LocalSystem, &NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0);
    MakeSid(Admins, &NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);
    MakeSid(NotInToken, &NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, 0x2468);

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &Process))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }
    UserInfo = QueryToken(Process, TokenUser);
    if (!UserInfo)
    {
        skip("TokenUser query failed: %lu\n", GetLastError());
        CloseHandle(Process);
        return;
    }
    CopySid(sizeof(UserBuf), User, UserInfo->User.Sid);
    HeapFree(GetProcessHeap(), 0, UserInfo);

    if (!DuplicateTokenEx(Process, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &Impersonation))
    {
        skip("DuplicateTokenEx failed: %lu\n", GetLastError());
        CloseHandle(Process);
        return;
    }

    TestGroups(Process);
    TestPrivileges(Process);
    TestFlags(Process);
    TestAccessMatrix(Impersonation);
    TestWriteRestricted(Impersonation);
    TestAdjustGroups(Process);

    CloseHandle(Impersonation);
    CloseHandle(Process);
}
