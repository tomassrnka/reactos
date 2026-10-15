/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Tests for the NtSetInformationToken API
 * COPYRIGHT:       Copyright 2022 George Bișoc <george.bisoc@reactos.org>
 */

#include "precomp.h"
#include <tlhelp32.h>

static
HANDLE
OpenCurrentToken(VOID)
{
    BOOL Success;
    HANDLE Token;

    Success = OpenProcessToken(GetCurrentProcess(),
                               TOKEN_READ | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                               &Token);
    if (!Success)
    {
        ok(0, "OpenProcessToken() has failed to get the process' token (error code: %lu)!\n", GetLastError());
        return NULL;
    }

    return Token;
}

static
PTOKEN_DEFAULT_DACL
QueryOriginalDefaultDacl(
    _In_ HANDLE Token,
    _Out_ PULONG DaclLength)
{
    NTSTATUS Status;
    PTOKEN_DEFAULT_DACL Dacl;
    ULONG BufferLength;

    *DaclLength = 0;

    Status = NtQueryInformationToken(Token,
                                     TokenDefaultDacl,
                                     NULL,
                                     0,
                                     &BufferLength);
    if (!NT_SUCCESS(Status) && (Status != STATUS_BUFFER_TOO_SMALL))
    {
        ok(0, "Failed to query buffer length, STATUS_BUFFER_TOO_SMALL has to be expected (Status code %lx)!\n", Status);
        return NULL;
    }

    Dacl = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferLength);
    if (!Dacl)
    {
        ok(0, "Failed to allocate from heap for token default DACL (required buffer length %lu)!\n", BufferLength);
        return NULL;
    }

    Status = NtQueryInformationToken(Token,
                                     TokenDefaultDacl,
                                     Dacl,
                                     BufferLength,
                                     &BufferLength);
    if (!NT_SUCCESS(Status))
    {
        ok(0, "Failed to query default DACL (Status code %lx)!\n", Status);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Dacl);
        return NULL;
    }

    *DaclLength = BufferLength;
    return Dacl;
}

static
PACL
CreateNewDefaultDacl(
    _Out_ PULONG DaclLength)
{
    NTSTATUS Status;
    PACL Dacl;
    ULONG Length;
    PSID LocalSystemSid;
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};

    *DaclLength = 0;

    Status = RtlAllocateAndInitializeSid(&NtAuthority,
                                         1,
                                         SECURITY_LOCAL_SYSTEM_RID,
                                         0, 0, 0, 0, 0, 0, 0,
                                         &LocalSystemSid);
    if (!NT_SUCCESS(Status))
    {
        ok(0, "Failed to allocate Local System SID (Status code %lx)!\n", Status);
        return NULL;
    }

    Length = sizeof(ACL) +
                 sizeof(ACCESS_ALLOWED_ACE) + RtlLengthSid(LocalSystemSid);

    Dacl = RtlAllocateHeap(RtlGetProcessHeap(),
                           HEAP_ZERO_MEMORY,
                           Length);
    if (!Dacl)
    {
        ok(0, "Failed to allocate from heap for DACL!\n");
        RtlFreeHeap(RtlGetProcessHeap(), 0, LocalSystemSid);
        return NULL;
    }

    Status = RtlCreateAcl(Dacl,
                          Length,
                          ACL_REVISION);
    if (!NT_SUCCESS(Status))
    {
        ok(0, "Failed to create ACL (Status code %lx)!\n", Status);
        RtlFreeHeap(RtlGetProcessHeap(), 0, LocalSystemSid);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Dacl);
        return NULL;
    }

    Status = RtlAddAccessAllowedAce(Dacl,
                                    ACL_REVISION,
                                    GENERIC_ALL,
                                    LocalSystemSid);
    if (!NT_SUCCESS(Status))
    {
        ok(0, "Failed to add access allowed ACE (Status code %lx)!\n", Status);
        RtlFreeHeap(RtlGetProcessHeap(), 0, LocalSystemSid);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Dacl);
        return NULL;
    }

    *DaclLength = Length;
    RtlFreeHeap(RtlGetProcessHeap(), 0, LocalSystemSid);
    return Dacl;
}

