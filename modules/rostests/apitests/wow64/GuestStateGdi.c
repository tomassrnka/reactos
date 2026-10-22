/* SPDX-License-Identifier: GPL-2.0-or-later
 * Exercise real gdi32 initialization, batching, cache and polling accesses.
 * Only unrelated language-pack initialization and UpdateColors' final syscall
 * are stubbed; the other kernel calls use the native test bridge. */
#define LANGPACK
#include "../../../../win32ss/gdi/gdi32/include/precomp.h"
#include <apitest.h>
#include "GuestState.h"
#include "../../../../win32ss/gdi/gdi32/main/dllmain.c"
#include "../../../../win32ss/gdi/gdi32/misc/misc.c"
#include "../../../../win32ss/gdi/gdi32/objects/palette.c"

HANDLE hProcessHeap;
CRITICAL_SECTION gcsClientObjLinks;
HGDIOBJ stock_objects[NB_STOCK_OBJECTS];

BOOL WINAPI LoadLPK(INT Function) { return FALSE; }
BOOL WINAPI NtGdiUpdateColors(HDC Dc) { return FALSE; }

BOOL WINAPI NtGdiInit(VOID)
{
    ULONGLONG Args[6] = {0};
    return GuestStateCall(Native_NtGdiInit, Args);
}

NTSTATUS WINAPI NtGdiFlush(VOID)
{
    ULONGLONG Args[6] = {0};
    return GuestStateCall(Native_NtGdiFlush, Args);
}

