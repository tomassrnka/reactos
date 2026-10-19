/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for CreateEventExA/W and CreateMutexExA/W
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/obfuncs.h>

#define CREATE_EVENT_MANUAL_RESET   0x1
#define CREATE_EVENT_INITIAL_SET    0x2
#define CREATE_MUTEX_INITIAL_OWNER  0x1

typedef HANDLE (WINAPI *PFN_CREATEEVENTEXW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
typedef HANDLE (WINAPI *PFN_CREATEEVENTEXA)(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);
typedef HANDLE (WINAPI *PFN_CREATEMUTEXEXW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
typedef HANDLE (WINAPI *PFN_CREATEMUTEXEXA)(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);

static PFN_CREATEEVENTEXW pCreateEventExW;
static PFN_CREATEEVENTEXA pCreateEventExA;
static PFN_CREATEMUTEXEXW pCreateMutexExW;
static PFN_CREATEMUTEXEXA pCreateMutexExA;

#define CheckAccess(Handle, Expected) CheckAccess_(Handle, Expected, __LINE__)
static void
CheckAccess_(HANDLE Handle, ACCESS_MASK Expected, int Line)
{
    OBJECT_BASIC_INFORMATION Info;
    NTSTATUS Status;

    Status = NtQueryObject(Handle, ObjectBasicInformation, &Info, sizeof(Info), NULL);
    ok_(__FILE__, Line)(Status == STATUS_SUCCESS, "NtQueryObject returned 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    ok_(__FILE__, Line)(Info.GrantedAccess == Expected,
                        "GrantedAccess 0x%lx, expected 0x%lx\n", Info.GrantedAccess, Expected);
}

#define CheckEvent(Event, Type, State) CheckEvent_(Event, Type, State, __LINE__)
static void
CheckEvent_(HANDLE Event, EVENT_TYPE ExpectedType, LONG ExpectedState, int Line)
{
    EVENT_BASIC_INFORMATION Info;
    NTSTATUS Status;

    Status = NtQueryEvent(Event, EventBasicInformation, &Info, sizeof(Info), NULL);
    ok_(__FILE__, Line)(Status == STATUS_SUCCESS, "NtQueryEvent returned 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    ok_(__FILE__, Line)(Info.EventType == ExpectedType,
                        "EventType %d, expected %d\n", Info.EventType, ExpectedType);
    ok_(__FILE__, Line)(Info.EventState == ExpectedState,
                        "EventState %ld, expected %ld\n", Info.EventState, ExpectedState);
}

#define CheckMutant(Mutant, Count, Owned) CheckMutant_(Mutant, Count, Owned, __LINE__)
static void
CheckMutant_(HANDLE Mutant, LONG ExpectedCount, BOOLEAN ExpectedOwned, int Line)
{
    MUTANT_BASIC_INFORMATION Info;
    NTSTATUS Status;

    Status = NtQueryMutant(Mutant, MutantBasicInformation, &Info, sizeof(Info), NULL);
    ok_(__FILE__, Line)(Status == STATUS_SUCCESS, "NtQueryMutant returned 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    ok_(__FILE__, Line)(Info.CurrentCount == ExpectedCount,
                        "CurrentCount %ld, expected %ld\n", Info.CurrentCount, ExpectedCount);
    ok_(__FILE__, Line)(Info.OwnedByCaller == ExpectedOwned,
                        "OwnedByCaller %u, expected %u\n", Info.OwnedByCaller, ExpectedOwned);
}

static void
TestEventFlags(void)
{
    static const struct
    {
        DWORD Flags;
        EVENT_TYPE Type;
        LONG State;
    } Tests[] =
    {
        { 0, SynchronizationEvent, 0 },
        { CREATE_EVENT_MANUAL_RESET, NotificationEvent, 0 },
        { CREATE_EVENT_INITIAL_SET, SynchronizationEvent, 1 },
        { CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET, NotificationEvent, 1 },
    };
    ULONG i;
    HANDLE Event;
    DWORD Expected;

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        SetLastError(0xdeadbeef);
        Event = pCreateEventExW(NULL, NULL, Tests[i].Flags, EVENT_ALL_ACCESS);
        ok(Event != NULL, "[%lu] CreateEventExW failed, error %lu\n", i, GetLastError());
        if (!Event)
            continue;
        ok(GetLastError() == ERROR_SUCCESS, "[%lu] error %lu\n", i, GetLastError());
        CheckEvent(Event, Tests[i].Type, Tests[i].State);
        CheckAccess(Event, EVENT_ALL_ACCESS);

        /* An auto-reset event is consumed by one wait, a manual-reset event stays signaled */
        ok(SetEvent(Event), "[%lu] SetEvent failed, error %lu\n", i, GetLastError());
        ok(WaitForSingleObject(Event, 0) == WAIT_OBJECT_0, "[%lu] first wait failed\n", i);
        Expected = (Tests[i].Type == NotificationEvent) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
        ok(WaitForSingleObject(Event, 0) == Expected, "[%lu] wrong second wait result\n", i);
        CloseHandle(Event);
    }
}

static void
TestEventAccessAndNames(void)
{
    WCHAR NameW[64];
    CHAR NameA[64];
    CHAR LongNameA[400];
    WCHAR LongNameW[400];
    HANDLE Event, Event2, Mutex;
    DWORD Flags;
    size_t Length;

    /* The handle has the requested access only */
    Event = pCreateEventExW(NULL, NULL, 0, SYNCHRONIZE);
    ok(Event != NULL, "CreateEventExW failed, error %lu\n", GetLastError());
    if (Event)
    {
        CheckAccess(Event, SYNCHRONIZE);
        SetLastError(0xdeadbeef);
        ok(!SetEvent(Event), "SetEvent succeeded without EVENT_MODIFY_STATE\n");
        ok(GetLastError() == ERROR_ACCESS_DENIED, "error %lu\n", GetLastError());
        ok(WaitForSingleObject(Event, 0) == WAIT_TIMEOUT, "wait failed\n");
        CloseHandle(Event);
    }

    Event = pCreateEventExW(NULL, NULL, CREATE_EVENT_INITIAL_SET, EVENT_MODIFY_STATE);
    ok(Event != NULL, "CreateEventExW failed, error %lu\n", GetLastError());
    if (Event)
    {
        CheckAccess(Event, EVENT_MODIFY_STATE);
        ok(ResetEvent(Event), "ResetEvent failed, error %lu\n", GetLastError());
        SetLastError(0xdeadbeef);
        ok(WaitForSingleObject(Event, 0) == WAIT_FAILED, "wait without SYNCHRONIZE did not fail\n");
        ok(GetLastError() == ERROR_ACCESS_DENIED, "error %lu\n", GetLastError());
        CloseHandle(Event);
    }

    /* A second create of the same name opens the same event */
    StringCbPrintfW(NameW, sizeof(NameW), L"Local\\ros_evmx_test_e_%lu", GetCurrentProcessId());
    Event = pCreateEventExW(NULL, NameW, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    ok(Event != NULL, "CreateEventExW failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_SUCCESS, "error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Event2 = pCreateEventExW(NULL, NameW, 0, SYNCHRONIZE);
    ok(Event2 != NULL, "second CreateEventExW failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_ALREADY_EXISTS, "error %lu\n", GetLastError());
    if (Event && Event2)
    {
        CheckAccess(Event2, SYNCHRONIZE);
        ok(WaitForSingleObject(Event2, 0) == WAIT_TIMEOUT, "wait failed\n");
        ok(SetEvent(Event), "SetEvent failed, error %lu\n", GetLastError());
        /* The event keeps the type it was created with */
        ok(WaitForSingleObject(Event2, 0) == WAIT_OBJECT_0, "wait failed\n");
        ok(WaitForSingleObject(Event2, 0) == WAIT_OBJECT_0, "wait failed\n");
    }

    /* The name is taken by an event */
    SetLastError(0xdeadbeef);
    Mutex = pCreateMutexExW(NULL, NameW, 0, MUTEX_ALL_ACCESS);
    ok(Mutex == NULL, "CreateMutexExW on an event name succeeded\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE, "error %lu\n", GetLastError());
    if (Mutex)
        CloseHandle(Mutex);
    if (Event2)
        CloseHandle(Event2);
    if (Event)
        CloseHandle(Event);

    /* ANSI version, with and without a name */
    StringCbPrintfA(NameA, sizeof(NameA), "Local\\ros_evmx_test_a_%lu", GetCurrentProcessId());
    SetLastError(0xdeadbeef);
    Event = pCreateEventExA(NULL, NameA, CREATE_EVENT_INITIAL_SET, EVENT_ALL_ACCESS);
    ok(Event != NULL, "CreateEventExA failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_SUCCESS, "error %lu\n", GetLastError());
    if (Event)
    {
        CheckEvent(Event, SynchronizationEvent, 1);
        StringCbPrintfW(NameW, sizeof(NameW), L"Local\\ros_evmx_test_a_%lu", GetCurrentProcessId());
        Event2 = OpenEventW(SYNCHRONIZE, FALSE, NameW);
        ok(Event2 != NULL, "OpenEventW failed, error %lu\n", GetLastError());
        if (Event2)
        {
            ok(WaitForSingleObject(Event2, 0) == WAIT_OBJECT_0, "wait failed\n");
            CloseHandle(Event2);
        }
        CloseHandle(Event);
    }
    Event = pCreateEventExA(NULL, NULL, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    ok(Event != NULL, "CreateEventExA failed, error %lu\n", GetLastError());
    if (Event)
    {
        CheckEvent(Event, NotificationEvent, 0);
        CloseHandle(Event);
    }

    /* An ANSI name longer than MAX_PATH is accepted */
    StringCbPrintfA(LongNameA, sizeof(LongNameA), "Local\\ros_evmx_test_l_%lu_",
                    GetCurrentProcessId());
    Length = strlen(LongNameA);
    memset(LongNameA + Length, 'a', sizeof(LongNameA) - 1 - Length);
    LongNameA[sizeof(LongNameA) - 1] = ANSI_NULL;
    SetLastError(0xdeadbeef);
    Event = pCreateEventExA(NULL, LongNameA, 0, EVENT_ALL_ACCESS);
    ok(Event != NULL, "CreateEventExA with a long name failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_SUCCESS, "error %lu\n", GetLastError());
    MultiByteToWideChar(CP_ACP, 0, LongNameA, -1, LongNameW, RTL_NUMBER_OF(LongNameW));
    SetLastError(0xdeadbeef);
    Event2 = pCreateEventExW(NULL, LongNameW, 0, SYNCHRONIZE);
    ok(Event2 != NULL, "CreateEventExW with a long name failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_ALREADY_EXISTS, "error %lu\n", GetLastError());
    if (Event2)
        CloseHandle(Event2);
    if (Event)
        CloseHandle(Event);

    /* Unknown flags */
    for (Flags = 4; Flags; Flags <<= 1)
    {
        SetLastError(0xdeadbeef);
        Event = pCreateEventExW(NULL, NULL, Flags, EVENT_ALL_ACCESS);
        ok(Event == NULL, "CreateEventExW with flags 0x%lx succeeded\n", Flags);
        ok(GetLastError() == ERROR_INVALID_PARAMETER,
           "flags 0x%lx: error %lu\n", Flags, GetLastError());
        if (Event)
            CloseHandle(Event);
    }
}

static void
TestMutex(void)
{
    WCHAR NameW[64];
    CHAR NameA[64];
    CHAR LongNameA[400];
    WCHAR LongNameW[400];
    HANDLE Mutex, Mutex2;
    DWORD Flags;
    size_t Length;

    SetLastError(0xdeadbeef);
    Mutex = pCreateMutexExW(NULL, NULL, 0, MUTEX_ALL_ACCESS);
    ok(Mutex != NULL, "CreateMutexExW failed, error %lu\n", GetLastError());
    if (Mutex)
    {
        ok(GetLastError() == ERROR_SUCCESS, "error %lu\n", GetLastError());
        CheckMutant(Mutex, 1, FALSE);
        CheckAccess(Mutex, MUTEX_ALL_ACCESS);
        SetLastError(0xdeadbeef);
        ok(!ReleaseMutex(Mutex), "ReleaseMutex of a free mutex succeeded\n");
        ok(GetLastError() == ERROR_NOT_OWNER, "error %lu\n", GetLastError());
        CloseHandle(Mutex);
    }

    Mutex = pCreateMutexExW(NULL, NULL, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    ok(Mutex != NULL, "CreateMutexExW failed, error %lu\n", GetLastError());
    if (Mutex)
    {
        CheckMutant(Mutex, 0, TRUE);
        ok(ReleaseMutex(Mutex), "ReleaseMutex failed, error %lu\n", GetLastError());
        CheckMutant(Mutex, 1, FALSE);
        CloseHandle(Mutex);
    }

    /* The handle has the requested access only */
    Mutex = pCreateMutexExW(NULL, NULL, CREATE_MUTEX_INITIAL_OWNER, SYNCHRONIZE);
    ok(Mutex != NULL, "CreateMutexExW failed, error %lu\n", GetLastError());
    if (Mutex)
    {
        CheckAccess(Mutex, SYNCHRONIZE);
        /* The owner may release through a handle without MUTEX_MODIFY_STATE */
        ok(ReleaseMutex(Mutex), "ReleaseMutex failed, error %lu\n", GetLastError());
        CloseHandle(Mutex);
    }

    /* Named, ANSI and Unicode views of the same mutex */
    StringCbPrintfA(NameA, sizeof(NameA), "Local\\ros_evmx_test_m_%lu", GetCurrentProcessId());
    StringCbPrintfW(NameW, sizeof(NameW), L"Local\\ros_evmx_test_m_%lu", GetCurrentProcessId());
    SetLastError(0xdeadbeef);
    Mutex = pCreateMutexExA(NULL, NameA, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    ok(Mutex != NULL, "CreateMutexExA failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_SUCCESS, "error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Mutex2 = pCreateMutexExW(NULL, NameW, 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    ok(Mutex2 != NULL, "CreateMutexExW failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_ALREADY_EXISTS, "error %lu\n", GetLastError());
    if (Mutex && Mutex2)
    {
        CheckAccess(Mutex2, SYNCHRONIZE | MUTEX_MODIFY_STATE);
        /* The same thread owns it through either handle */
        CheckMutant(Mutex2, 0, TRUE);
        ok(ReleaseMutex(Mutex2), "ReleaseMutex failed, error %lu\n", GetLastError());
        CheckMutant(Mutex, 1, FALSE);
    }
    if (Mutex2)
        CloseHandle(Mutex2);
    if (Mutex)
        CloseHandle(Mutex);

    Mutex = pCreateMutexExA(NULL, NULL, 0, MUTEX_ALL_ACCESS);
    ok(Mutex != NULL, "CreateMutexExA failed, error %lu\n", GetLastError());
    if (Mutex)
    {
        CheckMutant(Mutex, 1, FALSE);
        CloseHandle(Mutex);
    }

    /* An ANSI name longer than MAX_PATH is accepted */
    StringCbPrintfA(LongNameA, sizeof(LongNameA), "Local\\ros_evmx_test_n_%lu_",
                    GetCurrentProcessId());
    Length = strlen(LongNameA);
    memset(LongNameA + Length, 'm', sizeof(LongNameA) - 1 - Length);
    LongNameA[sizeof(LongNameA) - 1] = ANSI_NULL;
    SetLastError(0xdeadbeef);
    Mutex = pCreateMutexExA(NULL, LongNameA, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    ok(Mutex != NULL, "CreateMutexExA with a long name failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_SUCCESS, "error %lu\n", GetLastError());
    MultiByteToWideChar(CP_ACP, 0, LongNameA, -1, LongNameW, RTL_NUMBER_OF(LongNameW));
    SetLastError(0xdeadbeef);
    Mutex2 = pCreateMutexExW(NULL, LongNameW, 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    ok(Mutex2 != NULL, "CreateMutexExW with a long name failed, error %lu\n", GetLastError());
    ok(GetLastError() == ERROR_ALREADY_EXISTS, "error %lu\n", GetLastError());
    if (Mutex2)
    {
        CheckMutant(Mutex2, 0, TRUE);
        CloseHandle(Mutex2);
    }
    if (Mutex)
    {
        ok(ReleaseMutex(Mutex), "ReleaseMutex failed, error %lu\n", GetLastError());
        CloseHandle(Mutex);
    }

    /* Unknown flags */
    for (Flags = 2; Flags; Flags <<= 1)
    {
        SetLastError(0xdeadbeef);
        Mutex = pCreateMutexExW(NULL, NULL, Flags, MUTEX_ALL_ACCESS);
        ok(Mutex == NULL, "CreateMutexExW with flags 0x%lx succeeded\n", Flags);
        ok(GetLastError() == ERROR_INVALID_PARAMETER,
           "flags 0x%lx: error %lu\n", Flags, GetLastError());
        if (Mutex)
            CloseHandle(Mutex);
    }
}

START_TEST(CreateEventMutexEx)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

    pCreateEventExW = (PFN_CREATEEVENTEXW)GetProcAddress(hKernel32, "CreateEventExW");
    pCreateEventExA = (PFN_CREATEEVENTEXA)GetProcAddress(hKernel32, "CreateEventExA");
    pCreateMutexExW = (PFN_CREATEMUTEXEXW)GetProcAddress(hKernel32, "CreateMutexExW");
    pCreateMutexExA = (PFN_CREATEMUTEXEXA)GetProcAddress(hKernel32, "CreateMutexExA");
    if (!pCreateEventExW || !pCreateEventExA || !pCreateMutexExW || !pCreateMutexExA)
    {
        skip("CreateEventEx or CreateMutexEx is not available\n");
        return;
    }

    TestEventFlags();
    TestEventAccessAndNames();
    TestMutex();
}