static
VOID
SetTokenDefaultDaclTests(
    _In_ HANDLE Token)
{
    NTSTATUS Status;
    PACL NewDacl;
    TOKEN_DEFAULT_DACL NewDefaultDacl;
    PTOKEN_DEFAULT_DACL DefaultDacl;
    ULONG OriginalDaclLength, NewDaclLength;

    /*
     * Query the original DACL of the token first,
     * we don't want to leave the token tampered
     * later on.
     */
    DefaultDacl = QueryOriginalDefaultDacl(Token, &OriginalDaclLength);
    if (!DefaultDacl)
    {
        ok(0, "Failed to query token's default DACL!\n");
        return;
    }

    /* Allocate new DACL */
    NewDacl = CreateNewDefaultDacl(&NewDaclLength);
    if (!DefaultDacl)
    {
        ok(0, "Failed to allocate buffer for new DACL!\n");
        RtlFreeHeap(RtlGetProcessHeap(), 0, DefaultDacl);
        return;
    }

    NewDefaultDacl.DefaultDacl = NewDacl;

    /*
     * Set a new DACL for the token.
     */
    Status = NtSetInformationToken(Token,
                                   TokenDefaultDacl,
                                   &NewDefaultDacl,
                                   NewDaclLength);
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* Now set the original DACL */
    Status = NtSetInformationToken(Token,
                                   TokenDefaultDacl,
                                   DefaultDacl,
                                   OriginalDaclLength);
    ok_ntstatus(Status, STATUS_SUCCESS);

    RtlFreeHeap(RtlGetProcessHeap(), 0, DefaultDacl);
    RtlFreeHeap(RtlGetProcessHeap(), 0, NewDacl);
}

static
VOID
SetTokenSessionIdTests(
    _In_ HANDLE Token)
{
    NTSTATUS Status;
    ULONG SessionId = 1;

    /*
     * We're not allowed to set a session ID
     * because we don't have the TCB privilege.
     */
    Status = NtSetInformationToken(Token,
                                   TokenSessionId,
                                   &SessionId,
                                   sizeof(ULONG));
    ok_ntstatus(Status, STATUS_PRIVILEGE_NOT_HELD);
}

static
ULONG
QueryTokenUlong(
    _In_ HANDLE Token,
    _In_ TOKEN_INFORMATION_CLASS Class)
{
    ULONG Value = 0xdeadbeef, Length;
    NTSTATUS Status;

    Status = NtQueryInformationToken(Token, Class, &Value, sizeof(Value), &Length);
    ok_ntstatus(Status, STATUS_SUCCESS);
    return Value;
}

/* An impersonation token of winlogon.exe (LocalSystem), which holds SeTcbPrivilege */
static
HANDLE
OpenSystemImpersonationToken(VOID)
{
    PROCESSENTRY32W Entry;
    HANDLE Snapshot, Process, Token, Impersonation = NULL;
    BOOLEAN Old;
    BOOL More;

    RtlAdjustPrivilege(SE_DEBUG_PRIVILEGE, TRUE, FALSE, &Old);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return NULL;
    Entry.dwSize = sizeof(Entry);
    for (More = Process32FirstW(Snapshot, &Entry); More && !Impersonation; More = Process32NextW(Snapshot, &Entry))
    {
        if (_wcsicmp(Entry.szExeFile, L"winlogon.exe"))
            continue;
        Process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, Entry.th32ProcessID);
        if (!Process)
            continue;
        if (OpenProcessToken(Process, TOKEN_DUPLICATE | TOKEN_QUERY, &Token))
        {
            if (!DuplicateTokenEx(Token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &Impersonation))
                Impersonation = NULL;
            CloseHandle(Token);
        }
        CloseHandle(Process);
    }
    CloseHandle(Snapshot);
    return Impersonation;
}

/*
 * TokenVirtualizationEnabled and TokenUIAccess, as measured on Windows 7 and
 * 10: both need TOKEN_ADJUST_DEFAULT and a ULONG; enabling virtualization
 * succeeds but has no effect on a token that does not allow it; granting UI
 * access needs SeTcbPrivilege enabled in the caller, clearing it does not;
 * any nonzero value grants it.
 */
