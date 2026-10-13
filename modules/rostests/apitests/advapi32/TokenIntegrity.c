/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the integrity level of tokens and the mandatory label of token objects
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <aclapi.h>
#include <ndk/obfuncs.h>
#include <ndk/exfuncs.h>

static SID_IDENTIFIER_AUTHORITY MandatoryLabelAuthority = {SECURITY_MANDATORY_LABEL_AUTHORITY};

static BOOL GetIntegrityLevel(HANDLE hToken, PDWORD Rid, PDWORD Attributes)
{
    BYTE Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    PTOKEN_MANDATORY_LABEL Label = (PTOKEN_MANDATORY_LABEL)Buffer;
    DWORD Length = 0;
    PSID Sid;

    if (!GetTokenInformation(hToken, TokenIntegrityLevel, Buffer, sizeof(Buffer), &Length))
        return FALSE;
    Sid = Label->Label.Sid;
    ok(*GetSidSubAuthorityCount(Sid) == 1, "Sub authority count %u\n", *GetSidSubAuthorityCount(Sid));
    ok(!memcmp(GetSidIdentifierAuthority(Sid), &MandatoryLabelAuthority, sizeof(MandatoryLabelAuthority)),
       "Not a mandatory label SID\n");
    *Rid = *GetSidSubAuthority(Sid, 0);
    *Attributes = Label->Label.Attributes;
    return TRUE;
}

