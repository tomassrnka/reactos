/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the NT 6 token information classes (elevation, linked token, logon SID, UI access, virtualization)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <sddl.h>
#include <tlhelp32.h>

static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
static SID_IDENTIFIER_AUTHORITY MandatoryLabelAuthority = {SECURITY_MANDATORY_LABEL_AUTHORITY};

static BOOL QueryDword(HANDLE Token, TOKEN_INFORMATION_CLASS Class, PDWORD Value)
{
    DWORD Length = 0xdeadbeef;
    BOOL Ret;

    *Value = 0xdeadbeef;
    Ret = GetTokenInformation(Token, Class, Value, sizeof(*Value), &Length);
    ok(!Ret || Length == sizeof(DWORD), "Class %d: length %lu\n", Class, Length);
    return Ret;
}

static PTOKEN_GROUPS QueryGroups(HANDLE Token, TOKEN_INFORMATION_CLASS Class, PDWORD Error)
{
    PTOKEN_GROUPS Groups;
    DWORD Length = 0;

    SetLastError(0xdeadbeef);
    if (GetTokenInformation(Token, Class, NULL, 0, &Length) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        *Error = GetLastError();
        return NULL;
    }
    Groups = HeapAlloc(GetProcessHeap(), 0, Length);
    if (!Groups)
    {
        *Error = ERROR_OUTOFMEMORY;
        return NULL;
    }
    if (!GetTokenInformation(Token, Class, Groups, Length, &Length))
    {
        *Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Groups);
        return NULL;
    }
    *Error = 0;
    return Groups;
}

/* S-1-5-5-X-Y (IsWellKnownSid(WinLogonIdsSid) does not know it on ReactOS) */
static BOOL IsLogonSid(PSID Sid)
{
    return IsValidSid(Sid) &&
           !memcmp(GetSidIdentifierAuthority(Sid), &NtAuthority, sizeof(NtAuthority)) &&
           *GetSidSubAuthorityCount(Sid) == SECURITY_LOGON_IDS_RID_COUNT &&
           *GetSidSubAuthority(Sid, 0) == SECURITY_LOGON_IDS_RID;
}

