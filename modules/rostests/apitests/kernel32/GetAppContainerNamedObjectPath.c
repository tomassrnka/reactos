/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for GetAppContainerNamedObjectPath
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <sddl.h>

typedef BOOL (WINAPI *PFN_GetAppContainerNamedObjectPath)(HANDLE, PSID, ULONG, LPWSTR, PULONG);

#define PACKAGE L"S-1-15-2-3832788584-2518241925-2067964933-4005320318-1370276574-3608861722-327331109"

static PFN_GetAppContainerNamedObjectPath pGetPath;

static const struct
{
    PCWSTR Sid;
    PCWSTR Path;
} Good[] =
{
    /* the package SID, the SID of a child (the four last sub-authorities after a backslash) */
    { PACKAGE, L"AppContainerNamedObjects\\" PACKAGE },
    { PACKAGE L"-1-2-3-4", L"AppContainerNamedObjects\\" PACKAGE L"\\1-2-3-4" },
    { L"S-1-15-2-1-2-3-4-5-6-7", L"AppContainerNamedObjects\\S-1-15-2-1-2-3-4-5-6-7" },
    { L"S-1-15-2-0-0-0-0-0-0-0", L"AppContainerNamedObjects\\S-1-15-2-0-0-0-0-0-0-0" },
    { L"S-1-15-2-4294967295-2-3-4-5-6-7", L"AppContainerNamedObjects\\S-1-15-2-4294967295-2-3-4-5-6-7" },
    { L"S-1-15-2-1-2-3-4-5-6-7-8-9-10-11", L"AppContainerNamedObjects\\S-1-15-2-1-2-3-4-5-6-7\\8-9-10-11" },
};

/* an AppContainer SID has eight or twelve sub-authorities */
static const PCWSTR NotAppContainer[] =
{
    L"S-1-15-2-1",
    L"S-1-15-2-1-2-3-4-5-6-7-8",
    L"S-1-15-2-1-2-3-4-5-6-7-8-9",
    L"S-1-15-2-1-2-3-4-5-6-7-8-9-10",
    L"S-1-15-2-1-2-3-4-5-6-7-8-9-10-11-12",
    L"S-1-15-2-1-2-3-4-5-6-7-8-9-10-11-12-13",
    L"S-1-5-21-1-2-3-4-5-6-7",
    L"S-1-5-32-544",
    L"S-1-15-3-1",
    L"S-1-15-3-1024-1-2-3-4-5-6-7-8",
};

static void CheckFailure(PCSTR Name, HANDLE Token, PSID Sid, ULONG Length, PWSTR Buffer, BOOL WithReturnLength, DWORD ExpectedError)
{
    ULONG ReturnLength = 0x7777;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pGetPath(Token, Sid, Length, Buffer, WithReturnLength ? &ReturnLength : NULL);
    ok(!Ret && GetLastError() == ExpectedError, "%s: ret %d, error %lu, expected %lu\n", Name, Ret, GetLastError(), ExpectedError);
    if (ExpectedError != ERROR_INSUFFICIENT_BUFFER)
        ok(ReturnLength == 0x7777, "%s: the length was set to %lu\n", Name, ReturnLength);
}

