/*
 * PROJECT:     ReactOS Print Spooler DLL API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for DeletePrinter
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winspool.h>

static void
TestDeleteWithoutAdminAccess(PWSTR pwszPrinter, PPRINTER_DEFAULTSW pDefaults, PCSTR pszWhat)
{
    BOOL bResult;
    HANDLE hPrinter;

    if (!OpenPrinterW(pwszPrinter, &hPrinter, pDefaults))
    {
        ok(FALSE, "%s: OpenPrinterW failed with error %lu\n", pszWhat, GetLastError());
        return;
    }

    SetLastError(0xDEADBEEF);
    bResult = DeletePrinter(hPrinter);
    ok(!bResult, "%s: DeletePrinter returned %d\n", pszWhat, bResult);
    ok(GetLastError() == ERROR_ACCESS_DENIED, "%s: DeletePrinter error %lu\n", pszWhat, GetLastError());
    ClosePrinter(hPrinter);

    /* The printer must still be there */
    bResult = OpenPrinterW(pwszPrinter, &hPrinter, NULL);
    ok(bResult, "%s: printer '%S' cannot be opened anymore, error %lu\n", pszWhat, pwszPrinter, GetLastError());
    if (bResult)
        ClosePrinter(hPrinter);
}

START_TEST(DeletePrinter)
{
    BOOL bResult;
    DWORD cchPrinter;
    PRINTER_DEFAULTSW Defaults = { NULL, NULL, PRINTER_ACCESS_USE };
    WCHAR wszPrinter[MAX_PATH];

    SetLastError(0xDEADBEEF);
    bResult = DeletePrinter(NULL);
    ok(!bResult, "DeletePrinter(NULL) returned %d\n", bResult);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "DeletePrinter(NULL) error %lu\n", GetLastError());

    cchPrinter = _countof(wszPrinter);
    if (!GetDefaultPrinterW(wszPrinter, &cchPrinter))
    {
        skip("No default printer, error %lu\n", GetLastError());
        return;
    }

    TestDeleteWithoutAdminAccess(wszPrinter, NULL, "no defaults");
    TestDeleteWithoutAdminAccess(wszPrinter, &Defaults, "PRINTER_ACCESS_USE");
}
