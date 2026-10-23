/*
 * PROJECT:     appshim_apitest
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Reports the Windows version this process sees via GetVersion and RtlGetVersion.
 *              Exit code = (gvMajor<<24)|(gvMinor<<16)|(rtlMajor<<8)|rtlMinor.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI *RTLGETVERSIONPROC)(OSVERSIONINFOW *);

int main(void)
{
    DWORD v = GetVersion();
    DWORD gvMajor = LOBYTE(LOWORD(v));
    DWORD gvMinor = HIBYTE(LOWORD(v));
    DWORD rtlMajor = 0, rtlMinor = 0;
    OSVERSIONINFOW Rtl = { sizeof(Rtl) };
    RTLGETVERSIONPROC pRtlGetVersion =
        (RTLGETVERSIONPROC)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");

    if (pRtlGetVersion && pRtlGetVersion(&Rtl) == 0)
    {
        rtlMajor = Rtl.dwMajorVersion;
        rtlMinor = Rtl.dwMinorVersion;
    }
    printf("GetVersion %lu.%lu RtlGetVersion %lu.%lu\n", gvMajor, gvMinor, rtlMajor, rtlMinor);
    return (int)(((gvMajor & 0xFF) << 24) | ((gvMinor & 0xFF) << 16) |
                 ((rtlMajor & 0xFF) << 8) | (rtlMinor & 0xFF));
}
