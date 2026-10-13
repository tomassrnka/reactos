/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ACE inheritance flags in SetEntriesInAcl and for DACL inheritance in SetNamedSecurityInfo
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <aclapi.h>

#define INHERIT_BOTH (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)

static PSID CreateSid(WELL_KNOWN_SID_TYPE Type)
{
    DWORD Size = SECURITY_MAX_SID_SIZE;
    PSID Sid = LocalAlloc(LPTR, Size);

    if (Sid && !CreateWellKnownSid(Type, NULL, Sid, &Size))
    {
        LocalFree(Sid);
        Sid = NULL;
    }
    return Sid;
}

static void MakeEntry(PEXPLICIT_ACCESS_W Entry, PSID Sid, DWORD Access, ACCESS_MODE Mode, DWORD Inheritance)
{
    ZeroMemory(Entry, sizeof(*Entry));
    Entry->grfAccessPermissions = Access;
    Entry->grfAccessMode = Mode;
    Entry->grfInheritance = Inheritance;
    Entry->Trustee.TrusteeForm = TRUSTEE_IS_SID;
    Entry->Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    Entry->Trustee.ptstrName = (LPWSTR)Sid;
}

static PACE_HEADER FindAce(PACL Acl, PSID Sid, UCHAR AceFlags, ACCESS_MASK Mask)
{
    DWORD i;

    for (i = 0; i < Acl->AceCount; i++)
    {
        PACCESS_ALLOWED_ACE Ace;

        if (GetAce(Acl, i, (PVOID *)&Ace) &&
            Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
            Ace->Header.AceFlags == AceFlags &&
            Ace->Mask == Mask &&
            EqualSid((PSID)&Ace->SidStart, Sid))
        {
            return &Ace->Header;
        }
    }
    return NULL;
}