void GuestStateGdi(PTEB64 Teb)
{
    PPEB64 Peb = (PPEB64)(ULONG_PTR)Teb->ProcessEnvironmentBlock;
    PTEB Guest = NtCurrentTeb();
    GDI_TEB_BATCH SavedGuestBatch = Guest->GdiTebBatch;
    ULONG SavedGuestCount = Guest->GdiBatchCount;
    ULONG64 SavedSpins = Teb->Win32ClientInfo[1];
    ULONG SavedGuestSpins = ((PW32CLIENTINFO)Guest->Win32ClientInfo)->cSpins;
    PGDIBSOBJECT Record;
    KFLOATOBJ Float = 1.25f;

    GdiProcessSetup();
    ok((ULONG_PTR)GdiHandleTable == Peb->GdiSharedHandleTable, "wrong GDI handle table\n");
    ok((ULONG_PTR)GdiSharedHandleTable == Peb->GdiSharedHandleTable, "wrong GDI shared table\n");
    ok((ULONG_PTR)GdiDevCaps == Peb->GdiSharedHandleTable + FIELD_OFFSET(GDI_SHARED_HANDLE_TABLE, DevCaps),
       "wrong device capabilities\n");
    ok(GDI_BatchLimit == Peb->GdiDCAttributeList, "wrong batch limit %lu, native %lu\n",
       GDI_BatchLimit, Peb->GdiDCAttributeList);
    ok(GdiHandleCache == (PVOID)Peb->GdiHandleBuffer, "wrong PEB handle cache\n");
    ok(FOtoF(&Float) == 1.25f, "wide matrices must use the native FLOAT representation\n");

    /* Thread-attach resets only the native batch. Use different guest values
     * so writing both copies cannot pass. Do not make a syscall until reset. */
    Guest->GdiTebBatch.Offset = 12;
    Guest->GdiBatchCount = 2;
    Teb->GdiTebBatch.Offset = 16;
    Teb->GdiBatchCount = 3;
    GdiDllInitialize(NULL, DLL_THREAD_ATTACH, NULL);
    ok(Teb->GdiTebBatch.Offset == 0 && Teb->GdiBatchCount == 0,
       "thread attach did not reset the native batch\n");
    ok(Guest->GdiTebBatch.Offset == 12 && Guest->GdiBatchCount == 2,
       "thread attach modified the guest batch\n");
    Teb->GdiTebBatch.Offset = Teb->GdiBatchCount = 0;
    Teb->GdiTebBatch.HDC = 0;

    Record = GdiAllocBatchCommand((HDC)0x81230004, GdiBCDelObj);
    ok(Record == (PVOID)Teb->GdiTebBatch.Buffer, "batch record %p, native buffer %p\n",
       Record, Teb->GdiTebBatch.Buffer);
    ok(Teb->GdiTebBatch.HDC == 0x81230004ULL, "batch HDC must be zero-extended\n");
    ok(Teb->GdiTebBatch.Offset == sizeof(*Record) && Teb->GdiBatchCount == 1,
       "batch size/count not published in the native TEB\n");
    if (Record == (PVOID)Teb->GdiTebBatch.Buffer)
    {
        ok(Record->gbHdr.Size == sizeof(*Record) && Record->gbHdr.Cmd == GdiBCDelObj,
           "wrong batch header\n");
        ok(!GdiAllocBatchCommand((HDC)0x81230008, GdiBCDelObj), "mixed batch DCs accepted\n");
        /* Empty delete is harmless. Clear the synthetic DC before a syscall. */
        Record->hgdiobj_K64 = 0;
        Teb->GdiTebBatch.HDC = 0;
        NtGdiFlush();
        ok(Teb->GdiTebBatch.Offset == 0 && Teb->GdiBatchCount == 0,
           "kernel did not consume the guest batch\n");
    }
    Teb->GdiTebBatch.Offset = Teb->GdiBatchCount = 0;
    Teb->GdiTebBatch.HDC = 0;
    Guest->GdiTebBatch = SavedGuestBatch;
    Guest->GdiBatchCount = SavedGuestCount;

    Teb->Win32ClientInfo[1] = 0x1234567800000001ULL;
    ((PW32CLIENTINFO)Guest->Win32ClientInfo)->cSpins = 0x98765432;
    UpdateColors(NULL);
    ok(Teb->Win32ClientInfo[1] == 0, "polling reset did not clear native cSpins at full width\n");
    ok(((PW32CLIENTINFO)Guest->Win32ClientInfo)->cSpins == 0x98765432,
       "polling reset modified the guest CLIENTINFO\n");
    Teb->Win32ClientInfo[1] = SavedSpins;
    ((PW32CLIENTINFO)Guest->Win32ClientInfo)->cSpins = SavedGuestSpins;

    if (GdiHandleCache == (PVOID)Peb->GdiHandleBuffer &&
        (ULONG_PTR)GdiHandleTable == Peb->GdiSharedHandleTable)
    {
        /* Seed the native cache as an amd64 producer would, independently of
         * GDIHANDLECACHE's guest declaration. Consume through real gdi32. */
        BYTE SavedCache[sizeof(Peb->GdiHandleBuffer)];
        ULONG64 *NativeHandles = (PVOID)((PBYTE)Peb->GdiHandleBuffer + 24);
        PGDI_TABLE_ENTRY SavedTable = GdiHandleTable;
        GDI_TABLE_ENTRY Entry = {0};
        BRUSH_ATTR Attr = {0};
        HBRUSH Brush = (HBRUSH)0x80100000;

        /* Cache consumption is a unit test with a synthetic table entry.
         * The native table's real mapping was checked above; no kernel table
         * entry is modified and no synthetic handle reaches the kernel. */
        Entry.Type = 0x108010;
        Entry.ProcessId = (ULONG_PTR)CurrentProcessId;
        Entry.UserData_K64 = (ULONG_PTR)&Attr;
        GdiHandleTable = &Entry;
        Attr.AttrFlags = ATTR_CACHED;
        Attr.lbColor = RGB(10, 20, 30);
        CopyMemory(SavedCache, Peb->GdiHandleBuffer, sizeof(SavedCache));
        ZeroMemory(Peb->GdiHandleBuffer, sizeof(Peb->GdiHandleBuffer));
        NativeHandles[1] = (ULONG_PTR)Brush;
        Peb->GdiHandleBuffer[1] = 2;
        ok(hGetPEBHandle(hctBrushHandle, RGB(10, 20, 30)) == Brush,
           "cache did not read the second native handle slot\n");
        ok(NativeHandles[1] == 0 && Peb->GdiHandleBuffer[1] == 1,
           "cache did not clear the native slot/count\n");
        ok(!(Attr.AttrFlags & ATTR_CACHED), "cache did not release the brush\n");
        GdiHandleTable = SavedTable;
        CopyMemory(Peb->GdiHandleBuffer, SavedCache, sizeof(SavedCache));
    }
    GdiProcessShutdown();
}
