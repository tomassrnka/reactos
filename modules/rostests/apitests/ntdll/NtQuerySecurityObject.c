/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Native mandatory-label security queries on processes and threads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static VOID TestLabel(HANDLE Object, ULONG Level)
{
    ULONG Buffer[128], Length, i;
    SECURITY_DESCRIPTOR_RELATIVE *Sd = (PVOID)Buffer;
    PSYSTEM_MANDATORY_LABEL_ACE Ace;
    PACL Sacl;
    NTSTATUS Status;
    HANDLE Handle;
    static const ACCESS_MASK Rights[] = {0, READ_CONTROL, WRITE_OWNER};

    Status = NtQuerySecurityObject(Object, LABEL_SECURITY_INFORMATION, NULL, 0, &Length);
    ok_hex(Status, STATUS_BUFFER_TOO_SMALL);
    ok(Length == 48, "Label descriptor length %lu, expected 48\n", Length);
    Status = NtQuerySecurityObject(Object, LABEL_SECURITY_INFORMATION, Sd, sizeof(Buffer), &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    ok(Sd->Control == (SE_SELF_RELATIVE | SE_SACL_AUTO_INHERITED | SE_SACL_PRESENT),
       "Control %x\n", Sd->Control);
    ok(!Sd->Owner && !Sd->Group && !Sd->Dacl, "LABEL query returned unrelated security information\n");
    ok(Sd->Sacl != 0, "Missing SACL\n");
    if (Sd->Sacl)
    {
        Sacl = (PACL)((PBYTE)Sd + Sd->Sacl);
        ok(Sacl->AceCount == 1, "ACE count %u\n", Sacl->AceCount);
        Status = RtlGetAce(Sacl, 0, (PVOID *)&Ace);
        ok_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok(Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE, "Type %u\n", Ace->Header.AceType);
            ok(Ace->Mask == (SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP),
               "Default policy %lx\n", Ace->Mask);
            ok(*RtlSubAuthoritySid(&Ace->SidStart, 0) == Level, "Level %lx, expected %lx\n",
               *RtlSubAuthoritySid(&Ace->SidStart, 0), Level);
        }
    }
    for (i = 0; i < _countof(Rights); ++i)
    {
        Status = NtDuplicateObject(NtCurrentProcess(), Object, NtCurrentProcess(), &Handle, Rights[i], 0, 0);
        ok_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) continue;
        Status = NtQuerySecurityObject(Handle, LABEL_SECURITY_INFORMATION, Sd, sizeof(Buffer), &Length);
        ok_hex(Status, (Rights[i] & READ_CONTROL) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED);
        NtClose(Handle);
    }
}

START_TEST(NtQuerySecurityObject)
{
    UCHAR Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    PTOKEN_MANDATORY_LABEL Label = (PVOID)Buffer;
    ULONG Length, Level;
    HANDLE Token;
    NTSTATUS Status;

    Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &Token);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtQueryInformationToken(Token, TokenIntegrityLevel, Buffer, sizeof(Buffer), &Length);
    NtClose(Token);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Level = *RtlSubAuthoritySid(Label->Label.Sid, 0);
    TestLabel(NtCurrentProcess(), Level);
    TestLabel(NtCurrentThread(), Level);
}
