/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtDuplicateObject
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 */

#include "precomp.h"

#define OBJ_PROTECT_CLOSE 0x01

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

    /* Access-escalation contract (T100): a duplicated handle is capped to the
     * SOURCE handle's granted access. NtDuplicateObject must never grant a
     * user-mode caller more access than the source handle holds -- re-checking the
     * object's security descriptor to hand back extra rights would let a reduced
     * (e.g. read-only) handle be re-opened with full (e.g. writable) access.
     * Windows denies this; Chromium's read-only shared-memory validation relies
     * on it (DuplicateHandle(readOnlyHandle, FILE_MAP_WRITE) must fail). */
    {
        HANDLE hFull = NULL, hReduced = NULL, hDup;
        OBJECT_BASIC_INFORMATION obi;

        Status = NtCreateEvent(&hFull, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
        ok_hex(Status, STATUS_SUCCESS);

        /* Reduce to SYNCHRONIZE only */
        hReduced = NULL;
        Status = NtDuplicateObject(NtCurrentProcess(), hFull, NtCurrentProcess(),
                                   &hReduced, SYNCHRONIZE, 0, 0);
        ok_hex(Status, STATUS_SUCCESS);

        /* Elevation: ask for EVENT_MODIFY_STATE the reduced handle lacks -> DENIED */
        hDup = NULL;
        Status = NtDuplicateObject(NtCurrentProcess(), hReduced, NtCurrentProcess(),
                                   &hDup, EVENT_MODIFY_STATE, 0, 0);
        ok_hex(Status, STATUS_ACCESS_DENIED);
        ok(hDup == NULL, "escalation granted a handle %p\n", hDup);
        if (NT_SUCCESS(Status)) NtClose(hDup);

        /* Same-or-lesser access: allowed */
        hDup = NULL;
        Status = NtDuplicateObject(NtCurrentProcess(), hReduced, NtCurrentProcess(),
                                   &hDup, SYNCHRONIZE, 0, 0);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status)) NtClose(hDup);

        /* DUPLICATE_SAME_ACCESS: copies exactly the source handle's access */
        hDup = NULL;
        Status = NtDuplicateObject(NtCurrentProcess(), hReduced, NtCurrentProcess(),
                                   &hDup, 0, 0, DUPLICATE_SAME_ACCESS);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            RtlZeroMemory(&obi, sizeof(obi));
            Status = NtQueryObject(hDup, ObjectBasicInformation, &obi, sizeof(obi), NULL);
            ok_hex(Status, STATUS_SUCCESS);
            ok(obi.GrantedAccess == SYNCHRONIZE, "SAME_ACCESS granted 0x%lx\n", obi.GrantedAccess);
            NtClose(hDup);
        }

        /* MAXIMUM_ALLOWED: yields the source handle's access, never more */
        hDup = NULL;
        Status = NtDuplicateObject(NtCurrentProcess(), hReduced, NtCurrentProcess(),
                                   &hDup, MAXIMUM_ALLOWED, 0, 0);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            RtlZeroMemory(&obi, sizeof(obi));
            Status = NtQueryObject(hDup, ObjectBasicInformation, &obi, sizeof(obi), NULL);
            ok_hex(Status, STATUS_SUCCESS);
            ok(obi.GrantedAccess == SYNCHRONIZE, "MAXIMUM_ALLOWED granted 0x%lx\n", obi.GrantedAccess);
            NtClose(hDup);
        }

        NtClose(hReduced);
        NtClose(hFull);
    }
}
