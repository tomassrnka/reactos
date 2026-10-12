/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         NTFS File System Management
 * FILE:            dll/win32/untfs/untfs.c
 * PURPOSE:         untfs DLL initialisation
 * PROGRAMMERS:     Pierre Schweitzer
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>

extern RTL_CRITICAL_SECTION UntfsChkdskLock;

INT WINAPI
DllMain(
    IN HINSTANCE hinstDLL,
    IN DWORD     dwReason,
    IN LPVOID    lpvReserved)
{
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(lpvReserved);

    if (dwReason == DLL_PROCESS_ATTACH)
        RtlInitializeCriticalSection(&UntfsChkdskLock);
    else if (dwReason == DLL_PROCESS_DETACH)
        RtlDeleteCriticalSection(&UntfsChkdskLock);
    return TRUE;
}
