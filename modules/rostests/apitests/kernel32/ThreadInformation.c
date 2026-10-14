/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetThreadInformation and SetThreadInformation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define TestMemoryPriority      0
#define TestAbsolutePriority    1
#define TestDynamicCodePolicy   2
#define TestPowerThrottling     3

typedef BOOL (WINAPI *FN_ThreadInformation)(HANDLE, int, PVOID, DWORD);

static FN_ThreadInformation pGetThreadInformation, pSetThreadInformation;

typedef struct _THROTTLING
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} THROTTLING;

static void
ExpectGet(const char *Description, HANDLE Thread, int Class, PVOID Buffer, DWORD Size, BOOL Result, DWORD Error)
{
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pGetThreadInformation(Thread, Class, Buffer, Size);
    ok(Ret == Result, "Get %s: returned %d, expected %d (error %lu)\n", Description, Ret, Result, GetLastError());
    if (!Result)
        ok(GetLastError() == Error, "Get %s: error %lu, expected %lu\n", Description, GetLastError(), Error);
}

static void
ExpectSet(const char *Description, HANDLE Thread, int Class, PVOID Buffer, DWORD Size, BOOL Result, DWORD Error)
{
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pSetThreadInformation(Thread, Class, Buffer, Size);
    ok(Ret == Result, "Set %s: returned %d, expected %d (error %lu)\n", Description, Ret, Result, GetLastError());
    if (!Result)
        ok(GetLastError() == Error, "Set %s: error %lu, expected %lu\n", Description, GetLastError(), Error);
}

static DWORD
GetMemoryPriority(HANDLE Thread)
{
    DWORD Priority = 0xcccccccc;

    ok(pGetThreadInformation(Thread, TestMemoryPriority, &Priority, sizeof(Priority)),
       "GetThreadInformation failed with %lu\n", GetLastError());
    return Priority;
}

