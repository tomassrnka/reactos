/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for advapi32!EnableTraceEx2 (Windows 7 and later) with no tracing session present
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef ULONG (WINAPI *PFN_EnableTraceEx2)(ULONGLONG, const GUID *, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, PVOID);

#define PENDING_ERROR 0xdeadbeef
static PFN_EnableTraceEx2 pEnableTraceEx2;

/* The last error is set to the result, the arguments checked before a session is looked up always give 87 */
static void Check(ULONGLONG Handle, const GUID *Provider, ULONG Control, ULONG Expected, int Line)
{
    ULONG Result, Error;

    SetLastError(PENDING_ERROR);
    Result = pEnableTraceEx2(Handle, Provider, Control, 4, 0, 0, 0, NULL);
    Error = GetLastError();
    ok(Result == Expected, "line %d: got %lu, expected %lu\n", Line, Result, Expected);
    ok(Error == Expected, "line %d: last error %lu, expected %lu\n", Line, Error, Expected);
}

static const GUID TestProvider = { 0x4c1a3b86, 0x7d1e, 0x4a52, { 0x9e, 0x3b, 0x1f, 0x6a, 0x2d, 0x4e, 0x5c, 0x72 } };

START_TEST(EnableTraceEx2)
{
    HMODULE Advapi = GetModuleHandleW(L"advapi32.dll");
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    ULONG Control;

    /* a build for Windows Server 2003 has no event tracing of Windows Vista and later */
    if (!GetProcAddress(Kernel32, "GetTickCount64"))
    {
        skip("this system is older than Windows Vista\n");
        return;
    }

    pEnableTraceEx2 = (PFN_EnableTraceEx2)GetProcAddress(Advapi, "EnableTraceEx2");
    /* Windows Vista does not have it: it came with Windows 7 */
    if (!pEnableTraceEx2)
    {
        /* ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
        ok(!(is_reactos() && GetProcAddress(Kernel32, "K32EnumProcessModules")),
           "EnableTraceEx2 is not exported\n");
        skip("EnableTraceEx2 is not exported\n");
        return;
    }

    /*
     * A null session handle, a null provider and a control code past capture-state are refused before a
     * session is looked up. A real session handle is never used here: on Windows the call would act on it.
     */
    for (Control = 0; Control < 5; Control++)
        Check(0, &TestProvider, Control, ERROR_INVALID_PARAMETER, __LINE__);
    Check(0, NULL, 1, ERROR_INVALID_PARAMETER, __LINE__);
    Check(1, NULL, 1, ERROR_INVALID_PARAMETER, __LINE__);
    Check(1, NULL, 0, ERROR_INVALID_PARAMETER, __LINE__);
    Check(0x10000, NULL, 2, ERROR_INVALID_PARAMETER, __LINE__);
    Check(1, &TestProvider, 3, ERROR_INVALID_PARAMETER, __LINE__);
    Check(1, &TestProvider, 4, ERROR_INVALID_PARAMETER, __LINE__);
    Check(0x10000, &TestProvider, 7, ERROR_INVALID_PARAMETER, __LINE__);
    Check(0x7fffffffffffffffULL, &TestProvider, 3, ERROR_INVALID_PARAMETER, __LINE__);

    /* ReactOS has no session at all, so a well-formed request names a session that does not exist */
    if (is_reactos())
    {
        for (Control = 0; Control < 3; Control++)
        {
            Check(1, &TestProvider, Control, ERROR_WMI_INSTANCE_NOT_FOUND, __LINE__);
            Check(0x10000, &TestProvider, Control, ERROR_WMI_INSTANCE_NOT_FOUND, __LINE__);
        }
        ok_dec(pEnableTraceEx2(1, &TestProvider, 1, 0, 0, 0, 0, NULL), ERROR_WMI_INSTANCE_NOT_FOUND);
        ok_dec(pEnableTraceEx2(1, &TestProvider, 1, 255, 0, 0, 0, NULL), ERROR_WMI_INSTANCE_NOT_FOUND);
        ok_dec(pEnableTraceEx2(1, &TestProvider, 1, 4, ~0ULL, ~0ULL, 0, NULL), ERROR_WMI_INSTANCE_NOT_FOUND);
        ok_dec(pEnableTraceEx2(1, &TestProvider, 1, 4, 0, 0, 5, NULL), ERROR_WMI_INSTANCE_NOT_FOUND);
    }
}