/* The logon SID entry of TokenGroups, as TokenLogonSid must report it */
static PSID_AND_ATTRIBUTES FindLogonGroup(PTOKEN_GROUPS Groups)
{
    DWORD i;

    for (i = 0; i < Groups->GroupCount; i++)
    {
        if ((Groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
            return &Groups->Groups[i];
    }
    return NULL;
}

static void CheckLogonSid(HANDLE Token, PCSTR Name, DWORD ExpectedAttributes)
{
    PTOKEN_GROUPS Groups, LogonSid;
    PSID_AND_ATTRIBUTES Logon;
    DWORD Error;
    LPSTR SidString;

    Groups = QueryGroups(Token, TokenGroups, &Error);
    ok(Groups != NULL, "%s: TokenGroups failed with %lu\n", Name, Error);
    if (!Groups)
        return;
    Logon = FindLogonGroup(Groups);

    LogonSid = QueryGroups(Token, TokenLogonSid, &Error);
    if (!Logon)
    {
        trace("%s: no logon SID in TokenGroups, TokenLogonSid %s error %lu groups %lu\n",
              Name, LogonSid ? "ok" : "failed", Error, LogonSid ? LogonSid->GroupCount : 0);
        ok(LogonSid == NULL && Error == ERROR_NOT_FOUND, "%s: TokenLogonSid gave %p, error %lu\n", Name, LogonSid, Error);
    }
    else
    {
        ok(LogonSid != NULL, "%s: TokenLogonSid failed with %lu\n", Name, Error);
        if (LogonSid)
        {
            ok(LogonSid->GroupCount == 1, "%s: %lu groups\n", Name, LogonSid->GroupCount);
            if (LogonSid->GroupCount >= 1)
            {
                ok(EqualSid(LogonSid->Groups[0].Sid, Logon->Sid), "%s: SID differs from the TokenGroups entry\n", Name);
                ok(IsLogonSid(LogonSid->Groups[0].Sid), "%s: not a logon SID\n", Name);
                ok(LogonSid->Groups[0].Attributes == Logon->Attributes, "%s: attributes 0x%lx, TokenGroups 0x%lx\n",
                   Name, LogonSid->Groups[0].Attributes, Logon->Attributes);
                /* ReactOS: NtFilterToken does not apply SidsToDisable yet */
                todo_if((ExpectedAttributes & SE_GROUP_USE_FOR_DENY_ONLY) && is_reactos())
                ok(LogonSid->Groups[0].Attributes == ExpectedAttributes, "%s: attributes 0x%lx, expected 0x%lx\n",
                   Name, LogonSid->Groups[0].Attributes, ExpectedAttributes);
                if (ConvertSidToStringSidA(LogonSid->Groups[0].Sid, &SidString))
                {
                    trace("%s: logon SID %s attributes 0x%lx\n", Name, SidString, LogonSid->Groups[0].Attributes);
                    LocalFree(SidString);
                }
            }
        }
    }

    if (LogonSid)
        HeapFree(GetProcessHeap(), 0, LogonSid);
    HeapFree(GetProcessHeap(), 0, Groups);
}

static void CheckElevation(HANDLE Token, PCSTR Name, DWORD ExpectedElevated)
{
    TOKEN_LINKED_TOKEN Linked;
    DWORD Value, Length;
    BOOL Ret;

    ok(QueryDword(Token, TokenElevationType, &Value), "%s: TokenElevationType failed with %lu\n", Name, GetLastError());
    ok(Value == TokenElevationTypeDefault, "%s: elevation type %lu\n", Name, Value);

    ok(QueryDword(Token, TokenElevation, &Value), "%s: TokenElevation failed with %lu\n", Name, GetLastError());
    ok(Value == ExpectedElevated, "%s: elevated %lu, expected %lu\n", Name, Value, ExpectedElevated);

    /* An elevation type of default has no linked token */
    Linked.LinkedToken = (HANDLE)(ULONG_PTR)0xdeadbeef;
    Length = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = GetTokenInformation(Token, TokenLinkedToken, &Linked, sizeof(Linked), &Length);
    ok(!Ret, "%s: TokenLinkedToken succeeded with %p\n", Name, Linked.LinkedToken);
    ok(GetLastError() == ERROR_NO_SUCH_LOGON_SESSION, "%s: TokenLinkedToken error %lu\n", Name, GetLastError());
    if (Ret && Linked.LinkedToken)
        CloseHandle(Linked.LinkedToken);

    ok(QueryDword(Token, TokenVirtualizationAllowed, &Value), "%s: TokenVirtualizationAllowed failed with %lu\n", Name, GetLastError());
    ok(Value == 0, "%s: virtualization allowed %lu\n", Name, Value);
    ok(QueryDword(Token, TokenVirtualizationEnabled, &Value), "%s: TokenVirtualizationEnabled failed with %lu\n", Name, GetLastError());
    ok(Value == 0, "%s: virtualization enabled %lu\n", Name, Value);
    ok(QueryDword(Token, TokenUIAccess, &Value), "%s: TokenUIAccess failed with %lu\n", Name, GetLastError());
    ok(Value == 0, "%s: UI access %lu\n", Name, Value);
}

static BOOL IsAdminToken(HANDLE Token)
{
    PSID Admins = NULL;
    BOOL IsMember = FALSE;
    HANDLE Imp = NULL;

    if (!DuplicateToken(Token, SecurityIdentification, &Imp))
        return FALSE;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &Admins))
    {
        CheckTokenMembership(Imp, Admins, &IsMember);
        FreeSid(Admins);
    }
    CloseHandle(Imp);
    return IsMember;
}

static HANDLE DuplicateAtLevel(HANDLE Token, DWORD Rid)
{
    BYTE Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    PTOKEN_MANDATORY_LABEL Label = (PTOKEN_MANDATORY_LABEL)Buffer;
    PSID Sid = (PSID)(Label + 1);
    HANDLE Dup;

    if (!DuplicateTokenEx(Token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &Dup))
        return NULL;
    InitializeSid(Sid, &MandatoryLabelAuthority, 1);
    *GetSidSubAuthority(Sid, 0) = Rid;
    Label->Label.Sid = Sid;
    Label->Label.Attributes = SE_GROUP_INTEGRITY;
    if (!SetTokenInformation(Dup, TokenIntegrityLevel, Label, sizeof(TOKEN_MANDATORY_LABEL) + GetLengthSid(Sid)))
    {
        CloseHandle(Dup);
        return NULL;
    }
    return Dup;
}

/* The token of winlogon.exe, which runs as LocalSystem */
static HANDLE OpenSystemToken(void)
{
    PROCESSENTRY32W Entry;
    HANDLE Snapshot, Process, Token = NULL;
    BOOL More;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return NULL;
    Entry.dwSize = sizeof(Entry);
    for (More = Process32FirstW(Snapshot, &Entry); More && !Token; More = Process32NextW(Snapshot, &Entry))
    {
        if (_wcsicmp(Entry.szExeFile, L"winlogon.exe"))
            continue;
        Process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, Entry.th32ProcessID);
        if (Process)
        {
            if (!OpenProcessToken(Process, TOKEN_QUERY, &Token))
                Token = NULL;
            CloseHandle(Process);
        }
    }
    CloseHandle(Snapshot);
    return Token;
}

