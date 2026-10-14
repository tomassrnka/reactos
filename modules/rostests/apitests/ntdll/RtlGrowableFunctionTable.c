/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for RtlAddGrowableFunctionTable, RtlGrowFunctionTable and RtlDeleteGrowableFunctionTable
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifdef _M_AMD64

typedef DWORD (NTAPI *PFN_ADD)(PVOID *, PRUNTIME_FUNCTION, DWORD, DWORD, ULONG_PTR, ULONG_PTR);
typedef VOID (NTAPI *PFN_GROW)(PVOID, DWORD);
typedef VOID (NTAPI *PFN_DELETE)(PVOID);

static PFN_ADD pAdd;
static PFN_GROW pGrow;
static PFN_DELETE pDelete;

static
PRUNTIME_FUNCTION
Lookup(ULONG_PTR Pc, PULONG64 Base)
{
    *Base = 0xdeadbeef;
    return RtlLookupFunctionEntry(Pc, Base, NULL);
}

START_TEST(RtlGrowableFunctionTable)
{
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    RUNTIME_FUNCTION Funcs[4];
    ULONG_PTR Range;
    PVOID Table;
    DWORD Status;
    ULONG64 Base;
    PRUNTIME_FUNCTION Func;

    pAdd = (PFN_ADD)GetProcAddress(hNtdll, "RtlAddGrowableFunctionTable");
    pGrow = (PFN_GROW)GetProcAddress(hNtdll, "RtlGrowFunctionTable");
    pDelete = (PFN_DELETE)GetProcAddress(hNtdll, "RtlDeleteGrowableFunctionTable");
    if (!pAdd || !pGrow || !pDelete)
    {
        skip("Growable function tables are not available\n");
        return;
    }

    Range = (ULONG_PTR)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(Range != 0, "VirtualAlloc failed\n");
    if (!Range)
        return;

    RtlZeroMemory(Funcs, sizeof(Funcs));
    Funcs[0].BeginAddress = 0x100; Funcs[0].EndAddress = 0x110;
    Funcs[1].BeginAddress = 0x200; Funcs[1].EndAddress = 0x240;
    Funcs[2].BeginAddress = 0x300; Funcs[2].EndAddress = 0x340;

    /* A table starts with its current count; entries beyond it are invisible until it grows */
    Table = NULL;
    Status = pAdd(&Table, Funcs, 1, 3, Range, Range + 0x1000);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Table != NULL, "No table returned\n");
    Func = Lookup(Range + 0x108, &Base);
    ok(Func == &Funcs[0], "Lookup returned %p, expected %p\n", Func, &Funcs[0]);
    ok(Base == Range, "Base is %I64x, expected %Ix\n", Base, Range);
    Func = Lookup(Range + 0x208, &Base);
    ok(Func == NULL, "Lookup returned %p before the table grew\n", Func);

    pGrow(Table, 2);
    Func = Lookup(Range + 0x208, &Base);
    ok(Func == &Funcs[1], "Lookup returned %p, expected %p\n", Func, &Funcs[1]);
    ok(Base == Range, "Base is %I64x, expected %Ix\n", Base, Range);

    /* The range limits the lookup */
    Func = Lookup(Range + 0x1100, &Base);
    ok(Func == NULL, "Lookup returned %p outside of the range\n", Func);

    pDelete(Table);
    Func = Lookup(Range + 0x108, &Base);
    ok(Func == NULL, "Lookup returned %p after the table was deleted\n", Func);
    ok(Base == 0xdeadbeef, "Base changed to %I64x\n", Base);

    /* RtlDeleteFunctionTable removes a growable table, found by its function table; the handle is gone afterwards */
    Table = NULL;
    Status = pAdd(&Table, Funcs, 2, 2, Range, Range + 0x1000);
    ok_hex(Status, STATUS_SUCCESS);
    ok(RtlDeleteFunctionTable(Funcs) != FALSE, "RtlDeleteFunctionTable did not remove a growable table\n");
    Func = Lookup(Range + 0x208, &Base);
    ok(Func == NULL, "Lookup returned %p after RtlDeleteFunctionTable\n", Func);

    VirtualFree((PVOID)Range, 0, MEM_RELEASE);
}

#else

START_TEST(RtlGrowableFunctionTable)
{
    skip("Growable function tables exist on amd64 only\n");
}

#endif
