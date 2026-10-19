/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test for automatic inheritance in SeAssignSecurityEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include "se.h"

static GENERIC_MAPPING GenericMapping =
{
    STANDARD_RIGHTS_READ    | 0x1001,
    STANDARD_RIGHTS_WRITE   | 0x2002,
    STANDARD_RIGHTS_EXECUTE | 0x4004,
    STANDARD_RIGHTS_ALL     | 0x800F,
};

#define ACL_BUFFER_SIZE 256
#define AUTO_INHERIT_CONTROL (SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED)

static
PSECURITY_DESCRIPTOR
AssignSecurity(
    _In_opt_ PSECURITY_DESCRIPTOR Parent,
    _In_opt_ PSECURITY_DESCRIPTOR Creator,
    _In_ BOOLEAN IsDirectory,
    _In_ ULONG AutoInheritFlags,
    _In_ PSECURITY_SUBJECT_CONTEXT SubjectContext,
    _Out_ PSECURITY_DESCRIPTOR_CONTROL Control,
    _Out_ PACL *Dacl,
    _Out_ PACL *Sacl)
{
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR Descriptor = NULL;
    BOOLEAN Present, Defaulted;

    *Control = 0;
    *Dacl = NULL;
    *Sacl = NULL;
    Status = SeAssignSecurityEx(Parent,
                                Creator,
                                &Descriptor,
                                NULL,
                                IsDirectory,
                                AutoInheritFlags,
                                SubjectContext,
                                &GenericMapping,
                                PagedPool);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;
    *Control = ((PISECURITY_DESCRIPTOR)Descriptor)->Control;
    Status = RtlGetDaclSecurityDescriptor(Descriptor, &Present, Dacl, &Defaulted);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !Present)
        *Dacl = NULL;
    Status = RtlGetSaclSecurityDescriptor(Descriptor, &Present, Sacl, &Defaulted);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !Present)
        *Sacl = NULL;
    return Descriptor;
}

