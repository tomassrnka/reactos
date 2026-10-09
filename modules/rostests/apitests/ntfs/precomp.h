/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS file system behaviour tests (access control, system files, sharing, identity)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#pragma once

#include <apitest.h>
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <ndk/ntndk.h>
#include <lm.h>
#include <sddl.h>
#include <stdio.h>

/* One ACE of a test descriptor. */
typedef struct _NT_ACE
{
    PSID Sid;
    ACCESS_MASK Mask;
    BYTE Flags;
    BOOLEAN Deny;
} NT_ACE;

extern PSID SidAdmins, SidSystem, SidEveryone, SidCreatorOwner, SidUser;
extern HANDLE UserToken;

/* Prepares the test root (an NTFS directory) and the non-administrator test user; FALSE skips the test. */
BOOLEAN NtInit(PCSTR TestName);
/* "<root>\<test>\Name" in Out (MAX_PATH characters). */
VOID NtPath(PWSTR Out, PCWSTR Name);
PCWSTR NtTestDir(VOID);
BOOLEAN NtIsAdmin(VOID);
BOOLEAN NtHaveUser(VOID);
VOID NtBeginUser(VOID);
VOID NtEndUser(VOID);
BOOLEAN NtSetPrivilege(HANDLE Token, PCWSTR Name, BOOLEAN Enable);

/* NtCreateFile on a Win32 path; with Handle NULL a successful open is closed again. */
NTSTATUS NtOpen(PCWSTR Path, ACCESS_MASK Access, ULONG Share, ULONG Disposition, ULONG Options,
                PSECURITY_DESCRIPTOR Sd, PHANDLE Handle);
NTSTATUS NtOpenById(HANDLE VolumeOrDir, ULONGLONG FileId, ACCESS_MASK Access, ULONG Disposition, PHANDLE Handle);
/* Self-relative descriptor (LocalFree) with a DACL made of Aces; Protected sets SE_DACL_PROTECTED. */
PSECURITY_DESCRIPTOR NtMakeSd(const NT_ACE *Aces, ULONG Count, BOOLEAN Protected);
PSECURITY_DESCRIPTOR NtMakeSdControl(const NT_ACE *Aces, ULONG Count, SECURITY_DESCRIPTOR_CONTROL Control);
NTSTATUS NtMakeDir(PCWSTR Path, const NT_ACE *Aces, ULONG Count, BOOLEAN Protected);
NTSTATUS NtMakeDirControl(PCWSTR Path, const NT_ACE *Aces, ULONG Count, SECURITY_DESCRIPTOR_CONTROL Control);
NTSTATUS NtMakeFile(PCWSTR Path, const NT_ACE *Aces, ULONG Count, BOOLEAN Protected, PCSTR Data);
ACCESS_MASK NtGrantedAccess(HANDLE Handle);
ULONGLONG NtFileId(HANDLE Handle);
/* The DACL of Path as text ("A;0x1f01ff;0x10;S-1-5-32-544 ...") and as a copy (LocalFree). */
PACL NtGetDacl(PCWSTR Path, PSTR Text, ULONG TextSize, PSECURITY_DESCRIPTOR *SdOut);
/* Index of the first ACE of Dacl matching (Sid, Mask, flags & FlagMask == Flags, type), or -1. */
LONG NtFindAce(PACL Dacl, PSID Sid, ACCESS_MASK Mask, BYTE FlagMask, BYTE Flags, BOOLEAN Deny);
/* Removes Path, a file or a directory tree (as administrator, with backup semantics). */
VOID NtRemove(PCWSTR Path);
/* Removes the test directory tree (as administrator, with backup semantics). */
VOID NtCleanup(VOID);
