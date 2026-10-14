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

static DWORD QueryLevel(HANDLE Object)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    PACL Sacl = NULL;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;
    DWORD Level = MAXDWORD;

    if (GetSecurityInfo(Object, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                        NULL, NULL, NULL, &Sacl, &Sd) != ERROR_SUCCESS)
        return MAXDWORD;
    if (Sacl && Sacl->AceCount == 1 && GetAce(Sacl, 0, (PVOID *)&Ace))
        Level = *GetSidSubAuthority(&Ace->SidStart, 0);
    LocalFree(Sd);
    return Level;
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

static PWSTR CommandLineOf(PCWSTR Program)
{
    static WCHAR Command[MAX_PATH + 32];
    WCHAR Path[MAX_PATH];

    GetSystemDirectoryW(Path, _countof(Path));
    StringCchPrintfW(Command, _countof(Command), L"\"%s\\%s\" /c exit 0", Path, Program);
    return Command;
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

static BOOL MakeLevelToken(HANDLE Token, DWORD Rid, PHANDLE Out)
{
    SID Sid = {SID_REVISION, 1, {SECURITY_MANDATORY_LABEL_AUTHORITY}, {0}};
    TOKEN_MANDATORY_LABEL Label;
    BOOL Ret;

    Ret = DuplicateTokenEx(Token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, Out);
    ok(Ret, "DuplicateTokenEx: %lu\n", GetLastError());
    if (!Ret) return FALSE;
    Sid.SubAuthority[0] = Rid;
    Label.Label.Sid = &Sid;
    Label.Label.Attributes = SE_GROUP_INTEGRITY;
    Ret = SetTokenInformation(*Out, TokenIntegrityLevel, &Label, sizeof(Label) + GetLengthSid(&Sid));
    ok(Ret, "SetTokenInformation(%lx): %lu\n", Rid, GetLastError());
    if (!Ret) CloseHandle(*Out);
    return Ret;
}

/* Impersonate the caller with SeSecurityPrivilege disabled, so that no SACL is privileged. */
static BOOL ImpersonateWithoutSecurityPrivilege(HANDLE Token)
{
    TOKEN_PRIVILEGES Privileges;
    HANDLE Impersonation;
    BOOL Ret;

    Ret = DuplicateTokenEx(Token, TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE,
                            NULL, SecurityImpersonation, TokenImpersonation, &Impersonation);
    ok(Ret, "DuplicateTokenEx: %lu\n", GetLastError());
    if (!Ret) return FALSE;
    Privileges.PrivilegeCount = 1;
    LookupPrivilegeValueW(NULL, L"SeSecurityPrivilege", &Privileges.Privileges[0].Luid);
    Privileges.Privileges[0].Attributes = 0;
    AdjustTokenPrivileges(Impersonation, FALSE, &Privileges, 0, NULL, NULL);
    Ret = SetThreadToken(NULL, Impersonation);
    ok(Ret, "SetThreadToken: %lu\n", GetLastError());
    CloseHandle(Impersonation);
    return Ret;
}

static VOID CheckThreadCreation(PSECURITY_ATTRIBUTES Attributes, DWORD Error, PCSTR Name)
{
    HANDLE Event, Thread;

    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Event != NULL, "CreateEvent: %lu\n", GetLastError());
    if (!Event) return;
    SetLastError(0xdeadbeef);
    Thread = CreateThread(Attributes, 0, Worker, Event, 0, NULL);
    if (Error == MAXDWORD)
    {
        /* Server 2008 R2 accepts it, Windows 10 rejects it */
        ok(Thread != NULL || GetLastError() == ERROR_INVALID_ACL, "%s: CreateThread error %lu\n", Name, GetLastError());
    }
    else if (Error == ERROR_SUCCESS)
        ok(Thread != NULL, "%s: CreateThread failed with %lu\n", Name, GetLastError());
    else
        ok(Thread == NULL && GetLastError() == Error, "%s: CreateThread returned %p, error %lu, expected %lu\n",
           Name, Thread, Thread ? 0 : GetLastError(), Error);
    SetEvent(Event);
    if (Thread)
    {
        WaitForSingleObject(Thread, 10000);
        CloseHandle(Thread);
    }
    CloseHandle(Event);
}

