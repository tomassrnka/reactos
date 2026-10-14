/*
 * PROJECT:     appshim_apitest
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Reports which Windows version this process sees; the exit code is a bit mask
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <stdio.h>
#include "sysmain_probe.h"

typedef LONG (WINAPI *RTLGETVERSIONPROC)(OSVERSIONINFOW *);
typedef BOOL (WINAPI *VERIFYVERSIONINFOWPROC)(OSVERSIONINFOEXW *, DWORD, DWORDLONG);
typedef ULONGLONG (WINAPI *VERSETCONDITIONMASKPROC)(ULONGLONG, DWORD, BYTE);

static BOOL AtLeast1809(DWORD Major, DWORD Minor, DWORD Build)
{
    return Major > 10 || (Major == 10 && (Minor > 0 || Build >= PROBE_BUILD));
}

int main(void)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    RTLGETVERSIONPROC pRtlGetVersion = (RTLGETVERSIONPROC)GetProcAddress(Ntdll, "RtlGetVersion");
    VERIFYVERSIONINFOWPROC pVerifyVersionInfoW = (VERIFYVERSIONINFOWPROC)GetProcAddress(Kernel32, "VerifyVersionInfoW");
    VERSETCONDITIONMASKPROC pVerSetConditionMask = (VERSETCONDITIONMASKPROC)GetProcAddress(Kernel32, "VerSetConditionMask");
    OSVERSIONINFOEXW Info = { sizeof(Info) };
    OSVERSIONINFOW Rtl = { sizeof(Rtl) };
    WCHAR Build[32] = L"";
    DWORD Size = sizeof(Build), Mask = 0;
    HKEY Key;
    int Exit = 0;

    /* Without a manifest naming Windows 10, GetVersionExW says 6.2 on Windows 10; the probe has one */
    if (GetVersionExW((OSVERSIONINFOW *)&Info))
    {
        printf("GetVersionExW %lu.%lu.%lu\n", Info.dwMajorVersion, Info.dwMinorVersion, Info.dwBuildNumber);
        if (AtLeast1809(Info.dwMajorVersion, Info.dwMinorVersion, Info.dwBuildNumber))
            Exit |= PROBE_GETVERSIONEX;
    }

    if (pRtlGetVersion && pRtlGetVersion(&Rtl) == 0)
    {
        printf("RtlGetVersion %lu.%lu.%lu\n", Rtl.dwMajorVersion, Rtl.dwMinorVersion, Rtl.dwBuildNumber);
        if (AtLeast1809(Rtl.dwMajorVersion, Rtl.dwMinorVersion, Rtl.dwBuildNumber))
            Exit |= PROBE_RTLGETVERSION;
    }

    if (pVerifyVersionInfoW && pVerSetConditionMask)
    {
        OSVERSIONINFOEXW Want = { sizeof(Want), 10, 0, PROBE_BUILD };
        ULONGLONG Cond = 0;

        Cond = pVerSetConditionMask(Cond, VER_MAJORVERSION, VER_GREATER_EQUAL);
        Cond = pVerSetConditionMask(Cond, VER_MINORVERSION, VER_GREATER_EQUAL);
        Cond = pVerSetConditionMask(Cond, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        Mask = VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER;
        if (pVerifyVersionInfoW(&Want, Mask, Cond))
        {
            printf("VerifyVersionInfoW 10.0.%u: yes\n", PROBE_BUILD);
            Exit |= PROBE_VERIFYVERSION;
        }
        else
            printf("VerifyVersionInfoW 10.0.%u: no\n", PROBE_BUILD);
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(Key, L"CurrentBuildNumber", NULL, NULL, (BYTE *)Build, &Size) == ERROR_SUCCESS)
        {
            printf("CurrentBuildNumber %ls\n", Build);
            if (_wtoi(Build) >= PROBE_BUILD)
                Exit |= PROBE_REGISTRY;
        }
        RegCloseKey(Key);
    }

    return Exit;
}
