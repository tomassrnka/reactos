/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test the byte layout of the amd64 user-mode system call stubs
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifdef _M_AMD64

/*
 * Applications that hook system calls (the Chromium sandbox among them) accept a
 * stub only in one of the forms below, and overwrite its first 16 bytes:
 *
 * WIN7:  4C 8B D1        mov r10, rcx
 *        B8 xx xx xx xx  mov eax, id
 *        0F 05           syscall
 *        C3              ret
 *        (padding to the next 16-byte boundary)
 *
 * INT2E: 4C 8B D1 B8 xx xx xx xx, then
 *        F6 04 25 xx xx xx xx xx   test byte ptr [SharedUserData+xx], xx
 *        75 03                     jne +3
 *        0F 05 C3                  syscall; ret
 *        CD 2E C3                  int 2e; ret
 *
 * WIN8:  48 89 4C 24 08 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20
 *        (the four argument registers spilled), then the WIN7 code.
 *
 * OLD:   B8 xx xx xx xx 49 89 CA 0F 05 C3, packed (the earlier ReactOS stub).
 */
typedef enum _STUB_FORM
{
    FORM_NONE,
    FORM_WIN7,
    FORM_INT2E,
    FORM_WIN8,
    FORM_OLD
} STUB_FORM;

static const char *FormName[] = { "none", "win7", "int2e", "win8", "old" };

static const UCHAR MovR10MovEax[] = { 0x4C, 0x8B, 0xD1, 0xB8 };
static const UCHAR SyscallRet[] = { 0x0F, 0x05, 0xC3 };
static const UCHAR SpillArgs[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10,
                                   0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C, 0x24, 0x20 };
static const UCHAR OldMovR10[] = { 0x49, 0x89, 0xCA };

static
STUB_FORM
GetStubForm(_In_ const UCHAR *Code, _Out_ PULONG Id)
{
    *Id = 0;
    if (!memcmp(Code, MovR10MovEax, sizeof(MovR10MovEax)))
    {
        *Id = *(const ULONG UNALIGNED *)(Code + 4);
        if (!memcmp(Code + 8, SyscallRet, sizeof(SyscallRet)))
            return FORM_WIN7;
        if (Code[8] == 0xF6 && Code[9] == 0x04 && Code[10] == 0x25 &&
            Code[16] == 0x75 && Code[17] == 0x03 &&
            !memcmp(Code + 18, SyscallRet, sizeof(SyscallRet)) &&
            Code[21] == 0xCD && Code[22] == 0x2E && Code[23] == 0xC3)
        {
            return FORM_INT2E;
        }
        return FORM_NONE;
    }
    if (!memcmp(Code, SpillArgs, sizeof(SpillArgs)) &&
        !memcmp(Code + 20, MovR10MovEax, sizeof(MovR10MovEax)) &&
        !memcmp(Code + 28, SyscallRet, sizeof(SyscallRet)))
    {
        *Id = *(const ULONG UNALIGNED *)(Code + 24);
        return FORM_WIN8;
    }
    if (Code[0] == 0xB8 && !memcmp(Code + 5, OldMovR10, sizeof(OldMovR10)) &&
        !memcmp(Code + 8, SyscallRet, sizeof(SyscallRet)))
    {
        *Id = *(const ULONG UNALIGNED *)(Code + 1);
        return FORM_OLD;
    }
    return FORM_NONE;
}

/* Bytes of the stub that its code runs through */
static
SIZE_T
GetStubCodeSize(_In_ STUB_FORM Form)
{
    switch (Form)
    {
        case FORM_WIN7: return 11;
        case FORM_INT2E: return 24;
        case FORM_WIN8: return 31;
        default: return 0;
    }
}

static
BOOLEAN
IsFormExpected(_In_ STUB_FORM Form)
{
    ULONG Version = GetNTVersion();

    /* Windows 7 / Server 2008 R2 and earlier, and ReactOS */
    if (Version < _WIN32_WINNT_WIN8)
        return Form == FORM_WIN7;
    if (Version < _WIN32_WINNT_WIN10)
        return Form == FORM_WIN7 || Form == FORM_WIN8 || Form == FORM_INT2E;
    return Form == FORM_WIN7 || Form == FORM_INT2E;
}

