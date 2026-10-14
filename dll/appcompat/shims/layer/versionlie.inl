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

#ifdef VERSIONLIE_NT6

/* Programs that ask whether Windows is at least some version go through VerifyVersionInfo */
static BOOL SHIM_OBJ_NAME(VerifyResult)(NTSTATUS Status)
{
    if (NT_SUCCESS(Status))
        return TRUE;
    /* The same errors kernel32 reports */
    SetLastError(Status == STATUS_REVISION_MISMATCH ? ERROR_OLD_WIN_VERSION : ERROR_BAD_ARGUMENTS);
    return FALSE;
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_VerifyVersionInfoA)(LPOSVERSIONINFOEXA lpVersionInformation, DWORD dwTypeMask, DWORDLONG dwlConditionMask)
{
    if (!lpVersionInformation)
    {
        SetLastError(ERROR_BAD_ARGUMENTS);
        return FALSE;
    }
    return SHIM_OBJ_NAME(VerifyResult)(VerifyFakeVersionA(lpVersionInformation, dwTypeMask, dwlConditionMask, &VERSION_INFO));
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_VerifyVersionInfoW)(LPOSVERSIONINFOEXW lpVersionInformation, DWORD dwTypeMask, DWORDLONG dwlConditionMask)
{
    if (!lpVersionInformation)
    {
        SetLastError(ERROR_BAD_ARGUMENTS);
        return FALSE;
    }
    return SHIM_OBJ_NAME(VerifyResult)(VerifyFakeVersionW(lpVersionInformation, dwTypeMask, dwlConditionMask, &VERSION_INFO));
}

NTSTATUS NTAPI SHIM_OBJ_NAME(APIHook_RtlVerifyVersionInfo)(PRTL_OSVERSIONINFOEXW VersionInfo, ULONG TypeMask, ULONGLONG ConditionMask)
{
    if (!VersionInfo)
        return STATUS_INVALID_PARAMETER;
    return VerifyFakeVersionW((LPOSVERSIONINFOEXW)VersionInfo, TypeMask, ConditionMask, &VERSION_INFO);
}

VOID NTAPI SHIM_OBJ_NAME(APIHook_RtlGetNtVersionNumbers)(PULONG pMajor, PULONG pMinor, PULONG pBuild)
{
    if (pMajor)
        *pMajor = VERSION_INFO.dwMajorVersion;
    if (pMinor)
        *pMinor = VERSION_INFO.dwMinorVersion;
    /* The build number of a retail system carries the 'free build' bits */
    if (pBuild)
        *pBuild = VERSION_INFO.dwBuildNumber | 0xF0000000;
}

