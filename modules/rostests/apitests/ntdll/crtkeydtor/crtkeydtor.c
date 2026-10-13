/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DLL with the default CRT startup that registers a thread key destructor
 *              (built twice: without and with a TLS directory)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windef.h>
#include <winbase.h>

/* The CRT startup's thread key destructor registry (libgcc's gthreads use it) */
int __mingwthr_key_dtor(DWORD key, void (*dtor)(void *));

#ifdef WITH_TLS_DIRECTORY
/* Referencing it links the CRT's TLS directory and its callbacks */
extern ULONG _tls_index;
ULONG *volatile g_pTlsIndex = &_tls_index;
#endif

static DWORD g_Key = TLS_OUT_OF_INDEXES;
static volatile LONG g_DtorCalls = 0;
static BOOL g_FailAttach = FALSE;

/* Appends a letter to CRTKEYDTOR_TRACE, which outlives the DLL */
static void Trace(WCHAR Letter)
{
    WCHAR Buffer[32];
    DWORD Length = GetEnvironmentVariableW(L"CRTKEYDTOR_TRACE", Buffer, 30);

    if (Length >= 30)
        Length = 0;
    Buffer[Length] = Letter;
    Buffer[Length + 1] = L'\0';
    SetEnvironmentVariableW(L"CRTKEYDTOR_TRACE", Buffer);
}

static void KeyDtor(void *Value)
{
    if (Value == (void *)&g_Key)
    {
        InterlockedIncrement(&g_DtorCalls);
        if (g_FailAttach)
            Trace(L'D');
    }
}

BOOL WINAPI KeyDtorInit(void)
{
#ifdef WITH_TLS_DIRECTORY
    (void)*g_pTlsIndex;
#endif
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
    /* With CRTKEYDTOR_FAIL set, attach sets the key and fails */
    if (fdwReason == DLL_PROCESS_ATTACH && GetEnvironmentVariableW(L"CRTKEYDTOR_FAIL", NULL, 0))
    {
        g_FailAttach = TRUE;
        if (KeyDtorInit())
            KeyDtorSet();
        return FALSE;
    }
    if (fdwReason == DLL_PROCESS_DETACH && g_FailAttach)
        Trace(L'M');
    return TRUE;
}