/* The label ACE of an object, or FALSE if its SACL has none */
static BOOL GetObjectLabel(HANDLE hObject, PDWORD Rid, PDWORD Mask, PDWORD AceCount)
{
    BYTE Buffer[512];
    PSECURITY_DESCRIPTOR Sd = (PSECURITY_DESCRIPTOR)Buffer;
    DWORD Length = 0, i;
    BOOL Present = FALSE, Defaulted;
    PACL Sacl = NULL;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;

    if (!GetKernelObjectSecurity(hObject, LABEL_SECURITY_INFORMATION, Sd, sizeof(Buffer), &Length))
    {
        ok(0, "GetKernelObjectSecurity(LABEL) failed: %lu\n", GetLastError());
        return FALSE;
    }
    /* The length returned is what the descriptor takes, not the buffer size */
    ok(Length == GetSecurityDescriptorLength(Sd), "Length %lu, descriptor %lu\n", Length, GetSecurityDescriptorLength(Sd));
    ok(GetSecurityDescriptorSacl(Sd, &Present, &Sacl, &Defaulted), "GetSecurityDescriptorSacl failed\n");
    if (!Present || !Sacl)
        return FALSE;
    *AceCount = Sacl->AceCount;
    for (i = 0; i < Sacl->AceCount; i++)
    {
        if (GetAce(Sacl, i, (PVOID *)&Ace) && Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
        {
            *Rid = *GetSidSubAuthority((PSID)&Ace->SidStart, 0);
            *Mask = Ace->Mask;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL SetObjectLabel(HANDLE hObject, DWORD Rid, DWORD Mask)
{
    BYTE AclBuffer[64], SidBuffer[SECURITY_MAX_SID_SIZE];
    PACL Sacl = (PACL)AclBuffer;
    PSID Sid = (PSID)SidBuffer;
    SECURITY_DESCRIPTOR Sd;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;

    InitializeSid(Sid, &MandatoryLabelAuthority, 1);
    *GetSidSubAuthority(Sid, 0) = Rid;
    InitializeAcl(Sacl, sizeof(AclBuffer), ACL_REVISION);
    Ace = (PSYSTEM_MANDATORY_LABEL_ACE)(Sacl + 1);
    Ace->Header.AceType = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    Ace->Header.AceFlags = 0;
    Ace->Header.AceSize = (WORD)(FIELD_OFFSET(SYSTEM_MANDATORY_LABEL_ACE, SidStart) + GetLengthSid(Sid));
    Ace->Mask = Mask;
    CopySid(GetLengthSid(Sid), (PSID)&Ace->SidStart, Sid);
    Sacl->AceCount = 1;
    InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorSacl(&Sd, TRUE, Sacl, FALSE);
    return SetKernelObjectSecurity(hObject, LABEL_SECURITY_INFORMATION, &Sd);
}

/* The RID of the integrity group of a token, if it lists one */
static BOOL GetIntegrityGroup(HANDLE hToken, PDWORD Rid)
{
    BYTE Buffer[4096];
    PTOKEN_GROUPS Groups = (PTOKEN_GROUPS)Buffer;
    DWORD Length, i;

    if (!GetTokenInformation(hToken, TokenGroups, Buffer, sizeof(Buffer), &Length))
        return FALSE;
    for (i = 0; i < Groups->GroupCount; i++)
    {
        if (Groups->Groups[i].Attributes & SE_GROUP_INTEGRITY)
        {
            *Rid = *GetSidSubAuthority(Groups->Groups[i].Sid, 0);
            return TRUE;
        }
    }
    return FALSE;
}

/* Labels are SACL entries of any object, not something of tokens only */
static void TestEventLabel(void)
{
    WCHAR Name[64];
    HANDLE hEvent;
    DWORD Rid = 0, Mask = 0, AceCount = 0;

    /* Named, so that it has a security descriptor */
    StringCchPrintfW(Name, _countof(Name), L"TokenIntegrityTest%lu", GetCurrentProcessId());
    hEvent = CreateEventW(NULL, FALSE, FALSE, Name);
    ok(hEvent != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!hEvent)
        return;
    /* Lowering an object's label needs no privilege */
    ok(SetObjectLabel(hEvent, SECURITY_MANDATORY_LOW_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP),
       "SetKernelObjectSecurity(LABEL) on an event failed: %lu\n", GetLastError());
    ok(GetObjectLabel(hEvent, &Rid, &Mask, &AceCount), "The event has no label after setting it\n");
    ok(Rid == SECURITY_MANDATORY_LOW_RID, "Event label level 0x%lx\n", Rid);
    ok(Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, "Event label policy 0x%lx\n", Mask);
    ok(AceCount == 1, "%lu ACEs\n", AceCount);

    /* The same through GetSecurityInfo and SetSecurityInfo, as Chromium's sandbox uses them */
    {
        PSECURITY_DESCRIPTOR Sd = NULL;
        PACL Sacl = NULL;
        PSYSTEM_MANDATORY_LABEL_ACE Ace;
        DWORD Error;

        Error = GetSecurityInfo(hEvent, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL, &Sacl, &Sd);
        ok(Error == ERROR_SUCCESS, "GetSecurityInfo(LABEL) failed: %lu\n", Error);
        ok(Sacl != NULL, "GetSecurityInfo(LABEL) gave no SACL\n");
        if (Error == ERROR_SUCCESS && Sacl && Sacl->AceCount == 1 && GetAce(Sacl, 0, (PVOID *)&Ace))
        {
            ok(Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE, "ACE type %u\n", Ace->Header.AceType);
            ok(*GetSidSubAuthority((PSID)&Ace->SidStart, 0) == SECURITY_MANDATORY_LOW_RID, "Label level 0x%lx\n",
               *GetSidSubAuthority((PSID)&Ace->SidStart, 0));
            Ace->Mask |= SYSTEM_MANDATORY_LABEL_NO_READ_UP;
            Error = SetSecurityInfo(hEvent, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL, Sacl);
            ok(Error == ERROR_SUCCESS, "SetSecurityInfo(LABEL) failed: %lu\n", Error);
            ok(GetObjectLabel(hEvent, &Rid, &Mask, &AceCount), "No label after SetSecurityInfo\n");
            ok(Rid == SECURITY_MANDATORY_LOW_RID, "Label level 0x%lx after SetSecurityInfo\n", Rid);
            ok(Mask == (SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP),
               "Label policy 0x%lx after SetSecurityInfo\n", Mask);
        }
        if (Sd)
            LocalFree(Sd);
    }

    /* A label above the caller's own level needs SeRelabelPrivilege, which is not enabled here */
    {
        HANDLE hToken;
        DWORD Level = 0, Attributes;

        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        {
            if (GetIntegrityLevel(hToken, &Level, &Attributes) && Level < SECURITY_MANDATORY_SYSTEM_RID)
            {
                SetLastError(0xdeadbeef);
                ok(!SetObjectLabel(hEvent, SECURITY_MANDATORY_SYSTEM_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP),
                   "Setting a label above the caller's level succeeded\n");
                ok(GetLastError() == ERROR_INVALID_LABEL, "Error %lu\n", GetLastError());
            }
            else
            {
                skip("Running at system level\n");
            }
            CloseHandle(hToken);
        }
    }

    CloseHandle(hEvent);
}

/* A token handle can be duplicated with more rights when the token's DACL grants them */
static void TestDuplicateHandle(HANDLE hToken)
{
    BYTE AclBuffer[128], SdBuffer[SECURITY_DESCRIPTOR_MIN_LENGTH];
    SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};
    PSECURITY_DESCRIPTOR Sd = (PSECURITY_DESCRIPTOR)SdBuffer;
    SECURITY_ATTRIBUTES Attributes;
    PACL Dacl = (PACL)AclBuffer;
    PSID World = NULL;
    HANDLE hNew = NULL, hMore;
    BOOL Ret;

    if (!AllocateAndInitializeSid(&WorldAuthority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &World))
    {
        ok(0, "AllocateAndInitializeSid failed: %lu\n", GetLastError());
        return;
    }
    InitializeAcl(Dacl, sizeof(AclBuffer), ACL_REVISION);
    AddAccessAllowedAce(Dacl, ACL_REVISION, TOKEN_QUERY | TOKEN_QUERY_SOURCE, World);
    InitializeSecurityDescriptor(Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(Sd, TRUE, Dacl, FALSE);
    Attributes.nLength = sizeof(Attributes);
    Attributes.lpSecurityDescriptor = Sd;
    Attributes.bInheritHandle = FALSE;

    Ret = DuplicateTokenEx(hToken, TOKEN_QUERY, &Attributes, SecurityImpersonation, TokenPrimary, &hNew);
    ok(Ret, "DuplicateTokenEx failed: %lu\n", GetLastError());
    if (Ret)
    {
        Ret = DuplicateHandle(GetCurrentProcess(), hNew, GetCurrentProcess(), &hMore,
                              TOKEN_QUERY | TOKEN_QUERY_SOURCE, FALSE, 0);
        ok(Ret, "DuplicateHandle with a right the DACL grants failed: %lu\n", GetLastError());
        if (Ret)
            CloseHandle(hMore);

        SetLastError(0xdeadbeef);
        Ret = DuplicateHandle(GetCurrentProcess(), hNew, GetCurrentProcess(), &hMore,
                              TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, FALSE, 0);
        ok(!Ret && GetLastError() == ERROR_ACCESS_DENIED,
           "DuplicateHandle with a right the DACL does not grant: %d, error %lu\n", Ret, GetLastError());
        if (Ret)
            CloseHandle(hMore);

        /* The new token object has the label of its level */
        {
            DWORD Rid = 0, LabelRid = 0, Mask = 0, AceCount = 0, Attr;
            HANDLE hRead;

            if (DuplicateHandle(GetCurrentProcess(), hNew, GetCurrentProcess(), &hRead, TOKEN_QUERY | READ_CONTROL, FALSE, 0))
            {
                ok(GetIntegrityLevel(hRead, &Rid, &Attr), "No level on the new token\n");
                ok(GetObjectLabel(hRead, &LabelRid, &Mask, &AceCount), "The new token has no label\n");
                ok(LabelRid == Rid, "New token label 0x%lx, level 0x%lx\n", LabelRid, Rid);
                CloseHandle(hRead);
            }
        }
        CloseHandle(hNew);
    }
    FreeSid(World);
}

/* The SACL view and the label view, when this process may read SACLs */
static void TestSaclView(void)
{
    BYTE Buffer[1024];
    PSECURITY_DESCRIPTOR Sd = (PSECURITY_DESCRIPTOR)Buffer;
    TOKEN_PRIVILEGES Privileges;
    HANDLE hToken, hProcessToken;
    BOOL Present = FALSE, Defaulted, Found = FALSE;
    DWORD Length, LabelRid = 0, Mask = 0, AceCount = 0, i;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;
    PACL Sacl = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hProcessToken))
        return;
    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(NULL, L"SeSecurityPrivilege", &Privileges.Privileges[0].Luid) ||
        !AdjustTokenPrivileges(hProcessToken, FALSE, &Privileges, 0, NULL, NULL) ||
        GetLastError() != ERROR_SUCCESS)
    {
        skip("SeSecurityPrivilege is not available\n");
        CloseHandle(hProcessToken);
        return;
    }

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | READ_CONTROL | ACCESS_SYSTEM_SECURITY, &hToken))
    {
        ok(GetObjectLabel(hToken, &LabelRid, &Mask, &AceCount), "No label\n");
        /* SACL_SECURITY_INFORMATION shows the other entries only (Windows 7 and 10 alike) */
        ok(GetKernelObjectSecurity(hToken, SACL_SECURITY_INFORMATION, Sd, sizeof(Buffer), &Length),
           "GetKernelObjectSecurity(SACL) failed: %lu\n", GetLastError());
        if (GetSecurityDescriptorSacl(Sd, &Present, &Sacl, &Defaulted) && Present && Sacl)
        {
            for (i = 0; i < Sacl->AceCount; i++)
            {
                if (GetAce(Sacl, i, (PVOID *)&Ace) && Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                    Found = TRUE;
            }
        }
        ok(!Found, "The SACL view shows the label\n");

        /* Both together show the whole SACL, label included */
        Found = FALSE;
        Present = FALSE;
        Sacl = NULL;
        ok(GetKernelObjectSecurity(hToken, SACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, Sd, sizeof(Buffer), &Length),
           "GetKernelObjectSecurity(SACL | LABEL) failed: %lu\n", GetLastError());
        if (GetSecurityDescriptorSacl(Sd, &Present, &Sacl, &Defaulted) && Present && Sacl)
        {
            for (i = 0; i < Sacl->AceCount; i++)
            {
                if (GetAce(Sacl, i, (PVOID *)&Ace) && Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                {
                    Found = TRUE;
                    ok(*GetSidSubAuthority((PSID)&Ace->SidStart, 0) == LabelRid, "Label differs from the LABEL view\n");
                    ok(Ace->Mask == Mask, "Label policy 0x%lx, LABEL view 0x%lx\n", Ace->Mask, Mask);
                }
            }
        }
        ok(Found, "The SACL | LABEL view has no label\n");
        CloseHandle(hToken);
    }
    else
    {
        skip("Cannot open the token for its SACL: %lu\n", GetLastError());
    }

    Privileges.Privileges[0].Attributes = 0;
    AdjustTokenPrivileges(hProcessToken, FALSE, &Privileges, 0, NULL, NULL);
    CloseHandle(hProcessToken);
}

/* Appends a label ACE to an ACL that has room for it */
static void AddLabelAce(PACL Acl, DWORD Rid, DWORD Mask, BYTE AceFlags)
{
    BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
    PSID Sid = (PSID)SidBuffer;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;
    PACE_HEADER Entry = (PACE_HEADER)(Acl + 1);
    DWORD i;

    for (i = 0; i < Acl->AceCount; i++)
        Entry = (PACE_HEADER)((PBYTE)Entry + Entry->AceSize);
    InitializeSid(Sid, &MandatoryLabelAuthority, 1);
    *GetSidSubAuthority(Sid, 0) = Rid;
    Ace = (PSYSTEM_MANDATORY_LABEL_ACE)Entry;
    Ace->Header.AceType = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    Ace->Header.AceFlags = AceFlags;
    Ace->Header.AceSize = (WORD)(FIELD_OFFSET(SYSTEM_MANDATORY_LABEL_ACE, SidStart) + GetLengthSid(Sid));
    Ace->Mask = Mask;
    CopySid(GetLengthSid(Sid), (PSID)&Ace->SidStart, Sid);
    Acl->AceCount++;
}

static BOOL EnablePrivilege(PCWSTR Name, BOOL Enable)
{
    TOKEN_PRIVILEGES Privileges;
    HANDLE hToken;
    BOOL Ret;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        return FALSE;
    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = Enable ? SE_PRIVILEGE_ENABLED : 0;
    Ret = LookupPrivilegeValueW(NULL, Name, &Privileges.Privileges[0].Luid) &&
          AdjustTokenPrivileges(hToken, FALSE, &Privileges, 0, NULL, NULL) &&
          GetLastError() == ERROR_SUCCESS;
    CloseHandle(hToken);
    return Ret;
}

/* Several labels, removing them, and the DACL's protection across a label change */
static void TestLabelSet(void)
{
    BYTE AclBuffer[128], SdBuffer[512];
    PACL Acl = (PACL)AclBuffer, Sacl;
    PSECURITY_DESCRIPTOR Got = (PSECURITY_DESCRIPTOR)SdBuffer;
    SECURITY_DESCRIPTOR Sd;
    SECURITY_DESCRIPTOR_CONTROL Control;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;
    BOOL Present, Defaulted;
    DWORD Length, Revision, Rid, Mask, AceCount;
    WCHAR Name[64];
    HANDLE hEvent;

    StringCchPrintfW(Name, _countof(Name), L"TokenIntegritySet%lu", GetCurrentProcessId());
    hEvent = CreateEventW(NULL, FALSE, FALSE, Name);
    ok(hEvent != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!hEvent)
        return;

    /* Two labels are kept as they are, in order */
    InitializeAcl(Acl, sizeof(AclBuffer), ACL_REVISION);
    AddLabelAce(Acl, SECURITY_MANDATORY_LOW_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, 0);
    AddLabelAce(Acl, SECURITY_MANDATORY_MEDIUM_RID, SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP, 0);
    InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorSacl(&Sd, TRUE, Acl, FALSE);
    ok(SetKernelObjectSecurity(hEvent, LABEL_SECURITY_INFORMATION, &Sd), "Setting two labels failed: %lu\n", GetLastError());
    Present = FALSE;
    Sacl = NULL;
    ok(GetKernelObjectSecurity(hEvent, LABEL_SECURITY_INFORMATION, Got, sizeof(SdBuffer), &Length), "Query failed: %lu\n", GetLastError());
    ok(GetSecurityDescriptorSacl(Got, &Present, &Sacl, &Defaulted) && Present && Sacl, "No SACL\n");
    if (Present && Sacl)
    {
        ok(Sacl->AceCount == 2, "%u ACEs\n", Sacl->AceCount);
        if (Sacl->AceCount == 2 && GetAce(Sacl, 0, (PVOID *)&Ace))
            ok(*GetSidSubAuthority((PSID)&Ace->SidStart, 0) == SECURITY_MANDATORY_LOW_RID &&
               Ace->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, "First label 0x%lx, 0x%lx\n",
               *GetSidSubAuthority((PSID)&Ace->SidStart, 0), Ace->Mask);
        if (Sacl->AceCount == 2 && GetAce(Sacl, 1, (PVOID *)&Ace))
            ok(*GetSidSubAuthority((PSID)&Ace->SidStart, 0) == SECURITY_MANDATORY_MEDIUM_RID &&
               Ace->Mask == SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP, "Second label 0x%lx, 0x%lx\n",
               *GetSidSubAuthority((PSID)&Ace->SidStart, 0), Ace->Mask);
    }

    /* Removing the labels leaves an empty SACL, not an absent one */
    InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    ok(SetKernelObjectSecurity(hEvent, LABEL_SECURITY_INFORMATION, &Sd), "Removing the labels failed: %lu\n", GetLastError());
    Present = FALSE;
    Sacl = NULL;
    ok(GetKernelObjectSecurity(hEvent, LABEL_SECURITY_INFORMATION, Got, sizeof(SdBuffer), &Length), "Query failed: %lu\n", GetLastError());
    ok(GetSecurityDescriptorSacl(Got, &Present, &Sacl, &Defaulted), "GetSecurityDescriptorSacl failed\n");
    ok(Present, "No SACL after removing the labels\n");
    ok(Sacl && Sacl->AceCount == 0, "SACL %p with %u ACEs after removing the labels\n", Sacl, Sacl ? Sacl->AceCount : 0);

    /* A protected DACL stays protected when the label changes */
    InitializeAcl(Acl, sizeof(AclBuffer), ACL_REVISION);
    {
        BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
        SID_IDENTIFIER_AUTHORITY World = {SECURITY_WORLD_SID_AUTHORITY};
        PSID Everyone = (PSID)SidBuffer;

        InitializeSid(Everyone, &World, 1);
        *GetSidSubAuthority(Everyone, 0) = SECURITY_WORLD_RID;
        AddAccessAllowedAce(Acl, ACL_REVISION, EVENT_ALL_ACCESS, Everyone);
    }
    InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&Sd, TRUE, Acl, FALSE);
    SetSecurityDescriptorControl(&Sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
    ok(SetKernelObjectSecurity(hEvent, DACL_SECURITY_INFORMATION, &Sd), "Setting a protected DACL failed: %lu\n", GetLastError());
    ok(GetKernelObjectSecurity(hEvent, DACL_SECURITY_INFORMATION, Got, sizeof(SdBuffer), &Length), "Query failed: %lu\n", GetLastError());
    Control = 0;
    ok(GetSecurityDescriptorControl(Got, &Control, &Revision) && (Control & SE_DACL_PROTECTED),
       "DACL control 0x%x after setting a protected DACL\n", Control);
    ok(SetObjectLabel(hEvent, SECURITY_MANDATORY_LOW_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP), "Setting a label failed: %lu\n", GetLastError());
    ok(GetObjectLabel(hEvent, &Rid, &Mask, &AceCount) && Rid == SECURITY_MANDATORY_LOW_RID, "Label 0x%lx\n", Rid);
    ok(GetKernelObjectSecurity(hEvent, DACL_SECURITY_INFORMATION, Got, sizeof(SdBuffer), &Length), "Query failed: %lu\n", GetLastError());
    Control = 0;
    ok(GetSecurityDescriptorControl(Got, &Control, &Revision) && (Control & SE_DACL_PROTECTED),
       "DACL control 0x%x after a label change\n", Control);

    CloseHandle(hEvent);
}

/* A label in a directory's SACL reaches the objects created in it as its inheritance flags say */
static void TestLabelInheritance(void)
{
    static const struct { BYTE AceFlags; BOOL Inherited; } Cases[] =
    {
        { 0, FALSE },
        { OBJECT_INHERIT_ACE, TRUE },
        { CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE, FALSE },
    };
    BYTE AclBuffer[64];
    PACL Acl = (PACL)AclBuffer;
    SECURITY_DESCRIPTOR Sd;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Name;
    WCHAR NameBuffer[96];
    HANDLE hDir, hEvent;
    DWORD Rid, Mask, AceCount, i;
    NTSTATUS Status;
    BOOL Found;

    EnablePrivilege(L"SeSecurityPrivilege", TRUE);
    for (i = 0; i < ARRAYSIZE(Cases); i++)
    {
        InitializeAcl(Acl, sizeof(AclBuffer), ACL_REVISION);
        AddLabelAce(Acl, SECURITY_MANDATORY_LOW_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, Cases[i].AceFlags);
        InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&Sd, TRUE, NULL, FALSE);
        SetSecurityDescriptorSacl(&Sd, TRUE, Acl, FALSE);

        StringCchPrintfW(NameBuffer, _countof(NameBuffer), L"\\BaseNamedObjects\\TokenIntegrityDir%lu_%lu", GetCurrentProcessId(), i);
        RtlInitUnicodeString(&Name, NameBuffer);
        InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, &Sd);
        Status = NtCreateDirectoryObject(&hDir, DIRECTORY_ALL_ACCESS, &ObjectAttributes);
        ok(NT_SUCCESS(Status), "Case %lu: NtCreateDirectoryObject failed: 0x%lx\n", i, Status);
        if (!NT_SUCCESS(Status))
            continue;

        RtlInitUnicodeString(&Name, L"Child");
        InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, hDir, NULL);
        Status = NtCreateEvent(&hEvent, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
        ok(NT_SUCCESS(Status), "Case %lu: NtCreateEvent failed: 0x%lx\n", i, Status);
        if (NT_SUCCESS(Status))
        {
            Rid = 0;
            Found = GetObjectLabel(hEvent, &Rid, &Mask, &AceCount) && Rid == SECURITY_MANDATORY_LOW_RID;
            if (Cases[i].Inherited)
                ok(Found, "Case %lu (flags 0x%x): the child did not inherit the label (0x%lx)\n", i, Cases[i].AceFlags, Rid);
            else
                ok(!Found, "Case %lu (flags 0x%x): the child got a label that is not inherited\n", i, Cases[i].AceFlags);
            NtClose(hEvent);
        }
        NtClose(hDir);
    }
    EnablePrivilege(L"SeSecurityPrivilege", FALSE);
}

