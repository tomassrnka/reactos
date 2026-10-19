/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for PowerCreateRequest, PowerSetRequest and PowerClearRequest
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/pofuncs.h>

typedef HANDLE (WINAPI *FN_PowerCreateRequest)(PREASON_CONTEXT);
typedef BOOL (WINAPI *FN_PowerRequestAction)(HANDLE, POWER_REQUEST_TYPE);

static FN_PowerCreateRequest pPowerCreateRequest;
static FN_PowerRequestAction pPowerSetRequest, pPowerClearRequest;

static EXECUTION_STATE QueryExecutionState(VOID)
{
    EXECUTION_STATE State = 0xdeadbeef;
    NTSTATUS Status;

    Status = NtPowerInformation(SystemExecutionState, NULL, 0, &State, sizeof(State));
    ok(Status == STATUS_SUCCESS, "NtPowerInformation(SystemExecutionState) returned 0x%lx\n", Status);
    return State;
}

static HANDLE CreateSimpleRequest(VOID)
{
    REASON_CONTEXT Context;

    Context.Version = POWER_REQUEST_CONTEXT_VERSION;
    Context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    Context.Reason.SimpleReasonString = L"kernel32_apitest PowerRequest";
    return pPowerCreateRequest(&Context);
}

static void TestCreate(void)
{
    REASON_CONTEXT Context;
    HANDLE Request;

    Context.Version = POWER_REQUEST_CONTEXT_VERSION + 1;
    Context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    Context.Reason.SimpleReasonString = L"bad version";
    SetLastError(0xdeadbeef);
    Request = pPowerCreateRequest(&Context);
    ok(Request == INVALID_HANDLE_VALUE, "Request = %p\n", Request);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());

    Context.Version = POWER_REQUEST_CONTEXT_VERSION;
    Context.Flags = 0;
    SetLastError(0xdeadbeef);
    Request = pPowerCreateRequest(&Context);
    ok(Request == INVALID_HANDLE_VALUE, "Request = %p\n", Request);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    Request = CreateSimpleRequest();
    ok(Request != INVALID_HANDLE_VALUE && Request != NULL, "Request = %p, error %lu\n", Request, GetLastError());
    if (Request != INVALID_HANDLE_VALUE && Request != NULL)
        ok(CloseHandle(Request), "CloseHandle failed with %lu\n", GetLastError());
}

static void TestSetClear(void)
{
    HANDLE Request, Event;
    BOOL Ret;

    Request = CreateSimpleRequest();
    ok(Request != INVALID_HANDLE_VALUE, "PowerCreateRequest failed with %lu\n", GetLastError());
    if (Request == INVALID_HANDLE_VALUE)
        return;

    Ret = pPowerSetRequest(Request, PowerRequestSystemRequired);
    ok(Ret == TRUE, "PowerSetRequest failed with %lu\n", GetLastError());
    ok(QueryExecutionState() & ES_SYSTEM_REQUIRED, "ES_SYSTEM_REQUIRED is not set\n");

    /* Sets are counted, every one needs its clear */
    Ret = pPowerSetRequest(Request, PowerRequestSystemRequired);
    ok(Ret == TRUE, "PowerSetRequest failed with %lu\n", GetLastError());
    Ret = pPowerClearRequest(Request, PowerRequestSystemRequired);
    ok(Ret == TRUE, "PowerClearRequest failed with %lu\n", GetLastError());
    ok(QueryExecutionState() & ES_SYSTEM_REQUIRED, "ES_SYSTEM_REQUIRED is not set after one of two clears\n");
    Ret = pPowerClearRequest(Request, PowerRequestSystemRequired);
    ok(Ret == TRUE, "PowerClearRequest failed with %lu\n", GetLastError());

    /* Both sets are balanced now: one more clear has nothing to clear */
    SetLastError(0xdeadbeef);
    Ret = pPowerClearRequest(Request, PowerRequestSystemRequired);
    ok(Ret == FALSE, "Unbalanced PowerClearRequest succeeded\n");
    ok(GetLastError() == ERROR_NOT_SUPPORTED, "GetLastError() = %lu\n", GetLastError());

    /* A type that was never set */
    SetLastError(0xdeadbeef);
    Ret = pPowerClearRequest(Request, PowerRequestAwayModeRequired);
    ok(Ret == FALSE, "PowerClearRequest of an unset type succeeded\n");
    ok(GetLastError() == ERROR_NOT_SUPPORTED, "GetLastError() = %lu\n", GetLastError());

    Ret = pPowerSetRequest(Request, PowerRequestDisplayRequired);
    ok(Ret == TRUE, "PowerSetRequest failed with %lu\n", GetLastError());
    ok(QueryExecutionState() & ES_DISPLAY_REQUIRED, "ES_DISPLAY_REQUIRED is not set\n");

    SetLastError(0xdeadbeef);
    Ret = pPowerSetRequest(Request, (POWER_REQUEST_TYPE)99);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_NOT_SUPPORTED, "GetLastError() = %lu\n", GetLastError());

    /* Closing the handle drops the request (other processes may hold the display, so the system state is not checked) */
    ok(CloseHandle(Request), "CloseHandle failed with %lu\n", GetLastError());

    /* Not a power request */
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetLastError(0xdeadbeef);
    Ret = pPowerSetRequest(Event, PowerRequestSystemRequired);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "GetLastError() = %lu\n", GetLastError());
    CloseHandle(Event);
}

START_TEST(PowerRequest)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

    pPowerCreateRequest = (FN_PowerCreateRequest)GetProcAddress(hKernel32, "PowerCreateRequest");
    pPowerSetRequest = (FN_PowerRequestAction)GetProcAddress(hKernel32, "PowerSetRequest");
    pPowerClearRequest = (FN_PowerRequestAction)GetProcAddress(hKernel32, "PowerClearRequest");
    if (!pPowerCreateRequest && is_reactos() && !GetProcAddress(hKernel32, "K32EnumProcesses"))
    {
        skip("No NT 6.1 exports\n");
        return;
    }
    ok(pPowerCreateRequest && pPowerSetRequest && pPowerClearRequest, "Power request functions are not exported\n");
    if (!pPowerCreateRequest || !pPowerSetRequest || !pPowerClearRequest)
        return;

    TestCreate();
    TestSetClear();
}
