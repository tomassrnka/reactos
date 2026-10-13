/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DLL with the default CRT startup that registers a thread key destructor
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windef.h>
#include <winbase.h>

/* The CRT startup's thread key destructor registry (libgcc's gthreads use it) */
int __mingwthr_key_dtor(DWORD key, void (*dtor)(void *));

static DWORD g_Key = TLS_OUT_OF_INDEXES;
static volatile LONG g_DtorCalls = 0;

static void KeyDtor(void *Value)
{
    if (Value == (void *)&g_Key)
        InterlockedIncrement(&g_DtorCalls);
}

BOOL WINAPI KeyDtorInit(void)
{
    if (g_Key == TLS_OUT_OF_INDEXES)
    {
        g_Key = TlsAlloc();
        if (g_Key == TLS_OUT_OF_INDEXES)
            return FALSE;
        if (__mingwthr_key_dtor(g_Key, KeyDtor) != 0)
            return FALSE;
    }
    return TRUE;
}

VOID WINAPI KeyDtorSet(void)
{
    TlsSetValue(g_Key, &g_Key);
}

LONG WINAPI KeyDtorCalls(void)
{
    return g_DtorCalls;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    return TRUE;
}