static
VOID
TestAutoInherit(
    _In_ PSECURITY_SUBJECT_CONTEXT SubjectContext)
{
    NTSTATUS Status;
    SECURITY_DESCRIPTOR Parent;
    SECURITY_DESCRIPTOR Creator;
    ULONG ParentDaclBuffer[ACL_BUFFER_SIZE / sizeof(ULONG)];
    ULONG ParentSaclBuffer[ACL_BUFFER_SIZE / sizeof(ULONG)];
    ULONG CreatorDaclBuffer[ACL_BUFFER_SIZE / sizeof(ULONG)];
    PACL ParentDacl = (PACL)ParentDaclBuffer;
    PACL ParentSacl = (PACL)ParentSaclBuffer;
    PACL CreatorDacl = (PACL)CreatorDaclBuffer;
    PSECURITY_DESCRIPTOR Descriptor;
    SECURITY_DESCRIPTOR_CONTROL Control;
    PACL Dacl, Sacl;
    BOOLEAN IsDir;
    UCHAR InheritFlags;

    /* Parent: an inheritable World ACE and a non-inheritable System ACE */
    Status = RtlCreateAcl(ParentDacl, ACL_BUFFER_SIZE, ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAceEx(ParentDacl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, READ_CONTROL, SeExports->SeWorldSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAceEx(ParentDacl, ACL_REVISION, 0, SYNCHRONIZE, SeExports->SeLocalSystemSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlCreateAcl(ParentSacl, ACL_BUFFER_SIZE, ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlxAddAuditAccessAceEx(ParentSacl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, READ_CONTROL, SeExports->SeWorldSid, TRUE, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = RtlCreateSecurityDescriptor(&Parent, SECURITY_DESCRIPTOR_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlSetDaclSecurityDescriptor(&Parent, TRUE, ParentDacl, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlSetSaclSecurityDescriptor(&Parent, TRUE, ParentSacl, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Parent.Control |= SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED;

    /* Creator: one explicit Administrators ACE */
    Status = RtlCreateAcl(CreatorDacl, ACL_BUFFER_SIZE, ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAceEx(CreatorDacl, ACL_REVISION, 0, SYNCHRONIZE, SeExports->SeAliasAdminsSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlCreateSecurityDescriptor(&Creator, SECURITY_DESCRIPTOR_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlSetDaclSecurityDescriptor(&Creator, TRUE, CreatorDacl, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);

    for (IsDir = FALSE; IsDir <= TRUE; IsDir++)
    {
        InheritFlags = IsDir ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0;

        /* Plain inheritance marks nothing */
        Descriptor = AssignSecurity(&Parent, NULL, IsDir, 0, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, 0);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, InheritFlags, SeExports->SeWorldSid, READ_CONTROL);
            CheckAcl(Sacl, 1, SYSTEM_AUDIT_ACE_TYPE, InheritFlags | SUCCESSFUL_ACCESS_ACE_FLAG, SeExports->SeWorldSid, READ_CONTROL);
            SeDeassignSecurity(&Descriptor);
        }

        /* Auto-inheritance marks the inherited ACEs and the descriptor */
        Descriptor = AssignSecurity(&Parent, NULL, IsDir, SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, InheritFlags | INHERITED_ACE, SeExports->SeWorldSid, READ_CONTROL);
            CheckAcl(Sacl, 1, SYSTEM_AUDIT_ACE_TYPE, InheritFlags | INHERITED_ACE | SUCCESSFUL_ACCESS_ACE_FLAG, SeExports->SeWorldSid, READ_CONTROL);
            SeDeassignSecurity(&Descriptor);
        }

        /* Each flag only affects its own ACL */
        Descriptor = AssignSecurity(&Parent, NULL, IsDir, SEF_DACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, SE_DACL_AUTO_INHERITED);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, InheritFlags | INHERITED_ACE, SeExports->SeWorldSid, READ_CONTROL);
            CheckAcl(Sacl, 1, SYSTEM_AUDIT_ACE_TYPE, InheritFlags | SUCCESSFUL_ACCESS_ACE_FLAG, SeExports->SeWorldSid, READ_CONTROL);
            SeDeassignSecurity(&Descriptor);
        }

        /* Plain inheritance keeps an explicit DACL as it is */
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, 0, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, 0);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, SeExports->SeAliasAdminsSid, SYNCHRONIZE);
            SeDeassignSecurity(&Descriptor);
        }

        /* Auto-inheritance adds the inherited ACEs after the explicit ones */
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED);
            CheckAcl(Dacl, 2, ACCESS_ALLOWED_ACE_TYPE, 0, SeExports->SeAliasAdminsSid, SYNCHRONIZE,
                              ACCESS_ALLOWED_ACE_TYPE, InheritFlags | INHERITED_ACE, SeExports->SeWorldSid, READ_CONTROL);
            SeDeassignSecurity(&Descriptor);
        }

        /* A protected explicit DACL inherits nothing and stays protected */
        Creator.Control |= SE_DACL_PROTECTED;
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & (SE_DACL_PROTECTED | AUTO_INHERIT_CONTROL), SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, SeExports->SeAliasAdminsSid, SYNCHRONIZE);
            SeDeassignSecurity(&Descriptor);
        }
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, 0, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & (SE_DACL_PROTECTED | AUTO_INHERIT_CONTROL), SE_DACL_PROTECTED);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, SeExports->SeAliasAdminsSid, SYNCHRONIZE);
            SeDeassignSecurity(&Descriptor);
        }
        Creator.Control &= ~SE_DACL_PROTECTED;

        /* Auto-inheritance drops explicit ACEs that claim to be inherited */
        ((PACE_HEADER)(CreatorDacl + 1))->AceFlags = INHERITED_ACE;
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, SEF_DACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, InheritFlags | INHERITED_ACE, SeExports->SeWorldSid, READ_CONTROL);
            SeDeassignSecurity(&Descriptor);
        }
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, 0, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE, SeExports->SeAliasAdminsSid, SYNCHRONIZE);
            SeDeassignSecurity(&Descriptor);
        }
        ((PACE_HEADER)(CreatorDacl + 1))->AceFlags = 0;

        /* A defaulted explicit DACL gives way to the inherited ACEs */
        Creator.Control |= SE_DACL_DEFAULTED;
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, SEF_DACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, SE_DACL_AUTO_INHERITED);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, InheritFlags | INHERITED_ACE, SeExports->SeWorldSid, READ_CONTROL);
            SeDeassignSecurity(&Descriptor);
        }
        Creator.Control &= ~SE_DACL_DEFAULTED;

        /* The parent need not be auto-inherited itself */
        Parent.Control &= ~(SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED);
        Descriptor = AssignSecurity(&Parent, &Creator, IsDir, SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED);
            CheckAcl(Dacl, 2, ACCESS_ALLOWED_ACE_TYPE, 0, SeExports->SeAliasAdminsSid, SYNCHRONIZE,
                              ACCESS_ALLOWED_ACE_TYPE, InheritFlags | INHERITED_ACE, SeExports->SeWorldSid, READ_CONTROL);
            CheckAcl(Sacl, 1, SYSTEM_AUDIT_ACE_TYPE, InheritFlags | INHERITED_ACE | SUCCESSFUL_ACCESS_ACE_FLAG, SeExports->SeWorldSid, READ_CONTROL);
            SeDeassignSecurity(&Descriptor);
        }
        Parent.Control |= SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED;

        /* The flags mark the descriptor even when nothing is inherited */
        Descriptor = AssignSecurity(NULL, &Creator, IsDir, SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (Descriptor)
        {
            ok_eq_hex(Control & AUTO_INHERIT_CONTROL, SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED);
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, 0, SeExports->SeAliasAdminsSid, SYNCHRONIZE);
            ok(Sacl == NULL, "Sacl = %p\n", Sacl);
            SeDeassignSecurity(&Descriptor);
        }
    }
}

