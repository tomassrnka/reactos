/*
 * PROJECT:     ReactOS 'Layers' Shim library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Version lie implementation helper
 * COPYRIGHT:   Copyright 2016,2017 Mark Jansen <mark.jansen@reactos.org>
 */

#include <setup_shim.inl>

DWORD WINAPI SHIM_OBJ_NAME(APIHook_GetVersion)()
{
    return VERSION_INFO.FullVersion;
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExA)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(0, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &VERSION_INFO);
    }
    return FALSE;
}

/* We do not care about the actual type, FakeVersion will correctly handle it either way */
BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExW)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(1, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &VERSION_INFO);
    }
    return FALSE;
}

#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER 0xC000000DL
#define STATUS_SUCCESS 0
#endif

/* We do not care about the actual type, FakeVersion will correctly handle it either way */
DWORD WINAPI SHIM_OBJ_NAME(APIHook_RtlGetVersion)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(3, GETVERSIONEXAPROC)(lpOsVersionInfo) == STATUS_SUCCESS)
    {
        return FakeVersion(lpOsVersionInfo, &VERSION_INFO) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    return STATUS_INVALID_PARAMETER;
}

#ifdef SYSWOW64_GUEST
/* MSYS consults this entry after RtlGetVersion. Keep the explicitly selected
 * guest version layer consistent across both queries. */
VOID WINAPI SHIM_OBJ_NAME(APIHook_RtlGetNtVersionNumbers)(PDWORD Major, PDWORD Minor, PDWORD Build)
{
    if (Major) *Major = VERSION_INFO.dwMajorVersion;
    if (Minor) *Minor = VERSION_INFO.dwMinorVersion;
    if (Build) *Build = VERSION_INFO.dwBuildNumber | 0xf0000000;
}
#endif

BOOL WINAPI SHIM_OBJ_NAME(Notify)(DWORD fdwReason, PVOID ptr)
{
    if (fdwReason == SHIM_NOTIFY_ATTACH && VERSION_INFO.wServicePackMajor)
    {
        static CONST WCHAR szServicePack[] = {'S','e','r','v','i','c','e',' ','P','a','c','k',' ','%','u',0};
        HRESULT hr = StringCbPrintfA(VERSION_INFO.szCSDVersionA, sizeof(VERSION_INFO.szCSDVersionA),
            "Service Pack %u", VERSION_INFO.wServicePackMajor);
        if (FAILED(hr))
            return FALSE;
        hr = StringCbPrintfW(VERSION_INFO.szCSDVersionW, sizeof(VERSION_INFO.szCSDVersionW),
            szServicePack, VERSION_INFO.wServicePackMajor);
        if (FAILED(hr))
            return FALSE;
    }
    return TRUE;
}


#ifdef SYSWOW64_GUEST
#define SHIM_NUM_HOOKS  5
#define SHIM_VERSION_NUMBERS_HOOK \
    SHIM_HOOK(4, "NTDLL.DLL", "RtlGetNtVersionNumbers", SHIM_OBJ_NAME(APIHook_RtlGetNtVersionNumbers))
#else
#define SHIM_NUM_HOOKS  4
#define SHIM_VERSION_NUMBERS_HOOK
#endif
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "GetVersionExA", SHIM_OBJ_NAME(APIHook_GetVersionExA)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "GetVersionExW", SHIM_OBJ_NAME(APIHook_GetVersionExW)) \
    SHIM_HOOK(2, "KERNEL32.DLL", "GetVersion", SHIM_OBJ_NAME(APIHook_GetVersion)) \
    SHIM_HOOK(3, "NTDLL.DLL", "RtlGetVersion", SHIM_OBJ_NAME(APIHook_RtlGetVersion)) \
    SHIM_VERSION_NUMBERS_HOOK
#define SHIM_NOTIFY_FN SHIM_OBJ_NAME(Notify)

#include <implement_shim.inl>

#undef SHIM_VERSION_NUMBERS_HOOK
#undef VERSION_INFO
