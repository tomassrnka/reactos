/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ReadDirectoryChangesW (NtNotifyChangeDirectoryFile), CancelIoEx on it and
 *              SetFileCompletionNotificationModes, also from 32-bit processes on a 64-bit system
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#endif
#ifndef FILE_SKIP_SET_EVENT_ON_HANDLE
#define FILE_SKIP_SET_EVENT_ON_HANDLE 0x2
#endif

#define NAME_LEN 11
#define RECORD_LEN (FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName) + NAME_LEN * sizeof(WCHAR))

typedef BOOL WINAPI FN_CancelIoEx(_In_ HANDLE hFile, _In_opt_ LPOVERLAPPED lpOverlapped);
typedef BOOL WINAPI FN_SetFileCompletionNotificationModes(_In_ HANDLE FileHandle, _In_ UCHAR Flags);

static FN_CancelIoEx *pCancelIoEx;
static FN_SetFileCompletionNotificationModes *pSetFileCompletionNotificationModes;

static WCHAR Dir[MAX_PATH], File[MAX_PATH];
static ULONGLONG NotifyBuffer[32];
static volatile LONG RoutineCalls;
static volatile DWORD RoutineError = 0xdeadbeef, RoutineBytes = 0xdeadbeef;
static LPOVERLAPPED RoutineOverlapped;

