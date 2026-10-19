/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for advapi32!EventSetInformation (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <evntprov.h>
#include <pseh/pseh2.h>

typedef ULONG (WINAPI *PFN_EventSetInformation)(REGHANDLE, ULONG, PVOID, ULONG);

static const GUID TestProvider = { 0x4c1a3b86, 0x7d1e, 0x4a52, { 0x9e, 0x3b, 0x1f, 0x6a, 0x2d, 0x4e, 0x5c, 0x72 } };

static PFN_EventSetInformation pEventSetInformation;
static ULONG (WINAPI *pEventRegister)(LPCGUID, PENABLECALLBACK, PVOID, PREGHANDLE);
static ULONG (WINAPI *pEventUnregister)(REGHANDLE);

#define PENDING_ERROR 0xdeadbeef

/*
 * The last error is set for an unusable handle, a bad buffer, a trait block that cannot be set, a corrupt one and
 * a descriptor type of class 3 that is not 0 or 1; all other results leave it alone (measured)
 */
static ULONG SetInfo(REGHANDLE Handle, ULONG Class, PVOID Info, ULONG Length, ULONG Expected, int Line)
{
    ULONG Result, Error;

    SetLastError(PENDING_ERROR);
    Result = pEventSetInformation(Handle, Class, Info, Length);
    Error = GetLastError();
    ok(Result == Expected, "line %d: got %lu, expected %lu\n", Line, Result, Expected);
    if (Expected == ERROR_INVALID_HANDLE || Expected == ERROR_GEN_FAILURE || Expected == ERROR_FILE_CORRUPT ||
        Expected == ERROR_NOACCESS)
        ok(Error == Expected, "line %d: last error %lu, expected %lu\n", Line, Error, Expected);
    else
        ok(Error == PENDING_ERROR, "line %d: last error %lu was changed\n", Line, Error);
    return Result;
}

#define SET(h, c, i, l, e) SetInfo(h, c, (PVOID)(i), l, e, __LINE__)

/* A descriptor type that is not valid: the result and the last error are both ERROR_INVALID_PARAMETER (measured) */
static void SetBadType(REGHANDLE Handle, UCHAR Type, int Line)
{
    ULONG Result, Error;

    SetLastError(PENDING_ERROR);
    Result = pEventSetInformation(Handle, 3, &Type, 1);
    Error = GetLastError();
    ok(Result == ERROR_INVALID_PARAMETER, "line %d: got %lu\n", Line, Result);
    ok(Error == ERROR_INVALID_PARAMETER, "line %d: last error %lu\n", Line, Error);
}

static REGHANDLE Register(void)
{
    REGHANDLE Handle = 0;

    ok_dec(pEventRegister(&TestProvider, NULL, NULL, &Handle), ERROR_SUCCESS);
    ok(Handle != 0, "no registration handle\n");
    return Handle;
}

/* Traits: the size of the block, a zero-terminated name, then extensions of size, type and data. */
static ULONG SetTraits(REGHANDLE Handle, const UCHAR *Block, ULONG Length)
{
    return pEventSetInformation(Handle, 2, (PVOID)Block, Length);
}

static REGHANDLE g_Handle;
static ULONG g_Result;
static LONG g_Calls;
static ULONG g_NestedResult = 0xdead;
static BOOL g_Stuck;
static ULONG (WINAPI *pEventWriteString)(REGHANDLE, UCHAR, ULONGLONG, PCWSTR);

/* A handler for the exception of a bad buffer that uses the provider functions must not wait for the call that faulted */
static LONG CALLBACK Handler(PEXCEPTION_POINTERS Info)
{
    UNREFERENCED_PARAMETER(Info);
    InterlockedIncrement(&g_Calls);
    g_NestedResult = pEventWriteString(g_Handle, 4, 1, L"text");
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI BadBufferThread(PVOID Param)
{
    UNREFERENCED_PARAMETER(Param);
    g_Result = pEventSetInformation(g_Handle, 2, (PVOID)(ULONG_PTR)1, 8);
    return 0;
}

/*
 * On Windows 10 the call does not raise an exception for the bad buffer of the traits (it returns ERROR_NOACCESS), so the
 * handler is not called there. On ReactOS the fault is caught inside the call, after the handler has seen it.
 * If the call never returns, the thread stays inside it and no provider function is used again.
 */
static void TestExceptionHandler(void)
{
    PVOID Cookie;
    HANDLE Thread;
    DWORD Wait;

    pEventWriteString = (PVOID)GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "EventWriteString");
    if (!pEventWriteString)
        return;

    g_Handle = Register();
    Cookie = AddVectoredExceptionHandler(1, Handler);
    ok(Cookie != NULL, "the vectored handler was not installed\n");
    Thread = CreateThread(NULL, 0, BadBufferThread, NULL, 0, NULL);
    if (!Thread)
    {
        skip("no thread\n");
        RemoveVectoredExceptionHandler(Cookie);
        return;
    }
    Wait = WaitForSingleObject(Thread, 20000);
    ok(Wait == WAIT_OBJECT_0, "the call with a bad buffer did not return\n");
    if (Wait != WAIT_OBJECT_0)
    {
        g_Stuck = TRUE;
        return;
    }

    ok(g_Result == ERROR_NOACCESS, "got %lu\n", g_Result);
    CloseHandle(Thread);
    if (is_reactos())
    {
        ok(g_Calls > 0, "the vectored handler was not called\n");
        ok_dec(g_NestedResult, ERROR_SUCCESS);
    }
    RemoveVectoredExceptionHandler(Cookie);
    ok_dec(pEventUnregister(g_Handle), ERROR_SUCCESS);
}

/*
 * The descriptor type of class 3 is read without a guard: a bad pointer is the exception of the caller (measured on
 * Windows 10). The check runs in a process of its own, the test EventSetInformationFault, so that the state that a
 * fault inside the call leaves behind cannot reach the rest of this test.
 */
static void TestClass3FaultProcess(void)
{
    WCHAR Path[MAX_PATH], Command[MAX_PATH + 64];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0xdead;

    if (!GetModuleFileNameW(NULL, Path, RTL_NUMBER_OF(Path)))
    {
        skip("no module name\n");
        return;
    }
    lstrcpyW(Command, L"\"");
    lstrcatW(Command, Path);
    lstrcatW(Command, L"\" EventSetInformationFault");
    if (!CreateProcessW(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("the process did not start (%lu)\n", GetLastError());
        return;
    }
    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "the process of the exception check did not end\n");
    if (Wait == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        ok(ExitCode == 0, "the process of the exception check had %lu failures\n", ExitCode);
    }
    else
    {
        TerminateProcess(ProcessInfo.hProcess, 0);
    }
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

START_TEST(EventSetInformationFault)
{
    HMODULE Advapi = GetModuleHandleW(L"advapi32.dll");
    REGHANDLE Handle, Other;
    BOOL Faulted = FALSE;
    ULONG Status = 0xdead;

    pEventSetInformation = (PFN_EventSetInformation)GetProcAddress(Advapi, "EventSetInformation");
    pEventRegister = (PVOID)GetProcAddress(Advapi, "EventRegister");
    pEventUnregister = (PVOID)GetProcAddress(Advapi, "EventUnregister");
    if (!pEventSetInformation || !pEventRegister || !pEventUnregister)
    {
        skip("EventSetInformation is not exported\n");
        return;
    }

    Handle = Register();
    _SEH2_TRY
    {
        Status = pEventSetInformation(Handle, 3, (PVOID)(ULONG_PTR)1, 1);
    }
    _SEH2_EXCEPT(_SEH2_GetExceptionCode() == STATUS_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        Faulted = TRUE;
    }
    _SEH2_END;
    ok(Faulted, "the call returned %lu instead of raising the exception\n", Status);

    /* the registration can be used and ended afterwards, and another one can be made */
    ok_dec(pEventSetInformation(Handle, 0, NULL, 0), ERROR_SUCCESS);
    Other = Register();
    ok_dec(pEventUnregister(Other), ERROR_SUCCESS);
    ok_dec(pEventUnregister(Handle), ERROR_SUCCESS);
}

static void TestTraitsContents(void)
{
    static const struct
    {
        const char *Name;
        UCHAR Block[16];
        ULONG Length;
        ULONG Result;
    } Cases[] =
    {
        { "name abc",                  { 6, 0, 'a', 'b', 'c', 0 }, 6, ERROR_SUCCESS },
        { "empty name",                { 3, 0, 0 }, 3, ERROR_SUCCESS },
        { "name x",                    { 4, 0, 'x', 0 }, 4, ERROR_SUCCESS },
        { "name xy",                   { 5, 0, 'x', 'y', 0 }, 5, ERROR_SUCCESS },
        { "name bytes are not checked", { 5, 0, 0xff, 0xfe, 0 }, 5, ERROR_SUCCESS },
        { "one extension of three bytes", { 8, 0, 'x', 'y', 0, 3, 0, 1 }, 8, ERROR_SUCCESS },
        { "extension type 0",          { 8, 0, 'x', 'y', 0, 3, 0, 0 }, 8, ERROR_SUCCESS },
        { "extension type 0x80",       { 8, 0, 'x', 'y', 0, 3, 0, 0x80 }, 8, ERROR_SUCCESS },
        { "extension with data",       { 11, 0, 'x', 'y', 0, 6, 0, 1, 1, 2, 3 }, 11, ERROR_SUCCESS },
        { "size field too small",      { 2, 0, 'a', 'b', 'c', 0 }, 6, ERROR_FILE_CORRUPT },
        { "size field zero",           { 0, 0, 'x', 'y', 0 }, 5, ERROR_FILE_CORRUPT },
        { "size field larger",         { 6, 0, 'x', 'y', 0 }, 5, ERROR_FILE_CORRUPT },
        { "length larger",             { 5, 0, 'x', 'y', 0, 0 }, 6, ERROR_FILE_CORRUPT },
        { "length smaller",            { 5, 0, 'x', 'y', 0 }, 4, ERROR_FILE_CORRUPT },
        { "name without terminator",   { 4, 0, 'x', 'y' }, 4, ERROR_FILE_CORRUPT },
        { "name without terminator, 3", { 3, 0, 'x' }, 3, ERROR_FILE_CORRUPT },
        { "empty name, extension size too big", { 6, 0, 0, 'b', 'c', 0 }, 6, ERROR_FILE_CORRUPT },
        { "extension too long",        { 8, 0, 'x', 'y', 0, 4, 0, 1 }, 8, ERROR_FILE_CORRUPT },
        { "extension size below three", { 8, 0, 'x', 'y', 0, 2, 0, 1 }, 8, ERROR_FILE_CORRUPT },
        { "one byte after the name",   { 9, 0, 'x', 'y', 0, 3, 0, 1, 0 }, 9, ERROR_FILE_CORRUPT },
    };
    ULONG i, Result;

    for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
    {
        REGHANDLE Handle = Register();

        /* a fresh registration, so that an earlier success does not matter */
        Result = SetTraits(Handle, Cases[i].Block, Cases[i].Length);
        ok(Result == Cases[i].Result, "%s: got %lu, expected %lu\n", Cases[i].Name, Result, Cases[i].Result);
        if (Cases[i].Result == ERROR_SUCCESS)
        {
            /* the traits of a registration can be set once */
            Result = SetTraits(Handle, Cases[i].Block, Cases[i].Length);
            ok(Result == ERROR_GEN_FAILURE, "%s again: got %lu\n", Cases[i].Name, Result);
        }
        ok_dec(pEventUnregister(Handle), ERROR_SUCCESS);
    }
}

START_TEST(EventSetInformation)
{
    HMODULE Advapi = GetModuleHandleW(L"advapi32.dll");
    REGHANDLE Handle, Other;
    UCHAR Zeros[16] = { 0 };
    UCHAR Good[8] = { 8, 0, 'a', 'b', 'c', 'd', 'e', 0 };
    UCHAR Value;
    ULONG Class;
    REGHANDLE Invalid[] = { 0, 1, 0xdeadbeef, ~(REGHANDLE)0 };
    ULONG i;

    pEventSetInformation = (PFN_EventSetInformation)GetProcAddress(Advapi, "EventSetInformation");
    pEventRegister = (PVOID)GetProcAddress(Advapi, "EventRegister");
    pEventUnregister = (PVOID)GetProcAddress(Advapi, "EventUnregister");

    if (!pEventRegister || !pEventUnregister)
    {
        skip("the event provider functions are missing\n");
        return;
    }

    /* Windows 7 and older have no EventSetInformation: the function arrived with Windows 8 */
    if (!pEventSetInformation)
    {
        /* ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "EventSetInformation is not exported\n");
        skip("EventSetInformation is not exported\n");
        return;
    }

    Handle = Register();

    /* classes that are not supported: the answer comes before anything else is looked at */
    for (Class = 0; Class < 8; Class++)
    {
        if (Class == 0 || Class == 2 || Class == 3)
            continue;
        SET(Handle, Class, Zeros, 8, ERROR_NOT_SUPPORTED);
        SET(Handle, Class, NULL, 0, ERROR_NOT_SUPPORTED);
        SET(0, Class, Zeros, 8, ERROR_NOT_SUPPORTED);
        SET(0, Class, NULL, 0, ERROR_NOT_SUPPORTED);
    }
    SET(Handle, 99, Zeros, 8, ERROR_NOT_SUPPORTED);
    SET(0, 99, NULL, 0, ERROR_NOT_SUPPORTED);
    SET(Handle, 0x80000000, Zeros, 1, ERROR_NOT_SUPPORTED);
    SET(Handle, 0xffffffff, Zeros, 1, ERROR_NOT_SUPPORTED);

    /* class 0 takes no data */
    SET(Handle, 0, NULL, 0, ERROR_SUCCESS);
    SET(Handle, 0, Zeros, 0, ERROR_SUCCESS);
    SET(Handle, 0, Zeros, 1, ERROR_INVALID_PARAMETER);
    SET(Handle, 0, Zeros, 4, ERROR_INVALID_PARAMETER);
    SET(Handle, 0, NULL, 8, ERROR_INVALID_PARAMETER);

    /* class 2 (traits): at least three bytes; the checks run in the order length, handle, contents */
    SET(Handle, 2, Good, 0, ERROR_INVALID_PARAMETER);
    SET(Handle, 2, Good, 1, ERROR_INVALID_PARAMETER);
    SET(Handle, 2, Good, 2, ERROR_INVALID_PARAMETER);
    SET(Handle, 2, NULL, 0, ERROR_INVALID_PARAMETER);
    SET(Handle, 2, NULL, 8, ERROR_INVALID_PARAMETER);
    SET(0, 2, NULL, 8, ERROR_INVALID_PARAMETER);
    SET(0, 2, Good, 2, ERROR_INVALID_PARAMETER);
    SET(Handle, 2, Zeros, 3, ERROR_FILE_CORRUPT);
    SET(Handle, 2, Zeros, 4, ERROR_FILE_CORRUPT);
    SET(Handle, 2, Zeros, 8, ERROR_FILE_CORRUPT);
    SET(Handle, 2, (PVOID)(ULONG_PTR)1, 8, ERROR_NOACCESS);
    SET(Handle, 2, Good, 0x10000, ERROR_INVALID_PARAMETER);
    SET(Handle, 2, Good, 0xffffffff, ERROR_INVALID_PARAMETER);

    /* an unusable handle is checked before the contents, so zeros are an invalid handle here */
    for (i = 0; i < RTL_NUMBER_OF(Invalid); i++)
    {
        SET(Invalid[i], 2, Zeros, 8, ERROR_INVALID_HANDLE);
        SET(Invalid[i], 2, Good, 8, ERROR_INVALID_HANDLE);
        SET(Invalid[i], 2, Zeros, 3, ERROR_INVALID_HANDLE);
        SET(Invalid[i], 0, NULL, 0, ERROR_INVALID_HANDLE);
        SET(Invalid[i], 3, &Zeros[0], 1, ERROR_INVALID_HANDLE);
    }
    SET(Handle + 1, 2, Zeros, 8, ERROR_INVALID_HANDLE);
    SET(0, 0, Zeros, 4, ERROR_INVALID_PARAMETER);
    SET(0, 3, Zeros, 4, ERROR_INVALID_PARAMETER);

    /* class 3 (descriptor type): one byte, 0 or 1 */
    SET(Handle, 3, Zeros, 0, ERROR_INVALID_PARAMETER);
    SET(Handle, 3, Zeros, 2, ERROR_INVALID_PARAMETER);
    SET(Handle, 3, Zeros, 4, ERROR_INVALID_PARAMETER);
    SET(Handle, 3, Zeros, 8, ERROR_INVALID_PARAMETER);
    SET(Handle, 3, NULL, 1, ERROR_INVALID_PARAMETER);
    for (Value = 0; Value < 2; Value++)
        SET(Handle, 3, &Value, 1, ERROR_SUCCESS);
    for (Value = 2; Value < 6; Value++)
        SetBadType(Handle, Value, __LINE__);
    SetBadType(Handle, 0xff, __LINE__);
    Value = 0;
    SET(Handle, 3, &Value, 1, ERROR_SUCCESS);
    SET(Handle, 3, &Value, 1, ERROR_SUCCESS);

    /* the traits of one registration can be set once, and an invalid block does not use that up */
    Other = Register();
    SET(Other, 2, Zeros, 8, ERROR_FILE_CORRUPT);
    SET(Other, 2, Good, 8, ERROR_SUCCESS);
    SET(Other, 2, Good, 8, ERROR_GEN_FAILURE);
    SET(Other, 2, Zeros, 8, ERROR_GEN_FAILURE);
    SET(Other, 2, (PVOID)(ULONG_PTR)1, 8, ERROR_GEN_FAILURE);
    SET(Other, 2, Good, 2, ERROR_INVALID_PARAMETER);
    SET(Other, 2, NULL, 8, ERROR_INVALID_PARAMETER);
    SET(Other, 3, &Value, 1, ERROR_SUCCESS);
    SET(Other, 0, NULL, 0, ERROR_SUCCESS);
    ok_dec(pEventUnregister(Other), ERROR_SUCCESS);

    /* after the registration is gone its handle is invalid, and the length checks still come first */
    SET(Other, 2, Zeros, 8, ERROR_INVALID_HANDLE);
    SET(Other, 3, &Value, 1, ERROR_INVALID_HANDLE);
    SET(Other, 3, &Value, 4, ERROR_INVALID_PARAMETER);
    SET(Other, 0, NULL, 0, ERROR_INVALID_HANDLE);
    SET(Other, 5, NULL, 0, ERROR_NOT_SUPPORTED);

    TestTraitsContents();
    TestClass3FaultProcess();
    TestExceptionHandler();
    if (g_Stuck)
        return;

    ok_dec(pEventUnregister(Handle), ERROR_SUCCESS);
}
