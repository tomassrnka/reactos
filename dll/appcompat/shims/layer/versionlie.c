/*
 * PROJECT:     ReactOS 'Layers' Shim library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Version lie shims
 * COPYRIGHT:   Copyright 2016,2017 Mark Jansen <mark.jansen@reactos.org>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winnls.h>
#include <shimlib.h>
#include <strsafe.h>
#include "ntndk.h"

/* Generic version info to use with the shims */
typedef struct VersionLieInfo
{
    DWORD FullVersion;
    WORD dwMajorVersion;
    WORD dwMinorVersion;
    DWORD dwBuildNumber;
    WORD dwPlatformId;
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    char szCSDVersionA[20];
    WCHAR szCSDVersionW[20];
    /* What the registry says about this version, NT 6 and later only (NULL: leave the registry alone) */
    PCWSTR pszCurrentVersion;
    PCWSTR pszProductName;
    DWORD dwRegMajorVersion;    /* CurrentMajorVersionNumber, 0: the value does not exist */
    DWORD dwRegMinorVersion;    /* CurrentMinorVersionNumber */
} VersionLieInfo;

#define REG_NT6(Ver, Name)      .pszCurrentVersion = L##Ver, .pszProductName = L##Name
#define REG_NT10(Ver, Name)     REG_NT6(Ver, Name), .dwRegMajorVersion = 10, .dwRegMinorVersion = 0

VersionLieInfo g_Win95 = { 0xC3B60004, 4, 0, 950, VER_PLATFORM_WIN32_WINDOWS, 0, 0 };
VersionLieInfo g_WinNT4SP5 = { 0x05650004, 4, 0, 1381, VER_PLATFORM_WIN32_NT, 5, 0 };
VersionLieInfo g_Win98 = { 0xC0000A04, 4, 10, 0x040A08AE, VER_PLATFORM_WIN32_WINDOWS, 0, 0 };

VersionLieInfo g_Win2000 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 0, 0 };
VersionLieInfo g_Win2000SP1 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 1, 0 };
VersionLieInfo g_Win2000SP2 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 2, 0 };
VersionLieInfo g_Win2000SP3 = { 0x08930005, 5, 0, 2195, VER_PLATFORM_WIN32_NT, 3, 0 };

VersionLieInfo g_WinXP = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 0, 0 };
VersionLieInfo g_WinXPSP1 = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 1, 0 };
VersionLieInfo g_WinXPSP2 = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 2, 0 };
VersionLieInfo g_WinXPSP3 = { 0x0a280105, 5, 1, 2600, VER_PLATFORM_WIN32_NT, 3, 0 };

VersionLieInfo g_Win2k3RTM = { 0x0ece0205, 5, 2, 3790, VER_PLATFORM_WIN32_NT, 0, 0 };
VersionLieInfo g_Win2k3SP1 = { 0x0ece0205, 5, 2, 3790, VER_PLATFORM_WIN32_NT, 1, 0 };

VersionLieInfo g_WinVistaRTM = { 0x17700006, 6, 0, 6000, VER_PLATFORM_WIN32_NT, 0, 0, .pszCurrentVersion = L"6.0", .pszProductName = L"Windows Vista (TM) Business" };
VersionLieInfo g_WinVistaSP1 = { 0x17710006, 6, 0, 6001, VER_PLATFORM_WIN32_NT, 1, 0, .pszCurrentVersion = L"6.0", .pszProductName = L"Windows Vista (TM) Business" };
VersionLieInfo g_WinVistaSP2 = { 0x17720006, 6, 0, 6002, VER_PLATFORM_WIN32_NT, 2, 0, .pszCurrentVersion = L"6.0", .pszProductName = L"Windows Vista (TM) Business" };

VersionLieInfo g_Win7RTM = { 0x1db00106, 6, 1, 7600, VER_PLATFORM_WIN32_NT, 0, 0, REG_NT6("6.1", "Windows 7 Professional") };
VersionLieInfo g_Win7SP1 = { 0x1db10106, 6, 1, 7601, VER_PLATFORM_WIN32_NT, 1, 0, REG_NT6("6.1", "Windows 7 Professional") };

