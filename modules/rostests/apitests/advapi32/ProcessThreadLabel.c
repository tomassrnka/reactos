/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Mandatory labels on process and thread objects
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <aclapi.h>
#include <ndk/obfuncs.h>

#define PROCESS_LABEL_POLICY (SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP)

static DWORD GetLevel(HANDLE Token)
{
    BYTE Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD Length;
    BOOL Ret;

    Ret = GetTokenInformation(Token, TokenIntegrityLevel, Buffer, sizeof(Buffer), &Length);
    ok(Ret, "TokenIntegrityLevel: %lu\n", GetLastError());
    if (!Ret) return MAXDWORD;
    return *GetSidSubAuthority(((PTOKEN_MANDATORY_LABEL)Buffer)->Label.Sid, 0);
}

static VOID CheckLabel(HANDLE Object, DWORD Level, DWORD Policy)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    PACL Sacl = NULL;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;
    SID_IDENTIFIER_AUTHORITY Authority = {SECURITY_MANDATORY_LABEL_AUTHORITY};
    DWORD Error;

    Error = GetSecurityInfo(Object, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                            NULL, NULL, NULL, &Sacl, &Sd);
    ok(Error == ERROR_SUCCESS, "GetSecurityInfo: %lu\n", Error);
    if (Error != ERROR_SUCCESS) return;
    ok(Sacl != NULL, "Missing mandatory-label SACL\n");
    if (Sacl)
    {
        ok(Sacl->AceCount == 1, "ACE count %u\n", Sacl->AceCount);
        if (Sacl->AceCount == 1 && GetAce(Sacl, 0, (PVOID *)&Ace))
        {
            ok(Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE, "ACE type %u\n", Ace->Header.AceType);
            ok(Ace->Header.AceFlags == 0, "ACE flags %u\n", Ace->Header.AceFlags);
            ok(Ace->Mask == Policy, "Policy %lx, expected %lx\n", Ace->Mask, Policy);
            ok(IsValidSid(&Ace->SidStart), "Invalid label SID\n");
            ok(!memcmp(GetSidIdentifierAuthority(&Ace->SidStart), &Authority, sizeof(Authority)), "SID authority\n");
            ok(*GetSidSubAuthorityCount(&Ace->SidStart) == 1, "SID subauthority count\n");
            ok(*GetSidSubAuthority(&Ace->SidStart, 0) == Level, "Level %lx, expected %lx\n",
               *GetSidSubAuthority(&Ace->SidStart, 0), Level);
        }
    }
    LocalFree(Sd);
}