typedef struct _EXPORT_INFO
{
    ULONG_PTR *Addresses; /* every exported code address, sorted, unique */
    ULONG Count;
} EXPORT_INFO;

static
int
__cdecl
CompareAddress(const void *A, const void *B)
{
    ULONG_PTR X = *(const ULONG_PTR *)A, Y = *(const ULONG_PTR *)B;
    return X < Y ? -1 : X > Y;
}

static
PIMAGE_EXPORT_DIRECTORY
GetExportDirectory(_In_ HMODULE Module, _Out_ PULONG Size)
{
    return RtlImageDirectoryEntryToData(Module, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, Size);
}

static
BOOLEAN
CollectExports(_In_ HMODULE Module, _Out_ EXPORT_INFO *Info)
{
    PIMAGE_EXPORT_DIRECTORY Dir;
    ULONG Size, i, n = 0;
    PULONG Functions;
    ULONG_PTR Base = (ULONG_PTR)Module;

    Info->Addresses = NULL;
    Info->Count = 0;
    Dir = GetExportDirectory(Module, &Size);
    if (!Dir)
        return FALSE;
    Functions = (PULONG)(Base + Dir->AddressOfFunctions);
    Info->Addresses = HeapAlloc(GetProcessHeap(), 0, Dir->NumberOfFunctions * sizeof(ULONG_PTR));
    if (!Info->Addresses)
        return FALSE;
    for (i = 0; i < Dir->NumberOfFunctions; i++)
    {
        ULONG Rva = Functions[i];
        /* skip unused slots and forwarders */
        if (Rva == 0 || (Rva >= (ULONG_PTR)Dir - Base && Rva < (ULONG_PTR)Dir - Base + Size))
            continue;
        Info->Addresses[n++] = Base + Rva;
    }
    qsort(Info->Addresses, n, sizeof(ULONG_PTR), CompareAddress);
    Info->Count = 0;
    for (i = 0; i < n; i++)
    {
        if (Info->Count == 0 || Info->Addresses[Info->Count - 1] != Info->Addresses[i])
            Info->Addresses[Info->Count++] = Info->Addresses[i];
    }
    return TRUE;
}

/* Distance from Address to the next higher exported address, or 0 if there is none */
static
ULONG_PTR
SpaceToNextExport(_In_ const EXPORT_INFO *Info, _In_ ULONG_PTR Address)
{
    ULONG i;
    for (i = 0; i < Info->Count; i++)
    {
        if (Info->Addresses[i] > Address)
            return Info->Addresses[i] - Address;
    }
    return 0;
}

static
void
CheckStub(
    _In_ HMODULE Module,
    _In_ const EXPORT_INFO *Exports,
    _In_ PCSTR Name,
    _In_opt_ PCSTR AliasName,
    _In_ BOOLEAN Win32kTable,
    _Out_opt_ PULONG IdOut)
{
    const UCHAR *Code;
    ULONG Id;
    STUB_FORM Form;
    ULONG_PTR Space;

    if (IdOut)
        *IdOut = MAXULONG;
    Code = (const UCHAR *)GetProcAddress(Module, Name);
    ok(Code != NULL, "%s is not exported\n", Name);
    if (!Code)
        return;

    if (AliasName)
    {
        PVOID Alias = GetProcAddress(Module, AliasName);
        ok(Alias == Code, "%s at %p, %s at %p\n", AliasName, Alias, Name, Code);
    }

    Form = GetStubForm(Code, &Id);
    if (Win32kTable && Form == FORM_NONE)
    {
        /* a user32/gdi32 export is a direct stub only on some versions */
        skip("%s is not a direct system call stub here\n", Name);
        return;
    }
    ok(IsFormExpected(Form), "%s: stub form %s, not the Windows layout for NT 0x%lx\n",
       Name, FormName[Form], GetNTVersion());
    if (Form == FORM_NONE || Form == FORM_OLD)
        return;

    ok(((ULONG_PTR)Code & 15) == 0, "%s at %p is not 16-byte aligned\n", Name, Code);
    Space = SpaceToNextExport(Exports, (ULONG_PTR)Code);
    ok(Space == 0 || Space >= 16, "%s: next export only %Iu bytes after the stub\n", Name, Space);
    ok(Space == 0 || Space >= GetStubCodeSize(Form), "%s: next export %Iu bytes after a %Iu-byte stub\n",
       Name, Space, GetStubCodeSize(Form));
    if (Win32kTable)
        ok((Id & 0xF000) == 0x1000, "%s: service id 0x%lx is not in the win32k table\n", Name, Id);
    else
        ok(Id < 0x1000, "%s: service id 0x%lx is not in the native table\n", Name, Id);
    if (IdOut)
        *IdOut = Id;
}

