/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native win32k calls inside either a native or a real WoW64 process.
 * No guest GUI thunk or user32 is needed for these mapping-only tests.
 */
#include "../../win32u/win32nt.h"
#include <ndk/psfuncs.h>
#include <ndk/kefuncs.h>
#include <ndk/kdfuncs.h>
#include <callback.h>
#include "probe.h"

static SHARED_VIEW_RESULT *Result;
static PVOID Reservations[32768];
static NTSTATUS (NTAPI *Callbacks[USER32_CALLBACK_COUNT])(PVOID, ULONG);

static NTSTATUS NTAPI ClientThreadSetup(PVOID Data, ULONG Size)
{
    ++Result->Callbacks;
    return NtCallbackReturn(NULL, 0, STATUS_SUCCESS);
}

static NTSTATUS NTAPI UnexpectedCallback(PVOID Data, ULONG Size)
{
    Result->Failure = 1;
    return NtCallbackReturn(NULL, 0, STATUS_NOT_SUPPORTED);
}

static VOID FillLow(ULONG_PTR Limit)
{
    MEMORY_BASIC_INFORMATION Info;
    ULONG_PTR Address = 0x10000, Start, End;
    PVOID Base;
    SIZE_T Size;
    NTSTATUS Status;
    while (Address < Limit)
    {
        /* Leave exactly enough space for the global heap (mode 4), or the
         * global heap and GDI table (mode 5), but not the next shared view. */
        if (Result->Mode >= 4 && Address == 0x70000000)
            Address += Result->Mode == 4 ? 0x100000 : 0x290000;
        Status = NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)Address,
                                     MemoryBasicInformation, &Info, sizeof(Info), NULL);
        if (!NT_SUCCESS(Status) || !Info.RegionSize) break;
        End = min((ULONG_PTR)Info.BaseAddress + Info.RegionSize, Limit);
        if (Result->Mode >= 4 && Address < 0x70000000 && End > 0x70000000)
            End = 0x70000000;
        if (Info.State == MEM_FREE)
        {
            Start = (max(Address, (ULONG_PTR)Info.BaseAddress) + 0xFFFF) & ~(ULONG_PTR)0xFFFF;
            if (Start < End)
            {
                Base = (PVOID)Start;
                Size = End - Start;
                Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0,
                                                  &Size, MEM_RESERVE, PAGE_NOACCESS);
                if (!NT_SUCCESS(Status)) { Result->Failure = 2; break; }
                Reservations[Result->FillCount++] = Base;
                Result->Filled += Size;
            }
        }
        Address = End;
    }
}

static VOID Record(ULONG Index, PVOID Address)
{
    MEMORY_BASIC_INFORMATION Info;
    Result->Address[Index] = (ULONG_PTR)Address;
    if (Address && NT_SUCCESS(NtQueryVirtualMemory(NtCurrentProcess(), Address,
                             MemoryBasicInformation, &Info, sizeof(Info), NULL)))
    {
        /* Query the entire allocation, including uncommitted attribute pages. */
        PVOID Allocation = Info.AllocationBase;
        ULONG_PTR End;
        Result->Address[Index] = (ULONG_PTR)Address;
        do
        {
            End = (ULONG_PTR)Info.BaseAddress + Info.RegionSize;
            if (!NT_SUCCESS(NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)End,
                                                 MemoryBasicInformation, &Info,
                                                 sizeof(Info), NULL))) break;
        } while (Info.AllocationBase == Allocation);
        Result->End[Index] = End;
    }
}

