/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for AddMandatoryAce and RtlAddMandatoryAce
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static BOOL (WINAPI *pAddMandatoryAce)(PACL, DWORD, DWORD, DWORD, PSID);
static NTSTATUS (NTAPI *pRtlAddMandatoryAce)(PACL, ULONG, ULONG, PSID, UCHAR, ULONG);

START_TEST(AddMandatoryAce)
{
    static SID LowLevel = { SID_REVISION, 1, { SECURITY_MANDATORY_LABEL_AUTHORITY },
                            { SECURITY_MANDATORY_LOW_RID } };
    static SID MediumLevel = { SID_REVISION, 1, { SECURITY_MANDATORY_LABEL_AUTHORITY },
                               { SECURITY_MANDATORY_MEDIUM_RID } };
    static SID BadSid = { 7, 1, { SECURITY_MANDATORY_LABEL_AUTHORITY },
                          { SECURITY_MANDATORY_LOW_RID } };
    HMODULE hAdvapi32 = GetModuleHandleW(L"advapi32.dll");
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    SYSTEM_MANDATORY_LABEL_ACE *Ace;
    char AclBuffer[256], SmallBuffer[sizeof(ACL) + 8];
    ACL *Acl = (ACL *)AclBuffer, *SmallAcl = (ACL *)SmallBuffer;
    NTSTATUS Status;
    BOOL Ret;

    /* A build that exports the Windows Vista level has GetTickCount64, one that does not has no AddMandatoryAce either */
    if (!GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64"))
    {
        skip("The Windows Vista exports are not enabled\n");
        return;
    }

    pAddMandatoryAce = (void *)GetProcAddress(hAdvapi32, "AddMandatoryAce");
    pRtlAddMandatoryAce = (void *)GetProcAddress(hNtdll, "RtlAddMandatoryAce");
    ok(pAddMandatoryAce != NULL, "AddMandatoryAce is missing\n");
    if (!pAddMandatoryAce)
        return;

    ok(InitializeAcl(Acl, sizeof(AclBuffer), ACL_REVISION), "InitializeAcl failed with %lu\n", GetLastError());

    /* Mandatory policy bits that do not exist */
    SetLastError(0xdeadbeef);
    Ret = pAddMandatoryAce(Acl, ACL_REVISION, 0, 0x1234, &LowLevel);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());
    ok(Acl->AceCount == 0, "AceCount = %u\n", Acl->AceCount);

    /* Ace flags that do not exist */
    SetLastError(0xdeadbeef);
    Ret = pAddMandatoryAce(Acl, ACL_REVISION, 0x100, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, &LowLevel);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());

    /* An invalid SID */
    SetLastError(0xdeadbeef);
    Ret = pAddMandatoryAce(Acl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, &BadSid);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INVALID_SID, "GetLastError() = %lu\n", GetLastError());

    /* An ACL that is too small */
    ok(InitializeAcl(SmallAcl, sizeof(SmallBuffer), ACL_REVISION), "InitializeAcl failed with %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pAddMandatoryAce(SmallAcl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, &LowLevel);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_ALLOTTED_SPACE_EXCEEDED, "GetLastError() = %lu\n", GetLastError());

    /* A label */
    Ret = pAddMandatoryAce(Acl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, &LowLevel);
    ok(Ret == TRUE, "Ret = %d, GetLastError() = %lu\n", Ret, GetLastError());
    ok(Acl->AceCount == 1, "AceCount = %u\n", Acl->AceCount);
    Ret = GetAce(Acl, 0, (void **)&Ace);
    ok(Ret, "GetAce failed with %lu\n", GetLastError());
    if (Ret)
    {
        ok(Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE, "AceType = 0x%x\n", Ace->Header.AceType);
        ok(Ace->Header.AceFlags == 0, "AceFlags = 0x%x\n", Ace->Header.AceFlags);
        ok(Ace->Header.AceSize == sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + GetLengthSid(&LowLevel),
           "AceSize = %u\n", Ace->Header.AceSize);
        ok(Ace->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, "Mask = 0x%lx\n", Ace->Mask);
        ok(EqualSid(&Ace->SidStart, &LowLevel), "wrong SID\n");
    }

    /* A second label, with ace flags and all the policy bits */
    Ret = pAddMandatoryAce(Acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                           SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP |
                           SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP, &MediumLevel);
    ok(Ret == TRUE, "Ret = %d, GetLastError() = %lu\n", Ret, GetLastError());
    ok(Acl->AceCount == 2, "AceCount = %u\n", Acl->AceCount);
    Ret = GetAce(Acl, 1, (void **)&Ace);
    ok(Ret, "GetAce failed with %lu\n", GetLastError());
    if (Ret)
    {
        ok(Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE, "AceType = 0x%x\n", Ace->Header.AceType);
        ok(Ace->Header.AceFlags == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE), "AceFlags = 0x%x\n", Ace->Header.AceFlags);
        ok(Ace->Mask == 7, "Mask = 0x%lx\n", Ace->Mask);
        ok(EqualSid(&Ace->SidStart, &MediumLevel), "wrong SID\n");
    }

    SetLastError(0xdeadbeef);
    Ret = GetAce(Acl, 2, (void **)&Ace);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());

    /* The ACL takes the higher of the two revisions */
    ok(InitializeAcl(Acl, sizeof(AclBuffer), ACL_REVISION), "InitializeAcl failed with %lu\n", GetLastError());
    Ret = pAddMandatoryAce(Acl, ACL_REVISION3, 0, SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP, &MediumLevel);
    ok(Ret == TRUE, "Ret = %d, GetLastError() = %lu\n", Ret, GetLastError());
    ok(Acl->AclRevision == ACL_REVISION3, "AclRevision = %u\n", Acl->AclRevision);

    /* A revision that is too new */
    SetLastError(0xdeadbeef);
    Ret = pAddMandatoryAce(Acl, 5, 0, SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP, &MediumLevel);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_REVISION_MISMATCH, "GetLastError() = %lu\n", GetLastError());

    /* The native routine takes the SID before the ACE type and the policy; Windows 11 no longer has it */
    if (!pRtlAddMandatoryAce)
    {
        skip("RtlAddMandatoryAce is not exported\n");
        return;
    }
    ok(InitializeAcl(Acl, sizeof(AclBuffer), ACL_REVISION), "InitializeAcl failed with %lu\n", GetLastError());
    Status = pRtlAddMandatoryAce(Acl, ACL_REVISION, 0, &LowLevel, SYSTEM_MANDATORY_LABEL_ACE_TYPE,
                                 SYSTEM_MANDATORY_LABEL_NO_WRITE_UP);
    ok(Status == STATUS_SUCCESS, "Status = 0x%lx\n", Status);
    ok(Acl->AceCount == 1, "AceCount = %u\n", Acl->AceCount);
    Status = pRtlAddMandatoryAce(Acl, ACL_REVISION, 0, &LowLevel, SYSTEM_AUDIT_ACE_TYPE,
                                 SYSTEM_MANDATORY_LABEL_NO_WRITE_UP);
    ok(Status == STATUS_INVALID_PARAMETER, "Status = 0x%lx\n", Status);
    Status = pRtlAddMandatoryAce(Acl, ACL_REVISION, 0, &LowLevel, SYSTEM_MANDATORY_LABEL_ACE_TYPE, 0x8);
    ok(Status == STATUS_INVALID_PARAMETER, "Status = 0x%lx\n", Status);
    Status = pRtlAddMandatoryAce(Acl, ACL_REVISION, 0, &BadSid, SYSTEM_MANDATORY_LABEL_ACE_TYPE,
                                 SYSTEM_MANDATORY_LABEL_NO_WRITE_UP);
    ok(Status == STATUS_INVALID_SID, "Status = 0x%lx\n", Status);
    ok(Acl->AceCount == 1, "AceCount = %u\n", Acl->AceCount);
}