/* Gaps between address-adjacent "Nt*" stubs of a module */
static
void
CheckStride(_In_ HMODULE Module, _In_ PCSTR ModuleName, _In_ const EXPORT_INFO *Exports)
{
    PIMAGE_EXPORT_DIRECTORY Dir;
    ULONG Size, i, n = 0, Gap16 = 0, GapOther = 0, Misaligned = 0, Crowded = 0, Forms[5] = { 0 };
    PULONG Names;
    PUSHORT Ordinals;
    PULONG Functions;
    ULONG_PTR *Stubs, Base = (ULONG_PTR)Module, MinGap = MAXULONG_PTR, MaxGap = 0;

    Dir = GetExportDirectory(Module, &Size);
    ok(Dir != NULL, "%s has no export directory\n", ModuleName);
    if (!Dir)
        return;
    Names = (PULONG)(Base + Dir->AddressOfNames);
    Ordinals = (PUSHORT)(Base + Dir->AddressOfNameOrdinals);
    Functions = (PULONG)(Base + Dir->AddressOfFunctions);
    Stubs = HeapAlloc(GetProcessHeap(), 0, Dir->NumberOfNames * sizeof(ULONG_PTR));
    ok(Stubs != NULL, "HeapAlloc failed\n");
    if (!Stubs)
        return;

    for (i = 0; i < Dir->NumberOfNames; i++)
    {
        PCSTR Name = (PCSTR)(Base + Names[i]);
        ULONG Rva = Functions[Ordinals[i]];
        ULONG Id;
        STUB_FORM Form;

        if (Name[0] != 'N' || Name[1] != 't' || Name[2] < 'A' || Name[2] > 'Z')
            continue;
        if (Rva >= (ULONG_PTR)Dir - Base && Rva < (ULONG_PTR)Dir - Base + Size)
            continue;
        Form = GetStubForm((const UCHAR *)(Base + Rva), &Id);
        Forms[Form]++;
        if (Form == FORM_NONE)
            continue;
        Stubs[n++] = Base + Rva;
    }
    qsort(Stubs, n, sizeof(ULONG_PTR), CompareAddress);

    for (i = 0; i < n; i++)
    {
        ULONG_PTR Space = SpaceToNextExport(Exports, Stubs[i]);
        if (Stubs[i] & 15)
            Misaligned++;
        if (Space != 0 && Space < 16)
            Crowded++;
        if (i + 1 < n)
        {
            ULONG_PTR Gap = Stubs[i + 1] - Stubs[i];
            if (Gap == 16)
                Gap16++;
            else
                GapOther++;
            if (Gap < MinGap)
                MinGap = Gap;
            if (Gap > MaxGap)
                MaxGap = Gap;
        }
    }

    trace("%s: %lu Nt* stubs (win7 %lu, int2e %lu, win8 %lu, old %lu, other Nt* exports %lu); "
          "adjacent gaps: 16 bytes %lu, other %lu, min %Iu, max %Iu\n",
          ModuleName, n, Forms[FORM_WIN7], Forms[FORM_INT2E], Forms[FORM_WIN8], Forms[FORM_OLD],
          Forms[FORM_NONE], Gap16, GapOther, n > 1 ? MinGap : 0, n > 1 ? MaxGap : 0);

    ok(n > 100, "%s: only %lu Nt* exports are system call stubs\n", ModuleName, n);
    ok(Forms[FORM_OLD] == 0, "%s: %lu stubs in the old packed form\n", ModuleName, Forms[FORM_OLD]);
    ok(Misaligned == 0, "%s: %lu stubs not 16-byte aligned\n", ModuleName, Misaligned);
    ok(Crowded == 0, "%s: %lu stubs have another export within 16 bytes\n", ModuleName, Crowded);
    if (n > 1)
    {
        ok(MinGap >= 16 && (MinGap % 16) == 0, "%s: smallest stub gap %Iu\n", ModuleName, MinGap);
        if (GetNTVersion() < _WIN32_WINNT_WIN8)
        {
            /* Windows 7 x64: one stub per 16 bytes, back to back (Server 2008 R2: 398 of 399 gaps) */
            ok(MinGap == 16, "%s: smallest stub gap %Iu, expected 16\n", ModuleName, MinGap);
            ok(Gap16 * 10 >= (n - 1) * 9, "%s: only %lu of %lu stub gaps are 16 bytes (max %Iu)\n",
               ModuleName, Gap16, n - 1, MaxGap);
        }
    }
    HeapFree(GetProcessHeap(), 0, Stubs);
}

