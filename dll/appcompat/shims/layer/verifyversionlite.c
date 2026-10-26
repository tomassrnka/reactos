/*
 * PROJECT:     ReactOS 'Layers' Shim library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VerifyVersionInfoLite shim
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <shimlib.h>

/*
 * FIXME: what the Windows shim changes in a VerifyVersionInfo call is not documented, and not (yet) established
 * here. It is registered with the hooks Windows gives it so that it can be selected by name, and forwards the
 * calls untouched, so an application that selects it sees the system's own answers.
 */
typedef BOOL(WINAPI* VERIFYVERSIONINFOAPROC)(LPOSVERSIONINFOEXA, DWORD, DWORDLONG);
typedef BOOL(WINAPI* VERIFYVERSIONINFOWPROC)(LPOSVERSIONINFOEXW, DWORD, DWORDLONG);

#define SHIM_NS         VerifyVersionInfoLite
#include <setup_shim.inl>

BOOL WINAPI SHIM_OBJ_NAME(APIHook_VerifyVersionInfoA)(LPOSVERSIONINFOEXA lpVersionInformation, DWORD dwTypeMask, DWORDLONG dwlConditionMask)
{
    return CALL_SHIM(0, VERIFYVERSIONINFOAPROC)(lpVersionInformation, dwTypeMask, dwlConditionMask);
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_VerifyVersionInfoW)(LPOSVERSIONINFOEXW lpVersionInformation, DWORD dwTypeMask, DWORDLONG dwlConditionMask)
{
    return CALL_SHIM(1, VERIFYVERSIONINFOWPROC)(lpVersionInformation, dwTypeMask, dwlConditionMask);
}

#define SHIM_NUM_HOOKS  2
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "VerifyVersionInfoA", SHIM_OBJ_NAME(APIHook_VerifyVersionInfoA)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "VerifyVersionInfoW", SHIM_OBJ_NAME(APIHook_VerifyVersionInfoW))

#include <implement_shim.inl>