static
HANDLE
OpenDir(VOID)
{
    return CreateFileW(Dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
}

static
VOID
MakeFile(VOID)
{
    HANDLE Handle = CreateFileW(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    CloseHandle(Handle);
}

static
VOID
CheckRecord(_In_ PCSTR Label, _In_ DWORD Bytes)
{
    FILE_NOTIFY_INFORMATION *Info = (FILE_NOTIFY_INFORMATION *)NotifyBuffer;

    ok(Bytes == RECORD_LEN, "%s: %lu bytes, expected %lu\n", Label, Bytes, (DWORD)RECORD_LEN);
    ok(Info->NextEntryOffset == 0, "%s: next entry %lu\n", Label, Info->NextEntryOffset);
    ok(Info->Action == FILE_ACTION_ADDED, "%s: action %lu\n", Label, Info->Action);
    ok(Info->FileNameLength == NAME_LEN * sizeof(WCHAR) &&
       !memcmp(Info->FileName, L"newfile.txt", NAME_LEN * sizeof(WCHAR)),
       "%s: name %.*S\n", Label, (int)(Info->FileNameLength / sizeof(WCHAR)), Info->FileName);
}

static
VOID
TestEvent(VOID)
{
    OVERLAPPED Ov = {0};
    HANDLE DirHandle = OpenDir();
    DWORD Bytes = 0xdeadbeef;
    BOOL Ret;

    ok(DirHandle != INVALID_HANDLE_VALUE, "OpenDir failed: %lu\n", GetLastError());
    if (DirHandle == INVALID_HANDLE_VALUE)
        return;
    Ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    memset(NotifyBuffer, 0xcc, sizeof(NotifyBuffer));

    Ret = ReadDirectoryChangesW(DirHandle, NotifyBuffer, sizeof(NotifyBuffer), FALSE,
                                FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &Ov, NULL);
    ok(Ret, "ReadDirectoryChangesW failed: %lu\n", GetLastError());
    if (Ret)
    {
        ok(WaitForSingleObject(Ov.hEvent, 100) == WAIT_TIMEOUT, "completed with no change\n");
        ok((NTSTATUS)Ov.Internal == STATUS_PENDING, "Internal is 0x%lx\n", (ULONG)Ov.Internal);
        MakeFile();
        ok(WaitForSingleObject(Ov.hEvent, 5000) == WAIT_OBJECT_0, "no completion\n");
        ok((NTSTATUS)Ov.Internal == STATUS_SUCCESS, "Internal is 0x%lx\n", (ULONG)Ov.Internal);
        Ret = GetOverlappedResult(DirHandle, &Ov, &Bytes, FALSE);
        ok(Ret, "GetOverlappedResult failed: %lu\n", GetLastError());
        CheckRecord("event", Bytes);
    }
    CloseHandle(Ov.hEvent);
    CloseHandle(DirHandle);
    DeleteFileW(File);
}

static
VOID
CALLBACK
Routine(_In_ DWORD Error, _In_ DWORD Bytes, _In_ LPOVERLAPPED Overlapped)
{
    RoutineError = Error;
    RoutineBytes = Bytes;
    RoutineOverlapped = Overlapped;
    InterlockedIncrement(&RoutineCalls);
}

static
VOID
TestRoutine(VOID)
{
    OVERLAPPED Ov = {0};
    HANDLE DirHandle = OpenDir();
    DWORD Wait;
    BOOL Ret;

    ok(DirHandle != INVALID_HANDLE_VALUE, "OpenDir failed: %lu\n", GetLastError());
    if (DirHandle == INVALID_HANDLE_VALUE)
        return;
    memset(NotifyBuffer, 0xcc, sizeof(NotifyBuffer));
    RoutineCalls = 0;

    Ret = ReadDirectoryChangesW(DirHandle, NotifyBuffer, sizeof(NotifyBuffer), FALSE,
                                FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &Ov, Routine);
    ok(Ret, "ReadDirectoryChangesW failed: %lu\n", GetLastError());
    if (Ret)
    {
        MakeFile();
        Wait = SleepEx(5000, TRUE);
        ok(Wait == WAIT_IO_COMPLETION, "SleepEx returned %lu\n", Wait);
        ok(RoutineCalls == 1, "routine called %ld times\n", RoutineCalls);
        ok(RoutineError == ERROR_SUCCESS, "routine error %lu\n", RoutineError);
        ok(RoutineOverlapped == &Ov, "routine overlapped %p, expected %p\n", RoutineOverlapped, &Ov);
        CheckRecord("routine", RoutineBytes);
    }
    CloseHandle(DirHandle);
    DeleteFileW(File);
}

static
VOID
TestCancel(VOID)
{
    OVERLAPPED Ov = {0};
    HANDLE DirHandle = OpenDir();
    DWORD Bytes = 0xdeadbeef;
    BOOL Ret;

    ok(DirHandle != INVALID_HANDLE_VALUE, "OpenDir failed: %lu\n", GetLastError());
    if (DirHandle == INVALID_HANDLE_VALUE)
        return;
    Ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    Ret = ReadDirectoryChangesW(DirHandle, NotifyBuffer, sizeof(NotifyBuffer), FALSE,
                                FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &Ov, NULL);
    ok(Ret, "ReadDirectoryChangesW failed: %lu\n", GetLastError());
    if (Ret)
    {
        SetLastError(0xdeadbeef);
        Ret = pCancelIoEx(DirHandle, &Ov);
        ok(Ret, "CancelIoEx failed: %lu\n", GetLastError());
        ok(WaitForSingleObject(Ov.hEvent, 5000) == WAIT_OBJECT_0, "cancelled request did not complete\n");
        ok((NTSTATUS)Ov.Internal == (NTSTATUS)0xC0000120, "Internal is 0x%lx\n", (ULONG)Ov.Internal);
        SetLastError(0xdeadbeef);
        Ret = GetOverlappedResult(DirHandle, &Ov, &Bytes, FALSE);
        ok(!Ret, "GetOverlappedResult succeeded\n");
        ok(GetLastError() == ERROR_OPERATION_ABORTED, "error %lu\n", GetLastError());

        /* Nothing left to cancel: ERROR_NOT_FOUND */
        SetLastError(0xdeadbeef);
        Ret = pCancelIoEx(DirHandle, &Ov);
        ok(!Ret, "second CancelIoEx succeeded\n");
        ok(GetLastError() == ERROR_NOT_FOUND, "second CancelIoEx error %lu\n", GetLastError());
    }
    CloseHandle(Ov.hEvent);
    CloseHandle(DirHandle);
}

static
VOID
TestCompletionModes(VOID)
{
    HANDLE Handle;
    BOOL Ret;

    Handle = CreateFileW(File, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return;
    Ret = pSetFileCompletionNotificationModes(Handle, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
    ok(Ret, "SetFileCompletionNotificationModes failed: %lu\n", GetLastError());
    Ret = pSetFileCompletionNotificationModes(Handle,
                                              FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
    ok(Ret, "SetFileCompletionNotificationModes failed: %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pSetFileCompletionNotificationModes(Handle, 0x80);
    ok(!Ret, "invalid flags succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "invalid flags error %lu\n", GetLastError());
    CloseHandle(Handle);
    DeleteFileW(File);
}

START_TEST(ReadDirectoryChanges)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");

    pCancelIoEx = (FN_CancelIoEx *)GetProcAddress(Kernel32, "CancelIoEx");
    pSetFileCompletionNotificationModes =
        (FN_SetFileCompletionNotificationModes *)GetProcAddress(Kernel32, "SetFileCompletionNotificationModes");
    if (!pCancelIoEx || !pSetFileCompletionNotificationModes)
    {
        skip("CancelIoEx or SetFileCompletionNotificationModes is not available\n");
        return;
    }

    GetTempPathW(RTL_NUMBER_OF(Dir), Dir);
    StringCchCatW(Dir, RTL_NUMBER_OF(Dir), L"rostest_rdchanges");
    CreateDirectoryW(Dir, NULL);
    StringCchPrintfW(File, RTL_NUMBER_OF(File), L"%s\\newfile.txt", Dir);
    DeleteFileW(File);

    TestEvent();
    TestRoutine();
    TestCancel();
    TestCompletionModes();

    DeleteFileW(File);
    RemoveDirectoryW(Dir);
}