VOID NTAPI ProbeSharedViews(SHARED_VIEW_RESULT *Output)
{
    USERCONNECT Connect = {0};
    PCLIENTINFO Client = (PCLIENTINFO)NtCurrentTeb()->Win32ClientInfo;
    PGDI_TABLE_ENTRY Table;
    PVOID OldCallbacks = NtCurrentPeb()->KernelCallbackTable;
    ULONG_PTR Wow64 = 0;
    HDC Dc = NULL;
    HDC DisplayDc = NULL;
    HBRUSH Brush = NULL;
    HRGN Rgn = NULL;
    ULONG i;
    SIZE_T Size;

    Result = Output;
    NtQueryInformationProcess(NtCurrentProcess(), ProcessWow64Information,
                              &Wow64, sizeof(Wow64), NULL);
    Result->Wow64 = (Wow64 != 0);
    for (i = 0; i < USER32_CALLBACK_COUNT; ++i) Callbacks[i] = UnexpectedCallback;
    Callbacks[USER32_CALLBACK_CLIENTTHREADSTARTUP] = ClientThreadSetup;
    NtCurrentPeb()->KernelCallbackTable = (PVOID)Callbacks;

    if (Result->Mode == 1) FillLow(0x70000000);
    if (Result->Mode == 2 || Result->Mode >= 4) FillLow(0x80000000);
    Result->Status = NtUserProcessConnect(NtCurrentProcess(), &Connect, sizeof(Connect));
    /* Process setup can succeed before desktop/thread setup fails. A high
     * GDI table must still be detected in that partial-init case. */
    Record(3, NtCurrentPeb()->GdiSharedHandleTable);
    if (NT_SUCCESS(Result->Status))
    {
        if (Client->pDeskInfo)
            Record(0, (PVOID)((ULONG_PTR)Client->pDeskInfo->pvDesktopBase - Client->ulClientDelta));
        Record(1, Connect.siClient.psi);
        Record(2, Connect.siClient.aheList);
        Table = NtCurrentPeb()->GdiSharedHandleTable;
        Record(3, Table);
        if (Result->Mode == 3) FillLow(0x80000000);
        Dc = NtGdiCreateCompatibleDC(NULL);
        Brush = NtGdiCreateSolidBrush(RGB(1, 2, 3), NULL);
        if (Brush)
        {
            /* Initial brush creation allocates its attribute before inserting
             * the handle. Reallocate with an existing handle to publish the
             * attribute in that handle's shared-table entry. */
            HBRUSH Stock = NtGdiSetBrushAttributes(Brush, SC_BB_STOCKOBJ);
            if (Stock)
                Brush = NtGdiClearBrushAttributes(Stock, SC_BB_STOCKOBJ);
            else
                Result->Failure = 3;
        }
        Rgn = NtGdiCreateRectRgn(0, 0, 1, 1);
        DisplayDc = NtGdiOpenDCW(NULL, NULL, NULL, 0, TRUE, NULL, NULL);
        Result->Handles[0] = (ULONG_PTR)Dc;
        Result->Handles[1] = (ULONG_PTR)Brush;
        Result->Handles[2] = (ULONG_PTR)Rgn;
        Result->Handles[3] = (ULONG_PTR)DisplayDc;
        if (Table)
        {
            if (Dc) Record(4, Table[(ULONG_PTR)Dc & 0xffff].UserData);
            if (Brush) Record(5, Table[(ULONG_PTR)Brush & 0xffff].UserData);
            if (Rgn) Record(6, Table[(ULONG_PTR)Rgn & 0xffff].UserData);
            if (DisplayDc) Record(7, Table[(ULONG_PTR)DisplayDc & 0xffff].UserData);
        }
    }
    DbgPrint("SHARED-VIEWS mode=%lu wow64=%lu status=%08lx filled=%I64x\n",
             Result->Mode, Result->Wow64, Result->Status, Result->Filled);
    for (i = 0; i < RTL_NUMBER_OF(Result->Address); ++i)
        DbgPrint("SHARED-VIEWS view=%lu address=%I64x end=%I64x\n",
                 i, Result->Address[i], Result->End[i]);

    /* Release address pressure before cleanup can need scratch space. */
    for (i = 0; i < Result->FillCount; ++i)
    {
        Size = 0;
        NtFreeVirtualMemory(NtCurrentProcess(), &Reservations[i], &Size, MEM_RELEASE);
    }
    if (Dc) NtGdiDeleteObjectApp(Dc);
    if (Brush) NtGdiDeleteObjectApp(Brush);
    if (Rgn) NtGdiDeleteObjectApp(Rgn);
    if (DisplayDc) NtGdiDeleteObjectApp(DisplayDc);
    NtCurrentPeb()->KernelCallbackTable = OldCallbacks;
}