static void
test_MemoryPriority(void)
{
    HANDLE Thread = GetCurrentThread(), Handle;
    DWORD Value, Initial, Priority;
    int i;
    static const DWORD Valid[] = { 1, 2, 3, 4, 5 };
    static const DWORD Invalid[] = { 0, 6, 7, 8, 0xffffffff };
    static const DWORD BadSizes[] = { 0, 1, 3, 5, 8, 12 };

    /* Windows 10 takes the sizes and the pointer before the handle */
    Initial = GetMemoryPriority(Thread);
    ok(Initial >= 1 && Initial <= 5, "Initial priority %lu\n", Initial);
    for (i = 0; i < _countof(BadSizes); i++)
    {
        Value = 0xcccccccc;
        ExpectGet("size", Thread, TestMemoryPriority, &Value, BadSizes[i], FALSE, ERROR_BAD_LENGTH);
        ok(Value == 0xcccccccc, "Value was written: %lx\n", Value);
        Value = 3;
        ExpectSet("size", Thread, TestMemoryPriority, &Value, BadSizes[i], FALSE, ERROR_BAD_LENGTH);
    }
    ExpectGet("NULL buffer", Thread, TestMemoryPriority, NULL, 4, FALSE, ERROR_NOACCESS);
    ExpectSet("NULL buffer", Thread, TestMemoryPriority, NULL, 4, FALSE, ERROR_NOACCESS);

    /* Handles */
    Value = 3;
    ExpectGet("NULL handle", NULL, TestMemoryPriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);
    ExpectSet("NULL handle", NULL, TestMemoryPriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);
    ExpectGet("invalid handle", (HANDLE)(ULONG_PTR)0xdeadbee0, TestMemoryPriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);
    ExpectSet("invalid handle", (HANDLE)(ULONG_PTR)0xdeadbee0, TestMemoryPriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);
    ExpectGet("process handle", GetCurrentProcess(), TestMemoryPriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);
    ExpectSet("process handle", GetCurrentProcess(), TestMemoryPriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);

    /* The values 1 to 5 are accepted and read back; nothing else is */
    for (i = 0; i < _countof(Valid); i++)
    {
        Value = Valid[i];
        ExpectSet("valid value", Thread, TestMemoryPriority, &Value, 4, TRUE, 0);
        Priority = GetMemoryPriority(Thread);
        ok(Priority == Valid[i], "Set %lu, read %lu\n", Valid[i], Priority);
    }
    for (i = 0; i < _countof(Invalid); i++)
    {
        Value = Invalid[i];
        ExpectSet("invalid value", Thread, TestMemoryPriority, &Value, 4, FALSE, ERROR_INVALID_PARAMETER);
        Priority = GetMemoryPriority(Thread);
        ok(Priority == 5, "Priority changed to %lu by the invalid value %lu\n", Priority, Invalid[i]);
    }

    /* A handle without the right to change the thread */
    Handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    ok(Handle != NULL, "OpenThread failed with %lu\n", GetLastError());
    if (Handle)
    {
        Value = 2;
        ExpectSet("read-only handle", Handle, TestMemoryPriority, &Value, 4, FALSE, ERROR_ACCESS_DENIED);
        Priority = GetMemoryPriority(Handle);
        ok(Priority == 5, "Priority %lu\n", Priority);
        CloseHandle(Handle);
    }

    /* A handle that may only set the thread is taken by the setters and refused by the getters */
    Handle = OpenThread(THREAD_SET_INFORMATION, FALSE, GetCurrentThreadId());
    ok(Handle != NULL, "OpenThread failed with %lu\n", GetLastError());
    if (Handle)
    {
        THROTTLING State = { 1, 1, 0 };

        Value = 4;
        ExpectSet("set-only handle", Handle, TestMemoryPriority, &Value, 4, TRUE, 0);
        ExpectSet("set-only handle, power throttling", Handle, TestPowerThrottling, &State, sizeof(State), TRUE, 0);
        ExpectGet("set-only handle", Handle, TestMemoryPriority, &Value, 4, FALSE, ERROR_ACCESS_DENIED);
        ExpectGet("set-only handle, dynamic code policy", Handle, TestDynamicCodePolicy, &Value, 4, FALSE, ERROR_ACCESS_DENIED);
        ExpectGet("set-only handle, absolute priority", Handle, TestAbsolutePriority, &Value, 4, FALSE, ERROR_ACCESS_DENIED);
        CloseHandle(Handle);
        Value = 5;
        pSetThreadInformation(Thread, TestMemoryPriority, &Value, 4);
    }

    /* A handle that may query a little is enough to get; the thread it is does not matter for the default or the policy */
    if (is_reactos())
    {
        /* The kernel does not know the right to query a thread a little: a handle with it is no handle with any right */
        skip("THREAD_QUERY_LIMITED_INFORMATION is not given by the kernel\n");
    }
    else if (DuplicateHandle(GetCurrentProcess(), Thread, GetCurrentProcess(), &Handle, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
    {
        Value = 0xcccccccc;
        ExpectGet("query-limited handle", Handle, TestMemoryPriority, &Value, 4, TRUE, 0);
        ok(Value >= 1 && Value <= 5, "The memory priority is %lu\n", Value);
        Value = 0xcccccccc;
        ExpectGet("query-limited handle, dynamic code policy", Handle, TestDynamicCodePolicy, &Value, 4, TRUE, 0);
        ok(Value == 0, "The dynamic code policy is %lu\n", Value);
        Value = 3;
        ExpectSet("query-limited handle", Handle, TestMemoryPriority, &Value, 4, FALSE, ERROR_ACCESS_DENIED);
        CloseHandle(Handle);
    }
    else
    {
        skip("DuplicateHandle failed with %lu\n", GetLastError());
    }

    /* Another handle of the same thread shows the same value */
    Handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, GetCurrentThreadId());
    ok(Handle != NULL, "OpenThread failed with %lu\n", GetLastError());
    if (Handle)
    {
        Value = 3;
        ExpectSet("second handle", Handle, TestMemoryPriority, &Value, 4, TRUE, 0);
        Priority = GetMemoryPriority(Thread);
        ok(Priority == 3, "Priority %lu\n", Priority);
        CloseHandle(Handle);
    }

    Value = Initial;
    if (Initial >= 1 && Initial <= 5)
        pSetThreadInformation(Thread, TestMemoryPriority, &Value, 4);
}

static DWORD WINAPI
SleepThread(PVOID Parameter)
{
    Sleep(200);
    return 0;
}

static void
test_OtherThread(void)
{
    HANDLE Thread;
    DWORD Value;

    Thread = CreateThread(NULL, 0, SleepThread, NULL, CREATE_SUSPENDED, NULL);
    ok(Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!Thread)
        return;

    Value = 2;
    ExpectSet("other thread", Thread, TestMemoryPriority, &Value, 4, TRUE, 0);
    ok(GetMemoryPriority(Thread) == 2, "The priority of the other thread is not 2\n");
    ResumeThread(Thread);
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);
}

