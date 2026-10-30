/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for InterlockedPushListSListEx (kernel32) / RtlInterlockedPushListSListEx (ntdll)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * msquic.dll imports InterlockedPushListSListEx from KERNEL32.dll; without it the loader failed the
 * load. Windows 10 22H2 exports it from kernel32 (a forwarder to ntdll's Rtl function) and ntdll.
 */

#include "precomp.h"

typedef PSLIST_ENTRY (WINAPI *PFN_PushListEx)(PSLIST_HEADER, PSLIST_ENTRY, PSLIST_ENTRY, ULONG);

typedef struct DECLSPEC_ALIGN(16) _ITEM
{
    SLIST_ENTRY Entry;
    ULONG Value;
} ITEM;

static ITEM Items[8];
static SLIST_HEADER *Head;

static void CheckFlushOrder(const char *Name, ULONG Count, const ITEM **Expected)
{
    PSLIST_ENTRY Entry = InterlockedFlushSList(Head);
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        ok(Entry == &Expected[i]->Entry, "%s: entry %lu is %p, expected %p\n", Name, i, Entry, &Expected[i]->Entry);
        if (!Entry)
            return;
        Entry = Entry->Next;
    }
    ok(Entry == NULL, "%s: the list continues with %p\n", Name, Entry);
}

static void TestPush(const char *Name, PFN_PushListEx Push)
{
    PSLIST_ENTRY Old;
    const ITEM *Order[8];

    /* a chain of three: 0 -> 1 -> 2 */
    Items[0].Entry.Next = &Items[1].Entry;
    Items[1].Entry.Next = &Items[2].Entry;
    Items[2].Entry.Next = NULL;

    /* an empty list returns NULL and takes the whole chain */
    InitializeSListHead(Head);
    Old = Push(Head, &Items[0].Entry, &Items[2].Entry, 3);
    ok(Old == NULL, "%s: empty list: previous first entry %p\n", Name, Old);
    ok(QueryDepthSList(Head) == 3, "%s: depth %u after pushing three\n", Name, QueryDepthSList(Head));
    Order[0] = &Items[0]; Order[1] = &Items[1]; Order[2] = &Items[2];
    CheckFlushOrder(Name, 3, Order);
    ok(QueryDepthSList(Head) == 0, "%s: depth %u after the flush\n", Name, QueryDepthSList(Head));

    /* on a list holding one entry it returns that entry, and the chain goes in front of it */
    Items[3].Entry.Next = NULL;
    InitializeSListHead(Head);
    Old = InterlockedPushEntrySList(Head, &Items[3].Entry);
    ok(Old == NULL, "%s: first single push: %p\n", Name, Old);
    Items[0].Entry.Next = &Items[1].Entry;
    Items[1].Entry.Next = &Items[2].Entry;
    Items[2].Entry.Next = NULL;
    Old = Push(Head, &Items[0].Entry, &Items[2].Entry, 3);
    ok(Old == &Items[3].Entry, "%s: previous first entry %p, expected %p\n", Name, Old, &Items[3].Entry);
    ok(QueryDepthSList(Head) == 4, "%s: depth %u, expected 4\n", Name, QueryDepthSList(Head));
    ok(Items[2].Entry.Next == &Items[3].Entry, "%s: the chain end points at %p\n", Name, Items[2].Entry.Next);
    Order[0] = &Items[0]; Order[1] = &Items[1]; Order[2] = &Items[2]; Order[3] = &Items[3];
    CheckFlushOrder(Name, 4, Order);

    /* a chain of one is a plain push */
    Items[4].Entry.Next = NULL;
    Items[5].Entry.Next = NULL;
    InitializeSListHead(Head);
    Old = Push(Head, &Items[4].Entry, &Items[4].Entry, 1);
    ok(Old == NULL, "%s: single entry onto an empty list: %p\n", Name, Old);
    Old = Push(Head, &Items[5].Entry, &Items[5].Entry, 1);
    ok(Old == &Items[4].Entry, "%s: single entry: previous %p, expected %p\n", Name, Old, &Items[4].Entry);
    ok(QueryDepthSList(Head) == 2, "%s: depth %u, expected 2\n", Name, QueryDepthSList(Head));
    Order[0] = &Items[5]; Order[1] = &Items[4];
    CheckFlushOrder(Name, 2, Order);

    /* it pops like any entries pushed one by one */
    Items[0].Entry.Next = &Items[1].Entry;
    Items[1].Entry.Next = NULL;
    InitializeSListHead(Head);
    Push(Head, &Items[0].Entry, &Items[1].Entry, 2);
    ok(InterlockedPopEntrySList(Head) == &Items[0].Entry, "%s: first pop\n", Name);
    ok(InterlockedPopEntrySList(Head) == &Items[1].Entry, "%s: second pop\n", Name);
    ok(InterlockedPopEntrySList(Head) == NULL, "%s: third pop\n", Name);
}

START_TEST(InterlockedPushListSListEx)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_PushListEx pKernel32, pNtdll;
    void *Block;

    pKernel32 = (PFN_PushListEx)GetProcAddress(Kernel32, "InterlockedPushListSListEx");
    pNtdll = (PFN_PushListEx)GetProcAddress(Ntdll, "RtlInterlockedPushListSListEx");
    ok(pKernel32 != NULL, "kernel32 does not export InterlockedPushListSListEx\n");
    ok(pNtdll != NULL, "ntdll does not export RtlInterlockedPushListSListEx\n");

    Block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SLIST_HEADER) + 32);
    Head = (SLIST_HEADER *)(((ULONG_PTR)Block + 15) & ~(ULONG_PTR)15);

    if (pKernel32)
        TestPush("kernel32", pKernel32);
    if (pNtdll)
        TestPush("ntdll", pNtdll);

    HeapFree(GetProcessHeap(), 0, Block);
}