typedef NTSTATUS (NTAPI *PFN_NtYieldExecution)(VOID);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);
typedef NTSTATUS (NTAPI *PFN_NtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtQueryVirtualMemory)(HANDLE, PVOID, MEMORY_INFORMATION_CLASS, PVOID, SIZE_T, PSIZE_T);

static
void
CheckQueryProcess(_In_ PFN_NtQueryInformationProcess Query, _In_ PCSTR How)
{
    PROCESS_BASIC_INFORMATION Info;
    ULONG ReturnLength = 0xDEADBEEF;
    NTSTATUS Status;

    RtlFillMemory(&Info, sizeof(Info), 0xAA);
    Status = Query(GetCurrentProcess(), ProcessBasicInformation, &Info, sizeof(Info), &ReturnLength);
    ok(Status == STATUS_SUCCESS, "%s: NtQueryInformationProcess returned 0x%lx\n", How, Status);
    ok(ReturnLength == sizeof(Info), "%s: ReturnLength %lu\n", How, ReturnLength);
    ok(Info.UniqueProcessId == (ULONG_PTR)GetCurrentProcessId(), "%s: UniqueProcessId %Iu, expected %lu\n",
       How, Info.UniqueProcessId, GetCurrentProcessId());
    ok(Info.PebBaseAddress == NtCurrentTeb()->ProcessEnvironmentBlock, "%s: PebBaseAddress %p\n",
       How, Info.PebBaseAddress);
}

static
void
CheckCalls(_In_ HMODULE Ntdll)
{
    PFN_NtYieldExecution pYield = (PFN_NtYieldExecution)GetProcAddress(Ntdll, "NtYieldExecution");
    PFN_NtClose pClose = (PFN_NtClose)GetProcAddress(Ntdll, "NtClose");
    PFN_NtQueryInformationProcess pQuery =
        (PFN_NtQueryInformationProcess)GetProcAddress(Ntdll, "NtQueryInformationProcess");
    PFN_NtQueryVirtualMemory pQueryVm = (PFN_NtQueryVirtualMemory)GetProcAddress(Ntdll, "ZwQueryVirtualMemory");
    MEMORY_BASIC_INFORMATION Mbi;
    SIZE_T Length = 0xDEADBEEF;
    NTSTATUS Status;
    HANDLE Event;
    PUCHAR Copy;
    ULONG Id;
    STUB_FORM Form;
    SIZE_T CodeSize;

    if (!pYield || !pClose || !pQuery || !pQueryVm)
    {
        skip("missing exports\n");
        return;
    }

    /* through the exports */
    Status = pYield();
    ok(Status == STATUS_SUCCESS || Status == STATUS_NO_YIELD_PERFORMED, "NtYieldExecution returned 0x%lx\n", Status);

    Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(Event != NULL, "CreateEventW failed: %lu\n", GetLastError());
    Status = pClose(Event);
    ok(Status == STATUS_SUCCESS, "NtClose returned 0x%lx\n", Status);

    CheckQueryProcess(pQuery, "export");

    /* six arguments: the fifth and sixth go on the stack */
    Status = pQueryVm(GetCurrentProcess(), (PVOID)CheckCalls, MemoryBasicInformation, &Mbi, sizeof(Mbi), &Length);
    ok(Status == STATUS_SUCCESS, "ZwQueryVirtualMemory returned 0x%lx\n", Status);
    ok(Length == sizeof(Mbi), "ZwQueryVirtualMemory length %Iu\n", Length);
    ok(Mbi.AllocationBase == (PVOID)GetModuleHandleW(NULL), "AllocationBase %p, module %p\n",
       Mbi.AllocationBase, GetModuleHandleW(NULL));
    ok(Mbi.Type == MEM_IMAGE, "Type 0x%lx\n", Mbi.Type);

    /* A copy of the stub's own bytes, run elsewhere, as a hooking thunk runs the original */
    Form = GetStubForm((const UCHAR *)pQuery, &Id);
    CodeSize = GetStubCodeSize(Form);
    if (!IsFormExpected(Form) || CodeSize == 0)
    {
        skip("NtQueryInformationProcess: stub form %s, copy not run\n", FormName[Form]);
        return;
    }
    Copy = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ok(Copy != NULL, "VirtualAlloc failed: %lu\n", GetLastError());
    if (!Copy)
        return;
    memset(Copy, 0xCC, 4096);
    memcpy(Copy, (PVOID)pQuery, CodeSize);
    FlushInstructionCache(GetCurrentProcess(), Copy, CodeSize);
    CheckQueryProcess((PFN_NtQueryInformationProcess)Copy, "copy");
    VirtualFree(Copy, 0, MEM_RELEASE);
}

