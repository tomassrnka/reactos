/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ResolveDelayLoadedAPI / LdrResolveDelayLoadedAPI
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * PowerShell's getfilesiginforedist.dll imports ResolveDelayLoadedAPI from
 * api-ms-win-core-delayload-l1-1-1.dll; without it the loader's "Entry Point Not Found" box blocked
 * the 32-bit pwsh. The expected values were probed on Windows 10 22H2 with a hand-built,
 * RVA-based descriptor of this very program.
 */

#include "precomp.h"

typedef struct
{
    ULONG Size;
    PVOID Descriptor;
    PVOID Thunk;
    LPCSTR DllName;
    ULONG ByName;
    union { LPCSTR Name; ULONG Ordinal; } Import;
    PVOID ModuleBase;
    PVOID Unused;
    ULONG LastError;
} DELAY_INFO;

typedef struct
{
    ULONG Attributes, DllNameRVA, ModuleHandleRVA, IATRVA, INTRVA, BoundIATRVA, UnloadRVA, TimeDateStamp;
} DELAY_DESC;

typedef PVOID (WINAPI *PFN_Resolve)(PVOID, const DELAY_DESC *, PVOID, PVOID, PVOID, ULONG);

static DELAY_DESC g_Desc;
static HMODULE g_Module;
static ULONG_PTR g_Iat[4], g_Int[4];
static struct { WORD Hint; char Name[40]; } g_ByName[4];
static char g_DllName[64];

static int g_DllHookCalls, g_SysHookCalls;
static ULONG g_Reason;
static DELAY_INFO g_Info;
static char g_HookDll[64], g_HookImport[64], g_SysDll[64], g_SysImport[64];
static PVOID g_DllHookResult, g_SysHookResult;

static PVOID WINAPI DllHook(ULONG Reason, DELAY_INFO *Info)
{
    g_DllHookCalls++;
    g_Reason = Reason;
    g_Info = *Info;
    lstrcpynA(g_HookDll, Info->DllName, sizeof(g_HookDll));
    if (Info->ByName)
        lstrcpynA(g_HookImport, Info->Import.Name, sizeof(g_HookImport));
    else
        StringCchPrintfA(g_HookImport, sizeof(g_HookImport), "#%lu", Info->Import.Ordinal);
    return g_DllHookResult;
}

static PVOID WINAPI SysHook(LPCSTR Dll, LPCSTR Import)
{
    g_SysHookCalls++;
    lstrcpynA(g_SysDll, Dll, sizeof(g_SysDll));
    if ((ULONG_PTR)Import >> 16)
        lstrcpynA(g_SysImport, Import, sizeof(g_SysImport));
    else
        StringCchPrintfA(g_SysImport, sizeof(g_SysImport), "#%lu", (ULONG)(ULONG_PTR)Import);
    return g_SysHookResult;
}

static DWORD ImageRva(PVOID Address)
{
    return (DWORD)((PBYTE)Address - (PBYTE)GetModuleHandleW(NULL));
}

/* One delay-load import in slot Index: by name, or by ordinal when Name is NULL */
static void Setup(const char *Dll, const char *Name, ULONG Ordinal, int Index)
{
    memset(&g_Desc, 0, sizeof(g_Desc));
    memset(g_Iat, 0, sizeof(g_Iat));
    memset(g_Int, 0, sizeof(g_Int));
    memset(g_ByName, 0, sizeof(g_ByName));
    g_Module = NULL;
    lstrcpyA(g_DllName, Dll);
    g_Desc.Attributes = 1;
    g_Desc.DllNameRVA = ImageRva(g_DllName);
    g_Desc.ModuleHandleRVA = ImageRva(&g_Module);
    g_Desc.IATRVA = ImageRva(g_Iat);
    g_Desc.INTRVA = ImageRva(g_Int);
    /* an unresolved slot points into the image (the delay-load stub) */
    g_Iat[Index] = (ULONG_PTR)GetModuleHandleW(NULL) + 0x1000;
    if (Name)
    {
        lstrcpyA(g_ByName[Index].Name, Name);
        g_Int[Index] = ImageRva(&g_ByName[Index]);
    }
    else
    {
        g_Int[Index] = (ULONG_PTR)1 << (sizeof(ULONG_PTR) * 8 - 1) | Ordinal;
    }
}

static PVOID Call(PFN_Resolve Resolve, PVOID DllHookPtr, PVOID SysHookPtr, int Index)
{
    g_DllHookCalls = g_SysHookCalls = 0;
    SetLastError(0x5555);
    return Resolve(GetModuleHandleW(NULL), &g_Desc, DllHookPtr, SysHookPtr, &g_Iat[Index], 0);
}

