/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Tests for the NtAccessCheck API
 * COPYRIGHT:       Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 */

#include "precomp.h"

static
HANDLE
GetToken(VOID)
{
    NTSTATUS Status;
    HANDLE Token;
    HANDLE DuplicatedToken;
    OBJECT_ATTRIBUTES ObjectAttributes;
    SECURITY_QUALITY_OF_SERVICE Sqos;

    Status = NtOpenProcessToken(NtCurrentProcess(),
                                TOKEN_QUERY | TOKEN_DUPLICATE,
                                &Token);
    if (!NT_SUCCESS(Status))
    {
        trace("Failed to get current process token (Status 0x%08lx)\n", Status);
        return NULL;
    }

    Sqos.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
    Sqos.ImpersonationLevel = SecurityImpersonation;
    Sqos.ContextTrackingMode = 0;
    Sqos.EffectiveOnly = FALSE;

    InitializeObjectAttributes(&ObjectAttributes,
                               NULL,
                               0,
                               NULL,
                               NULL);
    ObjectAttributes.SecurityQualityOfService = &Sqos;

    Status = NtDuplicateToken(Token,
                              TOKEN_QUERY | TOKEN_DUPLICATE,
                              &ObjectAttributes,
                              FALSE,
                              TokenImpersonation,
                              &DuplicatedToken);
    if (!NT_SUCCESS(Status))
    {
        trace("Failed to duplicate token (Status 0x%08lx)\n", Status);
        NtClose(Token);
        return NULL;
    }

    return DuplicatedToken;
}

/*
 * Runs one NtAccessCheck with MAXIMUM_ALLOWED against a valid impersonation
 * token and a descriptor that grants everyone full access. The caller
 * supplies the generic mapping and, optionally, the privilege set length
 * pointer. Returns FALSE when the setup failed and was skipped.
 */
static
BOOLEAN
RunAccessCheck(
    _In_ PGENERIC_MAPPING GenericMapping,
    _In_opt_ PULONG PrivilegeSetLengthPointer,
    _Out_ PNTSTATUS CallStatus,
    _Out_ PNTSTATUS AccessStatus,
    _Out_ PACCESS_MASK GrantedAccess)
{
    NTSTATUS Status;
    PPRIVILEGE_SET PrivilegeSet = NULL;
    ULONG PrivilegeSetLength;
    HANDLE Token = NULL;
    PACL Dacl = NULL;
    ULONG DaclSize;
    SECURITY_DESCRIPTOR Sd;
    PSID WorldSid = NULL;
    BOOLEAN SetupOk = FALSE;
    static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};

    *CallStatus = STATUS_UNSUCCESSFUL;
    *AccessStatus = STATUS_UNSUCCESSFUL;
    *GrantedAccess = 0;

    /* Allocate all the stuff we need */
    PrivilegeSetLength = FIELD_OFFSET(PRIVILEGE_SET, Privilege[16]);
    PrivilegeSet = RtlAllocateHeap(RtlGetProcessHeap(), 0, PrivilegeSetLength);
    if (PrivilegeSet == NULL)
    {
        skip("Failed to allocate PrivilegeSet, skipping tests\n");
        return FALSE;
    }

    Status = RtlAllocateAndInitializeSid(&WorldAuthority,
                                         1,
                                         SECURITY_WORLD_RID,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         &WorldSid);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to create World SID, skipping tests\n");
        goto Quit;
    }

    Token = GetToken();
    if (Token == NULL)
    {
        skip("Failed to get token, skipping tests\n");
        goto Quit;
    }

    Status = RtlCreateSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to create a security descriptor, skipping tests\n");
        goto Quit;
    }

    DaclSize = sizeof(ACL) +
               sizeof(ACCESS_ALLOWED_OBJECT_ACE) + RtlLengthSid(WorldSid);
    Dacl = RtlAllocateHeap(RtlGetProcessHeap(),
                           HEAP_ZERO_MEMORY,
                           DaclSize);
    if (Dacl == NULL)
    {
        skip("Failed to allocate memory for DACL, skipping tests\n");
        goto Quit;
    }

    /* Setup a ACL and give full access to everyone */
    Status = RtlCreateAcl(Dacl,
                          DaclSize,
                          ACL_REVISION);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to create DACL, skipping tests\n");
        goto Quit;
    }

    Status = RtlAddAccessAllowedAce(Dacl,
                                    ACL_REVISION,
                                    GENERIC_ALL,
                                    WorldSid);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to add allowed ACE for World SID, skipping tests\n");
        goto Quit;
    }

    /* Setup the descriptor */
    RtlSetGroupSecurityDescriptor(&Sd, WorldSid, FALSE);
    RtlSetOwnerSecurityDescriptor(&Sd, WorldSid, FALSE);
    RtlSetDaclSecurityDescriptor(&Sd, TRUE, Dacl, FALSE);

    /* Do the access check with the caller-supplied mapping */
    *CallStatus = NtAccessCheck(&Sd,
                                Token,
                                MAXIMUM_ALLOWED,
                                GenericMapping,
                                PrivilegeSet,
                                PrivilegeSetLengthPointer ? PrivilegeSetLengthPointer : &PrivilegeSetLength,
                                GrantedAccess,
                                AccessStatus);
    SetupOk = TRUE;

