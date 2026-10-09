/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for NtNotifyChangeDirectoryFile
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define WAIT_MS 3000
#define NAME_FILTER (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME)

static WCHAR TempDir[MAX_PATH];
static HANDLE Event;
static ULONG Buffer[1024];
/* Global so that a request completing late never writes a dead stack frame */
static IO_STATUS_BLOCK Iosb;

static HANDLE
OpenDir(PCWSTR Path)
{
    return CreateFileW(Path, FILE_LIST_DIRECTORY,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
}

static NTSTATUS
Notify(HANDLE Dir, PIO_STATUS_BLOCK Iosb, PVOID Buf, ULONG Length, ULONG Filter, BOOLEAN Tree)
{
    ResetEvent(Event);
    Iosb->Status = 0x12345678;
    Iosb->Information = 0x12345678;
    return NtNotifyChangeDirectoryFile(Dir, Event, NULL, NULL, Iosb, Buf, Length, Filter, Tree);
}

/* Cancel a request that did not complete, so the test goes on */
static VOID
CancelPending(HANDLE Dir, DWORD Wait)
{
    IO_STATUS_BLOCK CancelIosb;

    if (Wait == WAIT_OBJECT_0)
        return;
    NtCancelIoFile(Dir, &CancelIosb);
    WaitForSingleObject(Event, WAIT_MS);
}

static BOOL
TouchFile(PCWSTR Dir, PCWSTR Name)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;

    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Dir, Name);
    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    CloseHandle(File);
    return TRUE;
}

static VOID
RemoveFile(PCWSTR Dir, PCWSTR Name)
{
    WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Dir, Name);
    DeleteFileW(Path);
}

static HANDLE
MakeDir(PWSTR Path, PCWSTR Name)
{
    StringCchPrintfW(Path, MAX_PATH, L"%s%s", TempDir, Name);
    RemoveDirectoryW(Path);
    if (!CreateDirectoryW(Path, NULL))
        return INVALID_HANDLE_VALUE;
    return OpenDir(Path);
}

static BOOL
HasRecord(ULONG Action, PCWSTR Name, ULONG_PTR Length)
{
    PFILE_NOTIFY_INFORMATION Info = (PFILE_NOTIFY_INFORMATION)Buffer;
    ULONG NameLength = (ULONG)wcslen(Name) * sizeof(WCHAR);
    ULONG_PTR Offset;

    if (Length == 0 || Length > sizeof(Buffer))
        return FALSE;
    for (;;)
    {
        Offset = (ULONG_PTR)((PUCHAR)Info - (PUCHAR)Buffer);
        if (Offset + FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName) > Length ||
            Offset + FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName) + Info->FileNameLength > Length)
        {
            return FALSE;
        }
        if (Info->Action == Action &&
            Info->FileNameLength == NameLength &&
            !memcmp(Info->FileName, Name, NameLength))
        {
            return TRUE;
        }
        if (Info->NextEntryOffset == 0)
            return FALSE;
        Info = (PFILE_NOTIFY_INFORMATION)((PUCHAR)Info + Info->NextEntryOffset);
    }
}

/* A pending request completes when its directory is deleted */
static VOID
TestDeleteWatched(VOID)
{
    WCHAR Path[MAX_PATH];
    NTSTATUS Status;
    HANDLE Dir;
    DWORD Wait;

    Dir = MakeDir(Path, L"ntncdf_del");
    ok(Dir != INVALID_HANDLE_VALUE, "OpenDir failed: %lu\n", GetLastError());
    if (Dir == INVALID_HANDLE_VALUE)
        return;

    Status = Notify(Dir, &Iosb, Buffer, sizeof(Buffer), NAME_FILTER, FALSE);
    ok_hex(Status, STATUS_PENDING);

    ok(RemoveDirectoryW(Path), "RemoveDirectoryW failed: %lu\n", GetLastError());
    Wait = WaitForSingleObject(Event, WAIT_MS);
    ok(Wait == WAIT_OBJECT_0, "Request on the deleted directory did not complete (%lu)\n", Wait);
    if (Wait == WAIT_OBJECT_0)
    {
        ok_hex(Iosb.Status, STATUS_DELETE_PENDING);
        ok_size_t(Iosb.Information, 0);
    }
    CancelPending(Dir, Wait);

    CloseHandle(Dir);
    ok(GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES, "Directory still exists\n");
}