static VOID CheckProcessCreation(HANDLE Token, PSECURITY_ATTRIBUTES Attributes, DWORD Error, PCSTR Name)
{
    PROCESS_INFORMATION Pi;
    BOOL Ret;

    memset(&Pi, 0xcc, sizeof(Pi));
    SetLastError(0xdeadbeef);
    Ret = CreateChild(Token, Attributes, &Pi);
    if (Error == ERROR_SUCCESS)
        ok(Ret, "%s: CreateProcess failed with %lu\n", Name, GetLastError());
    else
    {
        ok(!Ret && GetLastError() == Error, "%s: CreateProcess returned %d, error %lu, expected %lu\n",
           Name, Ret, Ret ? 0 : GetLastError(), Error);
        if (!Ret)
            ok(!Pi.hProcess && !Pi.hThread, "%s: handles %p %p left after failure\n", Name, Pi.hProcess, Pi.hThread);
    }
    if (Ret) CloseChild(&Pi);
}

#define CHILD_LABEL_AT_LEVEL   1
#define CHILD_LABEL_ABOVE_FAILS 2
#define CHILD_DEFAULT_AT_LEVEL 4

/* Runs in a low child: create threads in the parent through an inherited handle. */
static DWORD ChildRemoteThreads(HANDLE Parent)
{
    SECURITY_DESCRIPTOR Sd;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &Sd, FALSE};
    ULONG AclBuffer[16];
    HANDLE Token, Thread;
    DWORD Level, Result = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) return 0x100;
    Level = GetLevel(Token);
    CloseHandle(Token);
    if (Level != SECURITY_MANDATORY_LOW_RID) return 0x200;

    Thread = CreateRemoteThread(Parent, NULL, 0, (LPTHREAD_START_ROUTINE)ExitThread, NULL, CREATE_SUSPENDED, NULL);
    if (Thread)
    {
        if (QueryLevel(Thread) == Level) Result |= CHILD_DEFAULT_AT_LEVEL;
        TerminateThread(Thread, 0);
        CloseHandle(Thread);
    }
    MakeLabel(&Sd, (PACL)AclBuffer, Level, PROCESS_LABEL_POLICY);
    Thread = CreateRemoteThread(Parent, &Attributes, 0, (LPTHREAD_START_ROUTINE)ExitThread, NULL, CREATE_SUSPENDED, NULL);
    if (Thread)
    {
        if (QueryLevel(Thread) == Level) Result |= CHILD_LABEL_AT_LEVEL;
        TerminateThread(Thread, 0);
        CloseHandle(Thread);
    }
    MakeLabel(&Sd, (PACL)AclBuffer, Level + 0x1000, PROCESS_LABEL_POLICY);
    SetLastError(0xdeadbeef);
    Thread = CreateRemoteThread(Parent, &Attributes, 0, (LPTHREAD_START_ROUTINE)ExitThread, NULL, CREATE_SUSPENDED, NULL);
    if (!Thread && GetLastError() == ERROR_PRIVILEGE_NOT_HELD) Result |= CHILD_LABEL_ABOVE_FAILS;
    if (Thread)
    {
        TerminateThread(Thread, 0);
        CloseHandle(Thread);
    }
    return Result;
}

