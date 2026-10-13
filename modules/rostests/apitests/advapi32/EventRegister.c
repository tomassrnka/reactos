/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for EventRegister, EventUnregister, EventEnabled, EventProviderEnabled and EventWrite
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <evntprov.h>

typedef ULONG (WINAPI *FN_EventRegister)(LPCGUID, PENABLECALLBACK, PVOID, PREGHANDLE);
typedef ULONG (WINAPI *FN_EventUnregister)(REGHANDLE);
typedef BOOLEAN (WINAPI *FN_EventEnabled)(REGHANDLE, PCEVENT_DESCRIPTOR);
typedef BOOLEAN (WINAPI *FN_EventProviderEnabled)(REGHANDLE, UCHAR, ULONGLONG);
typedef ULONG (WINAPI *FN_EventWrite)(REGHANDLE, PCEVENT_DESCRIPTOR, ULONG, PEVENT_DATA_DESCRIPTOR);
typedef ULONG (WINAPI *FN_EventWriteString)(REGHANDLE, UCHAR, ULONGLONG, PCWSTR);

/* A provider no session knows: {6a3c1b5e-8f20-4d7a-9c41-0b7e2f9d3a18} */
static const GUID TestProvider = { 0x6a3c1b5e, 0x8f20, 0x4d7a, { 0x9c, 0x41, 0x0b, 0x7e, 0x2f, 0x9d, 0x3a, 0x18 } };

static LONG CallbackCalls;

static VOID NTAPI EnableCallback(LPCGUID SourceId, ULONG IsEnabled, UCHAR Level, ULONGLONG MatchAnyKeyword,
                                 ULONGLONG MatchAllKeyword, PEVENT_FILTER_DESCRIPTOR FilterData, PVOID CallbackContext)
{
    InterlockedIncrement(&CallbackCalls);
}

START_TEST(EventRegister)
{
    HMODULE hAdvapi32 = GetModuleHandleW(L"advapi32.dll");
    FN_EventRegister pEventRegister;
    FN_EventUnregister pEventUnregister;
    FN_EventEnabled pEventEnabled;
    FN_EventProviderEnabled pEventProviderEnabled;
    FN_EventWrite pEventWrite;
    FN_EventWriteString pEventWriteString;
    EVENT_DESCRIPTOR Descriptor = { 1, 0, 0, 4, 0, 0, 0 };
    EVENT_DATA_DESCRIPTOR Data;
    REGHANDLE Handle, Handle2;
    ULONG Value = 42, Error;

    pEventRegister = (FN_EventRegister)GetProcAddress(hAdvapi32, "EventRegister");
    pEventUnregister = (FN_EventUnregister)GetProcAddress(hAdvapi32, "EventUnregister");
    pEventEnabled = (FN_EventEnabled)GetProcAddress(hAdvapi32, "EventEnabled");
    pEventProviderEnabled = (FN_EventProviderEnabled)GetProcAddress(hAdvapi32, "EventProviderEnabled");
    pEventWrite = (FN_EventWrite)GetProcAddress(hAdvapi32, "EventWrite");
    pEventWriteString = (FN_EventWriteString)GetProcAddress(hAdvapi32, "EventWriteString");
    ok(pEventRegister && pEventUnregister && pEventEnabled && pEventProviderEnabled && pEventWrite && pEventWriteString,
       "Event provider functions are not exported\n");
    if (!pEventRegister || !pEventUnregister || !pEventEnabled || !pEventProviderEnabled || !pEventWrite || !pEventWriteString)
        return;

    Error = pEventRegister(&TestProvider, NULL, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "EventRegister without a handle pointer returned %lu\n", Error);

    Handle = 0;
    Error = pEventRegister(&TestProvider, EnableCallback, NULL, &Handle);
    ok(Error == ERROR_SUCCESS, "EventRegister returned %lu\n", Error);
    ok(Handle != 0, "Handle is 0\n");

    /* A second registration of the same provider is a separate handle */
    Handle2 = 0;
    Error = pEventRegister(&TestProvider, NULL, NULL, &Handle2);
    ok(Error == ERROR_SUCCESS, "Second EventRegister returned %lu\n", Error);
    ok(Handle2 != 0 && Handle2 != Handle, "Handle2 = 0x%I64x, Handle = 0x%I64x\n", Handle2, Handle);

    /* No session enables this provider */
    ok(pEventEnabled(Handle, &Descriptor) == FALSE, "EventEnabled returned TRUE\n");
    ok(pEventProviderEnabled(Handle, 4, 0) == FALSE, "EventProviderEnabled returned TRUE\n");
    ok(CallbackCalls == 0, "The enable callback was called %ld times\n", CallbackCalls);

    /* Writing for a provider nobody listens to succeeds and does nothing */
    Data.Ptr = (ULONG_PTR)&Value;
    Data.Size = sizeof(Value);
    Data.Reserved = 0;
    Error = pEventWrite(Handle, &Descriptor, 1, &Data);
    ok(Error == ERROR_SUCCESS, "EventWrite returned %lu\n", Error);
    Error = pEventWrite(Handle, &Descriptor, 0, NULL);
    ok(Error == ERROR_SUCCESS, "EventWrite without data returned %lu\n", Error);
    Error = pEventWriteString(Handle, 4, 0, L"advapi32_apitest");
    ok(Error == ERROR_SUCCESS, "EventWriteString returned %lu\n", Error);

    Error = pEventUnregister(Handle2);
    ok(Error == ERROR_SUCCESS, "EventUnregister returned %lu\n", Error);
    Error = pEventUnregister(Handle);
    ok(Error == ERROR_SUCCESS, "EventUnregister returned %lu\n", Error);

    /* Gone */
    Error = pEventUnregister(Handle);
    ok(Error != ERROR_SUCCESS, "Second EventUnregister returned %lu\n", Error);
}