LONG WINAPI SHIM_OBJ_NAME(APIHook_RegQueryValueExA)(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    WCHAR szValue[40];
    FakeRegValue Value;

    if (!lpReserved && AnsiToWide(lpValueName, szValue, RTL_NUMBER_OF(szValue)))
    {
        switch (GetFakeRegValue(&VERSION_INFO, hKey, NULL, szValue, &Value))
        {
        case FAKEREG_VALUE:
            return ReturnFakeRegValue(&Value, TRUE, lpType, lpData, lpcbData);
        case FAKEREG_ABSENT:
            return ReturnAbsentRegValue(lpType, lpData, lpcbData);
        default:
            break;
        }
    }
    return CALL_SHIM(8, REGQUERYVALUEEXAPROC)(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

LONG WINAPI SHIM_OBJ_NAME(APIHook_RegQueryValueExW)(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    FakeRegValue Value;

    if (!lpReserved)
    {
        switch (GetFakeRegValue(&VERSION_INFO, hKey, NULL, lpValueName, &Value))
        {
        case FAKEREG_VALUE:
            return ReturnFakeRegValue(&Value, FALSE, lpType, lpData, lpcbData);
        case FAKEREG_ABSENT:
            return ReturnAbsentRegValue(lpType, lpData, lpcbData);
        default:
            break;
        }
    }
    return CALL_SHIM(9, REGQUERYVALUEEXWPROC)(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

LONG WINAPI SHIM_OBJ_NAME(APIHook_RegGetValueA)(HKEY hKey, LPCSTR lpSubKey, LPCSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    WCHAR szSubKey[MAX_PATH], szValue[40];
    FakeRegValue Value;

    if (FakeRegFlagsOk(dwFlags) && AnsiToWide(lpValue, szValue, RTL_NUMBER_OF(szValue)) &&
        (!lpSubKey || !*lpSubKey || AnsiToWide(lpSubKey, szSubKey, RTL_NUMBER_OF(szSubKey))))
    {
        switch (GetFakeRegValue(&VERSION_INFO, hKey, (lpSubKey && *lpSubKey) ? szSubKey : NULL, szValue, &Value))
        {
        case FAKEREG_VALUE:
            return ReturnFakeRegGetValue(&Value, TRUE, dwFlags, pdwType, pvData, pcbData);
        case FAKEREG_ABSENT:
            return ReturnAbsentRegGetValue(dwFlags, pdwType, pvData, pcbData);
        default:
            break;
        }
    }
    return CALL_SHIM(10, REGGETVALUEAPROC)(hKey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
}

LONG WINAPI SHIM_OBJ_NAME(APIHook_RegGetValueW)(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    FakeRegValue Value;

    if (FakeRegFlagsOk(dwFlags))
    {
        switch (GetFakeRegValue(&VERSION_INFO, hKey, lpSubKey, lpValue, &Value))
        {
        case FAKEREG_VALUE:
            return ReturnFakeRegGetValue(&Value, FALSE, dwFlags, pdwType, pvData, pcbData);
        case FAKEREG_ABSENT:
            return ReturnAbsentRegGetValue(dwFlags, pdwType, pvData, pcbData);
        default:
            break;
        }
    }
    return CALL_SHIM(11, REGGETVALUEWPROC)(hKey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
}

#endif /* VERSIONLIE_NT6 */

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


#ifdef VERSIONLIE_NT6
#define SHIM_NUM_HOOKS  12
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "GetVersionExA", SHIM_OBJ_NAME(APIHook_GetVersionExA)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "GetVersionExW", SHIM_OBJ_NAME(APIHook_GetVersionExW)) \
    SHIM_HOOK(2, "KERNEL32.DLL", "GetVersion", SHIM_OBJ_NAME(APIHook_GetVersion)) \
    SHIM_HOOK(3, "NTDLL.DLL", "RtlGetVersion", SHIM_OBJ_NAME(APIHook_RtlGetVersion)) \
    SHIM_HOOK(4, "KERNEL32.DLL", "VerifyVersionInfoA", SHIM_OBJ_NAME(APIHook_VerifyVersionInfoA)) \
    SHIM_HOOK(5, "KERNEL32.DLL", "VerifyVersionInfoW", SHIM_OBJ_NAME(APIHook_VerifyVersionInfoW)) \
    SHIM_HOOK(6, "NTDLL.DLL", "RtlVerifyVersionInfo", SHIM_OBJ_NAME(APIHook_RtlVerifyVersionInfo)) \
    SHIM_HOOK(7, "NTDLL.DLL", "RtlGetNtVersionNumbers", SHIM_OBJ_NAME(APIHook_RtlGetNtVersionNumbers)) \
    SHIM_HOOK(8, "ADVAPI32.DLL", "RegQueryValueExA", SHIM_OBJ_NAME(APIHook_RegQueryValueExA)) \
    SHIM_HOOK(9, "ADVAPI32.DLL", "RegQueryValueExW", SHIM_OBJ_NAME(APIHook_RegQueryValueExW)) \
    SHIM_HOOK(10, "ADVAPI32.DLL", "RegGetValueA", SHIM_OBJ_NAME(APIHook_RegGetValueA)) \
    SHIM_HOOK(11, "ADVAPI32.DLL", "RegGetValueW", SHIM_OBJ_NAME(APIHook_RegGetValueW))
#else
#define SHIM_NUM_HOOKS  4
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "GetVersionExA", SHIM_OBJ_NAME(APIHook_GetVersionExA)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "GetVersionExW", SHIM_OBJ_NAME(APIHook_GetVersionExW)) \
    SHIM_HOOK(2, "KERNEL32.DLL", "GetVersion", SHIM_OBJ_NAME(APIHook_GetVersion)) \
    SHIM_HOOK(3, "NTDLL.DLL", "RtlGetVersion", SHIM_OBJ_NAME(APIHook_RtlGetVersion))
#endif
#define SHIM_NOTIFY_FN SHIM_OBJ_NAME(Notify)

#include <implement_shim.inl>

#undef VERSION_INFO
#undef VERSIONLIE_NT6