/* A low creator cannot label a thread in a higher process above its own level. */
static VOID TestLowCreator(HANDLE LowToken)
{
    WCHAR Path[MAX_PATH], Command[MAX_PATH + 64];
    STARTUPINFOW Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Pi;
    HANDLE Self;
    DWORD Code = 0xdeadbeef;
    BOOL Ret;

    Ret = DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &Self,
                          PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                          PROCESS_VM_READ | PROCESS_VM_WRITE | READ_CONTROL, TRUE, 0);
    ok(Ret, "DuplicateHandle: %lu\n", GetLastError());
    if (!Ret) return;
    GetModuleFileNameW(NULL, Path, _countof(Path));
    StringCchPrintfW(Command, _countof(Command), L"\"%s\" ProcessThreadLabel child %Ix", Path, (ULONG_PTR)Self);
    Ret = CreateProcessAsUserW(LowToken, Path, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Pi);
    ok(Ret, "CreateProcessAsUser for the low child: %lu\n", GetLastError());
    if (Ret)
    {
        ok(WaitForSingleObject(Pi.hProcess, 30000) == WAIT_OBJECT_0, "Low child did not finish\n");
        GetExitCodeProcess(Pi.hProcess, &Code);
        ok(Code == (CHILD_LABEL_AT_LEVEL | CHILD_LABEL_ABOVE_FAILS | CHILD_DEFAULT_AT_LEVEL),
           "Low child result %lx\n", Code);
        TerminateProcess(Pi.hProcess, 0);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
    }
    CloseHandle(Self);
}

/* An explicit label above the token that caps it fails creation cleanly. */
static VOID TestExplicitFailures(HANDLE Token, DWORD Level)
{
    SECURITY_DESCRIPTOR Sd;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &Sd, FALSE};
    ULONG AclBuffer[16];
    PROCESS_INFORMATION Pi;
    STARTUPINFOW Startup = {sizeof(Startup)};
    HANDLE LowToken, Thread;
    DWORD Error, Rid;
    BOOL Ret;

    if (Level >= SECURITY_MANDATORY_SYSTEM_RID)
    {
        skip("Caller level %lx leaves no level above it\n", Level);
        return;
    }

    MakeLabel(&Sd, (PACL)AclBuffer, Level + 0x1000, PROCESS_LABEL_POLICY);
    CheckProcessCreation(NULL, &Attributes, ERROR_PRIVILEGE_NOT_HELD, "Process label above caller");
    CheckThreadCreation(&Attributes, ERROR_PRIVILEGE_NOT_HELD, "Thread label above caller");
    MakeLabel(&Sd, (PACL)AclBuffer, Level, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP);
    CheckThreadCreation(&Attributes, ERROR_SUCCESS, "Thread label at caller level");

    if (!MakeLevelToken(Token, SECURITY_MANDATORY_LOW_RID, &LowToken)) return;

    /* The primary token, not an impersonation token, caps a thread label. */
    Ret = ImpersonateLoggedOnUser(LowToken);
    ok(Ret, "ImpersonateLoggedOnUser: %lu\n", GetLastError());
    if (Ret)
    {
        CheckThreadCreation(&Attributes, ERROR_SUCCESS, "Thread label at primary level while impersonating");
        ok(RevertToSelf(), "RevertToSelf\n");
    }

    Ret = CreateChild(LowToken, NULL, &Pi);
    if (!Ret && GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
    {
        skip("CreateProcessAsUser requires a privilege not held by this account\n");
        CloseHandle(LowToken);
        return;
    }
    ok(Ret, "CreateProcessAsUser: %lu\n", GetLastError());
    if (Ret)
    {
        /* A thread in another process is labelled and capped by its creator's token. */
        Thread = CreateRemoteThread(Pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)ExitThread,
                                    NULL, CREATE_SUSPENDED, NULL);
        ok(Thread != NULL, "CreateRemoteThread: %lu\n", GetLastError());
        if (Thread)
        {
            /* Server 2008 R2 takes the lower of both levels, Windows 10 the creator's */
            Rid = QueryLevel(Thread);
            ok(Rid == SECURITY_MANDATORY_LOW_RID || Rid == Level, "Remote thread level %lx\n", Rid);
            TerminateThread(Thread, 0);
            CloseHandle(Thread);
        }
        MakeLabel(&Sd, (PACL)AclBuffer, Level, PROCESS_LABEL_POLICY);
        Thread = CreateRemoteThread(Pi.hProcess, &Attributes, 0, (LPTHREAD_START_ROUTINE)ExitThread,
                                    NULL, CREATE_SUSPENDED, NULL);
        ok(Thread != NULL, "CreateRemoteThread with label at caller level: %lu\n", GetLastError());
        if (Thread)
        {
            CheckLabel(Thread, Level, PROCESS_LABEL_POLICY);
            TerminateThread(Thread, 0);
            CloseHandle(Thread);
        }
        MakeLabel(&Sd, (PACL)AclBuffer, Level + 0x1000, PROCESS_LABEL_POLICY);
        SetLastError(0xdeadbeef);
        Thread = CreateRemoteThread(Pi.hProcess, &Attributes, 0, (LPTHREAD_START_ROUTINE)ExitThread,
                                    NULL, CREATE_SUSPENDED, NULL);
        Error = Thread ? ERROR_SUCCESS : GetLastError();
        ok(Thread == NULL && Error == ERROR_PRIVILEGE_NOT_HELD,
           "CreateRemoteThread with label above caller: %p, error %lu\n", Thread, Error);
        if (Thread)
        {
            TerminateThread(Thread, 0);
            CloseHandle(Thread);
        }
        CloseChild(&Pi);
    }

    TestLowCreator(LowToken);

    /* The new process's token caps a process label. */
    MakeLabel(&Sd, (PACL)AclBuffer, SECURITY_MANDATORY_LOW_RID, PROCESS_LABEL_POLICY);
    Ret = CreateChild(LowToken, &Attributes, &Pi);
    ok(Ret, "CreateProcessAsUser with label at token level: %lu\n", GetLastError());
    if (Ret)
    {
        CheckLabel(Pi.hProcess, SECURITY_MANDATORY_LOW_RID, PROCESS_LABEL_POLICY);
        CheckLabel(Pi.hThread, SECURITY_MANDATORY_LOW_RID, PROCESS_LABEL_POLICY);
        CloseChild(&Pi);
    }
    MakeLabel(&Sd, (PACL)AclBuffer, Level, PROCESS_LABEL_POLICY);
    CheckProcessCreation(LowToken, &Attributes, ERROR_PRIVILEGE_NOT_HELD, "CreateProcessAsUser label at caller level");
    memset(&Pi, 0xcc, sizeof(Pi));
    SetLastError(0xdeadbeef);
    Ret = CreateProcessAsUserW(LowToken, NULL, CommandLineOf(L"cmd.exe"), NULL, &Attributes, FALSE,
                               CREATE_SUSPENDED, NULL, NULL, &Startup, &Pi);
    Error = Ret ? ERROR_SUCCESS : GetLastError();
    ok(!Ret && Error == ERROR_PRIVILEGE_NOT_HELD && !Pi.hProcess && !Pi.hThread,
       "CreateProcessAsUser thread label at caller level: %d, error %lu, %p %p\n", Ret, Error, Pi.hProcess, Pi.hThread);
    if (Ret) CloseChild(&Pi);
    MakeLabel(&Sd, (PACL)AclBuffer, Level + 0x1000, PROCESS_LABEL_POLICY);
    CheckProcessCreation(LowToken, &Attributes, ERROR_PRIVILEGE_NOT_HELD, "CreateProcessAsUser label above caller");
    CloseHandle(LowToken);
}