static VOID MakeLabel(SECURITY_DESCRIPTOR *Sd, PACL Sacl, DWORD Level, DWORD Policy)
{
    SID Sid = {SID_REVISION, 1, {SECURITY_MANDATORY_LABEL_AUTHORITY}, {0}};
    Sid.SubAuthority[0] = Level;
    ok(InitializeAcl(Sacl, 64, ACL_REVISION), "InitializeAcl\n");
    ok_hex(RtlAddMandatoryAce(Sacl, ACL_REVISION, 0, &Sid,
                             SYSTEM_MANDATORY_LABEL_ACE_TYPE, Policy), STATUS_SUCCESS);
    InitializeSecurityDescriptor(Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorSacl(Sd, TRUE, Sacl, FALSE);
}

static VOID TestAccess(HANDLE Object, DWORD Level)
{
    static const DWORD Rights[] = {0, READ_CONTROL, WRITE_OWNER, READ_CONTROL | WRITE_OWNER};
    DWORD i, Error, Length;
    HANDLE Handle;
    PSECURITY_DESCRIPTOR QuerySd;
    SECURITY_DESCRIPTOR Sd;
    ULONG AclBuffer[16], QueryBuffer[128];
    NTSTATUS Status;
    BOOL Ret;

    for (i = 0; i < _countof(Rights); ++i)
    {
        Ret = DuplicateHandle(GetCurrentProcess(), Object, GetCurrentProcess(),
                              &Handle, Rights[i], FALSE, 0);
        ok(Ret, "DuplicateHandle(%lx): %lu\n", Rights[i], GetLastError());
        if (!Ret) continue;
        QuerySd = NULL;
        Error = GetSecurityInfo(Handle, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                                NULL, NULL, NULL, NULL, &QuerySd);
        ok(Error == ((Rights[i] & READ_CONTROL) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED),
           "GetSecurityInfo(%lx): %lu\n", Rights[i], Error);
        if (QuerySd) LocalFree(QuerySd);
        Status = NtQuerySecurityObject(Handle, LABEL_SECURITY_INFORMATION, QueryBuffer,
                                       sizeof(QueryBuffer), &Length);
        ok(Status == ((Rights[i] & READ_CONTROL) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED),
           "NtQuerySecurityObject(%lx): %lx\n", Rights[i], Status);
        MakeLabel(&Sd, (PACL)AclBuffer, Level, PROCESS_LABEL_POLICY);
        Error = SetSecurityInfo(Handle, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                                NULL, NULL, NULL, (PACL)AclBuffer);
        ok(Error == ((Rights[i] & WRITE_OWNER) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED),
           "SetSecurityInfo(%lx): %lu\n", Rights[i], Error);
        Status = NtSetSecurityObject(Handle, LABEL_SECURITY_INFORMATION, &Sd);
        ok(Status == ((Rights[i] & WRITE_OWNER) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED),
           "NtSetSecurityObject(%lx): %lx\n", Rights[i], Status);
        CloseHandle(Handle);
    }

    for (i = 0; i <= SYSTEM_MANDATORY_LABEL_VALID_MASK; ++i)
    {
        MakeLabel(&Sd, (PACL)AclBuffer, Level, i);
        Error = SetSecurityInfo(Object, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                                NULL, NULL, NULL, (PACL)AclBuffer);
        ok(Error == ERROR_SUCCESS, "Set policy %lu: %lu\n", i, Error);
        CheckLabel(Object, Level, i);
    }
    MakeLabel(&Sd, (PACL)AclBuffer, Level, PROCESS_LABEL_POLICY);
    ok(NtSetSecurityObject(Object, LABEL_SECURITY_INFORMATION, &Sd) == STATUS_SUCCESS, "Restore policy\n");
}

static VOID TestRaise(HANDLE Object, HANDLE Token, DWORD Level)
{
    HANDLE Impersonation;
    TOKEN_PRIVILEGES Privileges;
    SECURITY_DESCRIPTOR Sd;
    ULONG AclBuffer[16];
    NTSTATUS Status;
    DWORD Error;
    BOOL Ret;

    Ret = DuplicateTokenEx(Token, TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE,
                            NULL, SecurityImpersonation, TokenImpersonation, &Impersonation);
    ok(Ret, "DuplicateTokenEx: %lu\n", GetLastError());
    if (!Ret) return;
    Privileges.PrivilegeCount = 1;
    LookupPrivilegeValueW(NULL, L"SeRelabelPrivilege", &Privileges.Privileges[0].Luid);
    Privileges.Privileges[0].Attributes = 0;
    ok(AdjustTokenPrivileges(Impersonation, FALSE, &Privileges, 0, NULL, NULL), "Disable SeRelabelPrivilege\n");
    Ret = SetThreadToken(NULL, Impersonation);
    ok(Ret, "SetThreadToken: %lu\n", GetLastError());
    if (Ret)
    {
        MakeLabel(&Sd, (PACL)AclBuffer, Level + 0x1000, PROCESS_LABEL_POLICY);
        Status = NtSetSecurityObject(Object, LABEL_SECURITY_INFORMATION, &Sd);
        Error = SetSecurityInfo(Object, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                                NULL, NULL, NULL, (PACL)AclBuffer);
        ok(RevertToSelf(), "RevertToSelf\n");
        ok(Status == STATUS_INVALID_LABEL, "Raise without privilege: %lx\n", Status);
        ok(Error == ERROR_INVALID_LABEL, "Raise without privilege: %lu\n", Error);
        CheckLabel(Object, Level, PROCESS_LABEL_POLICY);
    }
    CloseHandle(Impersonation);
}

static DWORD WINAPI Worker(PVOID Event)
{
    WaitForSingleObject(Event, 30000);
    return 0;
}

static BOOL CreateChild(HANDLE Token, PSECURITY_ATTRIBUTES Attributes, PPROCESS_INFORMATION Pi)
{
    WCHAR Path[MAX_PATH], Command[MAX_PATH + 32];
    STARTUPINFOW Startup = {sizeof(Startup)};

    GetSystemDirectoryW(Path, _countof(Path));
    StringCchCatW(Path, _countof(Path), L"\\cmd.exe");
    StringCchPrintfW(Command, _countof(Command), L"\"%s\" /c exit 0", Path);
    if (Token)
        return CreateProcessAsUserW(Token, Path, Command, Attributes, Attributes, FALSE,
                                    CREATE_SUSPENDED, NULL, NULL, &Startup, Pi);
    return CreateProcessW(Path, Command, Attributes, Attributes, FALSE,
                           CREATE_SUSPENDED, NULL, NULL, &Startup, Pi);
}

static VOID CloseChild(PPROCESS_INFORMATION Pi)
{
    TerminateProcess(Pi->hProcess, 0);
    WaitForSingleObject(Pi->hProcess, 10000);
    CloseHandle(Pi->hThread);
    CloseHandle(Pi->hProcess);
}

START_TEST(ProcessThreadLabel)
{
    HANDLE Token, Event, Thread, LowToken;
    SECURITY_DESCRIPTOR ExplicitSd;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &ExplicitSd, FALSE};
    ULONG AclBuffer[16];
    DWORD Level, Rid;
    PROCESS_INFORMATION Pi;
    TOKEN_MANDATORY_LABEL Label;
    SID Sid = {SID_REVISION, 1, {SECURITY_MANDATORY_LABEL_AUTHORITY}, {0}};
    BOOL Ret;

    Ret = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &Token);
    ok(Ret, "OpenProcessToken: %lu\n", GetLastError());
    if (!Ret) return;
    Level = GetLevel(Token);
    if (Level == MAXDWORD) { CloseHandle(Token); return; }
    trace("Caller integrity level %lx\n", Level);
    CheckLabel(GetCurrentProcess(), Level, PROCESS_LABEL_POLICY);
    CheckLabel(GetCurrentThread(), Level, PROCESS_LABEL_POLICY);
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Event != NULL, "CreateEvent: %lu\n", GetLastError());
    if (Event)
    {
        Thread = CreateThread(NULL, 0, Worker, Event, 0, NULL);
        ok(Thread != NULL, "CreateThread: %lu\n", GetLastError());
        if (Thread)
        {
            CheckLabel(Thread, Level, PROCESS_LABEL_POLICY);
            TestAccess(Thread, Level);
            TestRaise(Thread, Token, Level);
            SetEvent(Event);
            WaitForSingleObject(Thread, 10000);
            CloseHandle(Thread);
        }
        CloseHandle(Event);
    }
    Ret = CreateChild(NULL, NULL, &Pi);
    ok(Ret, "CreateProcess: %lu\n", GetLastError());
    if (Ret)
    {
        CheckLabel(Pi.hProcess, Level, PROCESS_LABEL_POLICY);
        CheckLabel(Pi.hThread, Level, PROCESS_LABEL_POLICY);
        TestAccess(Pi.hProcess, Level);
        TestRaise(Pi.hProcess, Token, Level);
        CloseChild(&Pi);
    }

    /* Explicit labels retain their level and gain the minimum creation policy. */
    MakeLabel(&ExplicitSd, (PACL)AclBuffer, SECURITY_MANDATORY_LOW_RID,
              SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP);
    Ret = CreateChild(NULL, &Attributes, &Pi);
    ok(Ret, "CreateProcess with explicit label: %lu\n", GetLastError());
    if (Ret)
    {
        CheckLabel(Pi.hProcess, SECURITY_MANDATORY_LOW_RID, SYSTEM_MANDATORY_LABEL_VALID_MASK);
        CheckLabel(Pi.hThread, SECURITY_MANDATORY_LOW_RID, SYSTEM_MANDATORY_LABEL_VALID_MASK);
        CloseChild(&Pi);
    }

    /* A different primary token must label both objects, including the initial thread. */
    for (Rid = SECURITY_MANDATORY_LOW_RID; Rid <= SECURITY_MANDATORY_MEDIUM_RID && Rid < Level; Rid += 0x1000)
    {
        Ret = DuplicateTokenEx(Token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &LowToken);
        ok(Ret, "DuplicateTokenEx: %lu\n", GetLastError());
        if (!Ret) continue;
        Sid.SubAuthority[0] = Rid;
        Label.Label.Sid = &Sid;
        Label.Label.Attributes = SE_GROUP_INTEGRITY;
        Ret = SetTokenInformation(LowToken, TokenIntegrityLevel, &Label, sizeof(Label) + GetLengthSid(&Sid));
        ok(Ret, "SetTokenInformation: %lu\n", GetLastError());
        if (Ret)
        {
            Event = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (Event)
            {
                Ret = ImpersonateLoggedOnUser(LowToken);
                ok(Ret, "ImpersonateLoggedOnUser: %lu\n", GetLastError());
                if (Ret)
                {
                    Thread = CreateThread(NULL, 0, Worker, Event, 0, NULL);
                    ok(RevertToSelf(), "RevertToSelf\n");
                    ok(Thread != NULL, "CreateThread while impersonating: %lu\n", GetLastError());
                    if (Thread)
                    {
                        CheckLabel(Thread, Level, PROCESS_LABEL_POLICY);
                        SetEvent(Event);
                        WaitForSingleObject(Thread, 10000);
                        CloseHandle(Thread);
                    }
                }
                CloseHandle(Event);
            }

            Ret = CreateChild(LowToken, NULL, &Pi);
            if (!Ret && GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
                skip("CreateProcessAsUser requires a privilege not held by this account\n");
            else
            {
                ok(Ret, "CreateProcessAsUser: %lu\n", GetLastError());
                if (Ret)
                {
                    CheckLabel(Pi.hProcess, Rid, PROCESS_LABEL_POLICY);
                    CheckLabel(Pi.hThread, Rid, PROCESS_LABEL_POLICY);
                    CloseChild(&Pi);
                }
            }
        }
        CloseHandle(LowToken);
    }
    CloseHandle(Token);
}
