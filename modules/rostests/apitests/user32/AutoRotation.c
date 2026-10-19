/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for GetDisplayAutoRotationPreferences, SetDisplayAutoRotationPreferences and GetAutoRotationState
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64) on a machine without a rotation sensor: the values that have
 * no bit outside 0xC000000F are accepted and kept for the process, any other one is an invalid parameter, and the
 * state is "no sensor" (0x10).
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_Get)(ULONG *);
typedef BOOL (WINAPI *PFN_Set)(ULONG);

#define PENDING_ERROR 0xdeadbeef

START_TEST(AutoRotation)
{
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    PFN_Get pGet = (PFN_Get)GetProcAddress(User32, "GetDisplayAutoRotationPreferences");
    PFN_Set pSet = (PFN_Set)GetProcAddress(User32, "SetDisplayAutoRotationPreferences");
    PFN_Get pState = (PFN_Get)GetProcAddress(User32, "GetAutoRotationState");
    static const ULONG Good[] = { 0, 1, 2, 3, 4, 5, 8, 9, 12, 15, 0x80000000, 0x80000001, 0x8000000f, 0x40000000, 0x40000001, 0xc0000000, 0xc000000f };
    static const ULONG Bad[] = { 0x10, 0x20, 0x40, 0x80, 0x100, 0x10000, 0x80000010, 0x80000100, 0x7fffffff, 0xffffffff };
    ULONG Original = 0x55, Value;
    DWORD Error;
    BOOL Result;
    ULONG i;

    /* Windows 7 and older have none; ReactOS has them from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pGet || !pSet || !pState)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "the auto rotation functions are not exported\n");
        skip("the auto rotation functions are not exported\n");
        return;
    }

    SetLastError(PENDING_ERROR);
    Result = pGet(&Original);
    Error = GetLastError();
    ok(Result == TRUE, "the preference was not read\n");
    ok(Error == PENDING_ERROR, "the last error became %lu\n", Error);

    for (i = 0; i < RTL_NUMBER_OF(Good); i++)
    {
        SetLastError(PENDING_ERROR);
        Result = pSet(Good[i]);
        Error = GetLastError();
        ok(Result == TRUE, "%#lx: not accepted\n", Good[i]);
        ok(Error == PENDING_ERROR, "%#lx: the last error became %lu\n", Good[i], Error);
        Value = 0x55;
        ok(pGet(&Value), "%#lx: not read back\n", Good[i]);
        ok(Value == Good[i], "%#lx: read back %#lx\n", Good[i], Value);
    }

    /* what is refused leaves the preference alone */
    ok(pSet(2), "2 is not accepted\n");
    for (i = 0; i < RTL_NUMBER_OF(Bad); i++)
    {
        SetLastError(PENDING_ERROR);
        Result = pSet(Bad[i]);
        Error = GetLastError();
        ok(Result == FALSE, "%#lx: accepted\n", Bad[i]);
        ok(Error == ERROR_INVALID_PARAMETER, "%#lx: last error %lu\n", Bad[i], Error);
        Value = 0x55;
        ok(pGet(&Value) && Value == 2, "%#lx: the preference became %#lx\n", Bad[i], Value);
    }

    SetLastError(PENDING_ERROR);
    Result = pGet(NULL);
    Error = GetLastError();
    ok(Result == FALSE, "no pointer: read\n");
    ok(Error == ERROR_INVALID_PARAMETER, "no pointer: last error %lu\n", Error);

    /* a machine without a rotation sensor */
    Value = 0x55;
    SetLastError(PENDING_ERROR);
    Result = pState(&Value);
    Error = GetLastError();
    ok(Result == TRUE, "the state was not read\n");
    /* ReactOS has no rotation sensor; other machines may have one, which the state then says */
    if (is_reactos())
        ok(Value == 0x10, "state %#lx\n", Value);
    ok(Error == PENDING_ERROR, "the last error became %lu\n", Error);
    SetLastError(PENDING_ERROR);
    Result = pState(NULL);
    Error = GetLastError();
    ok(Result == FALSE, "no pointer: state read\n");
    ok(Error == ERROR_INVALID_PARAMETER, "no pointer: last error %lu\n", Error);

    pSet(Original);
}