VersionLieInfo g_Win8RTM = { 0x23f00206, 6, 2, 9200, VER_PLATFORM_WIN32_NT, 0, 0, REG_NT6("6.2", "Windows 8 Pro") };
VersionLieInfo g_Win81RTM = { 0x25800306, 6, 3, 9600, VER_PLATFORM_WIN32_NT, 0, 0, REG_NT6("6.3", "Windows 8.1 Pro") };

VersionLieInfo g_Win10RTM = { 0x47ba000a, 10, 0, 18362, VER_PLATFORM_WIN32_NT, 0, 0, REG_NT10("6.3", "Windows 10 Pro") };

VersionLieInfo g_Win2k16RTM = { 0x3fab000a, 10, 0, 16299, VER_PLATFORM_WIN32_NT, 0, 0, REG_NT10("6.3", "Windows Server 2016 Standard") };
VersionLieInfo g_Win2k19RTM = { 0x4563000a, 10, 0, 17763, VER_PLATFORM_WIN32_NT, 0, 0, REG_NT10("6.3", "Windows Server 2019 Standard") };

/* Fill the OSVERSIONINFO[EX][W|A] struct with the info from the generic VersionLieInfo */

BOOL FakeVersion(LPOSVERSIONINFOEXA pResult, VersionLieInfo* pFake)
{
    if (pResult->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA) || pResult->dwOSVersionInfoSize == sizeof(OSVERSIONINFOA) ||
        pResult->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW) || pResult->dwOSVersionInfoSize == sizeof(OSVERSIONINFOW))
    {
        pResult->dwMajorVersion = pFake->dwMajorVersion;
        pResult->dwMinorVersion = pFake->dwMinorVersion;
        pResult->dwBuildNumber = pFake->dwBuildNumber;
        pResult->dwPlatformId = pFake->dwPlatformId;
        if (pResult->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA) || pResult->dwOSVersionInfoSize == sizeof(OSVERSIONINFOA))
        {
            if (FAILED(StringCbCopyA(pResult->szCSDVersion, sizeof(pResult->szCSDVersion), pFake->szCSDVersionA)))
                return FALSE;
            if (pResult->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA) && pFake->dwPlatformId != VER_PLATFORM_WIN32_WINDOWS)
            {
                pResult->wServicePackMajor = pFake->wServicePackMajor;
                pResult->wServicePackMinor = pFake->wServicePackMinor;
            }
        }
        else
        {
            LPOSVERSIONINFOEXW pResultW = (LPOSVERSIONINFOEXW)pResult;
            if (FAILED(StringCbCopyW(pResultW->szCSDVersion, sizeof(pResultW->szCSDVersion), pFake->szCSDVersionW)))
                return FALSE;
            if (pResultW->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW) && pFake->dwPlatformId != VER_PLATFORM_WIN32_WINDOWS)
            {
                pResultW->wServicePackMajor = pFake->wServicePackMajor;
                pResultW->wServicePackMinor = pFake->wServicePackMinor;
            }
        }
        return TRUE;
    }
    return FALSE;
}


typedef BOOL(WINAPI* GETVERSIONEXAPROC)(LPOSVERSIONINFOEXA);
typedef BOOL(WINAPI* VERIFYVERSIONINFOAPROC)(LPOSVERSIONINFOEXA, DWORD, DWORDLONG);
typedef BOOL(WINAPI* VERIFYVERSIONINFOWPROC)(LPOSVERSIONINFOEXW, DWORD, DWORDLONG);
typedef NTSTATUS(NTAPI* RTLVERIFYVERSIONINFOPROC)(PRTL_OSVERSIONINFOEXW, ULONG, ULONGLONG);
typedef LONG(WINAPI* REGQUERYVALUEEXWPROC)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI* REGQUERYVALUEEXAPROC)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI* REGGETVALUEWPROC)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef LONG(WINAPI* REGGETVALUEAPROC)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);

#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY   0x00010000
#define RRF_SUBKEY_WOW6432KEY   0x00020000
#endif

#ifndef STATUS_REVISION_MISMATCH
#define STATUS_REVISION_MISMATCH ((NTSTATUS)0xC0000059L)
#endif


/* Compare one field of the version the program asked about with the one we pretend to be */
static BOOL VerCompare(DWORD Have, DWORD Want, DWORD Condition)
{
    switch (Condition)
    {
    case VER_EQUAL:         return Have == Want;
    case VER_GREATER:       return Have > Want;
    case VER_GREATER_EQUAL: return Have >= Want;
    case VER_LESS:          return Have < Want;
    case VER_LESS_EQUAL:    return Have <= Want;
    }
    return FALSE;
}

