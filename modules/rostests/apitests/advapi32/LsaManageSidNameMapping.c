/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for LsaManageSidNameMapping, which OpenSSH imports statically from advapi32
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ntsecapi.h>

typedef struct _MAP_ADD_INPUT
{
    UNICODE_STRING DomainName;
    UNICODE_STRING AccountName;
    PSID Sid;
    ULONG Flags;
} MAP_ADD_INPUT;

typedef struct _MAP_OUTPUT
{
    ULONG ErrorCode;
} MAP_OUTPUT;

typedef NTSTATUS (WINAPI *MANAGE_MAPPING)(ULONG, PVOID, MAP_OUTPUT **);

#define MAP_OP_ADD    0
#define MAP_OP_REMOVE 1
#define MAP_NON_MAPPING_ERROR 1

static BOOL HasTcb(VOID)
{
    HANDLE Token;
    TOKEN_PRIVILEGES *Privileges;
    UCHAR Buffer[1024];
    DWORD Length, i;
    LUID Luid;
    BOOL Found = FALSE;

    if (!LookupPrivilegeValueW(NULL, L"SeTcbPrivilege", &Luid) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return FALSE;
    Privileges = (TOKEN_PRIVILEGES *)Buffer;
    if (GetTokenInformation(Token, TokenPrivileges, Buffer, sizeof(Buffer), &Length))
    {
        for (i = 0; i < Privileges->PrivilegeCount; i++)
        {
            if (Privileges->Privileges[i].Luid.LowPart == Luid.LowPart &&
                Privileges->Privileges[i].Luid.HighPart == Luid.HighPart &&
                (Privileges->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED))
                Found = TRUE;
        }
    }
    CloseHandle(Token);
    return Found;
}

START_TEST(LsaManageSidNameMapping)
{
    static WCHAR Domain[] = L"VIRTUAL USERS";
    static WCHAR Account[] = L"apitestaccount";
    HMODULE Advapi32 = GetModuleHandleW(L"advapi32.dll");
    MANAGE_MAPPING Manage;
    SID_IDENTIFIER_AUTHORITY Authority = {SECURITY_NT_AUTHORITY};
    MAP_ADD_INPUT Input;
    MAP_OUTPUT *Output;
    PSID Sid = NULL;
    NTSTATUS Status;
    ULONG Op;

    Manage = Advapi32 ? (MANAGE_MAPPING)GetProcAddress(Advapi32, "LsaManageSidNameMapping") : NULL;
    ok(Manage != NULL, "advapi32 does not export LsaManageSidNameMapping\n");
    if (!Manage)
        return;

    if (HasTcb())
    {
        skip("The caller holds SeTcbPrivilege; this test checks the refusal of a caller without it\n");
        return;
    }

    ok(AllocateAndInitializeSid(&Authority, 3, 111, 1234, 5678, 0, 0, 0, 0, 0, &Sid),
       "AllocateAndInitializeSid failed: %lu\n", GetLastError());
    if (!Sid)
        return;

    ZeroMemory(&Input, sizeof(Input));
    Input.DomainName.Buffer = Domain;
    Input.DomainName.Length = Input.DomainName.MaximumLength = sizeof(Domain) - sizeof(WCHAR);
    Input.AccountName.Buffer = Account;
    Input.AccountName.Length = Input.AccountName.MaximumLength = sizeof(Account) - sizeof(WCHAR);
    Input.Sid = Sid;

    /* A caller without SeTcbPrivilege is refused, and still gets an output block with a non-mapping error */
    for (Op = MAP_OP_ADD; Op <= MAP_OP_REMOVE; Op++)
    {
        Output = (MAP_OUTPUT *)(ULONG_PTR)0x1234;
        Status = Manage(Op, &Input, &Output);
        ok(Status == STATUS_PRIVILEGE_NOT_HELD, "operation %lu: status 0x%08lx\n", Op, Status);
        ok(Output != NULL && Output != (MAP_OUTPUT *)(ULONG_PTR)0x1234, "operation %lu: output %p\n", Op, Output);
        if (Output && Output != (MAP_OUTPUT *)(ULONG_PTR)0x1234)
        {
            ok(Output->ErrorCode == MAP_NON_MAPPING_ERROR, "operation %lu: error code %lu\n", Op, Output->ErrorCode);
            Status = LsaFreeMemory(Output);
            ok(Status == STATUS_SUCCESS, "LsaFreeMemory: 0x%08lx\n", Status);
        }
    }
    FreeSid(Sid);
}