static VOID PutLabelAce(PBYTE Buffer, DWORD Rid, BYTE Authority)
{
    PSYSTEM_MANDATORY_LABEL_ACE Ace = (PSYSTEM_MANDATORY_LABEL_ACE)Buffer;
    PISID Sid = (PISID)&Ace->SidStart;

    Ace->Header.AceType = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    Ace->Header.AceFlags = 0;
    Ace->Header.AceSize = 20;
    Ace->Mask = PROCESS_LABEL_POLICY;
    Sid->Revision = SID_REVISION;
    Sid->SubAuthorityCount = 1;
    memset(&Sid->IdentifierAuthority, 0, sizeof(Sid->IdentifierAuthority));
    Sid->IdentifierAuthority.Value[5] = Authority;
    Sid->SubAuthority[0] = Rid;
}

/* Builds the SACL of one odd-label case by hand, as a caller could pass it. */
static VOID MakeOddSacl(DWORD Variant, PBYTE Buffer, SECURITY_DESCRIPTOR *Sd)
{
    PACL Acl = (PACL)Buffer;
    PBYTE Ace = Buffer + sizeof(ACL);
    PSYSTEM_MANDATORY_LABEL_ACE Label = (PSYSTEM_MANDATORY_LABEL_ACE)Ace;
    PSYSTEM_AUDIT_ACE Audit;
    SID_IDENTIFIER_AUTHORITY World = {SECURITY_WORLD_SID_AUTHORITY};

    memset(Buffer, 0, 128);
    Acl->AclRevision = ACL_REVISION;
    Acl->AceCount = 1;
    Acl->AclSize = sizeof(ACL) + 20;
    PutLabelAce(Ace, SECURITY_MANDATORY_LOW_RID, 16);
    switch (Variant)
    {
        case 0: ((PISID)&Label->SidStart)->IdentifierAuthority.Value[5] = 5; break;
        case 1: Label->Mask = 8; break;
        case 2: ((PISID)&Label->SidStart)->Revision = 2; break;
        case 3: Label->Header.AceSize = 16; break;
        case 4: Acl->AclSize += 2; break;
        case 5: Acl->Sbz1 = 1; break;
        case 6: Acl->Sbz2 = 1; break;
        case 7:
            Audit = (PSYSTEM_AUDIT_ACE)(Ace + 20);
            Audit->Header.AceType = SYSTEM_AUDIT_ACE_TYPE;
            Audit->Header.AceFlags = SUCCESSFUL_ACCESS_ACE_FLAG;
            Audit->Header.AceSize = 20;
            Audit->Mask = GENERIC_ALL;
            InitializeSid((PSID)&Audit->SidStart, &World, 1);
            *GetSidSubAuthority((PSID)&Audit->SidStart, 0) = SECURITY_WORLD_RID;
            Acl->AceCount = 2;
            Acl->AclSize += 20;
            break;
        case 8:
        case 9:
            /* An ACE of an unknown type in front of the label */
            memset(Ace, 0, 28);
            Ace[0] = 0x15;
            Ace[2] = (Variant == 8) ? 6 : 8;
            PutLabelAce(Ace + Ace[2], SECURITY_MANDATORY_LOW_RID, 16);
            Acl->AceCount = 2;
            Acl->AclSize = sizeof(ACL) + 28;
            break;
        case 10: Label->Header.AceSize = 22; Acl->AclSize += 2; break;
    }
    InitializeSecurityDescriptor(Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorSacl(Sd, TRUE, Acl, FALSE);
}

/*
 * Odd label SACLs at creation, for a caller without an enabled SeSecurityPrivilege.
 * A malformed label is not a label that skips the SACL privilege, and nothing a
 * caller passes may stop the kernel. Where Server 2008 R2 and Windows 10 differ,
 * both results are accepted.
 */
static VOID TestMalformedLabels(HANDLE Token)
{
    static const struct
    {
        PCSTR Name;
        DWORD Error1, Error2;
    } Cases[] =
    {
        {"non-label SID", ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER},
        {"invalid policy bit", ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER},
        {"SID revision", ERROR_INVALID_ACL, ERROR_INVALID_SID},
        {"ACE too small for SID", ERROR_INVALID_ACL, ERROR_SUCCESS},
        {"ACL size not ULONG aligned", ERROR_SUCCESS, ERROR_SUCCESS},
        {"ACL Sbz1", ERROR_SUCCESS, ERROR_SUCCESS},
        {"ACL Sbz2", ERROR_SUCCESS, ERROR_SUCCESS},
        {"label and audit ACE", ERROR_PRIVILEGE_NOT_HELD, ERROR_PRIVILEGE_NOT_HELD},
        {"unaligned unknown ACE before label", ERROR_PRIVILEGE_NOT_HELD, ERROR_INVALID_ACL},
        {"unknown ACE before label", ERROR_PRIVILEGE_NOT_HELD, ERROR_INVALID_ACL},
        {"label ACE size not ULONG aligned", ERROR_SUCCESS, ERROR_INVALID_ACL},
    };
    SECURITY_DESCRIPTOR Sd;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &Sd, FALSE};
    ULONG AclBuffer[32];
    PROCESS_INFORMATION Pi;
    HANDLE Event, Thread;
    DWORD Variant, ThreadError, ProcessError;
    BOOL Ret;

    for (Variant = 0; Variant < _countof(Cases); ++Variant)
    {
        MakeOddSacl(Variant, (PBYTE)AclBuffer, &Sd);
        if (!ImpersonateWithoutSecurityPrivilege(Token)) return;

        Event = CreateEventW(NULL, TRUE, FALSE, NULL);
        SetLastError(0xdeadbeef);
        Thread = CreateThread(&Attributes, 0, Worker, Event, 0, NULL);
        ThreadError = Thread ? ERROR_SUCCESS : GetLastError();
        if (Event) SetEvent(Event);
        if (Thread)
        {
            WaitForSingleObject(Thread, 10000);
            CloseHandle(Thread);
        }
        if (Event) CloseHandle(Event);

        memset(&Pi, 0xcc, sizeof(Pi));
        SetLastError(0xdeadbeef);
        Ret = CreateChild(NULL, &Attributes, &Pi);
        ProcessError = Ret ? ERROR_SUCCESS : GetLastError();
        if (Ret)
            CloseChild(&Pi);
        else
            ok(!Pi.hProcess && !Pi.hThread, "%s: handles %p %p left after failure\n",
               Cases[Variant].Name, Pi.hProcess, Pi.hThread);
        ok(RevertToSelf(), "RevertToSelf\n");

        ok(ThreadError == Cases[Variant].Error1 || ThreadError == Cases[Variant].Error2,
           "%s: thread error %lu\n", Cases[Variant].Name, ThreadError);
        ok(ProcessError == Cases[Variant].Error1 || ProcessError == Cases[Variant].Error2,
           "%s: process error %lu\n", Cases[Variant].Name, ProcessError);
    }
}

