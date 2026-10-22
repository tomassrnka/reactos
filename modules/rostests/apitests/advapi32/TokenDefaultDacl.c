/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for querying and setting the default DACL of a token
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <aclapi.h>

static PTOKEN_DEFAULT_DACL QueryDefaultDacl(HANDLE hToken, PDWORD ReturnedLength)
{
    DWORD Length = 0, Length2 = 0;
    PTOKEN_DEFAULT_DACL Dacl;

    SetLastError(0xdeadbeef);
    ok(!GetTokenInformation(hToken, TokenDefaultDacl, NULL, 0, &Length), "Size query succeeded\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Error %lu\n", GetLastError());
    if (!Length)
        return NULL;
    Dacl = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Length);
    if (!Dacl)
        return NULL;
    if (!GetTokenInformation(hToken, TokenDefaultDacl, Dacl, Length, &Length2))
    {
        ok(0, "TokenDefaultDacl failed: %lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, Dacl);
        return NULL;
    }
    ok(Length2 == Length, "Length %lu, size query %lu\n", Length2, Length);
    if (Dacl->DefaultDacl)
    {
        ok((ULONG_PTR)Dacl->DefaultDacl >= (ULONG_PTR)(Dacl + 1) &&
           (ULONG_PTR)Dacl->DefaultDacl + Dacl->DefaultDacl->AclSize <= (ULONG_PTR)Dacl + Length,
           "ACL %p (%u bytes) outside the buffer %p+%lu\n", Dacl->DefaultDacl, Dacl->DefaultDacl->AclSize, Dacl, Length);
        ok(IsValidAcl(Dacl->DefaultDacl), "Invalid ACL\n");
    }
    if (ReturnedLength)
        *ReturnedLength = Length;
    return Dacl;
}

START_TEST(TokenDefaultDacl)
{
    HANDLE hProcessToken, hToken;
    PTOKEN_DEFAULT_DACL Original, Changed;
    TOKEN_DEFAULT_DACL NewDacl;
    EXPLICIT_ACCESSW Access = {0};
    BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
    DWORD Size = sizeof(SidBuffer), i;
    PACL Acl = NULL;
    BOOL Found = FALSE;
    PACCESS_ALLOWED_ACE Ace;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &hProcessToken))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }
    /* Work on a copy, so the process keeps its default DACL */
    if (!DuplicateTokenEx(hProcessToken, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, NULL, SecurityImpersonation,
                          TokenPrimary, &hToken))
    {
        skip("DuplicateTokenEx failed: %lu\n", GetLastError());
        CloseHandle(hProcessToken);
        return;
    }

    Original = QueryDefaultDacl(hToken, NULL);
    ok(Original != NULL && Original->DefaultDacl != NULL, "No default DACL\n");
    if (!Original || !Original->DefaultDacl)
        goto Cleanup;

    /* Add an ACE for Restricted Code, set the new DACL and read it back */
    ok(CreateWellKnownSid(WinRestrictedCodeSid, NULL, SidBuffer, &Size), "CreateWellKnownSid failed: %lu\n", GetLastError());
    Access.grfAccessMode = GRANT_ACCESS;
    Access.grfAccessPermissions = GENERIC_READ;
    Access.grfInheritance = NO_INHERITANCE;
    Access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    Access.Trustee.ptstrName = (LPWSTR)SidBuffer;
    ok(SetEntriesInAclW(1, &Access, Original->DefaultDacl, &Acl) == ERROR_SUCCESS, "SetEntriesInAcl failed\n");
    if (!Acl)
        goto Cleanup;
    NewDacl.DefaultDacl = Acl;
    ok(SetTokenInformation(hToken, TokenDefaultDacl, &NewDacl, sizeof(NewDacl)),
       "SetTokenInformation(TokenDefaultDacl) failed: %lu\n", GetLastError());

    Changed = QueryDefaultDacl(hToken, NULL);
    ok(Changed != NULL && Changed->DefaultDacl != NULL, "No default DACL after the change\n");
    if (Changed && Changed->DefaultDacl)
    {
        ok(Changed->DefaultDacl->AceCount == Acl->AceCount, "%u ACEs, set %u\n",
           Changed->DefaultDacl->AceCount, Acl->AceCount);
        ok(Changed->DefaultDacl->AclSize == Acl->AclSize, "ACL size %u, set %u\n",
           Changed->DefaultDacl->AclSize, Acl->AclSize);
        for (i = 0; i < Changed->DefaultDacl->AceCount; i++)
        {
            if (GetAce(Changed->DefaultDacl, i, (PVOID *)&Ace) && Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                EqualSid((PSID)&Ace->SidStart, SidBuffer))
            {
                Found = TRUE;
                ok(Ace->Mask == GENERIC_READ, "Mask 0x%lx\n", Ace->Mask);
            }
        }
        ok(Found, "The added ACE is missing\n");
        HeapFree(GetProcessHeap(), 0, Changed);
    }
    LocalFree(Acl);

Cleanup:
    if (Original)
        HeapFree(GetProcessHeap(), 0, Original);
    CloseHandle(hToken);
    CloseHandle(hProcessToken);
}