/* After a buffer overflow is reported, the next request waits for a change */
static VOID
TestOverflowReportedOnce(VOID)
{
    WCHAR Path[MAX_PATH];
    NTSTATUS Status;
    HANDLE Dir;
    DWORD Wait;

    Dir = MakeDir(Path, L"ntncdf_ovf");
    ok(Dir != INVALID_HANDLE_VALUE, "OpenDir failed: %lu\n", GetLastError());
    if (Dir == INVALID_HANDLE_VALUE)
        return;

    /* 8 bytes cannot hold a record with a name */
    Status = Notify(Dir, &Iosb, Buffer, 8, NAME_FILTER, FALSE);
    ok_hex(Status, STATUS_PENDING);
    ok(TouchFile(Path, L"a"), "TouchFile failed: %lu\n", GetLastError());
    Wait = WaitForSingleObject(Event, WAIT_MS);
    ok(Wait == WAIT_OBJECT_0, "First request did not complete (%lu)\n", Wait);
    ok_hex(Iosb.Status, STATUS_NOTIFY_ENUM_DIR);
    CancelPending(Dir, Wait);

    Status = Notify(Dir, &Iosb, Buffer, 8, NAME_FILTER, FALSE);
    ok_hex(Status, STATUS_PENDING);
    Wait = WaitForSingleObject(Event, 500);
    ok(Wait == WAIT_TIMEOUT, "Second request completed with no change, status 0x%lx\n", Iosb.Status);
    if (Wait == WAIT_TIMEOUT)
    {
        CancelPending(Dir, Wait);
        ok_hex(Iosb.Status, STATUS_CANCELLED);
    }

    CloseHandle(Dir);
    RemoveFile(Path, L"a");
    ok(RemoveDirectoryW(Path), "RemoveDirectoryW failed: %lu\n", GetLastError());
}

/* A watcher without a buffer is told to re-enumerate after a change it missed */
static VOID
TestZeroLengthBuffer(VOID)
{
    WCHAR Path[MAX_PATH];
    NTSTATUS Status;
    HANDLE Dir;
    DWORD Wait;

    Dir = MakeDir(Path, L"ntncdf_zero");
    ok(Dir != INVALID_HANDLE_VALUE, "OpenDir failed: %lu\n", GetLastError());
    if (Dir == INVALID_HANDLE_VALUE)
        return;

    Status = Notify(Dir, &Iosb, NULL, 0, NAME_FILTER, FALSE);
    ok_hex(Status, STATUS_PENDING);
    ok(TouchFile(Path, L"b"), "TouchFile failed: %lu\n", GetLastError());
    Wait = WaitForSingleObject(Event, WAIT_MS);
    ok(Wait == WAIT_OBJECT_0, "First request did not complete (%lu)\n", Wait);
    ok_hex(Iosb.Status, STATUS_NOTIFY_ENUM_DIR);
    CancelPending(Dir, Wait);

    /* Change made while no request is pending. Windows does not keep it for
     * a watcher without a buffer: the next request waits. ReactOS reports it
     * to that request. */
    ok(TouchFile(Path, L"c"), "TouchFile failed: %lu\n", GetLastError());
    Status = Notify(Dir, &Iosb, NULL, 0, NAME_FILTER, FALSE);
    ok_hex(Status, STATUS_PENDING);
    Wait = WaitForSingleObject(Event, 500);
    todo_if(is_reactos())
    ok(Wait == WAIT_TIMEOUT, "Change made between requests was reported, status 0x%lx\n", Iosb.Status);
    if (Wait == WAIT_OBJECT_0)
        ok_hex(Iosb.Status, STATUS_NOTIFY_ENUM_DIR);
    CancelPending(Dir, Wait);

    /* Reported once only */
    Status = Notify(Dir, &Iosb, NULL, 0, NAME_FILTER, FALSE);
    ok_hex(Status, STATUS_PENDING);
    Wait = WaitForSingleObject(Event, 500);
    ok(Wait == WAIT_TIMEOUT, "Third request completed with no change, status 0x%lx\n", Iosb.Status);
    CancelPending(Dir, Wait);

    CloseHandle(Dir);
    RemoveFile(Path, L"b");
    RemoveFile(Path, L"c");
    ok(RemoveDirectoryW(Path), "RemoveDirectoryW failed: %lu\n", GetLastError());
}