static VOID PutWorldAce(PBYTE Buffer, BYTE Type, WORD Size, BYTE Flags)
{
    PACCESS_ALLOWED_ACE Ace = (PACCESS_ALLOWED_ACE)Buffer;
    SID_IDENTIFIER_AUTHORITY World = {SECURITY_WORLD_SID_AUTHORITY};

    Ace->Header.AceType = Type;
    Ace->Header.AceFlags = Flags;
    Ace->Header.AceSize = Size;
    Ace->Mask = GENERIC_ALL;
    InitializeSid((PSID)&Ace->SidStart, &World, 1);
    *GetSidSubAuthority((PSID)&Ace->SidStart, 0) = SECURITY_WORLD_RID;
}

/*
 * Truncated and unaligned ACEs in any ACL reach the kernel's ACL propagation
 * before any privilege check, so capture must refuse what it cannot copy.
 */
static VOID TestOddAcls(HANDLE Token)
{
    SECURITY_DESCRIPTOR Sd;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &Sd, FALSE};
    ULONG Buffer[32];
    PACL Acl = (PACL)Buffer;
    PBYTE Ace = (PBYTE)(Acl + 1);
    HANDLE Handle, Duplicate;
    DWORD Variant, Error;
    BOOL Valid;

    for (Variant = 0; Variant < 2; ++Variant)
    {
        memset(Buffer, 0, sizeof(Buffer));
        Acl->AclRevision = ACL_REVISION;
        if (Variant == 0)
        {
            /* An allowed ACE too small for its mask and SID */
            PutWorldAce(Ace, ACCESS_ALLOWED_ACE_TYPE, 16, 0);
            Acl->AceCount = 1;
            Acl->AclSize = sizeof(ACL) + 20;
        }
        else
        {
            /* An ACE of an unknown type that leaves the next one unaligned */
            Ace[0] = 0x15;
            Ace[2] = 6;
            PutWorldAce(Ace + 6, ACCESS_ALLOWED_ACE_TYPE, 20, 0);
            Acl->AceCount = 2;
            Acl->AclSize = sizeof(ACL) + 28;
        }
        Valid = IsValidAcl(Acl);
        InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&Sd, TRUE, Acl, FALSE);

        SetLastError(0xdeadbeef);
        Handle = CreateEventW(&Attributes, FALSE, FALSE, NULL);
        Error = Handle ? ERROR_SUCCESS : GetLastError();
        if (Handle) CloseHandle(Handle);
        if (Variant == 0)
        {
            ok(!Valid, "Truncated ACE: IsValidAcl returned TRUE\n");
            ok(Error == ERROR_INVALID_ACL, "Truncated ACE: event error %lu\n", Error);
        }
        else
        {
            /* Server 2008 R2 accepts it, Windows 10 rejects it */
            ok(Error == ERROR_SUCCESS || Error == ERROR_INVALID_ACL, "Unaligned ACE: event error %lu\n", Error);
        }

        CheckThreadCreation(&Attributes, Variant == 0 ? ERROR_INVALID_ACL : MAXDWORD,
                            Variant == 0 ? "Truncated DACL ACE" : "Unaligned DACL ACE");

        SetLastError(0xdeadbeef);
        if (DuplicateTokenEx(Token, TOKEN_QUERY, &Attributes, SecurityImpersonation, TokenPrimary, &Duplicate))
        {
            Error = ERROR_SUCCESS;
            CloseHandle(Duplicate);
        }
        else
        {
            Error = GetLastError();
        }
        if (Variant == 0)
            ok(Error == ERROR_INVALID_ACL, "Truncated ACE: token error %lu\n", Error);
        else
            ok(Error == ERROR_SUCCESS || Error == ERROR_INVALID_ACL, "Unaligned ACE: token error %lu\n", Error);
    }

    /* A label followed by a truncated audit ACE */
    memset(Buffer, 0, sizeof(Buffer));
    Acl->AclRevision = ACL_REVISION;
    PutLabelAce(Ace, SECURITY_MANDATORY_LOW_RID, 16);
    PutWorldAce(Ace + 20, SYSTEM_AUDIT_ACE_TYPE, 16, SUCCESSFUL_ACCESS_ACE_FLAG);
    Acl->AceCount = 2;
    Acl->AclSize = sizeof(ACL) + 40;
    InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorSacl(&Sd, TRUE, Acl, FALSE);
    if (ImpersonateWithoutSecurityPrivilege(Token))
    {
        CheckThreadCreation(&Attributes, ERROR_INVALID_ACL, "Label and truncated audit ACE");
        ok(RevertToSelf(), "RevertToSelf\n");
    }
}