START_TEST(TokenVistaClasses)
{
    HANDLE Token, Derived, Thread;
    SID_AND_ATTRIBUTES Disable;
    PTOKEN_GROUPS Groups;
    PSID_AND_ATTRIBUTES Logon;
    PSID Admins;
    DWORD Error, Elevated;
    BOOL Admin;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &Token))
    {
        skip("OpenProcessToken failed with %lu\n", GetLastError());
        return;
    }

    /* The test runs in an administrator's logon session without UAC (ReactOS, the Windows reference machines) */
    Admin = IsAdminToken(Token);
    Elevated = Admin ? 1 : 0;
    trace("process token is %san administrator's\n", Admin ? "" : "not ");

    CheckElevation(Token, "process", Elevated);
    CheckLogonSid(Token, "process", SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED | SE_GROUP_LOGON_ID);

    /* A copy at medium integrity */
    Derived = DuplicateAtLevel(Token, SECURITY_MANDATORY_MEDIUM_RID);
    ok(Derived != NULL, "Medium copy failed with %lu\n", GetLastError());
    if (Derived)
    {
        CheckElevation(Derived, "medium copy", Elevated);
        CloseHandle(Derived);
    }

    /* Restricted tokens: no privileges, the administrators group for deny only, the logon SID for deny only */
    if (CreateRestrictedToken(Token, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &Derived))
    {
        CheckElevation(Derived, "no privileges", Elevated);
        CloseHandle(Derived);
    }
    else
    {
        ok(FALSE, "CreateRestrictedToken failed with %lu\n", GetLastError());
    }

    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &Admins))
    {
        Disable.Sid = Admins;
        Disable.Attributes = 0;
        ok(CreateRestrictedToken(Token, 0, 1, &Disable, 0, NULL, 0, NULL, &Derived), "CreateRestrictedToken failed with %lu\n", GetLastError());
        if (Derived)
        {
            CheckElevation(Derived, "administrators for deny only", Elevated);
            CloseHandle(Derived);
            Derived = NULL;
        }
        FreeSid(Admins);
    }

    Groups = QueryGroups(Token, TokenGroups, &Error);
    Logon = Groups ? FindLogonGroup(Groups) : NULL;
    if (Logon)
    {
        Disable.Sid = Logon->Sid;
        Disable.Attributes = 0;
        ok(CreateRestrictedToken(Token, 0, 1, &Disable, 0, NULL, 0, NULL, &Derived), "CreateRestrictedToken failed with %lu\n", GetLastError());
        if (Derived)
        {
            CheckLogonSid(Derived, "logon SID for deny only", SE_GROUP_USE_FOR_DENY_ONLY | SE_GROUP_LOGON_ID);
            CloseHandle(Derived);
            Derived = NULL;
        }

        /* Restricting SIDs do not change the logon SID group */
        ok(CreateRestrictedToken(Token, 0, 0, NULL, 0, NULL, 1, &Disable, &Derived), "CreateRestrictedToken failed with %lu\n", GetLastError());
        if (Derived)
        {
            CheckLogonSid(Derived, "restricted by the logon SID", SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED | SE_GROUP_LOGON_ID);
            CheckElevation(Derived, "restricted by the logon SID", Elevated);
            CloseHandle(Derived);
            Derived = NULL;
        }
    }
    else
    {
        skip("No logon SID in the process token\n");
    }
    if (Groups)
        HeapFree(GetProcessHeap(), 0, Groups);

    /* An impersonation token answers like its primary token */
    if (DuplicateToken(Token, SecurityIdentification, &Derived))
    {
        CheckElevation(Derived, "impersonation", Elevated);
        CheckLogonSid(Derived, "impersonation", SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED | SE_GROUP_LOGON_ID);
        CloseHandle(Derived);
    }

    /* The anonymous token has no logon SID and is not elevated */
    if (ImpersonateAnonymousToken(GetCurrentThread()))
    {
        Thread = NULL;
        ok(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Thread), "OpenThreadToken failed with %lu\n", GetLastError());
        RevertToSelf();
        if (Thread)
        {
            CheckElevation(Thread, "anonymous", 0);
            CheckLogonSid(Thread, "anonymous", 0);
            CloseHandle(Thread);
        }
    }
    else
    {
        skip("ImpersonateAnonymousToken failed with %lu\n", GetLastError());
    }

    /* LocalSystem is elevated */
    Thread = OpenSystemToken();
    if (Thread)
    {
        CheckElevation(Thread, "LocalSystem", 1);
        CloseHandle(Thread);
    }
    else
    {
        skip("Cannot open the token of winlogon.exe (error %lu)\n", GetLastError());
    }

    CloseHandle(Token);
}