/* Wait until a record for Name arrives, reissuing on unrelated changes */
static BOOL
WaitForRecord(HANDLE Dir, PCWSTR Name)
{
    DWORD Start = GetTickCount(), Elapsed, Wait;
    BOOL Issued = TRUE;
    NTSTATUS Status;

    for (;;)
    {
        if (!Issued)
        {
            Status = Notify(Dir, &Iosb, Buffer, sizeof(Buffer), FILE_NOTIFY_CHANGE_DIR_NAME, TRUE);
            ok_hex(Status, STATUS_PENDING);
        }
        Issued = FALSE;
        Elapsed = GetTickCount() - Start;
        Wait = WaitForSingleObject(Event, Elapsed < WAIT_MS ? WAIT_MS - Elapsed : 0);
        if (Wait != WAIT_OBJECT_0)
        {
            ok(0, "No record for %s within %u ms\n", wine_dbgstr_w(Name), WAIT_MS);
            CancelPending(Dir, Wait);
            return FALSE;
        }
        if (Iosb.Status != STATUS_SUCCESS)
        {
            ok(0, "Request for %s completed with 0x%lx\n", wine_dbgstr_w(Name), Iosb.Status);
            return FALSE;
        }
        if (HasRecord(FILE_ACTION_ADDED, Name, Iosb.Information))
            return TRUE;
    }
}

/* A tree watcher of the volume root gets correct names, and keeps getting them */
static VOID
TestRootWatcher(VOID)
{
    WCHAR Root[4], Sub[MAX_PATH], One[MAX_PATH], Two[MAX_PATH];
    NTSTATUS Status;
    HANDLE Dir;

    StringCchCopyNW(Root, _countof(Root), TempDir, 3);
    StringCchPrintfW(Sub, _countof(Sub), L"%sntncdf_root", Root);
    StringCchPrintfW(One, _countof(One), L"%s\\one", Sub);
    StringCchPrintfW(Two, _countof(Two), L"%s\\two", Sub);
    RemoveDirectoryW(One);
    RemoveDirectoryW(Two);
    RemoveDirectoryW(Sub);
    ok(CreateDirectoryW(Sub, NULL), "CreateDirectoryW failed: %lu\n", GetLastError());

    Dir = OpenDir(Root);
    ok(Dir != INVALID_HANDLE_VALUE, "OpenDir failed: %lu\n", GetLastError());
    if (Dir == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(Sub);
        return;
    }

    /* First request: the name relative to the root */
    Status = Notify(Dir, &Iosb, Buffer, sizeof(Buffer), FILE_NOTIFY_CHANGE_DIR_NAME, TRUE);
    ok_hex(Status, STATUS_PENDING);
    ok(CreateDirectoryW(One, NULL), "CreateDirectoryW failed: %lu\n", GetLastError());
    WaitForRecord(Dir, L"ntncdf_root\\one");

    /* Later requests still see changes inside subdirectories */
    Status = Notify(Dir, &Iosb, Buffer, sizeof(Buffer), FILE_NOTIFY_CHANGE_DIR_NAME, TRUE);
    ok_hex(Status, STATUS_PENDING);
    ok(CreateDirectoryW(Two, NULL), "CreateDirectoryW failed: %lu\n", GetLastError());
    WaitForRecord(Dir, L"ntncdf_root\\two");

    CloseHandle(Dir);
    ok(RemoveDirectoryW(Two), "RemoveDirectoryW failed: %lu\n", GetLastError());
    ok(RemoveDirectoryW(One), "RemoveDirectoryW failed: %lu\n", GetLastError());
    ok(RemoveDirectoryW(Sub), "RemoveDirectoryW failed: %lu\n", GetLastError());
}

START_TEST(NtNotifyChangeDirectoryFile)
{
    if (!GetTempPathW(_countof(TempDir), TempDir) || TempDir[1] != L':')
    {
        skip("No usable temporary directory\n");
        return;
    }

    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Event != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!Event)
        return;

    TestDeleteWatched();
    TestOverflowReportedOnce();
    TestZeroLengthBuffer();
    TestRootWatcher();

    CloseHandle(Event);
}