static
void
CheckWin32kCalls(_In_ HMODULE User32, _In_ HMODULE Gdi32)
{
    typedef UINT (WINAPI *PFN_GetDoubleClickTime)(VOID);
    typedef HRGN (WINAPI *PFN_CreateRoundRectRgn)(int, int, int, int, int, int);
    typedef int (WINAPI *PFN_GetRgnBox)(HRGN, LPRECT);
    typedef BOOL (WINAPI *PFN_DeleteObject)(HGDIOBJ);
    typedef BOOL (WINAPI *PFN_PtInRegion)(HRGN, int, int);
    PFN_GetDoubleClickTime GetDct = (PFN_GetDoubleClickTime)GetProcAddress(User32, "GetDoubleClickTime");
    PFN_CreateRoundRectRgn CreateRrr = (PFN_CreateRoundRectRgn)GetProcAddress(Gdi32, "CreateRoundRectRgn");
    PFN_GetRgnBox pGetRgnBox = (PFN_GetRgnBox)GetProcAddress(Gdi32, "GetRgnBox");
    PFN_DeleteObject pDeleteObject = (PFN_DeleteObject)GetProcAddress(Gdi32, "DeleteObject");
    PFN_PtInRegion pPtInRegion = (PFN_PtInRegion)GetProcAddress(Gdi32, "PtInRegion");
    HRGN Region;
    RECT Box;

    ok(GetDct && CreateRrr && pGetRgnBox && pDeleteObject && pPtInRegion, "missing exports\n");
    if (!GetDct || !CreateRrr || !pGetRgnBox || !pDeleteObject || !pPtInRegion)
        return;
    ok(GetDct() != 0, "GetDoubleClickTime returned 0\n");
    /* six arguments: the corner ellipse (the fifth and sixth, on the stack) rounds off the corner point */
    Region = CreateRrr(10, 20, 110, 70, 40, 40);
    ok(Region != NULL, "CreateRoundRectRgn failed: %lu\n", GetLastError());
    if (Region)
    {
        ok(pGetRgnBox(Region, &Box) != 0, "GetRgnBox failed\n");
        /* Windows 7 and 10 exclude the right and bottom edge */
        ok(Box.left == 10 && Box.top == 20 && Box.right == 109 && Box.bottom == 69,
           "region box (%ld,%ld)-(%ld,%ld)\n", Box.left, Box.top, Box.right, Box.bottom);
        ok(!pPtInRegion(Region, 11, 21), "corner point inside the round-rect region\n");
        ok(pPtInRegion(Region, 60, 45), "centre point outside the round-rect region\n");
        pDeleteObject(Region);
    }
}

