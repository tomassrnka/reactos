/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Base cryptographic API DLL
 * FILE:        dll/win32/cryptbase/stubs.c
 * PURPOSE:     DllMain for the cryptbase.dll forwarder
 */

#include <windef.h>

BOOL
WINAPI
DllMain(HINSTANCE InstDLL,
        DWORD Reason,
        LPVOID Reserved)
{
    return TRUE;
}
