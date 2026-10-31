/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for VirtualAlloc2 and VirtualAlloc2FromApp: argument checks, address requirements, error codes
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Cygwin reserves its thread stacks with VirtualAlloc2 and an address requirement and stops at start without it.
 * The expected results are those of Windows 10 22H2 (amd64).
 */

#include "precomp.h"

typedef PVOID (WINAPI *FN_VirtualAlloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);

static FN_VirtualAlloc2 pVirtualAlloc2;
static FN_VirtualAlloc2 pVirtualAlloc2FromApp;
static ULONG_PTR g_Window;      /* a free, 64 KB aligned window of WINDOW_SIZE bytes */

#define GRANULE     0x10000
#define WINDOW_SIZE 0x400000


static MEM_EXTENDED_PARAMETER
Param(ULONG Type, PVOID Value)
{
    MEM_EXTENDED_PARAMETER Parameter;

    memset(&Parameter, 0, sizeof(Parameter));
    Parameter.Type = Type;
    Parameter.Pointer = Value;
    return Parameter;
}

/* Allocate with one address requirement; the allocation is released again, its address returned in *Base */
static BOOL
TryRequirement(
    PVOID Low,
    PVOID High,
    SIZE_T Align,
    SIZE_T Size,
    ULONG Type,
    DWORD ExpectedError,
    PVOID *Base)
{
    MEM_ADDRESS_REQUIREMENTS Requirements;
    MEM_EXTENDED_PARAMETER Parameter;
    PVOID Result;
    DWORD Error;

    Requirements.LowestStartingAddress = Low;
    Requirements.HighestEndingAddress = High;
    Requirements.Alignment = Align;
    Parameter = Param(MemExtendedParameterAddressRequirements, &Requirements);

    SetLastError(0xdeadbeef);
    Result = pVirtualAlloc2(NULL, NULL, Size, Type, PAGE_READWRITE, &Parameter, 1);
    Error = GetLastError();
    if (Base)
        *Base = Result;
    if (Result)
    {
        ok(Error == 0xdeadbeef || Error == ERROR_SUCCESS, "error %lu on success\n", Error);
        VirtualFree(Result, 0, MEM_RELEASE);
        return TRUE;
    }
    ok(Error == ExpectedError, "low %p high %p align %Ix size %Ix: error %lu, expected %lu\n",
       Low, High, Align, Size, Error, ExpectedError);
    return FALSE;
}

