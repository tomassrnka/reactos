/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtDuplicateObject
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 *              Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define OBJ_PROTECT_CLOSE 0x01

/*
 * Widening-duplicate contract (T127b).
 *
 * DuplicateHandle/NtDuplicateObject asking for MORE access than the source
 * handle holds re-evaluates the object's security descriptor and grants the
 * extra rights iff the SD grants them -- exactly as Windows (validated on
 * Win10 and Server 2008 R2). This is the Windows-correct behaviour that the
 * earlier blanket user-mode deny (T100) got wrong:
 *
 *   - Default-SD object (no caller SD): the widen SUCCEEDS. A reduced handle
 *     on an object the caller owns can be re-widened, because the (default or
 *     absent) SD grants it. Denying this broke 24 legitimate cases.
 *
 *   - Restrictive-SD object (caller supplied a DACL that does NOT grant the
 *     wider right): the widen is DENIED, because SeAccessCheck re-evaluates
 *     the SD and the DACL refuses it. This is what blocks a read-only
 *     shared-memory section from being re-duplicated writable
 *     (Chromium/Electron read-only SHM validation).
 */

static PSID
GetCurrentUserSid(PBYTE Buffer, ULONG BufferSize)
{
    NTSTATUS Status;
    HANDLE Token = NULL;
    ULONG RetLen = 0;
    PTOKEN_USER UserInfo;

    Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &Token);
    if (!NT_SUCCESS(Status))
    {
        trace("NtOpenProcessToken failed 0x%lx\n", Status);
        return NULL;
    }

    Status = NtQueryInformationToken(Token, TokenUser, Buffer, BufferSize, &RetLen);
    NtClose(Token);
    if (!NT_SUCCESS(Status))
    {
        trace("NtQueryInformationToken(TokenUser) failed 0x%lx\n", Status);
        return NULL;
    }

    UserInfo = (PTOKEN_USER)Buffer;
    return UserInfo->User.Sid;
}

/* Create an anonymous (unnamed) section with an optional explicit SD. */
static NTSTATUS
CreateTestSection(PHANDLE SectionHandle, PSECURITY_DESCRIPTOR Sd)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    LARGE_INTEGER MaxSize;

    MaxSize.QuadPart = 0x1000;
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, Sd);

    return NtCreateSection(SectionHandle,
                           SECTION_ALL_ACCESS,
                           &ObjectAttributes,
                           &MaxSize,
                           PAGE_READWRITE,
                           SEC_COMMIT,
                           NULL);
}

static void
TestDefaultSdWidening(void)
{
    NTSTATUS Status;
    HANDLE hSection = NULL, hReadOnly = NULL, hWidened = NULL;
    OBJECT_BASIC_INFORMATION obi;

    /* Anonymous section, no caller SD -> default/absent security. */
    Status = CreateTestSection(&hSection, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Reduce to a read-only handle. */
    Status = NtDuplicateObject(NtCurrentProcess(), hSection, NtCurrentProcess(),
                               &hReadOnly, SECTION_MAP_READ, 0, 0);
    ok_hex(Status, STATUS_SUCCESS);

    /* Widen the read-only handle back to write: default SD -> SUCCEEDS. */
    hWidened = NULL;
    Status = NtDuplicateObject(NtCurrentProcess(), hReadOnly, NtCurrentProcess(),
                               &hWidened, SECTION_MAP_WRITE, 0, 0);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(&obi, sizeof(obi));
        Status = NtQueryObject(hWidened, ObjectBasicInformation, &obi, sizeof(obi), NULL);
        ok_hex(Status, STATUS_SUCCESS);
        ok((obi.GrantedAccess & SECTION_MAP_WRITE) != 0,
           "default-SD widen did not grant SECTION_MAP_WRITE, got 0x%lx\n",
           obi.GrantedAccess);
        NtClose(hWidened);
    }

    NtClose(hReadOnly);
    NtClose(hSection);
}

static void
TestRestrictiveSdWidening(void)
{
    NTSTATUS Status;
    HANDLE hSection = NULL, hReadOnly = NULL, hWidened = NULL;
    BYTE SidBuffer[256];
    BYTE DaclBuffer[256];
    PSID UserSid;
    PACL Dacl = (PACL)DaclBuffer;
    SECURITY_DESCRIPTOR Sd;

    UserSid = GetCurrentUserSid(SidBuffer, sizeof(SidBuffer));
    if (UserSid == NULL)
    {
        skip("Could not obtain current user SID\n");
        return;
    }

    /* DACL that grants the caller READ only -- deliberately no SECTION_MAP_WRITE. */
    Status = RtlCreateAcl(Dacl, sizeof(DaclBuffer), ACL_REVISION);
    ok_hex(Status, STATUS_SUCCESS);
    Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION,
                                    SECTION_MAP_READ | READ_CONTROL, UserSid);
    ok_hex(Status, STATUS_SUCCESS);

    Status = RtlCreateSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    ok_hex(Status, STATUS_SUCCESS);
    Status = RtlSetDaclSecurityDescriptor(&Sd, TRUE, Dacl, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    Status = RtlSetOwnerSecurityDescriptor(&Sd, UserSid, FALSE);
    ok_hex(Status, STATUS_SUCCESS);

    /* Create is not access-checked: the creator still gets SECTION_ALL_ACCESS. */
    Status = CreateTestSection(&hSection, &Sd);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Reduce to a read-only handle (the DACL grants read, so this is fine). */
    Status = NtDuplicateObject(NtCurrentProcess(), hSection, NtCurrentProcess(),
                               &hReadOnly, SECTION_MAP_READ, 0, 0);
    ok_hex(Status, STATUS_SUCCESS);

    /*
     * Widen the read-only handle to write. The section carries a restrictive
     * SD that does not grant SECTION_MAP_WRITE, so the re-evaluation must DENY.
     * This is the Chromium read-only-SHM validation shape.
     */
    hWidened = NULL;
    Status = NtDuplicateObject(NtCurrentProcess(), hReadOnly, NtCurrentProcess(),
                               &hWidened, SECTION_MAP_WRITE, 0, 0);
    ok_hex(Status, STATUS_ACCESS_DENIED);
    ok(hWidened == NULL, "restrictive-SD widen unexpectedly granted handle %p\n", hWidened);
    if (NT_SUCCESS(Status))
        NtClose(hWidened);

    /* A same-or-lesser (read) re-duplicate is still allowed by the DACL. */
    hWidened = NULL;
    Status = NtDuplicateObject(NtCurrentProcess(), hReadOnly, NtCurrentProcess(),
                               &hWidened, SECTION_MAP_READ, 0, 0);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        NtClose(hWidened);

    NtClose(hReadOnly);
    NtClose(hSection);
}

START_TEST(NtDuplicateObject)
{
    NTSTATUS Status;
    HANDLE Handle;

    Handle = NULL;
    Status = NtDuplicateObject(NtCurrentProcess(),
                               NtCurrentProcess(),
                               NtCurrentProcess(),
                               &Handle,
                               GENERIC_ALL,
                               OBJ_PROTECT_CLOSE,
                               0);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Handle != NULL && Handle != NtCurrentProcess(),
        "Handle = %p\n", Handle);
    Status = NtClose(Handle);
    ok_hex(Status, STATUS_HANDLE_NOT_CLOSABLE);

    /* Default-SD object: a widening duplicate SUCCEEDS (Windows-normal). */
    TestDefaultSdWidening();

    /* Restrictive-SD object: a widening duplicate is DENIED by the SD. */
    TestRestrictiveSdWidening();
}
