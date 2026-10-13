/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for numbers next to rights names in the SDDL access rights field
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <sddl.h>

START_TEST(SddlRights)
{
    static const struct
    {
        PCSTR Sddl;
        BOOL Valid;
        ACCESS_MASK Mask;
    } Tests[] =
    {
        /* the descriptor the WiX Burn engine builds for its pipe */
        { "D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW0x00100000;;;WD)", TRUE, GENERIC_ALL },
        { "D:(A;;GRGW0x00100000;;;WD)",    TRUE,  GENERIC_READ | GENERIC_WRITE | 0x00100000 },
        { "D:(A;;0x00100000GRGW;;;WD)",    TRUE,  GENERIC_READ | GENERIC_WRITE | 0x00100000 },
        { "D:(A;;GR0x1;;;WD)",             TRUE,  GENERIC_READ | 1 },
        { "D:(A;;0x1GR0x2GW;;;WD)",        TRUE,  GENERIC_READ | GENERIC_WRITE | 3 },
        { "D:(A;;1;;;WD)",                 TRUE,  1 },
        { "D:(A;;GR3;;;WD)",               TRUE,  GENERIC_READ | 3 },
        { "D:(A;;020000000000;;;WD)",      TRUE,  GENERIC_READ },
        { "D:(A;;0X40000000;;;WD)",        TRUE,  GENERIC_WRITE },
        { "D:(A;;0xFFFFFFFF;;;WD)",        TRUE,  0xFFFFFFFF },
        { "D:(A;;0;;;WD)",                 TRUE,  0 },
        { "D:(A;;0x;;;WD)",                FALSE, 0 },
        { "D:(A;;GR0x;;;WD)",              FALSE, 0 },
        { "D:(A;;0x100000000;;;WD)",       TRUE,  0xFFFFFFFF },   /* a value above 32 bits saturates */
        { "D:(A;;08;;;WD)",                TRUE,  8 },            /* octal 0, then decimal 8 */
        { "D:(A;;GA ;;;WD)",               FALSE, 0 },
    };
    ULONG i;

    for (i = 0; i < ARRAYSIZE(Tests); i++)
    {
        PSECURITY_DESCRIPTOR Sd = NULL;
        BOOL Ret;

        SetLastError(0xdeadbeef);
        Ret = ConvertStringSecurityDescriptorToSecurityDescriptorA(Tests[i].Sddl, SDDL_REVISION_1, &Sd, NULL);
        ok(Ret == Tests[i].Valid, "[%s] returned %d, error %lu\n", Tests[i].Sddl, Ret, GetLastError());
        ok(!Ret || Sd != NULL, "[%s] no descriptor\n", Tests[i].Sddl);
        if (Ret && Sd)
        {
            BOOL Present = FALSE, Defaulted;
            PACL Dacl = NULL;
            PACCESS_ALLOWED_ACE Ace = NULL;

            ok(GetSecurityDescriptorDacl(Sd, &Present, &Dacl, &Defaulted) && Present && Dacl, "[%s] no DACL\n", Tests[i].Sddl);
            if (Dacl)
            {
                BOOL GotAce = GetAce(Dacl, 0, (PVOID *)&Ace);

                ok(GotAce, "[%s] no ACE\n", Tests[i].Sddl);
                if (GotAce)
                    ok(Ace->Mask == Tests[i].Mask, "[%s] mask 0x%08lx, expected 0x%08lx\n",
                       Tests[i].Sddl, Ace->Mask, Tests[i].Mask);
            }
            LocalFree(Sd);
        }
    }
}