/* CreateProcessAsUser keeps what an explicit descriptor gives and defaults the rest. */
static VOID TestExplicitDescriptor(HANDLE LowToken)
{
    BYTE UserBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    BYTE GroupBuffer[sizeof(TOKEN_PRIMARY_GROUP) + SECURITY_MAX_SID_SIZE];
    BYTE OwnerBuffer[sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_OWNER DefaultOwner = (PTOKEN_OWNER)OwnerBuffer;
    PTOKEN_USER User = (PTOKEN_USER)UserBuffer;
    PTOKEN_PRIMARY_GROUP Group = (PTOKEN_PRIMARY_GROUP)GroupBuffer;
    SID_IDENTIFIER_AUTHORITY World = {SECURITY_WORLD_SID_AUTHORITY};
    BYTE WorldSid[SECURITY_MAX_SID_SIZE];
    SECURITY_DESCRIPTOR Sd;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &Sd, FALSE};
    ULONG SaclBuffer[16], DaclBuffer[16];
    PSECURITY_DESCRIPTOR Result;
    SECURITY_DESCRIPTOR_CONTROL Control;
    PROCESS_INFORMATION Pi;
    PSID Owner;
    PACL Dacl;
    DWORD Length, Revision, Variant;
    BOOL Ret;

    if (!GetTokenInformation(LowToken, TokenUser, UserBuffer, sizeof(UserBuffer), &Length) ||
        !GetTokenInformation(LowToken, TokenPrimaryGroup, GroupBuffer, sizeof(GroupBuffer), &Length) ||
        !GetTokenInformation(LowToken, TokenOwner, OwnerBuffer, sizeof(OwnerBuffer), &Length))
    {
        skip("Token user or group unavailable: %lu\n", GetLastError());
        return;
    }
    InitializeSid((PSID)WorldSid, &World, 1);
    *GetSidSubAuthority((PSID)WorldSid, 0) = SECURITY_WORLD_RID;
    InitializeAcl((PACL)DaclBuffer, sizeof(DaclBuffer), ACL_REVISION);
    AddAccessAllowedAce((PACL)DaclBuffer, ACL_REVISION, GENERIC_ALL, (PSID)WorldSid);

    for (Variant = 0; Variant < 3; ++Variant)
    {
        MakeLabel(&Sd, (PACL)SaclBuffer, SECURITY_MANDATORY_LOW_RID, PROCESS_LABEL_POLICY);
        if (Variant == 0)
        {
            /* Complete: owner, group and a protected DACL */
            SetSecurityDescriptorOwner(&Sd, User->User.Sid, FALSE);
            SetSecurityDescriptorGroup(&Sd, Group->PrimaryGroup, FALSE);
            SetSecurityDescriptorDacl(&Sd, TRUE, (PACL)DaclBuffer, FALSE);
            SetSecurityDescriptorControl(&Sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
        }
        else if (Variant == 1)
        {
            /* Partial: a NULL DACL only */
            SetSecurityDescriptorDacl(&Sd, TRUE, NULL, FALSE);
        }
        Ret = CreateChild(LowToken, &Attributes, &Pi);
        ok(Ret, "Explicit descriptor %lu: CreateProcessAsUser failed with %lu\n", Variant, GetLastError());
        if (!Ret) continue;
        Result = NULL;
        ok(GetSecurityInfo(Pi.hProcess, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                           &Owner, NULL, &Dacl, NULL, &Result) == ERROR_SUCCESS, "GetSecurityInfo\n");
        if (Result)
        {
            ok(Owner && EqualSid(Owner, Variant == 0 ? User->User.Sid : DefaultOwner->Owner),
               "Explicit descriptor %lu: unexpected owner\n", Variant);
            ok(GetSecurityDescriptorControl(Result, &Control, &Revision), "GetSecurityDescriptorControl\n");
            if (Variant == 0)
            {
                ok(Control & SE_DACL_PROTECTED, "Explicit descriptor 0: control %x\n", Control);
                ok(Dacl && Dacl->AceCount == 1, "Explicit descriptor 0: DACL %p\n", Dacl);
            }
            else if (Variant == 1)
                ok(Dacl == NULL, "Explicit descriptor 1: DACL %p, expected NULL\n", Dacl);
            else
                ok(Dacl != NULL, "Explicit descriptor 2: no default DACL\n");
            LocalFree(Result);
        }
        CheckLabel(Pi.hProcess, SECURITY_MANDATORY_LOW_RID, PROCESS_LABEL_POLICY);
        CloseChild(&Pi);
    }
}

START_TEST(ProcessThreadLabel)
{
    char **Argv;
    HANDLE Token, Event, Thread, LowToken;
    SECURITY_DESCRIPTOR ExplicitSd;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &ExplicitSd, FALSE};
    ULONG AclBuffer[16];
    DWORD Level, Rid;
    PROCESS_INFORMATION Pi;
    TOKEN_MANDATORY_LABEL Label;
    SID Sid = {SID_REVISION, 1, {SECURITY_MANDATORY_LABEL_AUTHORITY}, {0}};
    BOOL Ret;

    if (winetest_get_mainargs(&Argv) >= 4 && !strcmp(Argv[2], "child"))
        ExitProcess(ChildRemoteThreads((HANDLE)(ULONG_PTR)_strtoui64(Argv[3], NULL, 16)));

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

    TestExplicitFailures(Token, Level);
    TestMalformedLabels(Token);
    TestOddAcls(Token);
    if (MakeLevelToken(Token, SECURITY_MANDATORY_LOW_RID, &LowToken))
    {
        TestExplicitDescriptor(LowToken);
        CloseHandle(LowToken);
    }
    CloseHandle(Token);
}
