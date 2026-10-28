/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     SetSecurityInfo on a registry key handle without WRITE_DAC, and the
 *              OW / AC SDDL SID aliases. Google Updater (Chrome enterprise MSI) sets
 *              the DACL of HKLM\...\ClientStateMedium through the KEY_WRITE handle it
 *              created it with, and its mojo IPC pipe uses
 *              "D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)"; both succeed on Windows.
 */

#include "precomp.h"
#include <aclapi.h>
#include <sddl.h>

static void TestKeyWriteHandleDacl(void)
{
    static const WCHAR KeyName[] = L"Software\\ReactOS-apitest-SetSecurityInfoKey";
    PSECURITY_DESCRIPTOR Sd = NULL;
    PACL Dacl = NULL, NewDacl = NULL;
    EXPLICIT_ACCESSW Access;
    PSID Interactive = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    HKEY Key;
    DWORD Error;
    BOOL Found = FALSE;
    ACL_SIZE_INFORMATION Info;
    DWORD i;

    Error = RegCreateKeyExW(HKEY_CURRENT_USER, KeyName, 0, NULL, REG_OPTION_VOLATILE,
                            KEY_WRITE, NULL, &Key, NULL);
    ok_long(Error, ERROR_SUCCESS);
    if (Error) return;

    /* KEY_WRITE includes READ_CONTROL (STANDARD_RIGHTS_WRITE), not WRITE_DAC */
    Error = GetSecurityInfo(Key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                            NULL, NULL, &Dacl, NULL, &Sd);
    ok_long(Error, ERROR_SUCCESS);

    AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_INTERACTIVE_RID, 0, 0, 0, 0, 0, 0, 0, &Interactive);
    ZeroMemory(&Access, sizeof(Access));
    Access.grfAccessPermissions = KEY_READ | KEY_SET_VALUE | KEY_CREATE_SUB_KEY;
    Access.grfAccessMode = GRANT_ACCESS;
    Access.grfInheritance = CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE;
    Access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    Access.Trustee.ptstrName = Interactive;
    Error = SetEntriesInAclW(1, &Access, Dacl, &NewDacl);
    ok_long(Error, ERROR_SUCCESS);

    Error = SetSecurityInfo(Key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                            NULL, NULL, NewDacl, NULL);
    ok_long(Error, ERROR_SUCCESS);

    if (Sd) LocalFree(Sd);
    Sd = NULL; Dacl = NULL;
    Error = GetSecurityInfo(Key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                            NULL, NULL, &Dacl, NULL, &Sd);
    ok_long(Error, ERROR_SUCCESS);
    if (!Error && Dacl && GetAclInformation(Dacl, &Info, sizeof(Info), AclSizeInformation))
    {
        for (i = 0; i < Info.AceCount; i++)
        {
            ACCESS_ALLOWED_ACE *Ace;
            if (GetAce(Dacl, i, (PVOID *)&Ace) && Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                EqualSid((PSID)&Ace->SidStart, Interactive))
                Found = TRUE;
        }
    }
    ok(Found, "The INTERACTIVE ACE was not set\n");

    if (Sd) LocalFree(Sd);
    if (NewDacl) LocalFree(NewDacl);
    if (Interactive) FreeSid(Interactive);
    RegCloseKey(Key);
    RegDeleteKeyW(HKEY_CURRENT_USER, KeyName);
}

static void TestSddlAlias(PCWSTR Sddl, PCWSTR ExpectedSid)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    BOOL Present, Defaulted;
    PACL Dacl;
    ACCESS_ALLOWED_ACE *Ace;
    PWSTR String;

    ok(ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &Sd, NULL),
       "%ls: error %lu\n", Sddl, GetLastError());
    if (!Sd) return;
    if (GetSecurityDescriptorDacl(Sd, &Present, &Dacl, &Defaulted) && Present && Dacl &&
        GetAce(Dacl, Dacl->AceCount - 1, (PVOID *)&Ace) &&
        ConvertSidToStringSidW((PSID)&Ace->SidStart, &String))
    {
        ok(!wcscmp(String, ExpectedSid), "%ls: SID %ls, expected %ls\n", Sddl, String, ExpectedSid);
        LocalFree(String);
    }
    else
        ok(0, "%ls: no ACE\n", Sddl);
    LocalFree(Sd);
}

START_TEST(SetSecurityInfoKey)
{
    TestKeyWriteHandleDacl();
    TestSddlAlias(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)", L"S-1-3-4");
    TestSddlAlias(L"D:(A;;GR;;;AC)", L"S-1-15-2-1");
}