START_TEST(GetAppContainerNamedObjectPath)
{
    WCHAR Buffer[512];
    ULONG ReturnLength, i, Needed;
    PSID Sid;
    HANDLE Token, Event;
    BOOL Ret;

    pGetPath = (PFN_GetAppContainerNamedObjectPath)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetAppContainerNamedObjectPath");
    if (!pGetPath)
    {
        ok(!is_reactos(), "GetAppContainerNamedObjectPath is not exported by kernel32.dll\n");
        skip("GetAppContainerNamedObjectPath is not available\n");
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(Good); i++)
    {
        ok(ConvertStringSidToSidW(Good[i].Sid, &Sid), "%ls: ConvertStringSidToSid failed with %lu\n", Good[i].Sid, GetLastError());
        memset(Buffer, 0xcc, sizeof(Buffer));
        ReturnLength = 0x7777;
        SetLastError(0xdeadbeef);
        Ret = pGetPath(NULL, Sid, RTL_NUMBER_OF(Buffer), Buffer, &ReturnLength);
        Needed = (ULONG)wcslen(Good[i].Path) + 1;
        ok(Ret, "%ls: failed with %lu\n", Good[i].Sid, GetLastError());
        ok(!wcscmp(Buffer, Good[i].Path), "%ls: path %ls, expected %ls\n", Good[i].Sid, Buffer, Good[i].Path);
        ok(ReturnLength == Needed, "%ls: length %lu, expected %lu\n", Good[i].Sid, ReturnLength, Needed);
        ok(Buffer[Needed] == 0xcccc, "%ls: wrote beyond the terminator\n", Good[i].Sid);

        /* the length counts characters with the null: one less does not fit */
        ReturnLength = 0x7777;
        SetLastError(0xdeadbeef);
        memset(Buffer, 0xcc, sizeof(Buffer));
        Ret = pGetPath(NULL, Sid, Needed - 1, Buffer, &ReturnLength);
        ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "%ls: short buffer: ret %d, error %lu\n", Good[i].Sid, Ret, GetLastError());
        ok(ReturnLength == Needed, "%ls: short buffer: length %lu, expected %lu\n", Good[i].Sid, ReturnLength, Needed);
        ok(Buffer[0] == 0xcccc, "%ls: short buffer was written\n", Good[i].Sid);
        ReturnLength = 0x7777;
        Ret = pGetPath(NULL, Sid, Needed, Buffer, &ReturnLength);
        ok(Ret && ReturnLength == Needed, "%ls: exact buffer: ret %d, length %lu\n", Good[i].Sid, Ret, ReturnLength);

        /* asking for the size */
        ReturnLength = 0x7777;
        SetLastError(0xdeadbeef);
        Ret = pGetPath(NULL, Sid, 0, NULL, &ReturnLength);
        ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER && ReturnLength == Needed,
           "%ls: size query: ret %d, error %lu, length %lu\n", Good[i].Sid, Ret, GetLastError(), ReturnLength);
        LocalFree(Sid);
    }

    for (i = 0; i < RTL_NUMBER_OF(NotAppContainer); i++)
    {
        ok(ConvertStringSidToSidW(NotAppContainer[i], &Sid), "%ls: ConvertStringSidToSid failed with %lu\n", NotAppContainer[i], GetLastError());
        CheckFailure("not an AppContainer SID", NULL, Sid, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_NOT_APPCONTAINER);
        LocalFree(Sid);
    }

    /* no SID: the AppContainer SID of the token, and this process is not in an AppContainer */
    CheckFailure("current token", NULL, NULL, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_NOT_APPCONTAINER);
    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token), "OpenProcessToken failed with %lu\n", GetLastError());
    CheckFailure("process token", Token, NULL, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_NOT_APPCONTAINER);

    /* a token and a SID together, a missing length pointer, a length without a buffer */
    ConvertStringSidToSidW(PACKAGE, &Sid);
    CheckFailure("token and SID", Token, Sid, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_INVALID_PARAMETER);
    CheckFailure("invalid token and SID", (HANDLE)(ULONG_PTR)0xdeadbeef, Sid, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_INVALID_PARAMETER);
    CheckFailure("no length pointer", NULL, Sid, RTL_NUMBER_OF(Buffer), Buffer, FALSE, ERROR_INVALID_PARAMETER);
    CheckFailure("no length pointer, no SID", NULL, NULL, RTL_NUMBER_OF(Buffer), Buffer, FALSE, ERROR_INVALID_PARAMETER);
    CheckFailure("length without a buffer", NULL, NULL, 300, NULL, TRUE, ERROR_INVALID_PARAMETER);
    LocalFree(Sid);
    CloseHandle(Token);

    /* handles that are no usable token */
    CheckFailure("invalid token", (HANDLE)(ULONG_PTR)0xdeadbeef, NULL, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_INVALID_HANDLE);
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CheckFailure("event as token", Event, NULL, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_INVALID_HANDLE);
    CloseHandle(Event);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &Token))
    {
        CheckFailure("token without TOKEN_QUERY", Token, NULL, RTL_NUMBER_OF(Buffer), Buffer, TRUE, ERROR_ACCESS_DENIED);
        CloseHandle(Token);
    }
}