static
VOID
SetTokenUIAccessAndVirtualizationTests(VOID)
{
    NTSTATUS Status;
    HANDLE ProcessToken, Token, Weak, System;
    OBJECT_ATTRIBUTES ObjectAttributes;
    ULONG Value[2];
    BOOLEAN Old, HaveTcb;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &ProcessToken))
    {
        skip("OpenProcessToken failed with %lu\n", GetLastError());
        return;
    }
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    Status = NtDuplicateToken(ProcessToken, TOKEN_ALL_ACCESS, &ObjectAttributes, FALSE, TokenPrimary, &Token);
    ok_ntstatus(Status, STATUS_SUCCESS);
    NtClose(ProcessToken);
    if (!NT_SUCCESS(Status))
        return;
    Status = NtDuplicateObject(NtCurrentProcess(), Token, NtCurrentProcess(), &Weak, TOKEN_QUERY, 0, 0);
    ok_ntstatus(Status, STATUS_SUCCESS);

    ok(QueryTokenUlong(Token, TokenVirtualizationAllowed) == 0, "Virtualization allowed\n");
    ok(QueryTokenUlong(Token, TokenVirtualizationEnabled) == 0, "Virtualization enabled\n");
    ok(QueryTokenUlong(Token, TokenUIAccess) == 0, "UI access\n");

    /* Virtualization */
    Value[0] = 1;
    Status = NtSetInformationToken(Token, TokenVirtualizationEnabled, Value, sizeof(ULONG));
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(QueryTokenUlong(Token, TokenVirtualizationEnabled) == 0, "Virtualization enabled on a token that does not allow it\n");
    Value[0] = 0;
    Status = NtSetInformationToken(Token, TokenVirtualizationEnabled, Value, sizeof(ULONG));
    ok_ntstatus(Status, STATUS_SUCCESS);
    Value[0] = 1;
    Status = NtSetInformationToken(Token, TokenVirtualizationEnabled, Value, 2 * sizeof(ULONG));
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtSetInformationToken(Token, TokenVirtualizationEnabled, Value, sizeof(ULONG) - 1);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtSetInformationToken(Weak, TokenVirtualizationEnabled, Value, sizeof(ULONG));
    ok_ntstatus(Status, STATUS_ACCESS_DENIED);

    /* UI access without SeTcbPrivilege enabled */
    HaveTcb = (RtlAdjustPrivilege(SE_TCB_PRIVILEGE, FALSE, FALSE, &Old) == STATUS_SUCCESS);
    Value[0] = 1;
    Status = NtSetInformationToken(Token, TokenUIAccess, Value, sizeof(ULONG));
    ok_ntstatus(Status, STATUS_PRIVILEGE_NOT_HELD);
    ok(QueryTokenUlong(Token, TokenUIAccess) == 0, "UI access granted without the privilege\n");
    Value[0] = 0;
    Status = NtSetInformationToken(Token, TokenUIAccess, Value, sizeof(ULONG));
    ok_ntstatus(Status, STATUS_SUCCESS);
    Value[0] = 1;
    Status = NtSetInformationToken(Token, TokenUIAccess, Value, 2 * sizeof(ULONG));
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtSetInformationToken(Weak, TokenUIAccess, Value, sizeof(ULONG));
    ok_ntstatus(Status, STATUS_ACCESS_DENIED);

    /* UI access with SeTcbPrivilege enabled: our own when we hold it, else LocalSystem's */
    System = NULL;
    if (HaveTcb)
    {
        Status = RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, FALSE, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }
    else
    {
        System = OpenSystemImpersonationToken();
        if (System && SetThreadToken(NULL, System))
        {
            Status = RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, TRUE, &Old);
            ok_ntstatus(Status, STATUS_SUCCESS);
        }
        else
        {
            skip("No token with SeTcbPrivilege (error %lu)\n", GetLastError());
            if (System)
                CloseHandle(System);
            System = NULL;
        }
    }
    if (HaveTcb || System)
    {
        Value[0] = 1;
        Status = NtSetInformationToken(Token, TokenUIAccess, Value, sizeof(ULONG));
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(QueryTokenUlong(Token, TokenUIAccess) == 1, "UI access not granted\n");
        Value[0] = 2;
        Status = NtSetInformationToken(Token, TokenUIAccess, Value, sizeof(ULONG));
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(QueryTokenUlong(Token, TokenUIAccess) == 1, "UI access value not normalized\n");
        Value[0] = 0;
        Status = NtSetInformationToken(Token, TokenUIAccess, Value, sizeof(ULONG));
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(QueryTokenUlong(Token, TokenUIAccess) == 0, "UI access not cleared\n");

        if (HaveTcb)
            RtlAdjustPrivilege(SE_TCB_PRIVILEGE, FALSE, FALSE, &Old);
        if (System)
        {
            RevertToSelf();
            CloseHandle(System);
        }
    }

    NtClose(Weak);
    NtClose(Token);
}

START_TEST(NtSetInformationToken)
{
    NTSTATUS Status;
    ULONG DummyReturnLength = 0;
    HANDLE Token;

    /* Everything else is NULL */
    Status = NtSetInformationToken(NULL,
                                   TokenOwner,
                                   NULL,
                                   0);
    ok_ntstatus(Status, STATUS_INVALID_HANDLE);

    /* We don't give a token */
    Status = NtSetInformationToken(NULL,
                                   TokenOwner,
                                   NULL,
                                   DummyReturnLength);
    ok_ntstatus(Status, STATUS_INVALID_HANDLE);

    Token = OpenCurrentToken();

    /* We give a bogus token class */
    Status = NtSetInformationToken(Token,
                                   0xa0a,
                                   NULL,
                                   DummyReturnLength);
    ok_ntstatus(Status, STATUS_INVALID_INFO_CLASS);

    /* Now perform tests for each class */
    SetTokenDefaultDaclTests(Token);
    SetTokenSessionIdTests(Token);
    SetTokenUIAccessAndVirtualizationTests();

    CloseHandle(Token);
}