/* The condition of one field is 3 bits of the mask; the position is the number of the field's type bit */
static DWORD VerCondition(DWORDLONG ConditionMask, DWORD TypeBit)
{
    DWORD Shift = 0;
    while (!(TypeBit & 1))
    {
        TypeBit >>= 1;
        Shift++;
    }
    return (DWORD)(ConditionMask >> (Shift * VER_NUM_BITS_PER_CONDITION_MASK)) & VER_CONDITION_MASK;
}

/*
 * Answer VerifyVersionInfo / RtlVerifyVersionInfo as if the operating system were pFake.
 * 'Have' holds what the operating system really reports for the fields the lie does not cover
 * (product type, suite mask); the lie is applied on top of it.
 *
 * Major, minor, service pack major and service pack minor are one version that is compared from the
 * left. A field that is equal hands over to the next one; the condition of the first field is kept
 * for the later ones unless it is "equal", in which case each field brings its own. The last field
 * that is looked at decides, also when it is equal.
 */
static NTSTATUS VerifyFakeVersion(const RTL_OSVERSIONINFOEXW* Want, DWORD TypeMask, DWORDLONG ConditionMask,
                                  RTL_OSVERSIONINFOEXW* Have, VersionLieInfo* pFake)
{
    static const DWORD ChainBits[4] = { VER_MAJORVERSION, VER_MINORVERSION, VER_SERVICEPACKMAJOR, VER_SERVICEPACKMINOR };
    DWORD HaveChain[4], WantChain[4];
    DWORD Condition = VER_EQUAL, n;
    BOOL Satisfied = TRUE;

    if (!TypeMask || !ConditionMask)
        return STATUS_INVALID_PARAMETER;

    if (!FakeVersion((LPOSVERSIONINFOEXA)Have, pFake))
        return STATUS_INVALID_PARAMETER;

    if (TypeMask & VER_PRODUCT_TYPE)
    {
        if (!VerCompare(Have->wProductType, Want->wProductType, VerCondition(ConditionMask, VER_PRODUCT_TYPE)))
            return STATUS_REVISION_MISMATCH;
    }

    if (TypeMask & VER_SUITENAME)
    {
        switch (VerCondition(ConditionMask, VER_SUITENAME))
        {
        case VER_AND:
            if ((Want->wSuiteMask & Have->wSuiteMask) != Want->wSuiteMask)
                return STATUS_REVISION_MISMATCH;
            break;
        case VER_OR:
            if (Want->wSuiteMask && !(Want->wSuiteMask & Have->wSuiteMask))
                return STATUS_REVISION_MISMATCH;
            break;
        default:
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (TypeMask & VER_PLATFORMID)
    {
        if (!VerCompare(Have->dwPlatformId, Want->dwPlatformId, VerCondition(ConditionMask, VER_PLATFORMID)))
            return STATUS_REVISION_MISMATCH;
    }

    if (TypeMask & VER_BUILDNUMBER)
    {
        if (!VerCompare(Have->dwBuildNumber, Want->dwBuildNumber, VerCondition(ConditionMask, VER_BUILDNUMBER)))
            return STATUS_REVISION_MISMATCH;
    }

    HaveChain[0] = Have->dwMajorVersion;        WantChain[0] = Want->dwMajorVersion;
    HaveChain[1] = Have->dwMinorVersion;        WantChain[1] = Want->dwMinorVersion;
    HaveChain[2] = Have->wServicePackMajor;     WantChain[2] = Want->wServicePackMajor;
    HaveChain[3] = Have->wServicePackMinor;     WantChain[3] = Want->wServicePackMinor;

    for (n = 0; n < 4; ++n)
    {
        if (!(TypeMask & ChainBits[n]))
            continue;

        if (Condition == VER_EQUAL)
            Condition = VerCondition(ConditionMask, ChainBits[n]);

        Satisfied = VerCompare(HaveChain[n], WantChain[n], Condition);

        /* A field that differs decides; an equal one leaves the decision to the next field, or to itself when it is the last */
        if (HaveChain[n] != WantChain[n])
            break;
    }

    return Satisfied ? STATUS_SUCCESS : STATUS_REVISION_MISMATCH;
}

/* The wanted version as wide structure */
static NTSTATUS VerifyFakeVersionW(LPOSVERSIONINFOEXW Want, DWORD TypeMask, DWORDLONG ConditionMask, VersionLieInfo* pFake)
{
    RTL_OSVERSIONINFOEXW Have;

    RtlZeroMemory(&Have, sizeof(Have));
    Have.dwOSVersionInfoSize = sizeof(Have);
    if (RtlGetVersion((PRTL_OSVERSIONINFOW)&Have) != STATUS_SUCCESS)
        return STATUS_INVALID_PARAMETER;

    return VerifyFakeVersion((PRTL_OSVERSIONINFOEXW)Want, TypeMask, ConditionMask, &Have, pFake);
}

/* The same for an ANSI structure: only the numbers matter, the layout differs after the CSD string */
static NTSTATUS VerifyFakeVersionA(LPOSVERSIONINFOEXA Want, DWORD TypeMask, DWORDLONG ConditionMask, VersionLieInfo* pFake)
{
    RTL_OSVERSIONINFOEXW WantW;

    RtlZeroMemory(&WantW, sizeof(WantW));
    WantW.dwOSVersionInfoSize = sizeof(WantW);
    WantW.dwMajorVersion = Want->dwMajorVersion;
    WantW.dwMinorVersion = Want->dwMinorVersion;
    WantW.dwBuildNumber = Want->dwBuildNumber;
    WantW.dwPlatformId = Want->dwPlatformId;
    WantW.wServicePackMajor = Want->wServicePackMajor;
    WantW.wServicePackMinor = Want->wServicePackMinor;
    WantW.wSuiteMask = Want->wSuiteMask;
    WantW.wProductType = Want->wProductType;

    return VerifyFakeVersionW((LPOSVERSIONINFOEXW)&WantW, TypeMask, ConditionMask, pFake);
}


/*
 * Registry values installers read to learn the Windows version. They are answered for the
 * values and key that Windows keeps them in, so other keys and values are never touched.
 */
typedef struct FakeRegValue
{
    DWORD Type;
    DWORD Size;     /* bytes, including the terminator of a string */
    union
    {
        WCHAR Str[40];
        DWORD Dword;
    } u;
} FakeRegValue;

typedef enum FakeRegResult
{
    FAKEREG_NONE,       /* not ours, the real registry answers */
    FAKEREG_VALUE,      /* the value of the lie */
    FAKEREG_ABSENT      /* a value this version does not have */
} FakeRegResult;

/* The caller got the registry functions from advapi32, so it is loaded; the shim does not import it */
static LONG OpenKeyForQuery(HKEY hKey, PCWSTR pszSubKey, PHKEY phKey)
{
    typedef LONG(WINAPI* REGOPENKEYEXWPROC)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    HMODULE hAdvapi32 = GetModuleHandleW(L"advapi32.dll");
    REGOPENKEYEXWPROC pOpen = hAdvapi32 ? (REGOPENKEYEXWPROC)GetProcAddress(hAdvapi32, "RegOpenKeyExW") : NULL;

    return pOpen ? pOpen(hKey, pszSubKey, 0, KEY_QUERY_VALUE, phKey) : ERROR_PROC_NOT_FOUND;
}

/* Is this the key HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion (opened for reading), as seen through hKey and pszSubKey */
static BOOL IsCurrentVersionKey(HKEY hKey, PCWSTR pszSubKey)
{
    static const WCHAR szLocal[] = L"\\REGISTRY\\MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    static const WCHAR szWow[] = L"\\REGISTRY\\MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion";
    union
    {
        KEY_NAME_INFORMATION Info;
        WCHAR Buffer[200];
    } Name;
    OBJECT_BASIC_INFORMATION Basic;
    HKEY hOpened = NULL, hCheck = hKey;
    BOOL IsIt = FALSE;
    BOOL Predefined = HandleToULong(hKey) >= 0x80000000;
    ULONG Result;

    if (pszSubKey && *pszSubKey)
    {
        /* Let the registry resolve the path: predefined key overrides and registry views apply */
        if (OpenKeyForQuery(hKey, pszSubKey, &hOpened) != ERROR_SUCCESS)
            return FALSE;
        hCheck = hOpened;
    }
    else if (Predefined)
    {
        /* A root key is not the CurrentVersion key */
        return FALSE;
    }
    else if (!NT_SUCCESS(NtQueryObject(hKey, ObjectBasicInformation, &Basic, sizeof(Basic), &Result)) ||
             !(Basic.GrantedAccess & KEY_QUERY_VALUE))
    {
        /* Reading values needs the right; the real call reports the error */
        return FALSE;
    }

    if (NT_SUCCESS(NtQueryKey(hCheck, KeyNameInformation, &Name, sizeof(Name), &Result)))
    {
        ULONG Length = Name.Info.NameLength / sizeof(WCHAR);

        /* The name is not terminated */
        if (Length == RTL_NUMBER_OF(szLocal) - 1 && !_wcsnicmp(Name.Info.Name, szLocal, Length))
            IsIt = TRUE;
        else if (Length == RTL_NUMBER_OF(szWow) - 1 && !_wcsnicmp(Name.Info.Name, szWow, Length))
            IsIt = TRUE;
    }

    if (hOpened)
        NtClose(hOpened);
    return IsIt;
}

static FakeRegResult GetFakeRegValue(VersionLieInfo* pFake, HKEY hKey, PCWSTR pszSubKey, PCWSTR pszValue, FakeRegValue* Out)
{
    PCWSTR pszString = NULL;
    WCHAR szNumber[12];
    BOOL Absent = FALSE;

    if (!pFake->pszCurrentVersion || !pszValue)
        return FAKEREG_NONE;

    Out->Type = REG_SZ;
    if (!_wcsicmp(pszValue, L"CurrentVersion"))
    {
        pszString = pFake->pszCurrentVersion;
    }
    else if (!_wcsicmp(pszValue, L"CurrentBuild") || !_wcsicmp(pszValue, L"CurrentBuildNumber"))
    {
        StringCchPrintfW(szNumber, RTL_NUMBER_OF(szNumber), L"%u", pFake->dwBuildNumber);
        pszString = szNumber;
    }
    else if (!_wcsicmp(pszValue, L"ProductName"))
    {
        pszString = pFake->pszProductName;
    }
    else if (!_wcsicmp(pszValue, L"CurrentMajorVersionNumber") || !_wcsicmp(pszValue, L"CurrentMinorVersionNumber"))
    {
        /* Windows 10 has these two, the versions before it do not */
        Absent = !pFake->dwRegMajorVersion;
        Out->Type = REG_DWORD;
        Out->Size = sizeof(DWORD);
        Out->u.Dword = !_wcsnicmp(pszValue, L"CurrentMajor", 12) ? pFake->dwRegMajorVersion : pFake->dwRegMinorVersion;
    }
    else
    {
        return FAKEREG_NONE;
    }

    if (Out->Type == REG_SZ)
    {
        if (!pszString || FAILED(StringCchCopyW(Out->u.Str, RTL_NUMBER_OF(Out->u.Str), pszString)))
            return FAKEREG_NONE;
        Out->Size = (DWORD)(wcslen(Out->u.Str) + 1) * sizeof(WCHAR);
    }

    if (!IsCurrentVersionKey(hKey, pszSubKey))
        return FAKEREG_NONE;
    return Absent ? FAKEREG_ABSENT : FAKEREG_VALUE;
}

/* The value as the bytes the caller gets */
static DWORD FakeRegData(const FakeRegValue* Value, BOOL Ansi, BYTE* Raw)
{
    if (Ansi && Value->Type == REG_SZ)
    {
        DWORD n, Chars = Value->Size / sizeof(WCHAR);

        for (n = 0; n < Chars; ++n)
            Raw[n] = (BYTE)Value->u.Str[n];
        return Chars;
    }
    memcpy(Raw, &Value->u, Value->Size);
    return Value->Size;
}

/* RegQueryValueEx for a value of the lie. The size is read first: type and size may be the same variable */
static LONG ReturnFakeRegValue(const FakeRegValue* Value, BOOL Ansi, LPDWORD lpType, PVOID lpData, LPDWORD lpcbData)
{
    BYTE Raw[sizeof(Value->u)];
    DWORD Size = FakeRegData(Value, Ansi, Raw);
    DWORD cbIn = lpcbData ? *lpcbData : 0;

    if (lpData && !lpcbData)
        return ERROR_INVALID_PARAMETER;

    if (lpType)
        *lpType = Value->Type;
    if (!lpcbData)
        return ERROR_SUCCESS;
    if (!lpData)
    {
        *lpcbData = Size;
        return ERROR_SUCCESS;
    }
    if (cbIn < Size)
    {
        *lpcbData = Size;
        return ERROR_MORE_DATA;
    }
    memcpy(lpData, Raw, Size);
    *lpcbData = Size;
    return ERROR_SUCCESS;
}

/* RegQueryValueEx for a value this version does not have: the type is cleared, the size only when it is asked for */
static LONG ReturnAbsentRegValue(LPDWORD lpType, PVOID lpData, LPDWORD lpcbData)
{
    if (lpData && !lpcbData)
        return ERROR_INVALID_PARAMETER;
    if (lpType)
        *lpType = REG_NONE;
    if (!lpData && lpcbData)
        *lpcbData = 0;
    return ERROR_FILE_NOT_FOUND;
}

/*
 * RegGetValue for a value of the lie. The type and size are reported and the data is copied even when
 * the type is not one the caller asked for (the call then fails with ERROR_UNSUPPORTED_TYPE); a
 * failure zeroes the buffer when RRF_ZEROONFAILURE is set.
 */
static LONG ReturnFakeRegGetValue(const FakeRegValue* Value, BOOL Ansi, DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    BYTE Raw[sizeof(Value->u)];
    DWORD Size = FakeRegData(Value, Ansi, Raw);
    DWORD cbIn = pcbData ? *pcbData : 0;
    LONG ret = ERROR_SUCCESS;

    if (pvData && !pcbData)
        return ERROR_INVALID_PARAMETER;

    if (pdwType)
        *pdwType = Value->Type;
    if (pcbData)
    {
        if (!pvData)
        {
            *pcbData = Size;
        }
        else if (cbIn < Size)
        {
            *pcbData = Size;
            ret = ERROR_MORE_DATA;
        }
        else
        {
            memcpy(pvData, Raw, Size);
            *pcbData = Size;
        }
    }

    /* The type restriction applies to a value that did not fit too */
    if (ret == ERROR_SUCCESS || ret == ERROR_MORE_DATA)
    {
        BOOL Allowed = (Value->Type == REG_SZ) ? !!(dwFlags & RRF_RT_REG_SZ) : !!(dwFlags & RRF_RT_REG_DWORD);
        if (!Allowed)
            ret = ERROR_UNSUPPORTED_TYPE;
    }

    if (ret != ERROR_SUCCESS && pvData && (dwFlags & RRF_ZEROONFAILURE))
        memset(pvData, 0, cbIn);
    return ret;
}

/* RegGetValue for a value this version does not have */
static LONG ReturnAbsentRegGetValue(DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    DWORD cbIn = pcbData ? *pcbData : 0;
    LONG ret = ReturnAbsentRegValue(pdwType, pvData, pcbData);

    if (pvData && (dwFlags & RRF_ZEROONFAILURE))
        memset(pvData, 0, cbIn);
    return ret;
}

/* RegGetValue flags the fake handles itself; for anything else the real function decides */
static BOOL FakeRegFlagsOk(DWORD dwFlags)
{
    if (!(dwFlags & RRF_RT_ANY))
        return FALSE;
    /* Expanding strings has to be refused or allowed by the real function */
    if ((dwFlags & RRF_RT_REG_EXPAND_SZ) && !(dwFlags & RRF_NOEXPAND) && (dwFlags & RRF_RT_ANY) != RRF_RT_ANY)
        return FALSE;
    if ((dwFlags & (RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY)) == (RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY))
        return FALSE;
    return TRUE;
}

static BOOL AnsiToWide(LPCSTR pszIn, PWSTR pszOut, size_t cchOut)
{
    if (!pszIn || !*pszIn)
        return FALSE;
    return MultiByteToWideChar(CP_ACP, 0, pszIn, -1, pszOut, (int)cchOut) > 0;
}



#define SHIM_NS         Win95VersionLie
#include <setup_shim.inl>

DWORD WINAPI SHIM_OBJ_NAME(APIHook_GetVersion)()
{
    return g_Win95.FullVersion;
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExA)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(1, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &g_Win95);
    }
    return FALSE;
}