static void
test_PowerThrottling(void)
{
    HANDLE Thread = GetCurrentThread();
    THROTTLING State;
    BYTE Buffer[32];
    int i;
    static const struct
    {
        THROTTLING State;
        BOOL Result;
    } Cases[] =
    {
        { { 1, 0, 0 }, TRUE },
        { { 1, 1, 0 }, TRUE },
        { { 1, 1, 1 }, TRUE },
        { { 0, 1, 1 }, TRUE },
        { { 1, 2, 0 }, FALSE },
        { { 1, 3, 3 }, FALSE },
        { { 2, 1, 1 }, FALSE },
        { { 1, 0, 1 }, FALSE },
        { { 1, 1, 2 }, FALSE },
        { { 1, 0xffffffff, 0 }, FALSE },
    };

    /* The state can be set and can not be read */
    for (i = 0; i < _countof(Cases); i++)
    {
        State = Cases[i].State;
        ExpectSet("state", Thread, TestPowerThrottling, &State, sizeof(State), Cases[i].Result, ERROR_INVALID_PARAMETER);
    }

    /* A larger structure is taken; a smaller one is not */
    State.Version = 1;
    State.ControlMask = 1;
    State.StateMask = 0;
    memset(Buffer, 0, sizeof(Buffer));
    memcpy(Buffer, &State, sizeof(State));
    ExpectSet("16 bytes", Thread, TestPowerThrottling, Buffer, 16, TRUE, 0);
    ExpectSet("8 bytes", Thread, TestPowerThrottling, Buffer, 8, FALSE, ERROR_INVALID_PARAMETER);
    ExpectSet("0 bytes", Thread, TestPowerThrottling, Buffer, 0, FALSE, ERROR_INVALID_PARAMETER);

    for (i = 0; i <= 32; i += 4)
    {
        memset(Buffer, 0xcc, sizeof(Buffer));
        ExpectGet("state", Thread, TestPowerThrottling, Buffer, i, FALSE, ERROR_INVALID_PARAMETER);
    }
}

static LONG
ClassBasePriority(VOID)
{
    switch (GetPriorityClass(GetCurrentProcess()))
    {
        case IDLE_PRIORITY_CLASS: return 4;
        case BELOW_NORMAL_PRIORITY_CLASS: return 6;
        case NORMAL_PRIORITY_CLASS: return 8;
        case ABOVE_NORMAL_PRIORITY_CLASS: return 10;
        case HIGH_PRIORITY_CLASS: return 13;
        default: return 0;
    }
}