START_TEST(TokenIntegrity)
{
    HANDLE hToken = NULL, hDup = NULL;
    DWORD Rid, Attributes, LabelRid, Mask, AceCount;
    BOOL Ret;

    Ret = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | READ_CONTROL | WRITE_OWNER, &hToken);
    ok(Ret, "OpenProcessToken failed: %lu\n", GetLastError());
    if (!Ret)
        return;

    /* The level of the process token */
    Ret = GetIntegrityLevel(hToken, &Rid, &Attributes);
    ok(Ret, "GetTokenInformation(TokenIntegrityLevel) failed: %lu\n", GetLastError());
    if (!Ret)
    {
        CloseHandle(hToken);
        return;
    }
    ok(Rid == SECURITY_MANDATORY_MEDIUM_RID || Rid == SECURITY_MANDATORY_HIGH_RID || Rid == SECURITY_MANDATORY_SYSTEM_RID,
       "Integrity level 0x%lx\n", Rid);
    ok(Attributes & SE_GROUP_INTEGRITY, "Attributes 0x%lx\n", Attributes);

    /* The token object carries a mandatory label */
    Ret = GetObjectLabel(hToken, &LabelRid, &Mask, &AceCount);
    ok(Ret, "The process token has no mandatory label\n");
    if (Ret)
    {
        ok(AceCount == 1, "%lu ACEs\n", AceCount);
        ok(Mask & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, "Label policy 0x%lx\n", Mask);
    }

    /* What Chromium's sandbox does to its token: harden the label policy */
    if (Ret)
    {
        ok(SetObjectLabel(hToken, LabelRid, Mask | SYSTEM_MANDATORY_LABEL_NO_READ_UP | SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP),
           "SetKernelObjectSecurity(LABEL) failed: %lu\n", GetLastError());
        ok(GetObjectLabel(hToken, &Rid, &Mask, &AceCount), "No label after setting it\n");
        ok(Rid == LabelRid, "Label level 0x%lx, expected 0x%lx\n", Rid, LabelRid);
        ok((Mask & SYSTEM_MANDATORY_LABEL_VALID_MASK) == SYSTEM_MANDATORY_LABEL_VALID_MASK, "Label policy 0x%lx\n", Mask);
    }

    /* A duplicate runs at the same level, and can be lowered */
    Ret = DuplicateTokenEx(hToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &hDup);
    ok(Ret, "DuplicateTokenEx failed: %lu\n", GetLastError());
    if (!Ret)
    {
        CloseHandle(hToken);
        return;
    }
    ok(GetIntegrityLevel(hDup, &LabelRid, &Attributes), "No level on the duplicate\n");
    ok(GetIntegrityLevel(hToken, &Rid, &Attributes) && LabelRid == Rid, "Duplicate level 0x%lx, token 0x%lx\n", LabelRid, Rid);
    ok(GetObjectLabel(hDup, &LabelRid, &Mask, &AceCount), "The duplicate token has no mandatory label\n");
    {
        BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
        TOKEN_MANDATORY_LABEL Label;
        PSID Sid = (PSID)SidBuffer;

        InitializeSid(Sid, &MandatoryLabelAuthority, 1);
        *GetSidSubAuthority(Sid, 0) = SECURITY_MANDATORY_LOW_RID;
        Label.Label.Sid = Sid;
        Label.Label.Attributes = SE_GROUP_INTEGRITY;
        ok(SetTokenInformation(hDup, TokenIntegrityLevel, &Label, sizeof(Label) + GetLengthSid(Sid)),
           "SetTokenInformation(TokenIntegrityLevel) failed: %lu\n", GetLastError());
        ok(GetIntegrityLevel(hDup, &Rid, &Attributes) && Rid == SECURITY_MANDATORY_LOW_RID, "Lowered level 0x%lx\n", Rid);

        /* An integrity group, if the token lists one, follows the level */
        if (GetIntegrityGroup(hDup, &Rid))
            ok(Rid == SECURITY_MANDATORY_LOW_RID, "Integrity group 0x%lx after lowering\n", Rid);
        else
            trace("The token lists no integrity group\n");
    }

    TestDuplicateHandle(hToken);
    TestEventLabel();
    TestSaclView();
    TestLabelSet();
    TestLabelInheritance();

    CloseHandle(hDup);
    CloseHandle(hToken);
}
