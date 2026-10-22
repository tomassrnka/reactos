/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the event provider functions of advapi32 (Windows Vista and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <evntprov.h>

typedef ULONG (WINAPI *PFN_EventRegister)(LPCGUID, PENABLECALLBACK, PVOID, PREGHANDLE);
typedef ULONG (WINAPI *PFN_EventUnregister)(REGHANDLE);
typedef BOOLEAN (WINAPI *PFN_EventEnabled)(REGHANDLE, PCEVENT_DESCRIPTOR);
typedef BOOLEAN (WINAPI *PFN_EventProviderEnabled)(REGHANDLE, UCHAR, ULONGLONG);
typedef ULONG (WINAPI *PFN_EventWrite)(REGHANDLE, PCEVENT_DESCRIPTOR, ULONG, PEVENT_DATA_DESCRIPTOR);
typedef ULONG (WINAPI *PFN_EventWriteString)(REGHANDLE, UCHAR, ULONGLONG, PCWSTR);
typedef ULONG (WINAPI *PFN_EventWriteTransfer)(REGHANDLE, PCEVENT_DESCRIPTOR, LPCGUID, LPCGUID, ULONG, PEVENT_DATA_DESCRIPTOR);
typedef ULONG (WINAPI *PFN_EventActivityIdControl)(ULONG, LPGUID);

/* a provider id of this test */
static const GUID TestProvider = { 0x4c1a3b86, 0x7d1e, 0x4a52, { 0x9e, 0x3b, 0x1f, 0x6a, 0x2d, 0x4e, 0x5c, 0x71 } };

static PFN_EventActivityIdControl g_Control;
static BOOL g_ThreadIdIsZero;

static DWORD WINAPI ThreadProc(PVOID Param)
{
    GUID Id = { 9, 9, 9, { 9, 9, 9, 9, 9, 9, 9, 9 } };
    static const GUID Zero;

    UNREFERENCED_PARAMETER(Param);
    g_ThreadIdIsZero = g_Control(EVENT_ACTIVITY_CTRL_GET_ID, &Id) == ERROR_SUCCESS && IsEqualGUID(&Id, &Zero);
    return 0;
}

static void TestActivityId(PFN_EventActivityIdControl Control)
{
    GUID One = { 1, 1, 1, { 1, 1, 1, 1, 1, 1, 1, 1 } };
    GUID Two = { 2, 2, 2, { 2, 2, 2, 2, 2, 2, 2, 2 } };
    GUID Out, Now;
    static const GUID Zero;
    HANDLE Thread;

    g_Control = Control;

    /* the id of a thread starts as the zero id and is set and read back */
    Out = One;
    ok_dec(Control(EVENT_ACTIVITY_CTRL_GET_ID, &Out), ERROR_SUCCESS);
    ok(IsEqualGUID(&Out, &Zero), "the id of a new thread is not zero\n");
    Out = One;
    ok_dec(Control(EVENT_ACTIVITY_CTRL_SET_ID, &Out), ERROR_SUCCESS);
    ok(IsEqualGUID(&Out, &One), "SET_ID changed its argument\n");
    ok_dec(Control(EVENT_ACTIVITY_CTRL_GET_ID, &Out), ERROR_SUCCESS);
    ok(IsEqualGUID(&Out, &One), "GET_ID did not return the id that was set\n");

    /* CREATE_ID makes a new id and does not set it */
    ok_dec(Control(EVENT_ACTIVITY_CTRL_CREATE_ID, &Out), ERROR_SUCCESS);
    ok(!IsEqualGUID(&Out, &Zero) && !IsEqualGUID(&Out, &One), "CREATE_ID did not make a new id\n");
    ok_dec(Control(EVENT_ACTIVITY_CTRL_GET_ID, &Now), ERROR_SUCCESS);
    ok(IsEqualGUID(&Now, &One), "CREATE_ID changed the id of the thread\n");

    /* GET_SET_ID returns the old id and sets the given one */
    Out = Two;
    ok_dec(Control(EVENT_ACTIVITY_CTRL_GET_SET_ID, &Out), ERROR_SUCCESS);
    ok(IsEqualGUID(&Out, &One), "GET_SET_ID did not return the old id\n");
    ok_dec(Control(EVENT_ACTIVITY_CTRL_GET_ID, &Now), ERROR_SUCCESS);
    ok(IsEqualGUID(&Now, &Two), "GET_SET_ID did not set the id\n");

    /* CREATE_SET_ID returns the old id and sets a new one */
    ok_dec(Control(EVENT_ACTIVITY_CTRL_CREATE_SET_ID, &Out), ERROR_SUCCESS);
    ok(IsEqualGUID(&Out, &Two), "CREATE_SET_ID did not return the old id\n");
    ok_dec(Control(EVENT_ACTIVITY_CTRL_GET_ID, &Now), ERROR_SUCCESS);
    ok(!IsEqualGUID(&Now, &Zero) && !IsEqualGUID(&Now, &Two), "CREATE_SET_ID did not set a new id\n");

    /* the id belongs to the thread */
    Thread = CreateThread(NULL, 0, ThreadProc, NULL, 0, NULL);
    if (Thread)
    {
        WaitForSingleObject(Thread, 30000);
        CloseHandle(Thread);
        ok(g_ThreadIdIsZero, "another thread sees the id\n");
    }

    /* codes that do not exist */
    Out = One;
    ok_dec(Control(0, &Out), ERROR_INVALID_PARAMETER);
    ok_dec(Control(6, &Out), ERROR_INVALID_PARAMETER);
    ok_dec(Control(EVENT_ACTIVITY_CTRL_GET_ID, NULL), ERROR_INVALID_PARAMETER);
}

START_TEST(EventProvider)
{
    HMODULE Advapi = GetModuleHandleW(L"advapi32.dll");
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_EventRegister pEventRegister = (PFN_EventRegister)GetProcAddress(Advapi, "EventRegister");
    PFN_EventUnregister pEventUnregister = (PFN_EventUnregister)GetProcAddress(Advapi, "EventUnregister");
    PFN_EventEnabled pEventEnabled = (PFN_EventEnabled)GetProcAddress(Advapi, "EventEnabled");
    PFN_EventProviderEnabled pEventProviderEnabled = (PFN_EventProviderEnabled)GetProcAddress(Advapi, "EventProviderEnabled");
    PFN_EventWrite pEventWrite = (PFN_EventWrite)GetProcAddress(Advapi, "EventWrite");
    PFN_EventWriteString pEventWriteString = (PFN_EventWriteString)GetProcAddress(Advapi, "EventWriteString");
    PFN_EventWriteTransfer pEventWriteTransfer = (PFN_EventWriteTransfer)GetProcAddress(Advapi, "EventWriteTransfer");
    PFN_EventActivityIdControl pEventActivityIdControl = (PFN_EventActivityIdControl)GetProcAddress(Advapi, "EventActivityIdControl");
    EVENT_DESCRIPTOR Descriptor = { 1, 0, 0, 4, 0, 0, 1 };
    REGHANDLE Handle = 0;
    GUID Activity = { 1, 2, 3, { 4, 5, 6, 7, 8, 9, 10, 11 } };
    ULONG Err;

    /* a build for Windows Server 2003 has none of these functions */
    if (!GetProcAddress(Kernel32, "GetTickCount64"))
    {
        skip("this system is older than Windows Vista\n");
        return;
    }

    ok(pEventRegister != NULL, "EventRegister is not exported\n");
    ok(pEventUnregister != NULL, "EventUnregister is not exported\n");
    ok(pEventEnabled != NULL, "EventEnabled is not exported\n");
    ok(pEventProviderEnabled != NULL, "EventProviderEnabled is not exported\n");
    ok(pEventWrite != NULL, "EventWrite is not exported\n");
    ok(pEventWriteString != NULL, "EventWriteString is not exported\n");
    ok(pEventWriteTransfer != NULL, "EventWriteTransfer is not exported\n");
    ok(pEventActivityIdControl != NULL, "EventActivityIdControl is not exported\n");
    if (!pEventRegister || !pEventUnregister || !pEventEnabled || !pEventProviderEnabled || !pEventWrite ||
        !pEventWriteString || !pEventWriteTransfer || !pEventActivityIdControl)
    {
        skip("the event provider functions are missing\n");
        return;
    }

    Err = pEventRegister(&TestProvider, NULL, NULL, &Handle);
    ok_dec(Err, ERROR_SUCCESS);
    ok(Handle != 0, "no registration handle\n");

    /* nobody listens */
    ok_dec(pEventEnabled(Handle, &Descriptor), FALSE);
    ok_dec(pEventProviderEnabled(Handle, 4, 1), FALSE);

    /* so writing succeeds and does nothing */
    ok_dec(pEventWrite(Handle, &Descriptor, 0, NULL), ERROR_SUCCESS);
    ok_dec(pEventWriteString(Handle, 4, 1, L"text"), ERROR_SUCCESS);
    ok_dec(pEventWriteTransfer(Handle, &Descriptor, &Activity, NULL, 0, NULL), ERROR_SUCCESS);

    TestActivityId(pEventActivityIdControl);

    ok_dec(pEventUnregister(Handle), ERROR_SUCCESS);
}