static
VOID
AppendObjectAce(
    _Inout_ PACL Acl,
    _In_ UCHAR AceFlags,
    _In_ ACCESS_MASK Mask,
    _In_ PSID Sid)
{
    PACE_HEADER Header = (PACE_HEADER)(Acl + 1);
    PACCESS_ALLOWED_OBJECT_ACE Ace;
    ULONG i;

    for (i = 0; i < Acl->AceCount; i++)
        Header = (PACE_HEADER)((PUCHAR)Header + Header->AceSize);

    /* An object ACE without GUIDs: the SID takes the place of ObjectType */
    Ace = (PACCESS_ALLOWED_OBJECT_ACE)Header;
    Ace->Header.AceType = ACCESS_ALLOWED_OBJECT_ACE_TYPE;
    Ace->Header.AceFlags = AceFlags;
    Ace->Header.AceSize = (USHORT)(FIELD_OFFSET(ACCESS_ALLOWED_OBJECT_ACE, ObjectType) + RtlLengthSid(Sid));
    Ace->Mask = Mask;
    Ace->Flags = 0;
    RtlCopySid(RtlLengthSid(Sid), &Ace->ObjectType, Sid);
    Acl->AclRevision = ACL_REVISION_DS;
    Acl->AceCount++;
}

static
VOID
TestAutoInheritEdges(
    _In_ PSECURITY_SUBJECT_CONTEXT SubjectContext)
{
    NTSTATUS Status;
    SECURITY_DESCRIPTOR Parent;
    SECURITY_DESCRIPTOR Creator;
    ULONG ParentDaclBuffer[ACL_BUFFER_SIZE / sizeof(ULONG)];
    ULONG CreatorDaclBuffer[ACL_BUFFER_SIZE / sizeof(ULONG)];
    PACL ParentDacl = (PACL)ParentDaclBuffer;
    PACL CreatorDacl = (PACL)CreatorDaclBuffer;
    PACL BigParentDacl, BigCreatorDacl;
    PSECURITY_DESCRIPTOR Descriptor;
    SECURITY_DESCRIPTOR_CONTROL Control;
    PACL Dacl, Sacl;
    BOOLEAN IsDir;
    ULONG i, BigSize;

    /* A generic inheritable ACE is split for a directory; both parts are marked */
    Status = RtlCreateAcl(ParentDacl, ACL_BUFFER_SIZE, ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAceEx(ParentDacl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, GENERIC_READ, SeExports->SeWorldSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlCreateSecurityDescriptor(&Parent, SECURITY_DESCRIPTOR_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlSetDaclSecurityDescriptor(&Parent, TRUE, ParentDacl, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    for (IsDir = FALSE; IsDir <= TRUE; IsDir++)
    {
        Descriptor = AssignSecurity(&Parent, NULL, IsDir, SEF_DACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
        if (!Descriptor)
            continue;
        if (IsDir)
            CheckAcl(Dacl, 2, ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE, SeExports->SeWorldSid, STANDARD_RIGHTS_READ | 0x0001,
                              ACCESS_ALLOWED_ACE_TYPE, INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERITED_ACE, SeExports->SeWorldSid, GENERIC_READ);
        else
            CheckAcl(Dacl, 1, ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE, SeExports->SeWorldSid, STANDARD_RIGHTS_READ | 0x0001);
        SeDeassignSecurity(&Descriptor);
    }

    /* Object ACEs follow the inheritance flags too */
    Status = RtlCreateAcl(ParentDacl, ACL_BUFFER_SIZE, ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAceEx(ParentDacl, ACL_REVISION, OBJECT_INHERIT_ACE, READ_CONTROL, SeExports->SeWorldSid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    AppendObjectAce(ParentDacl, 0, FILE_WRITE_DATA, SeExports->SeWorldSid);
    Status = RtlSetDaclSecurityDescriptor(&Parent, TRUE, ParentDacl, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Descriptor = AssignSecurity(&Parent, NULL, FALSE, SEF_DACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
    if (Descriptor)
    {
        /* The inherited ACL keeps the parent's revision */
        ok(Dacl != NULL, "No DACL\n");
        if (Dacl)
        {
            ok_eq_uint(Dacl->AclRevision, ACL_REVISION_DS);
            ok_eq_uint(Dacl->AceCount, 1);
        }
        if (Dacl && Dacl->AceCount == 1)
        {
            PACCESS_ALLOWED_ACE Ace = (PACCESS_ALLOWED_ACE)(Dacl + 1);

            ok_eq_uint(Ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
            ok_eq_hex(Ace->Header.AceFlags, INHERITED_ACE);
            ok_eq_hex(Ace->Mask, READ_CONTROL);
            ok(RtlEqualSid(&Ace->SidStart, SeExports->SeWorldSid), "Wrong SID\n");
        }
        SeDeassignSecurity(&Descriptor);
    }

    Status = RtlCreateAcl(ParentDacl, ACL_BUFFER_SIZE, ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    AppendObjectAce(ParentDacl, OBJECT_INHERIT_ACE, FILE_WRITE_DATA, SeExports->SeWorldSid);
    Status = RtlSetDaclSecurityDescriptor(&Parent, TRUE, ParentDacl, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlCreateAcl(CreatorDacl, ACL_BUFFER_SIZE, ACL_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlCreateSecurityDescriptor(&Creator, SECURITY_DESCRIPTOR_REVISION);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = RtlSetDaclSecurityDescriptor(&Creator, TRUE, CreatorDacl, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Descriptor = AssignSecurity(&Parent, &Creator, FALSE, SEF_DACL_AUTO_INHERIT, SubjectContext, &Control, &Dacl, &Sacl);
    if (Descriptor)
    {
        ok(Dacl != NULL, "No DACL\n");
        if (Dacl)
        {
            ok_eq_uint(Dacl->AclRevision, ACL_REVISION_DS);
            ok_eq_uint(Dacl->AceCount, 1);
        }
        if (Dacl && Dacl->AceCount == 1)
        {
            PACCESS_ALLOWED_OBJECT_ACE Ace = (PACCESS_ALLOWED_OBJECT_ACE)(Dacl + 1);

            ok_eq_uint(Ace->Header.AceType, ACCESS_ALLOWED_OBJECT_ACE_TYPE);
            ok_eq_hex(Ace->Header.AceFlags, INHERITED_ACE);
            ok_eq_hex(Ace->Mask, FILE_WRITE_DATA);
            ok(RtlEqualSid(&Ace->ObjectType, SeExports->SeWorldSid), "Wrong SID\n");
        }
        SeDeassignSecurity(&Descriptor);
    }

    /* Explicit and inherited ACEs together must still fit in one ACL */
    BigSize = sizeof(ACL) + 2000 * (FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(SeExports->SeWorldSid));
    BigParentDacl = ExAllocatePoolWithTag(PagedPool, BigSize, 'ASmK');
    BigCreatorDacl = ExAllocatePoolWithTag(PagedPool, BigSize, 'ASmK');
    if (!skip(BigParentDacl && BigCreatorDacl, "Out of memory\n"))
    {
        Status = RtlCreateAcl(BigParentDacl, BigSize, ACL_REVISION);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = RtlCreateAcl(BigCreatorDacl, BigSize, ACL_REVISION);
        ok_eq_hex(Status, STATUS_SUCCESS);
        for (i = 0; i < 2000; i++)
        {
            Status = RtlAddAccessAllowedAceEx(BigParentDacl, ACL_REVISION, OBJECT_INHERIT_ACE, READ_CONTROL, SeExports->SeWorldSid);
            if (!NT_SUCCESS(Status))
                break;
            Status = RtlAddAccessAllowedAceEx(BigCreatorDacl, ACL_REVISION, 0, SYNCHRONIZE, SeExports->SeWorldSid);
            if (!NT_SUCCESS(Status))
                break;
        }
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = RtlSetDaclSecurityDescriptor(&Parent, TRUE, BigParentDacl, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = RtlSetDaclSecurityDescriptor(&Creator, TRUE, BigCreatorDacl, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Descriptor = NULL;
        Status = SeAssignSecurityEx(&Parent, &Creator, &Descriptor, NULL, FALSE, SEF_DACL_AUTO_INHERIT,
                                    SubjectContext, &GenericMapping, PagedPool);
        ok_eq_hex(Status, STATUS_BAD_INHERITANCE_ACL);
        if (NT_SUCCESS(Status))
        {
            ok(RtlValidSecurityDescriptor(Descriptor), "Invalid descriptor\n");
            SeDeassignSecurity(&Descriptor);
        }
    }
    if (BigParentDacl)
        ExFreePoolWithTag(BigParentDacl, 'ASmK');
    if (BigCreatorDacl)
        ExFreePoolWithTag(BigCreatorDacl, 'ASmK');
}

START_TEST(SeAutoInherit)
{
    SECURITY_SUBJECT_CONTEXT SubjectContext;

    SeCaptureSubjectContext(&SubjectContext);
    TestAutoInherit(&SubjectContext);
    TestAutoInheritEdges(&SubjectContext);
    SeReleaseSubjectContext(&SubjectContext);
}
