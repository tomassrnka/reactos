/*
 * PROJECT:     ReactOS user32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The display auto rotation functions (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 on a machine without a rotation sensor: the preference that a process sets is kept
 * for that process only (another process reads 0), a value is accepted when it has no bits outside 0xC000000F (0 to 15 and the two top bits) and any
 * other one is an invalid parameter, a missing pointer is an invalid parameter, and the state is "no sensor"
 * (0x10). There is no sensor here either.
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>

#define NDEBUG
#include <debug.h>

#define AUTO_ROTATION_NO_SENSOR 0x10
#define ORIENTATION_PREFERENCE_MASK 0xC000000F

static LONG g_Preference;

BOOL
WINAPI
GetDisplayAutoRotationPreferences(
    _Out_ ULONG *pOrientation)
{
    if (!pOrientation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *pOrientation = (ULONG)g_Preference;
    return TRUE;
}

BOOL
WINAPI
SetDisplayAutoRotationPreferences(
    _In_ ULONG Orientation)
{
    if (Orientation & ~ORIENTATION_PREFERENCE_MASK)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    InterlockedExchange(&g_Preference, (LONG)Orientation);
    return TRUE;
}

BOOL
WINAPI
GetAutoRotationState(
    _Out_ ULONG *pState)
{
    if (!pState)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *pState = AUTO_ROTATION_NO_SENSOR;
    return TRUE;
}