static const struct
{
    PCSTR Nt;
    PCSTR Zw;
} NativeStubs[] =
{
    /* functions the Chromium sandbox intercepts, and a few more */
    { "NtClose", "ZwClose" },
    { "NtYieldExecution", "ZwYieldExecution" },
    { "NtQueryInformationProcess", "ZwQueryInformationProcess" },
    { "NtQueryVirtualMemory", "ZwQueryVirtualMemory" },
    { "NtOpenProcess", "ZwOpenProcess" },
    { "NtOpenThread", "ZwOpenThread" },
    { "NtOpenProcessToken", "ZwOpenProcessToken" },
    { "NtOpenThreadToken", "ZwOpenThreadToken" },
    { "NtSetInformationThread", "ZwSetInformationThread" },
    { "NtCreateFile", "ZwCreateFile" },
    { "NtOpenFile", "ZwOpenFile" },
    { "NtQueryAttributesFile", "ZwQueryAttributesFile" },
    { "NtQueryFullAttributesFile", "ZwQueryFullAttributesFile" },
    { "NtSetInformationFile", "ZwSetInformationFile" },
    { "NtCreateSection", "ZwCreateSection" },
    { "NtOpenSection", "ZwOpenSection" },
    { "NtMapViewOfSection", "ZwMapViewOfSection" },
    { "NtUnmapViewOfSection", "ZwUnmapViewOfSection" },
    { "NtProtectVirtualMemory", "ZwProtectVirtualMemory" },
    { "NtCreateKey", "ZwCreateKey" },
    { "NtOpenKey", "ZwOpenKey" },
    { "NtCreateEvent", "ZwCreateEvent" },
    { "NtOpenEvent", "ZwOpenEvent" },
    { "NtCreateMutant", "ZwCreateMutant" },
};

START_TEST(SystemCallStubs)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE User32 = LoadLibraryW(L"user32.dll");
    HMODULE Gdi32 = LoadLibraryW(L"gdi32.dll");
    EXPORT_INFO Exports;
    ULONG Ids[RTL_NUMBER_OF(NativeStubs)];
    ULONG i, j;

    trace("NT version 0x%lx\n", GetNTVersion());
    ok(Ntdll != NULL, "no ntdll\n");
    if (!Ntdll)
        return;

    ok(CollectExports(Ntdll, &Exports), "cannot read the ntdll export table\n");
    for (i = 0; i < RTL_NUMBER_OF(NativeStubs); i++)
        CheckStub(Ntdll, &Exports, NativeStubs[i].Nt, NativeStubs[i].Zw, FALSE, &Ids[i]);
    for (i = 0; i < RTL_NUMBER_OF(NativeStubs); i++)
    {
        for (j = i + 1; j < RTL_NUMBER_OF(NativeStubs); j++)
        {
            ok(Ids[i] == MAXULONG || Ids[i] != Ids[j], "%s and %s share service id 0x%lx\n",
               NativeStubs[i].Nt, NativeStubs[j].Nt, Ids[i]);
        }
    }
    CheckStride(Ntdll, "ntdll", &Exports);
    HeapFree(GetProcessHeap(), 0, Exports.Addresses);

    CheckCalls(Ntdll);

    /* user32 and gdi32 exports that ReactOS implements as direct win32k stubs */
    if (User32 && Gdi32)
    {
        if (CollectExports(User32, &Exports))
        {
            CheckStub(User32, &Exports, "GetDoubleClickTime", NULL, TRUE, NULL);
            CheckStub(User32, &Exports, "GetProcessWindowStation", NULL, TRUE, NULL);
            HeapFree(GetProcessHeap(), 0, Exports.Addresses);
        }
        if (CollectExports(Gdi32, &Exports))
        {
            CheckStub(Gdi32, &Exports, "CreateRoundRectRgn", NULL, TRUE, NULL);
            CheckStub(Gdi32, &Exports, "AnyLinkedFonts", NULL, TRUE, NULL);
            HeapFree(GetProcessHeap(), 0, Exports.Addresses);
        }
        CheckWin32kCalls(User32, Gdi32);
    }
    else
    {
        skip("user32 or gdi32 not loaded\n");
    }
}

#else

START_TEST(SystemCallStubs)
{
    skip("amd64 only\n");
}

#endif
