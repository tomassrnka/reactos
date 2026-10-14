/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for UnmapViewOfFileEx (Windows 10 version 1703 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64).
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_UnmapViewOfFileEx)(PVOID, ULONG);

#define PENDING_ERROR 0xdeadbeef

static PFN_UnmapViewOfFileEx pUnmapViewOfFileEx;

static BOOL IsMapped(PVOID Address)
{
    MEMORY_BASIC_INFORMATION Info;

    return VirtualQuery(Address, &Info, sizeof(Info)) && Info.State == MEM_COMMIT;
}

/* The call, with the result, the last error and whether the view is still there */
static void Check(PVOID View, PVOID Address, ULONG Flags, BOOL Expected, DWORD ExpectedError, int Line)
{
    BOOL Result;
    DWORD Error;

    SetLastError(PENDING_ERROR);
    Result = pUnmapViewOfFileEx(Address, Flags);
    Error = GetLastError();
    ok(Result == Expected, "line %d: flags %#lx: got %d\n", Line, Flags, Result);
    ok(Error == ExpectedError, "line %d: flags %#lx: last error %lu, expected %lu\n", Line, Flags, Error, ExpectedError);
    if (View)
        ok(IsMapped(View) == !Expected, "line %d: flags %#lx: the view is %s\n", Line, Flags, IsMapped(View) ? "still there" : "gone");
}

START_TEST(UnmapViewOfFileEx)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    static const ULONG BadFlags[] = { 4, 8, 0x10, 0x100, 0x80000000, 0xfffffffc, 0xffffffff };
    HANDLE Mapping;
    PVOID View, Memory;
    ULONG i;

    pUnmapViewOfFileEx = (PFN_UnmapViewOfFileEx)GetProcAddress(Kernel32, "UnmapViewOfFileEx");
    /* Windows 8.1 and older have none; ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pUnmapViewOfFileEx)
    {
        ok(!(is_reactos() && GetProcAddress(Kernel32, "K32EnumProcessModules")), "UnmapViewOfFileEx is not exported\n");
        skip("UnmapViewOfFileEx is not exported\n");
        return;
    }

    Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x10000, NULL);
    ok(Mapping != NULL, "CreateFileMapping failed\n");
    if (!Mapping)
        return;

    /* flag 0 is UnmapViewOfFile, and the transient boost is a hint that changes nothing */
    for (i = 0; i < 2; i++)
    {
        View = MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        ok(View != NULL, "MapViewOfFile failed\n");
        if (!View)
            continue;
        Check(View, View, i, TRUE, PENDING_ERROR, __LINE__);
        Check(NULL, View, i, FALSE, ERROR_INVALID_ADDRESS, __LINE__);
    }

    /* an address inside the view is enough */
    View = MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    ok(View != NULL, "MapViewOfFile failed\n");
    if (View)
        Check(View, (PUCHAR)View + 0x3000, 0, TRUE, PENDING_ERROR, __LINE__);

    /* the placeholder flag needs a placeholder: a view is none, and stays mapped */
    for (i = 2; i < 4; i++)
    {
        View = MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        ok(View != NULL, "MapViewOfFile failed\n");
        if (!View)
            continue;
        Check(View, View, i, FALSE, ERROR_INVALID_ADDRESS, __LINE__);
        ok(UnmapViewOfFile(View), "the view could not be unmapped\n");
    }

    /* flags that are not known are refused before the address is looked at */
    View = MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    ok(View != NULL, "MapViewOfFile failed\n");
    if (View)
    {
        for (i = 0; i < RTL_NUMBER_OF(BadFlags); i++)
            Check(View, View, BadFlags[i], FALSE, ERROR_INVALID_PARAMETER, __LINE__);
        ok(UnmapViewOfFile(View), "the view could not be unmapped\n");
    }
    Check(NULL, NULL, 4, FALSE, ERROR_INVALID_PARAMETER, __LINE__);
    Check(NULL, (PVOID)(ULONG_PTR)0x10000, 0x80000000, FALSE, ERROR_INVALID_PARAMETER, __LINE__);

    /* memory that is no view, and addresses that are nothing */
    Memory = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok(Memory != NULL, "VirtualAlloc failed\n");
    if (Memory)
    {
        Check(Memory, Memory, 0, FALSE, ERROR_INVALID_ADDRESS, __LINE__);
        Check(Memory, Memory, 1, FALSE, ERROR_INVALID_ADDRESS, __LINE__);
        ok(VirtualFree(Memory, 0, MEM_RELEASE), "VirtualFree failed\n");
    }
    Check(NULL, NULL, 0, FALSE, ERROR_INVALID_ADDRESS, __LINE__);
    Check(NULL, (PVOID)(ULONG_PTR)0x10000, 0, FALSE, ERROR_INVALID_ADDRESS, __LINE__);
    Check(NULL, (PVOID)(ULONG_PTR)0x10000, 1, FALSE, ERROR_INVALID_ADDRESS, __LINE__);
    Check(NULL, (PVOID)(ULONG_PTR)0x10000, 2, FALSE, ERROR_INVALID_ADDRESS, __LINE__);

    CloseHandle(Mapping);
}
