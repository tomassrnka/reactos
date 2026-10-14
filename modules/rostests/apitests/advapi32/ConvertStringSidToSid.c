/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for ConvertStringSidToSidW with malformed and unusual strings
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <sddl.h>

typedef struct SID_ENTRY
{
    INT line;
    PCWSTR pszSid;
    BOOL bValid;
    BYTE Revision;
    ULONGLONG Authority;
    BYTE cSubAuthorities;
    DWORD SubAuthority[4];
} SID_ENTRY;

/* Measured on Windows Server 2008 R2 and Windows 10 */
static const SID_ENTRY s_Entries[] =
{
    { __LINE__, L"S-1-5-4", TRUE, 1, 5, 1, { 4 } },
    { __LINE__, L"s-1-5-4", TRUE, 1, 5, 1, { 4 } },
    { __LINE__, L"S-1-5-32-544", TRUE, 1, 5, 2, { 32, 544 } },
    { __LINE__, L"S-1-5-21-1-2-3", TRUE, 1, 5, 4, { 21, 1, 2, 3 } },
    { __LINE__, L"S-2-1-1", TRUE, 2, 1, 1, { 1 } },
    { __LINE__, L"S-1-0x10-4", TRUE, 1, 16, 1, { 4 } },
    { __LINE__, L"S-0x1-0XC-0x1a", TRUE, 1, 12, 1, { 26 } },
    { __LINE__, L"S-1-0xFFFFFFFFFFFF-1", TRUE, 1, 0xFFFFFFFFFFFFULL, 1, { 1 } },
    { __LINE__, L"S-1-4294967296-1", TRUE, 1, 0x100000000ULL, 1, { 1 } },
    { __LINE__, L"S-1-5-0x20-544", TRUE, 1, 5, 2, { 32, 544 } },
    { __LINE__, L"S-255-1-1", TRUE, 255, 1, 1, { 1 } },
    { __LINE__, L"S-01-1-1", TRUE, 1, 1, 1, { 1 } },
    { __LINE__, L"S-1-5-021", TRUE, 1, 5, 1, { 21 } },
    { __LINE__, L"S-1-5-4294967295-4294967296", TRUE, 1, 5, 2, { 0xFFFFFFFF, 0xFFFFFFFF } },
    /* The two-letter constants */
    { __LINE__, L"WD", TRUE, 1, 1, 1, { 0 } },
    { __LINE__, L"wD", TRUE, 1, 1, 1, { 0 } },
    { __LINE__, L"BA", TRUE, 1, 5, 2, { 32, 544 } },
    /* A sub authority above 32 bits is the largest 32-bit value */
    { __LINE__, L"S-1-5-4294967295", TRUE, 1, 5, 1, { 0xFFFFFFFF } },
    { __LINE__, L"S-1-5-4294967296", TRUE, 1, 5, 1, { 0xFFFFFFFF } },
    { __LINE__, L"", FALSE },
    { __LINE__, L"S", FALSE },
    { __LINE__, L"S-", FALSE },
    { __LINE__, L"S-1", FALSE },
    { __LINE__, L"S-1-", FALSE },
    { __LINE__, L"S-1-5", FALSE },
    { __LINE__, L"S-1-5-", FALSE },
    { __LINE__, L"S-1--5", FALSE },
    { __LINE__, L"S-1-5-32-544x", FALSE },
    { __LINE__, L"S-256-1-1", FALSE },
    { __LINE__, L"S-1-0x1000000000000-1", FALSE },
    { __LINE__, L"S-1-5-0x", FALSE },
    { __LINE__, L" S-1-5-4", FALSE },
    { __LINE__, L"S-1-5-4 ", FALSE },
    { __LINE__, L"XX", FALSE },
    { __LINE__, L"WDandmorecharacters", FALSE },
    { __LINE__, L"WD)", FALSE },
    { __LINE__, L"W", FALSE },
};

START_TEST(ConvertStringSidToSid)
{
    UINT i, j;

    for (i = 0; i < _countof(s_Entries); i++)
    {
        const SID_ENTRY *pEntry = &s_Entries[i];
        SID *pSid = NULL;
        BOOL ret;
        DWORD Error;

        /* An old error that looks like a failure of the conversion must not be reported */
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        ret = ConvertStringSidToSidW(pEntry->pszSid, (PSID *)&pSid);
        Error = GetLastError();
        ok(ret == pEntry->bValid, "Line %d: [%S] returned %d, expected %d\n",
           pEntry->line, pEntry->pszSid, ret, pEntry->bValid);
        if (!pEntry->bValid)
        {
            ok(Error == ERROR_INVALID_SID, "Line %d: [%S] error %lu, expected ERROR_INVALID_SID\n",
               pEntry->line, pEntry->pszSid, Error);
            if (ret)
                LocalFree(pSid);
            continue;
        }
        if (!ret)
            continue;
        ok(pSid != NULL, "Line %d: [%S] returned a NULL SID\n", pEntry->line, pEntry->pszSid);
        if (!pSid)
            continue;

        ok(pSid->Revision == pEntry->Revision, "Line %d: [%S] revision %u, expected %u\n",
           pEntry->line, pEntry->pszSid, pSid->Revision, pEntry->Revision);
        ok(pSid->SubAuthorityCount == pEntry->cSubAuthorities,
           "Line %d: [%S] %u sub authorities, expected %u\n",
           pEntry->line, pEntry->pszSid, pSid->SubAuthorityCount, pEntry->cSubAuthorities);
        {
            ULONGLONG Authority = 0;

            for (j = 0; j < 6; j++)
                Authority = (Authority << 8) | pSid->IdentifierAuthority.Value[j];
            ok(Authority == pEntry->Authority, "Line %d: [%S] authority %I64x, expected %I64x\n",
               pEntry->line, pEntry->pszSid, Authority, pEntry->Authority);
        }
        for (j = 0; j < pSid->SubAuthorityCount && j < pEntry->cSubAuthorities; j++)
        {
            ok(pSid->SubAuthority[j] == pEntry->SubAuthority[j],
               "Line %d: [%S] sub authority %u is %lu, expected %lu\n",
               pEntry->line, pEntry->pszSid, j, pSid->SubAuthority[j], pEntry->SubAuthority[j]);
        }
        LocalFree(pSid);
    }
}
