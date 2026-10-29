/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for RtlQueryProcessPlaceholderCompatibilityMode and RtlSetProcessPlaceholderCompatibilityMode
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Windows 10 1803 and later; PowerShell's FileSystem provider calls them. Measured on Windows 10 22H2: a process
 * starts in PHCM_APPLICATION_DEFAULT, modes 0 to 3 are taken and the previous one returned, others are refused.
 */

#include "precomp.h"

typedef CHAR (NTAPI *PFN_Query)(VOID);
typedef CHAR (NTAPI *PFN_Set)(CHAR);

START_TEST(RtlPlaceholderCompatibilityMode)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_Query pQuery = (PFN_Query)GetProcAddress(Ntdll, "RtlQueryProcessPlaceholderCompatibilityMode");
    PFN_Set pSet = (PFN_Set)GetProcAddress(Ntdll, "RtlSetProcessPlaceholderCompatibilityMode");
    CHAR Mode;

    if (!pQuery || !pSet)
    {
        skip("The placeholder compatibility functions are not exported\n");
        return;
    }

    ok(pQuery() == 0, "the process starts in mode %d\n", pQuery());

    Mode = pSet(2);
    ok(Mode == 0, "previous mode %d, expected 0\n", Mode);
    ok(pQuery() == 2, "mode %d, expected 2\n", pQuery());

    Mode = pSet(3);
    ok(Mode == 2, "previous mode %d, expected 2\n", Mode);
    ok(pQuery() == 3, "mode %d, expected 3\n", pQuery());

    Mode = pSet(4);
    ok(Mode == -1, "mode 4 gave %d, expected -1\n", Mode);
    Mode = pSet(-1);
    ok(Mode == -1, "mode -1 gave %d, expected -1\n", Mode);
    Mode = pSet(-2);
    ok(Mode == -1, "mode -2 gave %d, expected -1\n", Mode);
    ok(pQuery() == 3, "a refused mode changed it to %d\n", pQuery());

    Mode = pSet(1);
    ok(Mode == 3, "previous mode %d, expected 3\n", Mode);
    Mode = pSet(0);
    ok(Mode == 1, "previous mode %d, expected 1\n", Mode);
    ok(pQuery() == 0, "mode %d, expected 0\n", pQuery());
}
