/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for NtAccessCheckByTypeResultListAndAuditAlarmByHandle,
 *              the system service with 17 arguments
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

NTSYSAPI
NTSTATUS
NTAPI
NtAccessCheckByTypeResultListAndAuditAlarmByHandle(
    _In_ PUNICODE_STRING SubsystemName,
    _In_opt_ PVOID HandleId,
    _In_ HANDLE ClientToken,
    _In_ PUNICODE_STRING ObjectTypeName,
    _In_ PUNICODE_STRING ObjectName,
    _In_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _In_opt_ PSID PrincipalSelfSid,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ AUDIT_EVENT_TYPE AuditType,
    _In_ ULONG Flags,
    _In_reads_opt_(ObjectTypeListLength) POBJECT_TYPE_LIST ObjectTypeList,
    _In_ ULONG ObjectTypeListLength,
    _In_ PGENERIC_MAPPING GenericMapping,
    _In_ BOOLEAN ObjectCreation,
    _Out_writes_(ObjectTypeListLength) PACCESS_MASK GrantedAccessList,
    _Out_writes_(ObjectTypeListLength) PNTSTATUS AccessStatusList,
    _Out_ PBOOLEAN GenerateOnClose);

static GENERIC_MAPPING KeyMapping = {KEY_READ, KEY_WRITE, KEY_EXECUTE, KEY_ALL_ACCESS};
static GUID ObjectType = {0x3a1c6f0e, 0x52d4, 0x4c1b, {0x9e, 0x27, 0x61, 0x0b, 0x8d, 0x44, 0xa3, 0x15}};
static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};

static
HANDLE
GetImpersonationToken(VOID)
{
    NTSTATUS Status;
    HANDLE Token, Duplicate;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SECURITY_QUALITY_OF_SERVICE Sqos;

    Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_DUPLICATE, &Token);
    if (!NT_SUCCESS(Status))
    {
        trace("NtOpenProcessToken failed: 0x%08lx\n", Status);
        return NULL;
    }

    Sqos.Length = sizeof(Sqos);
    Sqos.ImpersonationLevel = SecurityImpersonation;
    Sqos.ContextTrackingMode = SECURITY_STATIC_TRACKING;
    Sqos.EffectiveOnly = FALSE;
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    ObjectAttributes.SecurityQualityOfService = &Sqos;

    Status = NtDuplicateToken(Token,
                              TOKEN_QUERY,
                              &ObjectAttributes,
                              FALSE,
                              TokenImpersonation,
                              &Duplicate);
    NtClose(Token);
    if (!NT_SUCCESS(Status))
    {
        trace("NtDuplicateToken failed: 0x%08lx\n", Status);
        return NULL;
    }
    return Duplicate;
}

START_TEST(NtAccessCheckByTypeResultListAndAuditAlarmByHandle)
{
    NTSTATUS Status;
    HANDLE Token;
    PSID EveryoneSid = NULL;
    SECURITY_DESCRIPTOR Sd;
    UNICODE_STRING SubsystemName, ObjectTypeName, ObjectName;
    OBJECT_TYPE_LIST ObjectTypeList[1];
    ACCESS_MASK GrantedAccess[1];
    NTSTATUS AccessStatus[1];
    BOOLEAN GenerateOnClose;

    Token = GetImpersonationToken();
    if (Token == NULL)
    {
        skip("No impersonation token\n");
        return;
    }

    Status = RtlAllocateAndInitializeSid(&WorldAuthority, 1, SECURITY_WORLD_RID,
                                         0, 0, 0, 0, 0, 0, 0, &EveryoneSid);
    if (!NT_SUCCESS(Status))
    {
        skip("RtlAllocateAndInitializeSid failed: 0x%08lx\n", Status);
        NtClose(Token);
        return;
    }

    /* An owner and a group are required; a NULL DACL grants every access */
    RtlCreateSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&Sd, EveryoneSid, FALSE);
    RtlSetGroupSecurityDescriptor(&Sd, EveryoneSid, FALSE);
    RtlSetDaclSecurityDescriptor(&Sd, TRUE, NULL, FALSE);

    RtlInitUnicodeString(&SubsystemName, L"ntdll_apitest");
    RtlInitUnicodeString(&ObjectTypeName, L"Key");
    RtlInitUnicodeString(&ObjectName, L"AuditAlarmByHandle");

    ObjectTypeList[0].Level = ACCESS_OBJECT_GUID;
    ObjectTypeList[0].Sbz = 0;
    ObjectTypeList[0].ObjectType = &ObjectType;

    /*
     * Every argument is valid. AUDIT_ALLOW_NO_PRIVILEGE lets a caller
     * without SeAuditPrivilege do the access check without an audit, so
     * the service fills the result lists (arguments 15 and 16) and
     * GenerateOnClose (argument 17).
     */
    GrantedAccess[0] = 0xdeadbeef;
    AccessStatus[0] = 0xdeadbeef;
    GenerateOnClose = 0x55;
    Status = NtAccessCheckByTypeResultListAndAuditAlarmByHandle(&SubsystemName,
                                                                (PVOID)1,
                                                                Token,
                                                                &ObjectTypeName,
                                                                &ObjectName,
                                                                &Sd,
                                                                NULL,
                                                                KEY_QUERY_VALUE,
                                                                AuditEventObjectAccess,
                                                                AUDIT_ALLOW_NO_PRIVILEGE,
                                                                ObjectTypeList,
                                                                RTL_NUMBER_OF(ObjectTypeList),
                                                                &KeyMapping,
                                                                FALSE,
                                                                GrantedAccess,
                                                                AccessStatus,
                                                                &GenerateOnClose);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(GrantedAccess[0], KEY_QUERY_VALUE);
    ok_hex(AccessStatus[0], STATUS_SUCCESS);
    ok(GenerateOnClose == FALSE, "GenerateOnClose = 0x%x, expected FALSE\n", GenerateOnClose);

    /* The same call with an invalid 17th argument */
    Status = NtAccessCheckByTypeResultListAndAuditAlarmByHandle(&SubsystemName,
                                                                (PVOID)1,
                                                                Token,
                                                                &ObjectTypeName,
                                                                &ObjectName,
                                                                &Sd,
                                                                NULL,
                                                                KEY_QUERY_VALUE,
                                                                AuditEventObjectAccess,
                                                                AUDIT_ALLOW_NO_PRIVILEGE,
                                                                ObjectTypeList,
                                                                RTL_NUMBER_OF(ObjectTypeList),
                                                                &KeyMapping,
                                                                FALSE,
                                                                GrantedAccess,
                                                                AccessStatus,
                                                                (PBOOLEAN)(ULONG_PTR)1);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    RtlFreeSid(EveryoneSid);
    NtClose(Token);
}
