/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     DLL with a TLS directory, loaded at run time by the DynamicTls test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#define NTOS_MODE_USER
#include <ndk/pstypes.h>

/* Referencing _tls_index links the CRT TLS directory into this DLL: its template starts at _tls_start */
extern ULONG _tls_index;
extern char _tls_start;

#ifdef _MSC_VER
#pragma section(".tls$AAB", read, write)
#pragma section(".CRT$XLB", read)
#define TLS_DATA __declspec(allocate(".tls$AAB"))
#define TLS_CALLBACK __declspec(allocate(".CRT$XLB"))
#else
#define TLS_DATA __attribute__((section(".tls$AAB"), used))
#define TLS_CALLBACK __attribute__((section(".CRT$XLB"), used))
#endif

/* A variable in the TLS template, reached the way compilers reach __declspec(thread) data */
static TLS_DATA ULONG TlsValue = 0x5EEDF00D;

/*
 * What happened, in order: 'T' + reason for TLS callbacks, 'M' + reason for
 * DllMain. Kept here until the host gives a log of its own, which outlives
 * the DLL (to see the detach).
 */
#define EVENTS_SIZE 64
static char Events[EVENTS_SIZE];
static LONG EventCount;
static char *Log = Events;
static LONG *LogCount = &EventCount;

static void Record(char Kind, DWORD Reason)
{
    LONG i = InterlockedExchangeAdd(LogCount, 2);
    if (i + 1 < EVENTS_SIZE - 1)
    {
        Log[i] = Kind;
        Log[i + 1] = (char)('0' + Reason);
    }
}

static VOID NTAPI TlsCallback(PVOID DllHandle, DWORD Reason, PVOID Reserved)
{
    Record('T', Reason);
}

static TLS_CALLBACK PIMAGE_TLS_CALLBACK TlsCallbackEntry = TlsCallback;

static ULONG *ThreadValue(VOID)
{
    PVOID *TlsVector = (PVOID *)NtCurrentTeb()->ThreadLocalStoragePointer;
    return (ULONG *)((char *)TlsVector[_tls_index] + ((char *)&TlsValue - &_tls_start));
}

ULONG WINAPI TlsDynGetValue(VOID)
{
    return *ThreadValue();
}

VOID WINAPI TlsDynSetValue(ULONG Value)
{
    *ThreadValue() = Value;
}

ULONG WINAPI TlsDynGetIndex(VOID)
{
    return _tls_index;
}

/* Where the template is in the image */
PVOID WINAPI TlsDynGetTemplate(VOID)
{
    return &TlsValue;
}

PCSTR WINAPI TlsDynGetEvents(VOID)
{
    return Events;
}

/* Events from now on go to the host's log of EVENTS_SIZE bytes, zeroed */
VOID WINAPI TlsDynSetLog(char *HostLog, LONG *HostCount)
{
    LogCount = HostCount;
    Log = HostLog;
}

/* What FlsGetValue gives for FlsWatchIndex when a thread detaches */
static DWORD FlsWatchIndex = FLS_OUT_OF_INDEXES;
static PVOID FlsAtDetach = (PVOID)1;

VOID WINAPI TlsDynWatchFls(DWORD Index)
{
    FlsWatchIndex = Index;
    FlsAtDetach = (PVOID)1;
}

PVOID WINAPI TlsDynGetFlsAtDetach(VOID)
{
    return FlsAtDetach;
}

/* A value that DLL_THREAD_DETACH stores with FlsSetValue, the first FLS of a thread that had none */
static DWORD FlsSetIndex = FLS_OUT_OF_INDEXES;
static PVOID FlsSetData;
static BOOL FlsSetHadNoData, FlsSetResult;

VOID WINAPI TlsDynSetFlsAtDetach(DWORD Index, PVOID Data)
{
    FlsSetHadNoData = FlsSetResult = FALSE;
    FlsSetData = Data;
    FlsSetIndex = Index;
}

/* Whether the last detach found the thread without FLS data, and what its FlsSetValue returned */
BOOL WINAPI TlsDynGetFlsSetResult(BOOL *HadNoData)
{
    *HadNoData = FlsSetHadNoData;
    return FlsSetResult;
}

/* The events so far go to this handle at the process detach, the last thing a program that exits can still do */
static HANDLE DumpHandle;

VOID WINAPI TlsDynDumpAtProcessDetach(HANDLE Handle)
{
    DumpHandle = Handle;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    Record('M', fdwReason);
    if (fdwReason == DLL_PROCESS_DETACH && DumpHandle)
    {
        DWORD Written;
        LONG Count = *LogCount;
        if (Count > EVENTS_SIZE - 1)
            Count = EVENTS_SIZE - 1;
        WriteFile(DumpHandle, Log, Count, &Written, NULL);
    }
    if (fdwReason == DLL_THREAD_DETACH && FlsWatchIndex != FLS_OUT_OF_INDEXES)
        FlsAtDetach = FlsGetValue(FlsWatchIndex);
    if (fdwReason == DLL_THREAD_DETACH && FlsSetIndex != FLS_OUT_OF_INDEXES)
    {
        FlsSetHadNoData = (NtCurrentTeb()->FlsData == NULL);
        FlsSetResult = FlsSetValue(FlsSetIndex, FlsSetData);
    }
    return TRUE;
}
