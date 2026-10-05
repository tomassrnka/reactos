/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for GetQueuedCompletionStatusEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef
BOOL
WINAPI
FN_GetQueuedCompletionStatusEx(
    _In_ HANDLE CompletionPort,
    _Out_writes_to_(ulCount, *ulNumEntriesRemoved) LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    _In_ ULONG ulCount,
    _Out_ PULONG ulNumEntriesRemoved,
    _In_ DWORD dwMilliseconds,
    _In_ BOOL fAlertable);

static FN_GetQueuedCompletionStatusEx *pGetQueuedCompletionStatusEx;
static LONG ApcCount;

static
VOID
CALLBACK
TestApc(_In_ ULONG_PTR Parameter)
{
    InterlockedIncrement(&ApcCount);
}

#define POSTED 40

static
VOID
TestDrain(VOID)
{
    OVERLAPPED_ENTRY Entries[64];
    HANDLE Port;
    ULONG Removed, Total = 0, Calls = 0, i;
    BOOL Ret;

    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed: %lu\n", GetLastError());
    if (!Port)
        return;

    for (i = 0; i < POSTED; i++)
    {
        Ret = PostQueuedCompletionStatus(Port, i * 3, i, (LPOVERLAPPED)(ULONG_PTR)(i + 1));
        ok(Ret, "PostQueuedCompletionStatus(%lu) failed: %lu\n", i, GetLastError());
    }

    /* The call returns up to the requested count; the caller loops until the port is empty */
    while (Total < POSTED && Calls < POSTED)
    {
        Removed = 0xdeadbeef;
        Ret = pGetQueuedCompletionStatusEx(Port, Entries, _countof(Entries), &Removed, 0, FALSE);
        Calls++;
        ok(Ret, "Call %lu failed: %lu\n", Calls, GetLastError());
        if (!Ret)
            break;
        ok(Removed >= 1 && Removed <= _countof(Entries), "Call %lu removed %lu entries\n", Calls, Removed);
        if (Removed < 1 || Removed > _countof(Entries))
            break;

        for (i = 0; i < Removed && Total + i < POSTED; i++)
        {
            ok_eq_ulongptr(Entries[i].lpCompletionKey, (ULONG_PTR)(Total + i));
            ok_eq_pointer(Entries[i].lpOverlapped, (LPOVERLAPPED)(ULONG_PTR)(Total + i + 1));
            ok_eq_ulong(Entries[i].dwNumberOfBytesTransferred, (Total + i) * 3);
            ok_eq_ulongptr(Entries[i].Internal, (ULONG_PTR)0);
        }
        Total += Removed;
    }
    ok_eq_ulong(Total, (ULONG)POSTED);

    Removed = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = pGetQueuedCompletionStatusEx(Port, Entries, _countof(Entries), &Removed, 0, FALSE);
    ok(!Ret, "Empty port returned TRUE\n");
    ok_eq_ulong(GetLastError(), (ULONG)WAIT_TIMEOUT);
    ok_eq_ulong(Removed, 0UL);

    CloseHandle(Port);
}

static
VOID
TestTimeoutAndApc(VOID)
{
    OVERLAPPED_ENTRY Entry;
    HANDLE Port;
    ULONG Removed;
    DWORD Start, Elapsed;
    BOOL Ret;

    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed: %lu\n", GetLastError());
    if (!Port)
        return;

    Start = GetTickCount();
    SetLastError(0xdeadbeef);
    Ret = pGetQueuedCompletionStatusEx(Port, &Entry, 1, &Removed, 200, FALSE);
    Elapsed = GetTickCount() - Start;
    ok(!Ret, "Empty port returned TRUE\n");
    ok_eq_ulong(GetLastError(), (ULONG)WAIT_TIMEOUT);
    ok(Elapsed >= 150, "Timed out after %lu ms\n", Elapsed);

    /* A non-alertable wait leaves the APC queued */
    ApcCount = 0;
    ok(QueueUserAPC(TestApc, GetCurrentThread(), 0), "QueueUserAPC failed: %lu\n", GetLastError());
    Ret = pGetQueuedCompletionStatusEx(Port, &Entry, 1, &Removed, 50, FALSE);
    ok(!Ret, "Empty port returned TRUE\n");
    ok_eq_long(ApcCount, 0L);

    /* An alertable wait runs it and reports WAIT_IO_COMPLETION */
    SetLastError(0xdeadbeef);
    Ret = pGetQueuedCompletionStatusEx(Port, &Entry, 1, &Removed, 5000, TRUE);
    ok(!Ret, "Alertable wait returned TRUE\n");
    ok_eq_ulong(GetLastError(), (ULONG)WAIT_IO_COMPLETION);
    ok_eq_long(ApcCount, 1L);

    CloseHandle(Port);
}

START_TEST(GetQueuedCompletionStatusEx)
{
    HMODULE hModule;

    hModule = GetModuleHandleW(L"kernel32.dll");
    pGetQueuedCompletionStatusEx = (FN_GetQueuedCompletionStatusEx*)GetProcAddress(hModule, "GetQueuedCompletionStatusEx");
    if (!pGetQueuedCompletionStatusEx)
    {
        hModule = LoadLibraryW(L"kernel32_vista.dll");
        if (hModule)
            pGetQueuedCompletionStatusEx = (FN_GetQueuedCompletionStatusEx*)GetProcAddress(hModule, "GetQueuedCompletionStatusEx");
    }
    if (!pGetQueuedCompletionStatusEx)
    {
        skip("GetQueuedCompletionStatusEx not found\n");
        return;
    }

    TestDrain();
    TestTimeoutAndApc();
}
