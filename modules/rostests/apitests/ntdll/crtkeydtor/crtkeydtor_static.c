/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Imports crtkeydtor(_tls).dll at load time; exits with its destructor count
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windef.h>
#include <winbase.h>

BOOL WINAPI KeyDtorInit(void);
VOID WINAPI KeyDtorSet(void);
LONG WINAPI KeyDtorCalls(void);

static DWORD WINAPI ThreadProc(LPVOID Parameter)
{
    KeyDtorSet();
    return 0;
}

int main(void)
{
    HANDLE hThread;

    if (!KeyDtorInit())
        return 100;
    hThread = CreateThread(NULL, 0, ThreadProc, NULL, 0, NULL);
    if (!hThread)
        return 101;
    if (WaitForSingleObject(hThread, 30000) != WAIT_OBJECT_0)
        return 102;
    CloseHandle(hThread);
    return (int)KeyDtorCalls();
}