Quit:
    if (Dacl)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Dacl);
    }

    if (Token)
    {
        NtClose(Token);
    }

    if (WorldSid)
    {
        RtlFreeSid(WorldSid);
    }

    if (PrivilegeSet)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, PrivilegeSet);
    }

    return SetupOk;
}

static
VOID
AccessCheckEmptyMappingTest(VOID)
{
    NTSTATUS Status;
    NTSTATUS AccessStatus;
    ACCESS_MASK GrantedAccess;
    static GENERIC_MAPPING EmptyMapping = {0, 0, 0, 0};

    if (!RunAccessCheck(&EmptyMapping, NULL, &Status, &AccessStatus, &GrantedAccess))
        return;

    ok_hex(Status, STATUS_SUCCESS);
    ok(AccessStatus == STATUS_SUCCESS, "Expected a success status but got 0x%08lx\n", AccessStatus);
    trace("GrantedAccess == 0x%08lx\n", GrantedAccess);
}

/*
 * ProbeForRead does not touch the page, so a mapping on a no-access page
 * passes the probe; the kernel must capture it under SEH and fail the call.
 */
static
VOID
AccessCheckNoAccessMappingTest(VOID)
{
    NTSTATUS Status;
    NTSTATUS CallStatus;
    NTSTATUS AccessStatus;
    ACCESS_MASK GrantedAccess;
    PVOID BaseAddress = NULL;
    SIZE_T RegionSize = sizeof(GENERIC_MAPPING);

    /* Reserve and commit one page with no access at all */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &BaseAddress,
                                     0,
                                     &RegionSize,
                                     MEM_COMMIT | MEM_RESERVE,
                                     PAGE_NOACCESS);
    if (!NT_SUCCESS(Status) || BaseAddress == NULL)
    {
        skip("Failed to allocate a no-access page (Status 0x%08lx)\n", Status);
        return;
    }

    if (!RunAccessCheck((PGENERIC_MAPPING)BaseAddress, NULL, &CallStatus, &AccessStatus, &GrantedAccess))
        goto Quit;

    ok_hex(CallStatus, STATUS_ACCESS_VIOLATION);

Quit:
    RegionSize = 0;
    NtFreeVirtualMemory(NtCurrentProcess(),
                        &BaseAddress,
                        &RegionSize,
                        MEM_RELEASE);
}

/*
 * A zero privilege set length makes the kernel store the required length.
 * The length is only probed for read, so a read-only page must fail the
 * call instead of faulting the kernel.
 */
static
VOID
AccessCheckReadOnlyLengthTest(VOID)
{
    NTSTATUS Status;
    NTSTATUS CallStatus;
    NTSTATUS AccessStatus;
    ACCESS_MASK GrantedAccess;
    PVOID BaseAddress = NULL;
    PVOID ProtectAddress;
    SIZE_T RegionSize = PAGE_SIZE;
    ULONG OldProtect;
    static GENERIC_MAPPING Mapping = {STANDARD_RIGHTS_READ, STANDARD_RIGHTS_WRITE,
                                      STANDARD_RIGHTS_EXECUTE, STANDARD_RIGHTS_ALL};

    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &BaseAddress,
                                     0,
                                     &RegionSize,
                                     MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status) || BaseAddress == NULL)
    {
        skip("Failed to allocate a page (Status 0x%08lx)\n", Status);
        return;
    }

    *(PULONG)BaseAddress = 0;
    ProtectAddress = BaseAddress;
    RegionSize = PAGE_SIZE;
    Status = NtProtectVirtualMemory(NtCurrentProcess(),
                                    &ProtectAddress,
                                    &RegionSize,
                                    PAGE_READONLY,
                                    &OldProtect);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to make the page read-only (Status 0x%08lx)\n", Status);
        goto Quit;
    }

    if (!RunAccessCheck(&Mapping, (PULONG)BaseAddress, &CallStatus, &AccessStatus, &GrantedAccess))
        goto Quit;

    ok_hex(CallStatus, STATUS_ACCESS_VIOLATION);
    ok(*(PULONG)BaseAddress == 0, "The read-only length was changed to %lu\n", *(PULONG)BaseAddress);

Quit:
    RegionSize = 0;
    NtFreeVirtualMemory(NtCurrentProcess(),
                        &BaseAddress,
                        &RegionSize,
                        MEM_RELEASE);
}

START_TEST(NtAccessCheck)
{
    AccessCheckEmptyMappingTest();
    AccessCheckReadOnlyLengthTest();
    AccessCheckNoAccessMappingTest();
}