static void
test_AbsolutePriority(void)
{
    HANDLE Thread = GetCurrentThread();
    LONG Base = ClassBasePriority(), OldPriority = GetThreadPriority(Thread);
    DWORD Value;
    BYTE Buffer[16];
    BOOL Ret;
    int i;
    static const int Relative[] = { THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST };
    static const DWORD Invalid[] = { 0, 32, 33, 100, 0x7fffffff, 0xffffffff };

    /* The sizes, the pointer, the handle */
    for (i = 0; i < 4; i++)
    {
        static const DWORD Sizes[] = { 0, 1, 8, 16 };

        ExpectGet("size", Thread, TestAbsolutePriority, Buffer, Sizes[i], FALSE, ERROR_BAD_LENGTH);
        ExpectSet("size", Thread, TestAbsolutePriority, Buffer, Sizes[i], FALSE, ERROR_BAD_LENGTH);
    }
    ExpectGet("NULL buffer", Thread, TestAbsolutePriority, NULL, 4, FALSE, ERROR_NOACCESS);
    ExpectGet("NULL handle", NULL, TestAbsolutePriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);
    ExpectGet("process handle", GetCurrentProcess(), TestAbsolutePriority, &Value, 4, FALSE, ERROR_INVALID_HANDLE);

    /* The value is the priority of the process class, and the increment of the thread added to it */
    ok(Base != 0, "The priority class has no base priority here\n");
    if (Base)
    {
        for (i = 0; i < _countof(Relative); i++)
        {
            ok(SetThreadPriority(Thread, Relative[i]), "SetThreadPriority(%d) failed with %lu\n", Relative[i], GetLastError());
            Value = 0xcccccccc;
            ExpectGet("absolute priority", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
            ok(Value == (DWORD)(Base + Relative[i]), "Increment %d: the priority is %lu, expected %ld\n", Relative[i], Value, Base + Relative[i]);
        }
    }
    ok(SetThreadPriority(Thread, THREAD_PRIORITY_IDLE), "SetThreadPriority failed with %lu\n", GetLastError());
    ExpectGet("idle", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
    ok(Value == 1, "Idle: %lu\n", Value);
    ok(SetThreadPriority(Thread, THREAD_PRIORITY_TIME_CRITICAL), "SetThreadPriority failed with %lu\n", GetLastError());
    ExpectGet("time critical", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
    ok(Value == 15 || Value == 31, "Time critical: %lu\n", Value);
    SetThreadPriority(Thread, THREAD_PRIORITY_NORMAL);

    /* Setting it: the values that the increments reach, the lowest and the highest of the class */
    for (i = 0; i < _countof(Invalid); i++)
    {
        Value = Invalid[i];
        ExpectSet("invalid value", Thread, TestAbsolutePriority, &Value, 4, FALSE, ERROR_INVALID_PARAMETER);
    }
    if (Base)
    {
        Value = Base + 1;
        ExpectSet("above the process", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
        ok(GetThreadPriority(Thread) == THREAD_PRIORITY_ABOVE_NORMAL, "The relative priority is %d\n", GetThreadPriority(Thread));
        Value = Base - 2;
        ExpectSet("two below the process", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
        ok(GetThreadPriority(Thread) == THREAD_PRIORITY_LOWEST, "The relative priority is %d\n", GetThreadPriority(Thread));
    }

    /* The lowest and the highest priority of the class are set; what the thread reports as its priority differs */
    Value = 1;
    ExpectSet("lowest", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
    Value = 0xcccccccc;
    ExpectGet("after Set 1", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
    ok(Value == 1, "The priority is %lu\n", Value);
    Value = 15;
    Ret = pSetThreadInformation(Thread, TestAbsolutePriority, &Value, 4);
    ok(Ret, "Set 15 failed with %lu\n", GetLastError());
    Value = 0xcccccccc;
    ExpectGet("after Set 15", Thread, TestAbsolutePriority, &Value, 4, TRUE, 0);
    ok(Value == 15, "The priority is %lu\n", Value);

    /* Realtime priorities need a privilege; a value above them does not exist */
    Value = 40;
    ExpectSet("above 31", Thread, TestAbsolutePriority, &Value, 4, FALSE, ERROR_INVALID_PARAMETER);

    SetThreadPriority(Thread, OldPriority);
}

static void
test_OtherClasses(void)
{
    HANDLE Thread = GetCurrentThread();
    BYTE Buffer[32];
    DWORD Value = 1;
    int Class;

    /* No class above the four of the header can be used */
    for (Class = 4; Class <= 6; Class++)
    {
        ExpectGet("unknown class", Thread, Class, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
        ExpectSet("unknown class", Thread, Class, &Value, 4, FALSE, ERROR_INVALID_PARAMETER);
    }
    ExpectGet("class 99", Thread, 99, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
    ExpectSet("class 99", Thread, 99, &Value, 4, FALSE, ERROR_INVALID_PARAMETER);
    ExpectGet("class -1", Thread, -1, Buffer, 4, FALSE, ERROR_INVALID_PARAMETER);
    ExpectSet("class -1", Thread, -1, &Value, 4, FALSE, ERROR_INVALID_PARAMETER);

    /* The dynamic code policy of a thread is read, and can not be changed */
    Value = 0xcccccccc;
    ExpectGet("dynamic code policy", Thread, TestDynamicCodePolicy, &Value, 4, TRUE, 0);
    ok(Value == 0, "The dynamic code policy is %lx\n", Value);
    ExpectGet("dynamic code policy size 8", Thread, TestDynamicCodePolicy, &Value, 8, FALSE, ERROR_BAD_LENGTH);
    Value = 1;
    ExpectSet("dynamic code policy", Thread, TestDynamicCodePolicy, &Value, 4, FALSE, ERROR_ACCESS_DENIED);
    Value = 3;
    ExpectSet("dynamic code policy, invalid value", Thread, TestDynamicCodePolicy, &Value, 4, FALSE, ERROR_INVALID_PARAMETER);
    ExpectSet("dynamic code policy, NULL buffer", Thread, TestDynamicCodePolicy, NULL, 4, FALSE, ERROR_NOACCESS);
    ExpectGet("dynamic code policy, NULL buffer", Thread, TestDynamicCodePolicy, NULL, 4, FALSE, ERROR_NOACCESS);
    ExpectGet("dynamic code policy, NULL handle", NULL, TestDynamicCodePolicy, &Value, 4, FALSE, ERROR_INVALID_HANDLE);
    ExpectGet("class 99 with a NULL buffer", Thread, 99, NULL, 4, FALSE, ERROR_INVALID_PARAMETER);
}

START_TEST(ThreadInformation)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

    pGetThreadInformation = (FN_ThreadInformation)GetProcAddress(hKernel32, "GetThreadInformation");
    pSetThreadInformation = (FN_ThreadInformation)GetProcAddress(hKernel32, "SetThreadInformation");
    if ((!pGetThreadInformation || !pSetThreadInformation) && !is_reactos())
    {
        skip("GetThreadInformation or SetThreadInformation is not exported\n");
        return;
    }
    ok(pGetThreadInformation != NULL, "GetThreadInformation is not exported\n");
    ok(pSetThreadInformation != NULL, "SetThreadInformation is not exported\n");
    if (!pGetThreadInformation || !pSetThreadInformation)
        return;

    test_MemoryPriority();
    test_OtherThread();
    test_PowerThrottling();
    test_AbsolutePriority();
    test_OtherClasses();
}
