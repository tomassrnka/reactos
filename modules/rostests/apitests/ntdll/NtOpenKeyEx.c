/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtOpenKeyEx, natively and through the WoW64 thunk
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define TEST_STR    L"\\Registry\\Machine\\SOFTWARE"

typedef NTSTATUS (NTAPI *PFN_NtOpenKeyEx)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);

START_TEST(NtOpenKeyEx)
{
    PFN_NtOpenKeyEx pNtOpenKeyEx;
    OBJECT_ATTRIBUTES Object;
    UNICODE_STRING String;
    HANDLE KeyHandle;
    NTSTATUS Status;

    /* Vista+ API: not exported by the 2003-targeted ntdll */
    pNtOpenKeyEx = (PFN_NtOpenKeyEx)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenKeyEx");
    if (!pNtOpenKeyEx)
    {
        skip("NtOpenKeyEx is not available\n");
        return;
    }

    RtlInitUnicodeString(&String, TEST_STR);
    InitializeObjectAttributes(&Object, &String, OBJ_CASE_INSENSITIVE, NULL, NULL);

    KeyHandle = NULL;
    Status = pNtOpenKeyEx(&KeyHandle, KEY_QUERY_VALUE, &Object, 0);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(KeyHandle != NULL, "KeyHandle is NULL\n");
    if (NT_SUCCESS(Status))
        NtClose(KeyHandle);

    KeyHandle = NULL;
    Status = pNtOpenKeyEx(&KeyHandle, KEY_QUERY_VALUE, &Object, REG_OPTION_BACKUP_RESTORE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        NtClose(KeyHandle);

    /* Illegal option bits are rejected */
    KeyHandle = NULL;
    Status = pNtOpenKeyEx(&KeyHandle, KEY_QUERY_VALUE, &Object, REG_OPTION_VOLATILE);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    if (NT_SUCCESS(Status))
        NtClose(KeyHandle);
}
