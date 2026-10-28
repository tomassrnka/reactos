/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     GetIfTable from several threads at once
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * Every interface row costs tcpip a few NDIS queries (OID_GEN_PHYSICAL_MEDIUM,
 * OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR...). Many threads asking at once put
 * several requests on one miniport, and NDIS has to run them one at a time and
 * complete each to its own caller. When it did not, a caller could be woken for
 * another request and return while its own still sat in the queue, so the miniport
 * wrote into a dead stack frame, or a caller was never woken at all.
 * Each call has to return and succeed, and a row has to keep its index, type and
 * hardware address. Speed, MTU, status and counters may legitimately change while
 * the test runs and are not compared, nor is the number of rows.
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <iphlpapi.h>

#define THREADS 8
#define SECONDS 5
#define MAX_ROWS 32

static MIB_IFROW BaseRows[MAX_ROWS];
static DWORD BaseCount;
static volatile LONG Stop;
static volatile LONG Calls, Failures, Mismatches;

/* Rows are matched by index: an interface that came or went meanwhile is not a mismatch */
static BOOL RowChanged(const MIB_IFROW *Row)
{
    DWORD i;

    for (i = 0; i < BaseCount; i++)
    {
        if (BaseRows[i].dwIndex != Row->dwIndex)
            continue;
        return BaseRows[i].dwType != Row->dwType ||
               BaseRows[i].dwPhysAddrLen != Row->dwPhysAddrLen ||
               memcmp(BaseRows[i].bPhysAddr, Row->bPhysAddr, min(Row->dwPhysAddrLen, MAXLEN_PHYSADDR));
    }
    return FALSE;
}

static DWORD WINAPI QueryThread(PVOID Context)
{
    PMIB_IFTABLE Table;
    ULONG Size, i;
    DWORD Error;

    UNREFERENCED_PARAMETER(Context);

    Table = HeapAlloc(GetProcessHeap(), 0, 0x10000);
    if (!Table)
        return 1;

    while (!Stop)
    {
        Size = 0x10000;
        Error = GetIfTable(Table, &Size, FALSE);
        InterlockedIncrement(&Calls);
        if (Error != NO_ERROR)
        {
            InterlockedIncrement(&Failures);
            continue;
        }
        for (i = 0; i < Table->dwNumEntries; i++)
        {
            if (RowChanged(&Table->table[i]))
            {
                InterlockedIncrement(&Mismatches);
                break;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, Table);
    return 0;
}

START_TEST(GetIfTableConcurrent)
{
    HANDLE Threads[THREADS];
    PMIB_IFTABLE Table;
    ULONG Size, i, Started = 0;
    DWORD Wait, Error;

    Table = HeapAlloc(GetProcessHeap(), 0, 0x10000);
    if (!Table)
    {
        skip("Out of memory\n");
        return;
    }
    Size = 0x10000;
    Error = GetIfTable(Table, &Size, FALSE);
    ok(Error == NO_ERROR, "GetIfTable returned %lu\n", Error);
    if (Error != NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, Table);
        return;
    }
    BaseCount = min(Table->dwNumEntries, MAX_ROWS);
    memcpy(BaseRows, Table->table, BaseCount * sizeof(MIB_IFROW));
    HeapFree(GetProcessHeap(), 0, Table);

    for (i = 0; i < THREADS; i++)
    {
        Threads[Started] = CreateThread(NULL, 0, QueryThread, NULL, 0, NULL);
        ok(Threads[Started] != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (Threads[Started])
            Started++;
    }

    Sleep(SECONDS * 1000);
    Stop = TRUE;

    /* A call that never returns is the failure this test is about: do not wait forever for it */
    Wait = Started ? WaitForMultipleObjects(Started, Threads, TRUE, 60 * 1000) : WAIT_OBJECT_0;
    ok(Wait == WAIT_OBJECT_0, "A GetIfTable call did not return (wait %lu)\n", Wait);
    ok(Calls > 0, "No call completed\n");
    ok(Failures == 0, "%ld of %ld calls failed\n", Failures, Calls);
    ok(Mismatches == 0, "%ld of %ld calls returned a row whose index, type or address changed\n", Mismatches, Calls);
    trace("%ld calls from %lu threads in %u seconds\n", Calls, Started, SECONDS);

    if (Wait == WAIT_OBJECT_0)
    {
        for (i = 0; i < Started; i++)
            CloseHandle(Threads[i]);
    }
    else
    {
        /* The stuck threads keep the process alive; report now */
        fflush(stdout);
        ExitProcess(1);
    }
}