/* We do not call about the actual type, FakeVersion will correctly handle it either way */
BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExW)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(2, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &g_Win95);
    }
    return FALSE;
}

#define SHIM_NUM_HOOKS  3
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "GetVersion", SHIM_OBJ_NAME(APIHook_GetVersion)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "GetVersionExA", SHIM_OBJ_NAME(APIHook_GetVersionExA)) \
    SHIM_HOOK(2, "KERNEL32.DLL", "GetVersionExW", SHIM_OBJ_NAME(APIHook_GetVersionExW))

#include <implement_shim.inl>




#define SHIM_NS         Win98VersionLie
#include <setup_shim.inl>

DWORD WINAPI SHIM_OBJ_NAME(APIHook_GetVersion)()
{
    return g_Win98.FullVersion;
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExA)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(0, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &g_Win98);
    }
    return FALSE;
}

/* We do not call about the actual type, FakeVersion will correctly handle it either way */
BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExW)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(1, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &g_Win98);
    }
    return FALSE;
}

#define SHIM_NUM_HOOKS  3
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(2, "KERNEL32.DLL", "GetVersion", SHIM_OBJ_NAME(APIHook_GetVersion)) \
    SHIM_HOOK(0, "KERNEL32.DLL", "GetVersionExA", SHIM_OBJ_NAME(APIHook_GetVersionExA)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "GetVersionExW", SHIM_OBJ_NAME(APIHook_GetVersionExW))

