/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS: descriptors of new files and directories inherit from their directory
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

/* The owner of Path (a copy in Owner, SECURITY_MAX_SID_SIZE bytes). */
static BOOLEAN GetOwner(PCWSTR Path, PSID Owner)
{
    UCHAR Buffer[512];
    PSID Sid;
    BOOLEAN Defaulted;
    ULONG Len;
    HANDLE H;
    BOOLEAN Ok = FALSE;
    if (!NT_SUCCESS(NtOpen(Path, READ_CONTROL, SHARE_ALL, FILE_OPEN, FILE_OPEN_FOR_BACKUP_INTENT, NULL, &H)))
        return FALSE;
    if (NT_SUCCESS(NtQuerySecurityObject(H, OWNER_SECURITY_INFORMATION, Buffer, sizeof(Buffer), &Len)) &&
        NT_SUCCESS(RtlGetOwnerSecurityDescriptor(Buffer, &Sid, &Defaulted)) && Sid)
        Ok = CopySid(SECURITY_MAX_SID_SIZE, Owner, Sid);
    NtClose(H);
    return Ok;
}

static VOID TestInherit(PCWSTR Base, BOOLEAN Auto)
{
    WCHAR Dir[MAX_PATH], File[MAX_PATH], Sub[MAX_PATH], SubFile[MAX_PATH], Explicit[MAX_PATH], Prot[MAX_PATH];
    WCHAR NoInh[MAX_PATH], NoInhFile[MAX_PATH], UserFile[MAX_PATH];
    UCHAR Owner[SECURITY_MAX_SID_SIZE], UserOwner[SECURITY_MAX_SID_SIZE];
    CHAR Text[1024];
    NT_ACE Aces[5];
    PSECURITY_DESCRIPTOR Sd;
    PACL Dacl;
    LONG i;
    /* A create through NtCreateFile inherits ACEs unmarked (no INHERITED_ACE, no
     * SE_DACL_AUTO_INHERITED) and lets an explicit DACL replace them, also when the directory's
     * DACL is marked auto-inherited (Windows Server 2008 R2 and Windows 10 22H2 both do this). */
    const BYTE Inh = 0;
    const BYTE AllFlags = 0x1f;
    WCHAR Name[64];

#define NT_SUB(Var, Rel) (_snwprintf(Name, RTL_NUMBER_OF(Name) - 1, L"%ls\\%ls", Base, Rel), Name[RTL_NUMBER_OF(Name) - 1] = 0, NtPath(Var, Name))
    NT_SUB(Dir, L"");
    Dir[wcslen(Dir) - 1] = 0;
    NT_SUB(File, L"file.txt");
    NT_SUB(Sub, L"sub");
    NT_SUB(SubFile, L"sub\\file.txt");
    NT_SUB(Explicit, L"explicit.txt");
    NT_SUB(Prot, L"protected.txt");
    NT_SUB(UserFile, L"byuser.txt");
    NT_SUB(NoInh, L"noinh");
    NT_SUB(NoInhFile, L"noinh\\file.txt");

    /* inh: administrators full (inherited by all), the user reads files, CREATOR OWNER full,
     * Everyone may execute in subdirectories only (one level), the user may add files here. */
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE; Aces[0].Deny = FALSE;
    Aces[1].Sid = SidUser; Aces[1].Mask = FILE_GENERIC_READ; Aces[1].Flags = OBJECT_INHERIT_ACE; Aces[1].Deny = FALSE;
    Aces[2].Sid = SidCreatorOwner; Aces[2].Mask = GENERIC_ALL;
    Aces[2].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE; Aces[2].Deny = FALSE;
    Aces[3].Sid = SidEveryone; Aces[3].Mask = FILE_GENERIC_EXECUTE;
    Aces[3].Flags = CONTAINER_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE; Aces[3].Deny = FALSE;
    Aces[4].Sid = SidUser; Aces[4].Mask = FILE_ADD_FILE | FILE_TRAVERSE | FILE_LIST_DIRECTORY | SYNCHRONIZE; Aces[4].Flags = 0; Aces[4].Deny = FALSE;
    ok_hex(NtMakeDirControl(Dir, Aces, 5, SE_DACL_PROTECTED | (Auto ? SE_DACL_AUTO_INHERITED : 0)), STATUS_SUCCESS);
    ok_hex(NtMakeFile(File, NULL, 0, FALSE, "file"), STATUS_SUCCESS);
    ok_hex(NtMakeDir(Sub, NULL, 0, FALSE), STATUS_SUCCESS);
    ok_hex(NtMakeFile(SubFile, NULL, 0, FALSE, "subfile"), STATUS_SUCCESS);

    /* A file: the effective ACEs inherited by files, CREATOR OWNER replaced by the owner, generic rights mapped. */
    ok(GetOwner(File, (PSID)Owner), "No owner\n");
    Dacl = NtGetDacl(File, Text, sizeof(Text), &Sd);
    trace("file.txt: %s\n", Text);
    ok(Dacl != NULL, "No DACL\n");
    ok(NtFindAce(Dacl, SidAdmins, FILE_ALL_ACCESS, AllFlags, Inh, FALSE) >= 0, "No inherited administrators ACE\n");
    ok(NtFindAce(Dacl, SidUser, FILE_GENERIC_READ, AllFlags, Inh, FALSE) >= 0, "No inherited user read ACE\n");
    ok(NtFindAce(Dacl, (PSID)Owner, FILE_ALL_ACCESS, AllFlags, Inh, FALSE) >= 0, "No owner ACE from CREATOR OWNER\n");
    ok(NtFindAce(Dacl, SidCreatorOwner, GENERIC_ALL, 0, 0, FALSE) < 0, "CREATOR OWNER ACE on a file\n");
    ok(NtFindAce(Dacl, SidEveryone, FILE_GENERIC_EXECUTE, 0, 0, FALSE) < 0, "Container-only ACE on a file\n");
    ok(NtFindAce(Dacl, SidUser, FILE_ADD_FILE | FILE_TRAVERSE | FILE_LIST_DIRECTORY | SYNCHRONIZE, 0, 0, FALSE) < 0,
       "Non-inheritable ACE inherited\n");
    if (Dacl)
        ok(Dacl->AceCount == 3 || (Dacl->AceCount == 2 && EqualSid((PSID)Owner, SidAdmins)),
           "file.txt has %u ACEs\n", Dacl->AceCount);   /* Windows 10 merges the owner ACE with an equal one */
    if (Sd)
    {
        SECURITY_DESCRIPTOR_CONTROL Control = 0;
        ULONG Rev;
        RtlGetControlSecurityDescriptor(Sd, &Control, &Rev);
        ok((Control & SE_DACL_AUTO_INHERITED) == 0, "SE_DACL_AUTO_INHERITED 0x%04x\n", Control);
        ok((Control & SE_DACL_PROTECTED) == 0, "SE_DACL_PROTECTED set\n");
        LocalFree(Sd);
    }

    /* A subdirectory: container ACEs stay inheritable, NO_PROPAGATE ones become effective only. */
    Dacl = NtGetDacl(Sub, Text, sizeof(Text), &Sd);
    trace("sub: %s\n", Text);
    ok(NtFindAce(Dacl, SidAdmins, FILE_ALL_ACCESS, AllFlags, Inh | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, FALSE) >= 0,
       "No inherited inheritable administrators ACE\n");
    ok(NtFindAce(Dacl, SidUser, FILE_GENERIC_READ, AllFlags, Inh | OBJECT_INHERIT_ACE | INHERIT_ONLY_ACE, FALSE) >= 0,
       "No inherit-only user ACE for files below\n");
    ok(NtFindAce(Dacl, SidEveryone, FILE_GENERIC_EXECUTE, AllFlags, Inh, FALSE) >= 0, "No one-level Everyone ACE\n");
    ok(NtFindAce(Dacl, (PSID)Owner, FILE_ALL_ACCESS, AllFlags, Inh, FALSE) >= 0, "No effective owner ACE\n");
    ok(NtFindAce(Dacl, SidCreatorOwner, GENERIC_ALL, AllFlags,
                 Inh | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE, FALSE) >= 0,
       "No inherit-only CREATOR OWNER ACE\n");
    if (Sd)
        LocalFree(Sd);

    /* Two levels down: the NO_PROPAGATE ACE is gone, the user's file ACE arrives through the inherit-only one. */
    Dacl = NtGetDacl(SubFile, Text, sizeof(Text), &Sd);
    trace("sub\\file.txt: %s\n", Text);
    ok(NtFindAce(Dacl, SidUser, FILE_GENERIC_READ, AllFlags, Inh, FALSE) >= 0, "No user read ACE two levels down\n");
    ok(NtFindAce(Dacl, SidEveryone, FILE_GENERIC_EXECUTE, 0, 0, FALSE) < 0, "NO_PROPAGATE ACE propagated\n");
    if (Sd)
        LocalFree(Sd);

    /* An explicit descriptor at create: its ACEs first, then the inherited ones (unless protected). */
    Aces[0].Sid = SidEveryone; Aces[0].Mask = FILE_GENERIC_READ; Aces[0].Flags = 0; Aces[0].Deny = FALSE;
    Sd = NtMakeSd(Aces, 1, FALSE);
    ok_hex(NtOpen(Explicit, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, Sd, NULL), STATUS_SUCCESS);
    LocalFree(Sd);
    Dacl = NtGetDacl(Explicit, Text, sizeof(Text), &Sd);
    trace("explicit.txt: %s\n", Text);
    i = NtFindAce(Dacl, SidEveryone, FILE_GENERIC_READ, AllFlags, 0, FALSE);
    ok(i == 0, "Explicit ACE at %ld\n", i);
    ok(Dacl && Dacl->AceCount == 1, "explicit.txt has %u ACEs\n", Dacl ? Dacl->AceCount : 0);
    if (Sd)
        LocalFree(Sd);
    Sd = NtMakeSd(Aces, 1, TRUE);
    ok_hex(NtOpen(Prot, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, Sd, NULL), STATUS_SUCCESS);
    LocalFree(Sd);
    Dacl = NtGetDacl(Prot, Text, sizeof(Text), &Sd);
    trace("protected.txt: %s\n", Text);
    ok(Dacl && Dacl->AceCount == 1, "Protected DACL has %u ACEs\n", Dacl ? Dacl->AceCount : 0);
    if (Sd)
        LocalFree(Sd);

    /* What inheritance grants is enforced: the user reads file.txt but may not write it, and the
     * user's own new file is the user's (CREATOR OWNER full access). */
    NtBeginUser();
    ok_hex(NtOpen(File, FILE_GENERIC_READ, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SUCCESS);
    ok_hex(NtOpen(File, FILE_WRITE_DATA, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_ACCESS_DENIED);
    ok_hex(NtOpen(UserFile, FILE_GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, NULL), STATUS_SUCCESS);
    ok_hex(NtOpen(UserFile, FILE_GENERIC_WRITE | DELETE | WRITE_DAC, SHARE_ALL, FILE_OPEN, 0, NULL, NULL), STATUS_SUCCESS);
    NtEndUser();
    ok(GetOwner(UserFile, (PSID)UserOwner), "No owner\n");
    ok(EqualSid((PSID)UserOwner, SidUser), "The user's file is not owned by the user\n");

    /* A directory without inheritable ACEs: the new file gets the creator's default DACL. */
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = 0; Aces[0].Deny = FALSE;
    ok_hex(NtMakeDirControl(NoInh, Aces, 1, SE_DACL_PROTECTED | (Auto ? SE_DACL_AUTO_INHERITED : 0)), STATUS_SUCCESS);
    ok_hex(NtMakeFile(NoInhFile, NULL, 0, FALSE, "x"), STATUS_SUCCESS);
    Dacl = NtGetDacl(NoInhFile, Text, sizeof(Text), &Sd);
    trace("noinh\\file.txt: %s\n", Text);
    ok(Dacl != NULL && Dacl->AceCount > 0, "No default DACL\n");
    ok(NtFindAce(Dacl, SidEveryone, FILE_ALL_ACCESS, 0, 0, FALSE) < 0, "Everyone full access on a new file\n");
    if (Sd)
        LocalFree(Sd);
#undef NT_SUB
}

START_TEST(NtfsInherit)
{
    if (!NtInit("NtfsInherit") || !NtHaveUser())
        return;
    TestInherit(L"legacy", FALSE);
    TestInherit(L"auto", TRUE);
    NtCleanup();
}
