/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Process placeholder compatibility mode (Windows 10 1803 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The mode tells the file system how a process sees cloud file placeholders. There are no placeholders here, so
 * the mode is only kept and reported, as Windows does: the process starts in PHCM_APPLICATION_DEFAULT, a mode up
 * to PHCM_DISGUISE_FULL_PLACEHOLDER is taken and the previous one returned, anything else is refused with
 * PHCM_ERROR_INVALID_PARAMETER (measured on Windows 10 22H2). PowerShell's FileSystem provider calls this pair.
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

#define PHCM_APPLICATION_DEFAULT        ((CHAR)0)
#define PHCM_DISGUISE_FULL_PLACEHOLDER  ((CHAR)3)
#define PHCM_ERROR_INVALID_PARAMETER    ((CHAR)-1)

static CHAR RtlpProcessPlaceholderCompatibilityMode = PHCM_APPLICATION_DEFAULT;

CHAR
NTAPI
RtlQueryProcessPlaceholderCompatibilityMode(VOID)
{
    return *(volatile CHAR *)&RtlpProcessPlaceholderCompatibilityMode;
}

CHAR
NTAPI
RtlSetProcessPlaceholderCompatibilityMode(
    _In_ CHAR Mode)
{
    if (Mode < PHCM_APPLICATION_DEFAULT || Mode > PHCM_DISGUISE_FULL_PLACEHOLDER)
        return PHCM_ERROR_INVALID_PARAMETER;

    return _InterlockedExchange8(&RtlpProcessPlaceholderCompatibilityMode, Mode);
}
