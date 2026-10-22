/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetProcessInformation and SetProcessInformation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define TestMemoryPriority      0
#define TestAppMemoryInfo       2
#define TestPowerThrottling     4
#define TestProtectionLevel     7
#define TestLeapSecond          8

typedef BOOL (WINAPI *FN_ProcessInformation)(HANDLE, int, PVOID, DWORD);

static FN_ProcessInformation pGetProcessInformation, pSetProcessInformation;

typedef struct _THROTTLING
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} THROTTLING;

static BOOL
Get(HANDLE Process, int Class, PVOID Buffer, DWORD Size, DWORD *Error)
{
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pGetProcessInformation(Process, Class, Buffer, Size);
    *Error = GetLastError();
    return Ret;
}

static BOOL
Set(HANDLE Process, int Class, PVOID Buffer, DWORD Size, DWORD *Error)
{
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pSetProcessInformation(Process, Class, Buffer, Size);
    *Error = GetLastError();
    return Ret;
}

#define EXPECT_GET(Process, Class, Buffer, Size, Result, Err) \
    do { \
        DWORD Error_; \
        BOOL Ret_ = Get(Process, Class, Buffer, Size, &Error_); \
        ok(Ret_ == (Result), "Get class %d size %lu: returned %d, expected %d (error %lu)\n", Class, (ULONG)(Size), Ret_, (BOOL)(Result), Error_); \
        if (!(Result)) \
            ok(Error_ == (Err), "Get class %d size %lu: error %lu, expected %lu\n", Class, (ULONG)(Size), Error_, (DWORD)(Err)); \
    } while (0)

#define EXPECT_SET(Process, Class, Buffer, Size, Result, Err) \
    do { \
        DWORD Error_; \
        BOOL Ret_ = Set(Process, Class, Buffer, Size, &Error_); \
        ok(Ret_ == (Result), "Set class %d size %lu: returned %d, expected %d (error %lu)\n", Class, (ULONG)(Size), Ret_, (BOOL)(Result), Error_); \
        if (!(Result)) \
            ok(Error_ == (Err), "Set class %d size %lu: error %lu, expected %lu\n", Class, (ULONG)(Size), Error_, (DWORD)(Err)); \
    } while (0)