static BOOL HasInheritedEffectiveAce(PACL Acl, PSID Sid, ACCESS_MASK Mask)
{
    DWORD i;

    for (i = 0; i < Acl->AceCount; i++)
    {
        PACCESS_ALLOWED_ACE Ace;

        if (GetAce(Acl, i, (PVOID *)&Ace) &&
            Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
            (Ace->Header.AceFlags & (INHERITED_ACE | INHERIT_ONLY_ACE)) == INHERITED_ACE &&
            (Ace->Mask & Mask) == Mask &&
            EqualSid((PSID)&Ace->SidStart, Sid))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL DaclSupported(PCWSTR Dir)
{
    WCHAR Root[4] = L"C:\\";
    WCHAR FsName[MAX_PATH] = L"";
    DWORD Flags = 0;

    Root[0] = Dir[0];
    if (!GetVolumeInformationW(Root, NULL, 0, NULL, NULL, &Flags, FsName, _countof(FsName)))
        return FALSE;

    /* ReactOS NTFS does not report FILE_PERSISTENT_ACLS */
    return (Flags & FILE_PERSISTENT_ACLS) || !_wcsicmp(FsName, L"NTFS");
}

static void TestSetEntries(PSID Admins, PSID Users)
{
    EXPLICIT_ACCESSW Entries[3];
    PACL Acl = NULL;
    PACCESS_ALLOWED_ACE Ace;
    DWORD Err;

    MakeEntry(&Entries[0], Admins, FILE_ALL_ACCESS, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    MakeEntry(&Entries[1], Users, FILE_GENERIC_READ, GRANT_ACCESS, SUB_CONTAINERS_ONLY_INHERIT);
    MakeEntry(&Entries[2], Users, FILE_GENERIC_WRITE, DENY_ACCESS, NO_INHERITANCE);
    Err = SetEntriesInAclW(3, Entries, NULL, &Acl);
    ok_dec(Err, ERROR_SUCCESS);
    if (Err != ERROR_SUCCESS || !Acl)
        return;

    ok_dec((int)Acl->AceCount, 3);
    if (Acl->AceCount == 3)
    {
        /* the deny entry comes first */
        GetAce(Acl, 0, (PVOID *)&Ace);
        ok_dec(Ace->Header.AceType, ACCESS_DENIED_ACE_TYPE);
        ok_hex(Ace->Header.AceFlags, 0);
        GetAce(Acl, 1, (PVOID *)&Ace);
        ok_dec(Ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
        ok_hex(Ace->Header.AceFlags, INHERIT_BOTH);
        GetAce(Acl, 2, (PVOID *)&Ace);
        ok_hex(Ace->Header.AceFlags, CONTAINER_INHERIT_ACE);
    }
    LocalFree(Acl);
}

static void TestInheritance(PCWSTR Base, PSID Admins, PSID System, PSID Users)
{
    WCHAR Parent[MAX_PATH], Child[MAX_PATH], Child2[MAX_PATH], File[MAX_PATH], Path[MAX_PATH];
    EXPLICIT_ACCESSW Entries[3];
    PACL Acl = NULL, Dacl = NULL, Empty = NULL;
    ACL EmptyAcl;
    PSECURITY_DESCRIPTOR Sd = NULL;
    HANDLE Handle;
    DWORD Err;

    StringCchPrintfW(Parent, _countof(Parent), L"%ls\\parent", Base);
    StringCchPrintfW(Child, _countof(Child), L"%ls\\child", Parent);
    StringCchPrintfW(Child2, _countof(Child2), L"%ls\\child2", Parent);
    StringCchPrintfW(File, _countof(File), L"%ls\\file.txt", Parent);
    ok(CreateDirectoryW(Parent, NULL), "CreateDirectory failed with %lu\n", GetLastError());

    /* a protected DACL whose ACEs can be inherited */
    MakeEntry(&Entries[0], Admins, FILE_ALL_ACCESS, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    MakeEntry(&Entries[1], System, FILE_ALL_ACCESS, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    MakeEntry(&Entries[2], Users, GENERIC_READ | GENERIC_EXECUTE, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    Err = SetEntriesInAclW(3, Entries, NULL, &Acl);
    ok_dec(Err, ERROR_SUCCESS);
    if (Err != ERROR_SUCCESS)
        return;
    Err = SetNamedSecurityInfoW(Parent, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, Acl, NULL);
    ok_dec(Err, ERROR_SUCCESS);
    LocalFree(Acl);

    Err = GetNamedSecurityInfoW(Parent, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
    ok_dec(Err, ERROR_SUCCESS);
    ok(Dacl != NULL, "the parent has no DACL\n");
    if (Err == ERROR_SUCCESS && Dacl)
    {
        ok(FindAce(Dacl, Admins, INHERIT_BOTH, FILE_ALL_ACCESS) != NULL, "Administrators ACE lost its inheritance flags\n");
        ok(FindAce(Dacl, System, INHERIT_BOTH, FILE_ALL_ACCESS) != NULL, "SYSTEM ACE lost its inheritance flags\n");
    }
    if (Sd)
        LocalFree(Sd);

    /* reset a child directory to the inherited DACL with an empty, unprotected one (what the Burn engine does) */
    ok(CreateDirectoryW(Child, NULL), "CreateDirectory failed with %lu\n", GetLastError());
    InitializeAcl(&EmptyAcl, sizeof(EmptyAcl), ACL_REVISION);
    Err = SetNamedSecurityInfoW(Child, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, &EmptyAcl, NULL);
    ok_dec(Err, ERROR_SUCCESS);

    Dacl = NULL;
    Sd = NULL;
    Err = GetNamedSecurityInfoW(Child, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
    ok_dec(Err, ERROR_SUCCESS);
    ok(Dacl != NULL, "the reset directory has no DACL\n");
    if (Err == ERROR_SUCCESS && Dacl)
    {
        ok(FindAce(Dacl, Admins, INHERIT_BOTH | INHERITED_ACE, FILE_ALL_ACCESS) != NULL,
           "Administrators ACE was not inherited by the directory\n");
        ok(FindAce(Dacl, System, INHERIT_BOTH | INHERITED_ACE, FILE_ALL_ACCESS) != NULL,
           "SYSTEM ACE was not inherited by the directory\n");
        /* Windows splits a generic inheritable ACE into an effective and an inherit-only ACE, ReactOS maps it in place */
        ok(HasInheritedEffectiveAce(Dacl, Users, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE) ||
           FindAce(Dacl, Users, INHERIT_BOTH | INHERITED_ACE, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE) != NULL,
           "ACE of Users was not inherited by the directory\n");
    }
    if (Sd)
        LocalFree(Sd);

    /* the directory is usable */
    StringCchPrintfW(Path, _countof(Path), L"%ls\\x.txt", Child);
    Handle = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "creating a file in the reset directory failed with %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);
    DeleteFileW(Path);

    /* the same through an extended-length path */
    {
        WCHAR Child3[MAX_PATH], Extended[MAX_PATH + 8];

        StringCchPrintfW(Child3, _countof(Child3), L"%ls\\child3", Parent);
        StringCchPrintfW(Extended, _countof(Extended), L"\\\\?\\%ls", Child3);
        ok(CreateDirectoryW(Child3, NULL), "CreateDirectory failed with %lu\n", GetLastError());
        Err = SetNamedSecurityInfoW(Extended, SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, &EmptyAcl, NULL);
        ok_dec(Err, ERROR_SUCCESS);
        Dacl = NULL;
        Sd = NULL;
        Err = GetNamedSecurityInfoW(Child3, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
        ok_dec(Err, ERROR_SUCCESS);
        ok(Dacl != NULL, "the directory reset through an extended path has no DACL\n");
        if (Err == ERROR_SUCCESS && Dacl)
        {
            ok(FindAce(Dacl, Admins, INHERIT_BOTH | INHERITED_ACE, FILE_ALL_ACCESS) != NULL,
               "Administrators ACE was not inherited through an extended path\n");
        }
        if (Sd)
            LocalFree(Sd);
        RemoveDirectoryW(Child3);
    }

    /* an extended path keeps its components: "p." and "p" are different directories there */
    {
        WCHAR Dir1[MAX_PATH + 8], Dir2[MAX_PATH + 8], Kid[MAX_PATH + 8];
        EXPLICIT_ACCESSW One;
        PACL OneAcl = NULL;

        StringCchPrintfW(Dir1, _countof(Dir1), L"\\\\?\\%ls\\p", Parent);
        StringCchPrintfW(Dir2, _countof(Dir2), L"\\\\?\\%ls\\p.", Parent);
        StringCchPrintfW(Kid, _countof(Kid), L"%ls\\kid", Dir2);
        ok(CreateDirectoryW(Dir1, NULL), "CreateDirectory failed with %lu\n", GetLastError());
        ok(CreateDirectoryW(Dir2, NULL), "CreateDirectory failed with %lu\n", GetLastError());

        /* "p" only grants SYSTEM, "p." only grants Administrators */
        MakeEntry(&One, System, FILE_ALL_ACCESS, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        Err = SetEntriesInAclW(1, &One, NULL, &OneAcl);
        ok_dec(Err, ERROR_SUCCESS);
        Err = SetNamedSecurityInfoW(Dir1, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, OneAcl, NULL);
        ok_dec(Err, ERROR_SUCCESS);
        LocalFree(OneAcl);
        OneAcl = NULL;
        MakeEntry(&One, Admins, FILE_ALL_ACCESS, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        Err = SetEntriesInAclW(1, &One, NULL, &OneAcl);
        ok_dec(Err, ERROR_SUCCESS);
        Err = SetNamedSecurityInfoW(Dir2, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, OneAcl, NULL);
        ok_dec(Err, ERROR_SUCCESS);
        LocalFree(OneAcl);

        if (CreateDirectoryW(Kid, NULL))
        {
            Err = SetNamedSecurityInfoW(Kid, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                        NULL, NULL, &EmptyAcl, NULL);
            ok_dec(Err, ERROR_SUCCESS);
            Dacl = NULL;
            Sd = NULL;
            Err = GetNamedSecurityInfoW(Kid, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
            ok_dec(Err, ERROR_SUCCESS);
            if (Err == ERROR_SUCCESS && Dacl)
            {
                ok(FindAce(Dacl, Admins, INHERIT_BOTH | INHERITED_ACE, FILE_ALL_ACCESS) != NULL,
                   "the directory did not inherit from \"p.\"\n");
                ok(FindAce(Dacl, System, INHERIT_BOTH | INHERITED_ACE, FILE_ALL_ACCESS) == NULL,
                   "the directory inherited from \"p\" instead of \"p.\"\n");
            }
            if (Sd)
                LocalFree(Sd);
            RemoveDirectoryW(Kid);
        }
        else
            ok(0, "CreateDirectory(kid) failed with %lu\n", GetLastError());
        RemoveDirectoryW(Dir2);
        RemoveDirectoryW(Dir1);
    }

    /* an explicit ACE with UNPROTECTED comes first and the parent's ACEs follow */
    ok(CreateDirectoryW(Child2, NULL), "CreateDirectory failed with %lu\n", GetLastError());
    MakeEntry(&Entries[0], Users, FILE_GENERIC_READ, GRANT_ACCESS, NO_INHERITANCE);
    Err = SetEntriesInAclW(1, Entries, NULL, &Acl);
    ok_dec(Err, ERROR_SUCCESS);
    Err = SetNamedSecurityInfoW(Child2, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, Acl, NULL);
    ok_dec(Err, ERROR_SUCCESS);
    LocalFree(Acl);
    Dacl = NULL;
    Sd = NULL;
    Err = GetNamedSecurityInfoW(Child2, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
    ok_dec(Err, ERROR_SUCCESS);
    ok(Dacl != NULL, "the directory with an explicit ACE has no DACL\n");
    if (Err == ERROR_SUCCESS && Dacl)
    {
        PACCESS_ALLOWED_ACE First;

        ok(Dacl->AceCount > 1, "only %u ACEs\n", Dacl->AceCount);
        if (GetAce(Dacl, 0, (PVOID *)&First))
        {
            ok_hex(First->Header.AceFlags, 0);
            ok_hex(First->Mask, FILE_GENERIC_READ);
        }
        ok(FindAce(Dacl, Admins, INHERIT_BOTH | INHERITED_ACE, FILE_ALL_ACCESS) != NULL,
           "Administrators ACE was not inherited next to the explicit ACE\n");
    }
    if (Sd)
        LocalFree(Sd);

    /* a file takes only the ACEs that objects inherit, as effective ACEs */
    Handle = CreateFileW(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "CreateFile failed with %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Handle);
        Err = SetNamedSecurityInfoW(File, SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, &EmptyAcl, NULL);
        ok_dec(Err, ERROR_SUCCESS);
        Dacl = NULL;
        Sd = NULL;
        Err = GetNamedSecurityInfoW(File, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
        ok_dec(Err, ERROR_SUCCESS);
        ok(Dacl != NULL, "the reset file has no DACL\n");
        if (Err == ERROR_SUCCESS && Dacl)
        {
            ok(FindAce(Dacl, Admins, INHERITED_ACE, FILE_ALL_ACCESS) != NULL, "file did not inherit the Administrators ACE\n");
            ok(HasInheritedEffectiveAce(Dacl, Users, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE),
               "file did not inherit the effective ACE of Users\n");
        }
        if (Sd)
            LocalFree(Sd);
    }

    /* a protected DACL keeps exactly the ACEs given */
    MakeEntry(&Entries[0], Users, FILE_GENERIC_READ, GRANT_ACCESS, NO_INHERITANCE);
    Err = SetEntriesInAclW(1, Entries, NULL, &Empty);
    ok_dec(Err, ERROR_SUCCESS);
    Err = SetNamedSecurityInfoW(Child2, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, Empty, NULL);
    ok_dec(Err, ERROR_SUCCESS);
    LocalFree(Empty);
    Dacl = NULL;
    Sd = NULL;
    Err = GetNamedSecurityInfoW(Child2, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
    ok_dec(Err, ERROR_SUCCESS);
    ok(Dacl != NULL, "the protected directory has no DACL\n");
    if (Err == ERROR_SUCCESS && Dacl)
        ok_dec((int)Dacl->AceCount, 1);
    if (Sd)
        LocalFree(Sd);

    /* clean up: the parent DACL grants Administrators full control */
    DeleteFileW(File);
    RemoveDirectoryW(Child2);
    RemoveDirectoryW(Child);
    RemoveDirectoryW(Parent);
}

static void TestVolumeRootChild(PCWSTR Base, PSID Admins)
{
    WCHAR Root[4] = L"C:\\";
    WCHAR Child[MAX_PATH];
    ACL EmptyAcl;
    PACL Dacl = NULL;
    PSECURITY_DESCRIPTOR Sd = NULL;
    BOOL RootInherits = FALSE;
    DWORD Err, i;

    Root[0] = Base[0];
    Err = GetNamedSecurityInfoW(Root, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
    if (Err != ERROR_SUCCESS || !Dacl)
    {
        skip("cannot read the DACL of %ls: %lu\n", Root, Err);
        if (Sd)
            LocalFree(Sd);
        return;
    }
    for (i = 0; i < Dacl->AceCount; i++)
    {
        PACCESS_ALLOWED_ACE Ace;

        if (GetAce(Dacl, i, (PVOID *)&Ace) && Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
            (Ace->Header.AceFlags & CONTAINER_INHERIT_ACE) && !(Ace->Header.AceFlags & INHERIT_ONLY_ACE))
        {
            RootInherits = TRUE;
        }
    }
    LocalFree(Sd);
    if (!RootInherits)
    {
        skip("the root of the volume has no inheritable ACE\n");
        return;
    }

    StringCchPrintfW(Child, _countof(Child), L"%lssni-root-%lu", Root, GetCurrentProcessId());
    if (!CreateDirectoryW(Child, NULL))
    {
        skip("cannot create %ls: %lu\n", Child, GetLastError());
        return;
    }

    InitializeAcl(&EmptyAcl, sizeof(EmptyAcl), ACL_REVISION);
    Err = SetNamedSecurityInfoW(Child, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, &EmptyAcl, NULL);
    ok_dec(Err, ERROR_SUCCESS);
    Dacl = NULL;
    Sd = NULL;
    Err = GetNamedSecurityInfoW(Child, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &Dacl, NULL, &Sd);
    ok_dec(Err, ERROR_SUCCESS);
    ok(Dacl != NULL && Dacl->AceCount > 0, "a directory in the volume root inherited nothing\n");
    if (Dacl)
    {
        BOOL Inherited = FALSE;

        for (i = 0; i < Dacl->AceCount; i++)
        {
            PACE_HEADER Ace;

            if (GetAce(Dacl, i, (PVOID *)&Ace) && (Ace->AceFlags & INHERITED_ACE))
                Inherited = TRUE;
        }
        ok(Inherited, "no ACE of the directory in the volume root is marked inherited\n");
    }
    if (Sd)
        LocalFree(Sd);
    RemoveDirectoryW(Child);
    UNREFERENCED_PARAMETER(Admins);
}

START_TEST(SetNamedSecurityInfoInherit)
{
    WCHAR Temp[MAX_PATH], Base[MAX_PATH];
    PSID Admins = CreateSid(WinBuiltinAdministratorsSid);
    PSID System = CreateSid(WinLocalSystemSid);
    PSID Users = CreateSid(WinBuiltinUsersSid);

    if (!Admins || !System || !Users)
    {
        skip("could not create the well-known SIDs\n");
        LocalFree(Admins);
        LocalFree(System);
        LocalFree(Users);
        return;
    }

    TestSetEntries(Admins, Users);

    GetTempPathW(_countof(Temp), Temp);
    StringCchPrintfW(Base, _countof(Base), L"%lssni-%lu", Temp, GetCurrentProcessId());
    if (!CreateDirectoryW(Base, NULL))
    {
        skip("cannot create %ls: %lu\n", Base, GetLastError());
        LocalFree(Admins);
        LocalFree(System);
        LocalFree(Users);
        return;
    }

    if (!DaclSupported(Base))
        skip("the volume of %ls does not keep ACLs\n", Base);
    else
    {
        TestInheritance(Base, Admins, System, Users);
        TestVolumeRootChild(Base, Admins);
    }

    RemoveDirectoryW(Base);
    LocalFree(Admins);
    LocalFree(System);
    LocalFree(Users);
}
