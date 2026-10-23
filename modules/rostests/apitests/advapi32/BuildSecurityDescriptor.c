/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for BuildSecurityDescriptorW/A
 *              (T90: Crashpad/Electron named-pipe security descriptor)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <aclapi.h>
#include <sddl.h>

/* Crashpad's named-pipe SDDL: SYSTEM full, app packages r/w, Untrusted label */
#define CRASHPAD_SDDL L"D:(A;;GA;;;SY)(A;;GWGR;;;S-1-15-2-1)S:(ML;;;;;S-1-16-0)"

#define SENTINEL ((PSECURITY_DESCRIPTOR)(ULONG_PTR)0xDEADBEEF)

START_TEST(BuildSecurityDescriptor)
{
    PSECURITY_DESCRIPTOR Base = NULL, NewSd;
    EXPLICIT_ACCESSW ea;
    ULONG Size;
    DWORD Ret;
    BOOL Ok, Present, Defaulted;
    PACL Dacl, Sacl;

    /* A NULL output must be rejected, never reported as success */
    Size = 0;
    Ret = BuildSecurityDescriptorW(NULL, NULL, 0, NULL, 0, NULL, NULL, &Size, NULL);
    ok(Ret == ERROR_INVALID_PARAMETER, "NULL pNewSD: Ret = %lu\n", Ret);

    Ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(CRASHPAD_SDDL,
                                                              SDDL_REVISION_1,
                                                              &Base, NULL);
    ok(Ok, "ConvertStringSD failed %lu\n", GetLastError());
    if (!Ok)
        return;

    BuildExplicitAccessWithNameW(&ea, (LPWSTR)L"CURRENT_USER", GENERIC_ALL,
                                 GRANT_ACCESS, NO_INHERITANCE);

    /*
     * Seed the outputs with sentinels. The old stub returned
     * FALSE (== ERROR_SUCCESS) WITHOUT writing them, so a caller would use an
     * uninitialized descriptor pointer (the T90 Crashpad 0x3E6 root cause).
     */
    NewSd = SENTINEL;
    Size = 0xDEADBEEF;
    Ret = BuildSecurityDescriptorW(NULL, NULL, 1, &ea, 0, NULL, Base, &Size, &NewSd);
    ok(Ret == ERROR_SUCCESS, "BuildSecurityDescriptorW = %lu\n", Ret);
    ok(NewSd != NULL && NewSd != SENTINEL, "NewSd was not written: %p\n", NewSd);
    ok(Size != 0 && Size != 0xDEADBEEF, "Size was not written: %lu\n", Size);

    if (Ret == ERROR_SUCCESS && NewSd != NULL && NewSd != SENTINEL)
    {
        ok(IsValidSecurityDescriptor(NewSd), "result descriptor is invalid\n");

        Dacl = NULL;
        Present = FALSE;
        Defaulted = FALSE;
        ok(GetSecurityDescriptorDacl(NewSd, &Present, &Dacl, &Defaulted),
           "GetSecurityDescriptorDacl failed %lu\n", GetLastError());
        ok(Present && Dacl != NULL, "DACL not present in result\n");
        /* SY + app-package + merged CURRENT_USER == at least 3 ACEs */
        if (Present && Dacl != NULL)
            ok(Dacl->AceCount >= 3, "DACL AceCount = %u (expected >= 3)\n", Dacl->AceCount);

        Sacl = NULL;
        Present = FALSE;
        Defaulted = FALSE;
        ok(GetSecurityDescriptorSacl(NewSd, &Present, &Sacl, &Defaulted),
           "GetSecurityDescriptorSacl failed %lu\n", GetLastError());
        ok(Present && Sacl != NULL, "SACL (mandatory label) was not carried over\n");
        if (Present && Sacl != NULL)
            ok(Sacl->AceCount >= 1, "SACL AceCount = %u (expected >= 1)\n", Sacl->AceCount);

        LocalFree(NewSd);
    }

    LocalFree(Base);
}
