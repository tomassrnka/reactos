/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the integrity level of tokens and the mandatory label of token objects
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

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

/* The SACL shows the same label, when this process may read SACLs */
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
        ok(GetKernelObjectSecurity(hToken, SACL_SECURITY_INFORMATION, Sd, sizeof(Buffer), &Length),
           "GetKernelObjectSecurity(SACL) failed: %lu\n", GetLastError());
        if (GetSecurityDescriptorSacl(Sd, &Present, &Sacl, &Defaulted) && Present && Sacl)
        {
            for (i = 0; i < Sacl->AceCount; i++)
            {
                if (GetAce(Sacl, i, (PVOID *)&Ace) && Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                {
                    Found = TRUE;
                    ok(*GetSidSubAuthority((PSID)&Ace->SidStart, 0) == LabelRid, "SACL label differs from the LABEL view\n");
                    ok(Ace->Mask == Mask, "SACL label policy 0x%lx, LABEL view 0x%lx\n", Ace->Mask, Mask);
                }
            }
        }
        ok(Found, "The SACL has no label\n");
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

    CloseHandle(hDup);
    CloseHandle(hToken);
}