#include <implement_shim.inl>



#define SHIM_NS         WinNT4SP5VersionLie
#define VERSION_INFO    g_WinNT4SP5
#include "versionlie.inl"


#define SHIM_NS         Win2000VersionLie
#define VERSION_INFO    g_Win2000
#include "versionlie.inl"

#define SHIM_NS         Win2000SP1VersionLie
#define VERSION_INFO    g_Win2000SP1
#include "versionlie.inl"

#define SHIM_NS         Win2000SP2VersionLie
#define VERSION_INFO    g_Win2000SP2
#include "versionlie.inl"

#define SHIM_NS         Win2000SP3VersionLie
#define VERSION_INFO    g_Win2000SP3
#include "versionlie.inl"


#define SHIM_NS         WinXPVersionLie
#define VERSION_INFO    g_WinXP
#include "versionlie.inl"

#define SHIM_NS         WinXPSP1VersionLie
#define VERSION_INFO    g_WinXPSP1
#include "versionlie.inl"

#define SHIM_NS         WinXPSP2VersionLie
#define VERSION_INFO    g_WinXPSP2
#include "versionlie.inl"

#define SHIM_NS         WinXPSP3VersionLie
#define VERSION_INFO    g_WinXPSP3
#include "versionlie.inl"


#define SHIM_NS         Win2k3RTMVersionLie
#define VERSION_INFO    g_Win2k3RTM
#include "versionlie.inl"

#define SHIM_NS         Win2k3SP1VersionLie
#define VERSION_INFO    g_Win2k3SP1
#include "versionlie.inl"


#define SHIM_NS         VistaRTMVersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_WinVistaRTM
#include "versionlie.inl"

#define SHIM_NS         VistaSP1VersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_WinVistaSP1
#include "versionlie.inl"

#define SHIM_NS         VistaSP2VersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_WinVistaSP2
#include "versionlie.inl"


#define SHIM_NS         Win7RTMVersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_Win7RTM
#include "versionlie.inl"

#define SHIM_NS         Win7SP1VersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_Win7SP1
#include "versionlie.inl"


#define SHIM_NS         Win8RTMVersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_Win8RTM
#include "versionlie.inl"

#define SHIM_NS         Win81RTMVersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_Win81RTM
#include "versionlie.inl"


#define SHIM_NS         Win10RTMVersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_Win10RTM
#include "versionlie.inl"


#define SHIM_NS         Win2k16RTMVersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_Win2k16RTM
#include "versionlie.inl"


#define SHIM_NS         Win2k19RTMVersionLie
#define VERSIONLIE_NT6
#define VERSION_INFO    g_Win2k19RTM
#include "versionlie.inl"