static void
test_NoRequirement(void)
{
    PVOID Base, Base2;
    DWORD Error;

    SetLastError(0xdeadbeef);
    Base = pVirtualAlloc2(NULL, NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    ok(Base != NULL, "no parameters: error %lu\n", GetLastError());
    if (Base)
    {
        ok(((ULONG_PTR)Base & (GRANULE - 1)) == 0, "base %p is not 64 KB aligned\n", Base);
        *(volatile BYTE *)Base = 1;
        VirtualFree(Base, 0, MEM_RELEASE);
    }

    Base = pVirtualAlloc2(GetCurrentProcess(), NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    ok(Base != NULL, "current process: error %lu\n", GetLastError());
    if (Base)
        VirtualFree(Base, 0, MEM_RELEASE);

    /* reserve, then commit part of it */
    Base = pVirtualAlloc2(NULL, NULL, 0x20000, MEM_RESERVE, PAGE_READWRITE, NULL, 0);
    ok(Base != NULL, "reserve: error %lu\n", GetLastError());
    if (Base)
    {
        Base2 = pVirtualAlloc2(NULL, Base, 0x10000, MEM_COMMIT, PAGE_READWRITE, NULL, 0);
        ok(Base2 == Base, "commit returned %p, expected %p\n", Base2, Base);
        VirtualFree(Base, 0, MEM_RELEASE);
    }

    /* the address is taken */
    Base = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE);
    ok(Base != NULL, "VirtualAlloc failed\n");
    if (Base)
    {
        SetLastError(0xdeadbeef);
        Base2 = pVirtualAlloc2(NULL, Base, 0x10000, MEM_RESERVE, PAGE_READWRITE, NULL, 0);
        Error = GetLastError();
        ok(Base2 == NULL, "reserved the same address twice\n");
        ok(Error == ERROR_INVALID_ADDRESS, "error %lu\n", Error);
        VirtualFree(Base, 0, MEM_RELEASE);
    }

    SetLastError(0xdeadbeef);
    Base = pVirtualAlloc2(NULL, NULL, 0, MEM_RESERVE, PAGE_READWRITE, NULL, 0);
    ok(Base == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "size 0: %p, error %lu\n", Base, GetLastError());
}

static void
test_Parameters(void)
{
    MEM_ADDRESS_REQUIREMENTS Requirements = { NULL, NULL, 0 }, Requirements2;
    MEM_EXTENDED_PARAMETER Parameters[2];
    PVOID Base;
    ULONG Node;

    /* a requirement that asks for nothing is ignored, an address is still allowed */
    Parameters[0] = Param(MemExtendedParameterAddressRequirements, &Requirements);
    Base = pVirtualAlloc2(NULL, (PVOID)g_Window, 0x10000, MEM_RESERVE, PAGE_READWRITE, Parameters, 1);
    ok(Base == (PVOID)g_Window, "address with an empty requirement: %p, error %lu\n", Base, GetLastError());
    if (Base)
        VirtualFree(Base, 0, MEM_RELEASE);

    SetLastError(0xdeadbeef);
    Base = pVirtualAlloc2(NULL, NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE, NULL, 1);
    ok(Base == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "count without array: %p, error %lu\n", Base, GetLastError());

    Parameters[0] = Param(MemExtendedParameterAddressRequirements, &Requirements);
    Parameters[1] = Param(MemExtendedParameterAddressRequirements, &Requirements);
    SetLastError(0xdeadbeef);
    Base = pVirtualAlloc2(NULL, NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE, Parameters, 2);
    ok(Base == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "two requirements: %p, error %lu\n", Base, GetLastError());

    Parameters[0] = Param(MemExtendedParameterAddressRequirements, NULL);
    SetLastError(0xdeadbeef);
    Base = pVirtualAlloc2(NULL, NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE, Parameters, 1);
    ok(Base == NULL && GetLastError() == ERROR_NOACCESS, "requirement without data: %p, error %lu\n", Base, GetLastError());

    /* NUMA node: only the existing node is valid */
    Parameters[0] = Param(MemExtendedParameterNumaNode, NULL);
    Parameters[0].ULong = 0;
    Base = pVirtualAlloc2(NULL, NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, Parameters, 1);
    ok(Base != NULL, "node 0: error %lu\n", GetLastError());
    if (Base)
        VirtualFree(Base, 0, MEM_RELEASE);
    for (Node = 0; Node < 2; Node++)
    {
        Parameters[0].ULong = Node ? 0xFFFFFFFF : 77;
        SetLastError(0xdeadbeef);
        Base = pVirtualAlloc2(NULL, NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, Parameters, 1);
        ok(Base == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "node %#lx: %p, error %lu\n",
           Parameters[0].ULong, Base, GetLastError());
        if (Base)
            VirtualFree(Base, 0, MEM_RELEASE);
    }

    /* types that need placeholders or special memory, and types that do not exist */
    for (Node = 0; Node < 5; Node++)
    {
        static const ULONG Types[] = { MemExtendedParameterInvalidType, MemExtendedParameterPartitionHandle,
                                       MemExtendedParameterAttributeFlags, MemExtendedParameterImageMachine, 99 };

        Parameters[0] = Param(Types[Node], NULL);
        SetLastError(0xdeadbeef);
        Base = pVirtualAlloc2(NULL, NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE, Parameters, 1);
        ok(Base == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "type %lu: %p, error %lu\n",
           Types[Node], Base, GetLastError());
        if (Base)
            VirtualFree(Base, 0, MEM_RELEASE);
    }

    /* an address with a requirement that asks for something */
    Requirements2.LowestStartingAddress = NULL;
    Requirements2.HighestEndingAddress = NULL;
    Requirements2.Alignment = 2 * GRANULE;
    Parameters[0] = Param(MemExtendedParameterAddressRequirements, &Requirements2);
    SetLastError(0xdeadbeef);
    Base = pVirtualAlloc2(NULL, (PVOID)g_Window, 0x10000, MEM_RESERVE, PAGE_READWRITE, Parameters, 1);
    ok(Base == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "address with alignment: %p, error %lu\n", Base, GetLastError());
    if (Base)
        VirtualFree(Base, 0, MEM_RELEASE);
}

static void
test_Requirement(void)
{
    PVOID Low = (PVOID)g_Window;
    PVOID High = (PVOID)(g_Window + WINDOW_SIZE - 1);
    PVOID Base = NULL;
    SYSTEM_INFO SystemInfo;
    SIZE_T Align;

    GetSystemInfo(&SystemInfo);

    ok(TryRequirement(Low, High, 0, 0x10000, MEM_RESERVE, 0, &Base), "window\n");
    ok((ULONG_PTR)Base >= g_Window && (ULONG_PTR)Base + 0x10000 - 1 <= g_Window + WINDOW_SIZE - 1,
       "%p is outside of the window\n", Base);
    ok(Base == (PVOID)g_Window, "the lowest address is %p, got %p\n", (PVOID)g_Window, Base);

    /* a size that is not a multiple of the granularity, or of the page */
    ok(TryRequirement(Low, High, 0, 1, MEM_RESERVE, 0, &Base), "size 1\n");
    ok(TryRequirement(Low, High, 0, 0x11000, MEM_RESERVE | MEM_COMMIT, 0, &Base), "size 0x11000\n");

    /* alignment */
    for (Align = GRANULE; Align <= WINDOW_SIZE / 4; Align *= 2)
    {
        ok(TryRequirement(Low, High, Align, 0x10000, MEM_RESERVE | MEM_COMMIT, 0, &Base), "align %Ix\n", Align);
        ok(((ULONG_PTR)Base & (Align - 1)) == 0, "%p is not aligned to %Ix\n", Base, Align);
        ok((ULONG_PTR)Base >= g_Window && (ULONG_PTR)Base + 0xFFFF <= g_Window + WINDOW_SIZE - 1, "%p outside\n", Base);
    }

    /* the highest address is the end of the allocation, MEM_TOP_DOWN takes the last place that fits */
    ok(TryRequirement(Low, High, 0, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, 0, &Base), "top down\n");
    ok(Base == (PVOID)(g_Window + WINDOW_SIZE - 0x10000), "top down: %p, expected %p\n",
       Base, (PVOID)(g_Window + WINDOW_SIZE - 0x10000));
    ok(TryRequirement(Low, (PVOID)(g_Window + WINDOW_SIZE - 0x1001), 0, 0x10000, MEM_RESERVE | MEM_TOP_DOWN, 0, &Base),
       "highest below the end of a page\n");
    ok(Base == (PVOID)(g_Window + WINDOW_SIZE - 0x20000), "highest 0x...EFFF: %p\n", Base);
    ok(TryRequirement(NULL, High, 0, 0x10000, MEM_RESERVE, 0, &Base), "no lowest\n");
    ok((ULONG_PTR)Base + 0xFFFF <= (ULONG_PTR)High, "%p above the highest address\n", Base);
    ok(TryRequirement(Low, NULL, 0, 0x10000, MEM_RESERVE, 0, &Base), "no highest\n");
    ok((ULONG_PTR)Base >= (ULONG_PTR)Low, "%p below the lowest address\n", Base);
    ok(TryRequirement(Low, NULL, GRANULE * 16, 0x10000, MEM_RESERVE, 0, &Base), "no highest, align\n");
    ok(((ULONG_PTR)Base & (GRANULE * 16 - 1)) == 0 && (ULONG_PTR)Base >= (ULONG_PTR)Low, "%p\n", Base);
    ok(TryRequirement(NULL, SystemInfo.lpMaximumApplicationAddress, 0, 0x10000, MEM_RESERVE, 0, &Base), "highest is the last user address\n");
    ok(TryRequirement(Low, High, 0, 0x10000, MEM_COMMIT, 0, &Base), "commit without reserve\n");
    ok((ULONG_PTR)Base >= g_Window && (ULONG_PTR)Base <= g_Window + WINDOW_SIZE - 0x10000, "commit: %p\n", Base);

    /* arguments that are not valid */
    TryRequirement(Low, High, 0x30000, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    /* the lowest address has to be a multiple of the alignment */
    TryRequirement((PVOID)(g_Window + GRANULE), High, 2 * GRANULE, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement((PVOID)(g_Window + GRANULE), NULL, 2 * GRANULE, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    ok(TryRequirement((PVOID)(g_Window + GRANULE), High, GRANULE, 0x10000, MEM_RESERVE, 0, &Base), "low aligned to 64 KB\n");
    ok(Base == (PVOID)(g_Window + GRANULE), "got %p\n", Base);
    TryRequirement(Low, High, 0x8000, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(Low, High, 0x1000, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(Low, High, 1, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(Low, (PVOID)(g_Window + WINDOW_SIZE - 0x1000), 0, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(Low, (PVOID)-1, 0, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement((PVOID)(g_Window + 0x1000), High, 0, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement((PVOID)(g_Window + 1), High, 0, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(High, Low, 0, 0x10000, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(Low, High, 0, 0, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(Low, High, 0, WINDOW_SIZE + 1, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    TryRequirement(Low, (PVOID)(g_Window + 0x1FFFF), 0, 0x20001, MEM_RESERVE, ERROR_INVALID_PARAMETER, NULL);
    /* nothing can be allocated below the first granule */
    TryRequirement(NULL, (PVOID)0xFFFF, 0, 0x10000, MEM_RESERVE, ERROR_INVALID_ADDRESS, NULL);
}

static void
test_Occupied(void)
{
    PVOID Low = (PVOID)g_Window;
    PVOID High = (PVOID)(g_Window + 0x3FFFF);
    PVOID Block1, Block2, Base = NULL;

    /* the whole window is taken */
    Block1 = VirtualAlloc(Low, 0x40000, MEM_RESERVE, PAGE_READWRITE);
    ok(Block1 == Low, "could not reserve the window: %p, error %lu\n", Block1, GetLastError());
    if (Block1 == Low)
    {
        TryRequirement(Low, High, 0, 0x10000, MEM_RESERVE, ERROR_NOT_ENOUGH_MEMORY, NULL);
        VirtualFree(Block1, 0, MEM_RELEASE);
    }

    /* the first granule is taken: the next one is used */
    Block1 = VirtualAlloc(Low, 0x10000, MEM_RESERVE, PAGE_READWRITE);
    if (Block1 == Low)
    {
        ok(TryRequirement(Low, High, 0, 0x10000, MEM_RESERVE, 0, &Base), "first granule taken\n");
        ok(Base == (PVOID)(g_Window + 0x10000), "got %p, expected %p\n", Base, (PVOID)(g_Window + 0x10000));
        VirtualFree(Block1, 0, MEM_RELEASE);
    }

    /* a hole too small for the size, and one that fits */
    Block1 = VirtualAlloc(Low, 0x10000, MEM_RESERVE, PAGE_READWRITE);
    Block2 = VirtualAlloc((PVOID)(g_Window + 0x20000), 0x10000, MEM_RESERVE, PAGE_READWRITE);
    if (Block1 == Low && Block2 == (PVOID)(g_Window + 0x20000))
    {
        TryRequirement(Low, (PVOID)(g_Window + 0x2FFFF), 0, 0x20000, MEM_RESERVE, ERROR_NOT_ENOUGH_MEMORY, NULL);
        ok(TryRequirement(Low, (PVOID)(g_Window + 0x5FFFF), 0, 0x20000, MEM_RESERVE, 0, &Base), "size 0x20000 behind the second block\n");
        ok(Base == (PVOID)(g_Window + 0x30000), "got %p, expected %p\n", Base, (PVOID)(g_Window + 0x30000));
        ok(TryRequirement(Low, (PVOID)(g_Window + 0x2FFFF), 0, 0x10000, MEM_RESERVE, 0, &Base), "the hole\n");
        ok(Base == (PVOID)(g_Window + 0x10000), "got %p, expected %p\n", Base, (PVOID)(g_Window + 0x10000));
    }
    if (Block1)
        VirtualFree(Block1, 0, MEM_RELEASE);
    if (Block2)
        VirtualFree(Block2, 0, MEM_RELEASE);
}

static void
test_FromApp(void)
{
    PVOID Base;

    if (!pVirtualAlloc2FromApp)
    {
        skip("VirtualAlloc2FromApp is not available\n");
        return;
    }

    Base = pVirtualAlloc2FromApp(NULL, NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    ok(Base != NULL, "PAGE_READWRITE: error %lu\n", GetLastError());
    if (Base)
        VirtualFree(Base, 0, MEM_RELEASE);

    SetLastError(0xdeadbeef);
    Base = pVirtualAlloc2FromApp(NULL, NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, NULL, 0);
    ok(Base == NULL, "got executable memory from the app function\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
    if (Base)
        VirtualFree(Base, 0, MEM_RELEASE);
}

START_TEST(VirtualAlloc2)
{
    HMODULE Module = GetModuleHandleW(L"kernelbase.dll");
    PVOID Window;

    if (!Module)
        Module = LoadLibraryW(L"kernelbase.dll");
    pVirtualAlloc2 = Module ? (PVOID)GetProcAddress(Module, "VirtualAlloc2") : NULL;
    pVirtualAlloc2FromApp = Module ? (PVOID)GetProcAddress(Module, "VirtualAlloc2FromApp") : NULL;
    if (!pVirtualAlloc2)
    {
        skip("VirtualAlloc2 is not available\n");
        return;
    }

    /* find a free window by reserving one and giving it back */
    Window = VirtualAlloc(NULL, 2 * WINDOW_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ok(Window != NULL, "cannot reserve %#x bytes, error %lu\n", 2 * WINDOW_SIZE, GetLastError());
    if (!Window)
        return;
    VirtualFree(Window, 0, MEM_RELEASE);
    /* the window starts at a multiple of its size, so that every alignment up to its size can start there */
    g_Window = ((ULONG_PTR)Window + WINDOW_SIZE - 1) & ~((ULONG_PTR)WINDOW_SIZE - 1);

    test_NoRequirement();
    test_Parameters();
    test_Requirement();
    test_Occupied();
    test_FromApp();
}