static ULONG OrdinalOf(HMODULE Module, const char *Name)
{
    PBYTE Base = (PBYTE)Module;
    PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    PIMAGE_EXPORT_DIRECTORY Exports = (PIMAGE_EXPORT_DIRECTORY)(Base + Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    PDWORD Names = (PDWORD)(Base + Exports->AddressOfNames);
    PWORD Ordinals = (PWORD)(Base + Exports->AddressOfNameOrdinals);
    ULONG i;

    for (i = 0; i < Exports->NumberOfNames; i++)
        if (!strcmp((char *)Base + Names[i], Name))
            return Ordinals[i] + Exports->Base;
    return 0;
}

static void TestResolve(PFN_Resolve Resolve, const char *Label)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PVOID Expected = GetProcAddress(Kernel32, "GetTickCount");
    PVOID Result;
    ULONG Ordinal = OrdinalOf(Kernel32, "GetTickCount");

    /* by name */
    Setup("kernel32.dll", "GetTickCount", 0, 0);
    Result = Call(Resolve, NULL, NULL, 0);
    ok(Result == Expected, "%s: by name returned %p, expected %p\n", Label, Result, Expected);
    ok((PVOID)g_Iat[0] == Expected, "%s: table entry %p\n", Label, (PVOID)g_Iat[0]);
    ok(g_Module == Kernel32, "%s: module slot %p\n", Label, g_Module);
    ok(GetLastError() == 0x5555, "%s: last error changed to %lu\n", Label, GetLastError());

    /* the same, second table slot */
    Setup("kernel32.dll", "GetTickCount", 0, 1);
    Result = Call(Resolve, NULL, NULL, 1);
    ok(Result == Expected && (PVOID)g_Iat[1] == Expected, "%s: slot 1 returned %p, entry %p\n", Label, Result, (PVOID)g_Iat[1]);
    ok(g_Iat[0] == 0, "%s: slot 0 touched\n", Label);

    /* by ordinal */
    if (Ordinal)
    {
        Setup("kernel32.dll", NULL, Ordinal, 0);
        Result = Call(Resolve, NULL, NULL, 0);
        ok(Result == Expected && (PVOID)g_Iat[0] == Expected, "%s: ordinal %lu returned %p, entry %p\n", Label, Ordinal, Result, (PVOID)g_Iat[0]);
    }

    /* the file name does not need its extension */
    Setup("KERNEL32", "GetTickCount", 0, 0);
    Result = Call(Resolve, NULL, NULL, 0);
    ok(Result == Expected, "%s: name without extension returned %p\n", Label, Result);

    /* an entry that does not point into the image any more is already resolved */
    Setup("kernel32.dll", "GetTickCount", 0, 0);
    g_Iat[0] = 0x11111111;
    Result = Call(Resolve, NULL, NULL, 0);
    ok(Result == (PVOID)(ULONG_PTR)0x11111111, "%s: resolved entry returned %p\n", Label, Result);
    ok(g_Module == NULL, "%s: module loaded for a resolved entry\n", Label);

    /* a module slot that is set is used as it is */
    Setup("kernel32.dll", "GetTickCount", 0, 0);
    g_Module = GetModuleHandleW(L"ntdll.dll");
    Result = Call(Resolve, NULL, NULL, 0);
    ok(Result == NULL, "%s: GetTickCount in ntdll returned %p\n", Label, Result);
    ok(g_Iat[0] == (ULONG_PTR)GetModuleHandleW(NULL) + 0x1000, "%s: table entry changed to %p\n", Label, (PVOID)g_Iat[0]);

    /* failures raise nothing and return NULL */
    Setup("kernel32.dll", "NoSuchFunctionXyz", 0, 0);
    Result = Call(Resolve, NULL, NULL, 0);
    ok(Result == NULL, "%s: missing import returned %p\n", Label, Result);
    ok(g_Module == Kernel32, "%s: the module handle is kept: %p\n", Label, g_Module);
    ok(GetLastError() == 0x5555, "%s: last error changed to %lu\n", Label, GetLastError());
    Setup("nosuchdll_xyz.dll", "Foo", 0, 0);
    Result = Call(Resolve, NULL, NULL, 0);
    ok(Result == NULL, "%s: missing DLL returned %p\n", Label, Result);
    ok(g_Module == NULL, "%s: module slot %p\n", Label, g_Module);

    /* the failure hook of the DLL: reason 4, what was asked for, the result is used */
    Setup("kernel32.dll", "NoSuchFunctionXyz", 0, 0);
    g_DllHookResult = (PVOID)(ULONG_PTR)0x1234;
    Result = Call(Resolve, DllHook, SysHook, 0);
    ok(Result == (PVOID)(ULONG_PTR)0x1234, "%s: hook result %p\n", Label, Result);
    ok(g_DllHookCalls == 1 && g_SysHookCalls == 0, "%s: %d DLL hook calls, %d system calls\n", Label, g_DllHookCalls, g_SysHookCalls);
    ok(g_Reason == 4, "%s: reason %lu\n", Label, g_Reason);
    ok(g_Info.Size == sizeof(DELAY_INFO), "%s: size %lu, expected %lu\n", Label, g_Info.Size, (ULONG)sizeof(DELAY_INFO));
    ok(g_Info.Descriptor == &g_Desc && g_Info.Thunk == &g_Iat[0], "%s: descriptor %p, thunk %p\n", Label, g_Info.Descriptor, g_Info.Thunk);
    ok(!strcmp(g_HookDll, "kernel32.dll") && g_Info.ByName == 1 && !strcmp(g_HookImport, "NoSuchFunctionXyz"),
       "%s: asked for %s, %s (by name %lu)\n", Label, g_HookDll, g_HookImport, g_Info.ByName);
    ok(g_Info.ModuleBase == Kernel32, "%s: module base %p\n", Label, g_Info.ModuleBase);
    ok(g_Info.LastError == ERROR_PROC_NOT_FOUND, "%s: last error %lu\n", Label, g_Info.LastError);
    ok(g_Iat[0] == (ULONG_PTR)GetModuleHandleW(NULL) + 0x1000, "%s: table entry changed to %p\n", Label, (PVOID)g_Iat[0]);

    Setup("nosuchdll_xyz.dll", "Foo", 0, 0);
    Result = Call(Resolve, DllHook, NULL, 0);
    ok(Result == (PVOID)(ULONG_PTR)0x1234, "%s: hook result %p for a missing DLL\n", Label, Result);
    ok(g_Info.ModuleBase == NULL && g_Info.LastError == ERROR_MOD_NOT_FOUND, "%s: module %p, last error %lu\n", Label, g_Info.ModuleBase, g_Info.LastError);

    /* the system routine: when there is no DLL hook, or it returned NULL; with its asked-for names */
    Setup("nosuchdll_xyz.dll", "Foo", 0, 0);
    g_SysHookResult = (PVOID)(ULONG_PTR)0x4321;
    Result = Call(Resolve, NULL, SysHook, 0);
    ok(Result == (PVOID)(ULONG_PTR)0x4321, "%s: system routine result %p\n", Label, Result);
    ok(g_SysHookCalls == 1 && !strcmp(g_SysDll, "nosuchdll_xyz.dll") && !strcmp(g_SysImport, "Foo"), "%s: %d calls (%s, %s)\n", Label, g_SysHookCalls, g_SysDll, g_SysImport);
    g_DllHookResult = NULL;
    Result = Call(Resolve, DllHook, SysHook, 0);
    ok(Result == (PVOID)(ULONG_PTR)0x4321 && g_DllHookCalls == 1 && g_SysHookCalls == 1, "%s: both hooks: %p, %d, %d\n", Label, Result, g_DllHookCalls, g_SysHookCalls);

    /* an ordinal that does not exist: the last error, and the answer of a hook goes in the table */
    Setup("kernel32.dll", NULL, 0x7FFF, 0);
    g_DllHookResult = NULL;
    Result = Call(Resolve, DllHook, SysHook, 0);
    ok(Result == (PVOID)(ULONG_PTR)0x4321, "%s: missing ordinal returned %p\n", Label, Result);
    ok(g_Info.ByName == 0 && !strcmp(g_HookImport, "#32767") && g_Info.LastError == ERROR_INVALID_ORDINAL,
       "%s: asked for %s (by name %lu), last error %lu\n", Label, g_HookImport, g_Info.ByName, g_Info.LastError);
    ok(!strcmp(g_SysImport, "#32767"), "%s: system routine got %s\n", Label, g_SysImport);
    ok((PVOID)g_Iat[0] == (PVOID)(ULONG_PTR)0x4321, "%s: table entry %p\n", Label, (PVOID)g_Iat[0]);
}

START_TEST(ResolveDelayLoadedAPI)
{
    PFN_Resolve pResolve = NULL;
    HMODULE hApiSet, hNtdll = GetModuleHandleW(L"ntdll.dll");
    PFN_Resolve pLdr = (PFN_Resolve)GetProcAddress(hNtdll, "LdrResolveDelayLoadedAPI");

    /* the route getfilesiginforedist.dll takes: the API set DLL */
    hApiSet = LoadLibraryW(L"api-ms-win-core-delayload-l1-1-1.dll");
    ok(hApiSet != NULL, "api-ms-win-core-delayload-l1-1-1.dll does not load: %lu\n", GetLastError());
    if (hApiSet)
        pResolve = (PFN_Resolve)GetProcAddress(hApiSet, "ResolveDelayLoadedAPI");
    ok(pResolve != NULL, "api-ms-win-core-delayload-l1-1-1.dll does not export ResolveDelayLoadedAPI\n");
    if (pResolve)
        TestResolve(pResolve, "ResolveDelayLoadedAPI");

    ok(pLdr != NULL, "ntdll does not export LdrResolveDelayLoadedAPI\n");
    if (pLdr)
        TestResolve(pLdr, "LdrResolveDelayLoadedAPI");
}
