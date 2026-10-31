/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DeriveCapabilitySidsFromName (Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

NTSYSAPI
NTSTATUS
NTAPI
RtlDeriveCapabilitySidsFromName(
    _In_ PUNICODE_STRING CapabilityName,
    _Out_ PSID CapabilityGroupSid,
    _Out_ PSID CapabilitySid);

/* The capability SID area has ten sub-authorities, the group SID nine (measured on Windows 10 22H2) */
#define CAPABILITY_SID_SUBAUTHORITIES   10
#define CAPABILITY_GROUP_SUBAUTHORITIES 9

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 *
 * Windows exports this from kernelbase.dll (and through api-ms-win-security-base-l1-2-2); the implementation lives
 * here and kernelbase forwards to it. It returns one capability group SID and one capability SID for a name, each
 * in an array of one pointer; the arrays and the SIDs are separate LocalAlloc blocks that the caller frees with
 * LocalFree. The capability SID block always has room for ten sub-authorities, also for the fixed capabilities
 * such as internetClient (S-1-15-3-1). The last error is left alone on success.
 */
BOOL
WINAPI
DeriveCapabilitySidsFromName(
    _In_ LPCWSTR CapName,
    _Outptr_result_buffer_(*CapabilityGroupSidCount) PSID **CapabilityGroupSids,
    _Out_ PDWORD CapabilityGroupSidCount,
    _Outptr_result_buffer_(*CapabilitySidCount) PSID **CapabilitySids,
    _Out_ PDWORD CapabilitySidCount)
{
    UNICODE_STRING Name;
    PSID *GroupArray = NULL, *CapabilityArray = NULL;
    PSID GroupSid = NULL, CapabilitySid = NULL;
    NTSTATUS Status;

    if (!CapName || !CapabilityGroupSids || !CapabilityGroupSidCount ||
        !CapabilitySids || !CapabilitySidCount)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = RtlInitUnicodeStringEx(&Name, CapName);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    GroupArray = LocalAlloc(LMEM_FIXED, sizeof(PSID));
    CapabilityArray = LocalAlloc(LMEM_FIXED, sizeof(PSID));
    GroupSid = LocalAlloc(LMEM_FIXED, RtlLengthRequiredSid(CAPABILITY_GROUP_SUBAUTHORITIES));
    CapabilitySid = LocalAlloc(LMEM_FIXED, RtlLengthRequiredSid(CAPABILITY_SID_SUBAUTHORITIES));
    if (!GroupArray || !CapabilityArray || !GroupSid || !CapabilitySid)
    {
        Status = STATUS_NO_MEMORY;
        goto Failure;
    }

    Status = RtlDeriveCapabilitySidsFromName(&Name, GroupSid, CapabilitySid);
    if (!NT_SUCCESS(Status))
        goto Failure;

    GroupArray[0] = GroupSid;
    CapabilityArray[0] = CapabilitySid;
    *CapabilityGroupSids = GroupArray;
    *CapabilityGroupSidCount = 1;
    *CapabilitySids = CapabilityArray;
    *CapabilitySidCount = 1;
    return TRUE;

Failure:
    if (GroupArray) LocalFree(GroupArray);
    if (CapabilityArray) LocalFree(CapabilityArray);
    if (GroupSid) LocalFree(GroupSid);
    if (CapabilitySid) LocalFree(CapabilitySid);
    BaseSetLastNTError(Status);
    return FALSE;
}