static void
TestMemoryPriorityClass(void)
{
    HANDLE Process = GetCurrentProcess();
    HANDLE Limited, SetOnly, Both, Event;
    ULONG Value, Old;
    DWORD Error;

    /* Whatever the priority of the process is, it is one that can be set again */
    Value = 0xcccccccc;
    EXPECT_GET(Process, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
    ok(Value >= 1 && Value <= 5, "Memory priority %lu\n", Value);
    Old = Value;

    for (Value = 1; Value <= 5; Value++)
    {
        ULONG Got = 0xcccccccc;

        EXPECT_SET(Process, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
        EXPECT_GET(Process, TestMemoryPriority, &Got, sizeof(Got), TRUE, 0);
        ok(Got == Value, "Memory priority %lu after setting %lu\n", Got, Value);
    }

    /* 0 and above 5 are refused, and the priority stays */
    Value = 0;
    EXPECT_SET(Process, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_INVALID_PARAMETER);
    Value = 6;
    EXPECT_SET(Process, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_INVALID_PARAMETER);
    Value = 0xcccccccc;
    EXPECT_GET(Process, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
    ok(Value == 5, "Memory priority %lu\n", Value);

    /* The size is the size of the structure, and a buffer is needed */
    Value = 3;
    EXPECT_SET(Process, TestMemoryPriority, &Value, 0, FALSE, ERROR_BAD_LENGTH);
    EXPECT_SET(Process, TestMemoryPriority, &Value, 8, FALSE, ERROR_BAD_LENGTH);
    EXPECT_SET(Process, TestMemoryPriority, NULL, sizeof(Value), FALSE, ERROR_NOACCESS);
    EXPECT_GET(Process, TestMemoryPriority, &Value, 0, FALSE, ERROR_BAD_LENGTH);
    EXPECT_GET(Process, TestMemoryPriority, &Value, 3, FALSE, ERROR_BAD_LENGTH);
    EXPECT_GET(Process, TestMemoryPriority, &Value, 8, FALSE, ERROR_BAD_LENGTH);
    EXPECT_GET(Process, TestMemoryPriority, NULL, sizeof(Value), FALSE, ERROR_NOACCESS);

    /* The handle: invalid, of another type, and the access it has */
    Value = 4;
    EXPECT_SET(NULL, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_INVALID_HANDLE);
    EXPECT_GET(NULL, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_INVALID_HANDLE);
    Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(Event != NULL, "CreateEvent failed %lu\n", GetLastError());
    EXPECT_SET(Event, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_INVALID_HANDLE);
    EXPECT_GET(Event, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_INVALID_HANDLE);
    CloseHandle(Event);

    Limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    SetOnly = OpenProcess(PROCESS_SET_INFORMATION, FALSE, GetCurrentProcessId());
    Both = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Limited != NULL && SetOnly != NULL && Both != NULL, "OpenProcess failed %lu\n", GetLastError());
    if (Limited && SetOnly && Both)
    {
        Value = 0xcccccccc;
        EXPECT_GET(Limited, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
        ok(Value == 5, "Memory priority %lu through the handle\n", Value);
        EXPECT_GET(SetOnly, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_ACCESS_DENIED);
        Value = 4;
        EXPECT_SET(Limited, TestMemoryPriority, &Value, sizeof(Value), FALSE, ERROR_ACCESS_DENIED);
        EXPECT_SET(SetOnly, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
        /* A handle that may set but not query cannot say which process it is, the hint is taken and not kept here */
        Value = 3;
        EXPECT_SET(Both, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
        Value = 0xcccccccc;
        EXPECT_GET(Process, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
        ok(Value == 3, "Memory priority %lu after setting through a handle\n", Value);
        Value = 0xcccccccc;
        EXPECT_GET(Both, TestMemoryPriority, &Value, sizeof(Value), TRUE, 0);
        ok(Value == 3, "Memory priority %lu through a handle\n", Value);
    }
    if (Limited) CloseHandle(Limited);
    if (SetOnly) CloseHandle(SetOnly);
    if (Both) CloseHandle(Both);

    Value = Old;
    ok(Set(Process, TestMemoryPriority, &Value, sizeof(Value), &Error), "Restore failed %lu\n", Error);
}

static void
TestPowerThrottlingClass(void)
{
    HANDLE Process = GetCurrentProcess();
    THROTTLING State;
    DWORD Size = sizeof(State);

    /* Set only: Windows gives no way to read the state */
    State.Version = 1; State.ControlMask = 1; State.StateMask = 1;
    EXPECT_SET(Process, TestPowerThrottling, &State, Size, TRUE, 0);
    State.StateMask = 0;
    EXPECT_SET(Process, TestPowerThrottling, &State, Size, TRUE, 0);
    State.ControlMask = 0;
    EXPECT_SET(Process, TestPowerThrottling, &State, Size, TRUE, 0);
    EXPECT_GET(Process, TestPowerThrottling, &State, Size, FALSE, ERROR_INVALID_PARAMETER);

    /* A state needs its control bit; the version is 1; the bits that do not exist */
    State.Version = 1; State.ControlMask = 1; State.StateMask = 2;
    EXPECT_SET(Process, TestPowerThrottling, &State, Size, FALSE, ERROR_INVALID_PARAMETER);
    State.ControlMask = 8; State.StateMask = 8;
    EXPECT_SET(Process, TestPowerThrottling, &State, Size, FALSE, ERROR_INVALID_PARAMETER);
    State.Version = 0; State.ControlMask = 1; State.StateMask = 1;
    EXPECT_SET(Process, TestPowerThrottling, &State, Size, FALSE, ERROR_INVALID_PARAMETER);
    State.Version = 2;
    EXPECT_SET(Process, TestPowerThrottling, &State, Size, FALSE, ERROR_INVALID_PARAMETER);

    State.Version = 1;
    EXPECT_SET(Process, TestPowerThrottling, &State, 0, FALSE, ERROR_BAD_LENGTH);
    EXPECT_SET(Process, TestPowerThrottling, &State, 8, FALSE, ERROR_BAD_LENGTH);
    EXPECT_SET(Process, TestPowerThrottling, &State, 16, FALSE, ERROR_BAD_LENGTH);
    EXPECT_SET(Process, TestPowerThrottling, NULL, Size, FALSE, ERROR_NOACCESS);
    EXPECT_SET(NULL, TestPowerThrottling, &State, Size, FALSE, ERROR_INVALID_HANDLE);
}

static void
TestLeapSecondClass(void)
{
    HANDLE Process = GetCurrentProcess();
    ULONG Info[2];
    ULONG Old;
    DWORD Error;

    Info[0] = Info[1] = 0xcccccccc;
    EXPECT_GET(Process, TestLeapSecond, Info, sizeof(Info), TRUE, 0);
    ok(Info[1] == 0, "Reserved field %lx\n", Info[1]);
    Old = Info[0];

    Info[0] = 1; Info[1] = 0;
    EXPECT_SET(Process, TestLeapSecond, Info, sizeof(Info), TRUE, 0);
    Info[0] = Info[1] = 0xcccccccc;
    EXPECT_GET(Process, TestLeapSecond, Info, sizeof(Info), TRUE, 0);
    ok(Info[0] == 1, "Leap second flags %lx\n", Info[0]);

    Info[0] = 0; Info[1] = 0;
    EXPECT_SET(Process, TestLeapSecond, Info, sizeof(Info), TRUE, 0);
    Info[0] = Info[1] = 0xcccccccc;
    EXPECT_GET(Process, TestLeapSecond, Info, sizeof(Info), TRUE, 0);
    ok(Info[0] == 0, "Leap second flags %lx\n", Info[0]);

    Info[0] = 2; Info[1] = 0;
    EXPECT_SET(Process, TestLeapSecond, Info, sizeof(Info), FALSE, ERROR_INVALID_PARAMETER);
    EXPECT_SET(Process, TestLeapSecond, Info, 4, FALSE, ERROR_BAD_LENGTH);
    EXPECT_SET(Process, TestLeapSecond, Info, 12, FALSE, ERROR_BAD_LENGTH);
    EXPECT_GET(Process, TestLeapSecond, Info, 4, FALSE, ERROR_BAD_LENGTH);

    Info[0] = Old; Info[1] = 0;
    ok(Set(Process, TestLeapSecond, Info, sizeof(Info), &Error), "Restore failed %lu\n", Error);
}

static void
TestOtherClasses(void)
{
    HANDLE Process = GetCurrentProcess();
    ULONG Buffer[8] = { 0 };
    DWORD Error;
    int Class;

    /* A process is not protected on a system that has no protected processes (and none here) */
    Buffer[0] = 0xcccccccc;
    if (Get(Process, TestProtectionLevel, Buffer, 4, &Error))
        ok(Buffer[0] == 0xfffffffe, "Protection level %lx\n", Buffer[0]);
    else
        ok(FALSE, "Get protection level failed %lu\n", Error);
    EXPECT_GET(Process, TestProtectionLevel, Buffer, 8, FALSE, ERROR_BAD_LENGTH);

    /* The classes that Windows does not know to get or to set */
    EXPECT_GET(Process, 99, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
    EXPECT_GET(Process, -1, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
    EXPECT_SET(Process, 99, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
    EXPECT_SET(Process, -1, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
    for (Class = 0; Class <= 12; Class++)
    {
        if (Class == TestMemoryPriority || Class == TestPowerThrottling || Class == TestLeapSecond)
            continue;
        if (Class == 6)
            continue; /* the size of this class is not known */
        EXPECT_SET(Process, Class, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
    }
}

START_TEST(ProcessInformation)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");

    pGetProcessInformation = (FN_ProcessInformation)GetProcAddress(Kernel32, "GetProcessInformation");
    pSetProcessInformation = (FN_ProcessInformation)GetProcAddress(Kernel32, "SetProcessInformation");
    if (!pGetProcessInformation || !pSetProcessInformation)
    {
        skip("GetProcessInformation and SetProcessInformation are not available\n");
        return;
    }

    TestMemoryPriorityClass();
    TestPowerThrottlingClass();
    TestLeapSecondClass();
    TestOtherClasses();
}
