/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for CloseTrace
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <wmistr.h>
#include <evntrace.h>

typedef ULONG (WINAPI *PFN_CloseTrace)(TRACEHANDLE);

START_TEST(CloseTrace)
{
    PFN_CloseTrace pCloseTrace =
        (PFN_CloseTrace)GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "CloseTrace");
    TRACEHANDLE Handles[] = { 0, 1, 0x12345678, INVALID_PROCESSTRACE_HANDLE, ~(TRACEHANDLE)0 };
    ULONG i, Err, LastError;

    ok(pCloseTrace != NULL, "CloseTrace is not exported\n");
    if (!pCloseTrace)
    {
        skip("CloseTrace is missing\n");
        return;
    }

    /* none of these came from OpenTrace; Windows rejects them and leaves the last error alone */
    for (i = 0; i < _countof(Handles); i++)
    {
        SetLastError(0xdeadbeef);
        Err = pCloseTrace(Handles[i]);
        LastError = GetLastError();
        ok_dec(Err, ERROR_INVALID_HANDLE);
        ok_hex(LastError, 0xdeadbeef);
    }
}
