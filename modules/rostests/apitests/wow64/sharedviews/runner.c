/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "probe.h"

#ifndef _WIN64
void __cdecl SharedViewCall64(ULONGLONG *Frame);
static ULONGLONG Call64(ULONGLONG Function, ULONGLONG A, ULONGLONG B, ULONGLONG C, ULONGLONG D)
{
    ULONGLONG Frame[6] = {Function, A, B, C, D, 0};
    SharedViewCall64(Frame);
    return Frame[5];
}
static void Read64(void *Destination, ULONGLONG Source, ULONG Size)
{
    Call64(1, (ULONG_PTR)Destination, Source, Size, 0);
}
static ULONGLONG Export64(ULONGLONG Module, const char *Name)
{
    IMAGE_DOS_HEADER Dos;
    IMAGE_NT_HEADERS64 Nt;
    IMAGE_EXPORT_DIRECTORY Exports;
    ULONG i, Rva;
    USHORT Ordinal;
    char Buffer[80];
    Read64(&Dos, Module, sizeof(Dos));
    Read64(&Nt, Module + Dos.e_lfanew, sizeof(Nt));
    Read64(&Exports, Module + Nt.OptionalHeader.DataDirectory[0].VirtualAddress, sizeof(Exports));
    for (i = 0; i < Exports.NumberOfNames; ++i)
    {
        Read64(&Rva, Module + Exports.AddressOfNames + i * sizeof(ULONG), sizeof(Rva));
        Read64(Buffer, Module + Rva, sizeof(Buffer));
        Buffer[sizeof(Buffer) - 1] = 0;
        if (strcmp(Buffer, Name)) continue;
        Read64(&Ordinal, Module + Exports.AddressOfNameOrdinals + i * sizeof(USHORT), sizeof(Ordinal));
        Read64(&Rva, Module + Exports.AddressOfFunctions + Ordinal * sizeof(ULONG), sizeof(Rva));
        return Module + Rva;
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
    static const char *Names[] = {"desktop", "server-info", "USER-table", "GDI-table", "DC_ATTR", "BRUSH_ATTR", "RGN_ATTR", "display-DC_ATTR"};
    SHARED_VIEW_RESULT Result = {0};
    BOOL Wow64 = FALSE;
    unsigned i, Failures = 0;
    Result.Mode = argc > 1 ? atoi(argv[1]) : 0;
    IsWow64Process(GetCurrentProcess(), &Wow64);
    printf("SHARED-VIEWS begin bits=%u wow64=%u mode=%lu\n", (unsigned)sizeof(void *) * 8, Wow64, Result.Mode);
    fflush(stdout);
#ifdef _WIN64
    {
        HMODULE Module = LoadLibraryW(L"C:\\harness\\w643b5_probe.dll");
        void (WINAPI *Probe)(SHARED_VIEW_RESULT *) = Module ? (void *)GetProcAddress(Module, "ProbeSharedViews") : NULL;
        if (!Probe) { printf("load failed %lu\n", GetLastError()); return 2; }
        Probe(&Result);
    }
#else
    {
        WCHAR Path[] = L"C:\\harness\\w643b5_probe.dll";
        struct { USHORT Length, MaximumLength; ULONG Padding; ULONGLONG Buffer; } String;
        ULONGLONG Ntdll, Loader, Module = 0, Probe;
        LONG Status;
        if (!Wow64) { printf("requires amd64 WoW64\n"); return 2; }
        Ntdll = Call64(0, 0, 0, 0, 0);
        Loader = Export64(Ntdll, "LdrLoadDll");
        if (!Loader) return 2;
        String.Length = sizeof(Path) - sizeof(WCHAR);
        String.MaximumLength = sizeof(Path);
        String.Padding = 0;
        String.Buffer = (ULONG_PTR)Path;
        Status = Call64(Loader, 0, 0, (ULONG_PTR)&String, (ULONG_PTR)&Module);
        if (Status < 0) { printf("load failed %08lx\n", Status); return 2; }
        Probe = Export64(Module, "ProbeSharedViews");
        if (!Probe) return 2;
        Call64(Probe, (ULONG_PTR)&Result, 0, 0, 0);
    }
#endif
    printf("status=%08lx wow64=%lu callbacks=%lu filled=%I64x reservations=%lu internal=%lu\n",
           Result.Status, Result.Wow64, Result.Callbacks, Result.Filled, Result.FillCount, Result.Failure);
    if (Result.Failure || Result.Wow64 != Wow64) ++Failures;
    if (Result.Mode == 1 && Result.Filled < 0x60000000) ++Failures;
    if (Result.Mode >= 2 && Result.Filled < 0x70000000) ++Failures;
    for (i = 0; i < 8; ++i)
    {
        printf("%s=%016I64x end=%016I64x\n", Names[i], Result.Address[i], Result.End[i]);
        if (Wow64 && (Result.Address[i] >= 0x80000000ULL || Result.End[i] > 0x80000000ULL)) ++Failures;
        if ((Result.Mode <= 1 || !Wow64) && (!Result.Address[i] || !Result.End[i])) ++Failures;
    }
    if (Result.Mode <= 1 && Result.Status < 0) ++Failures;
    if ((Result.Mode == 2 || Result.Mode >= 4) && Wow64 && Result.Status >= 0) ++Failures;
    if (Result.Mode == 3 && Wow64)
    {
        if (Result.Status < 0) ++Failures;
        for (i = 4; i < 8; ++i) if (Result.Address[i]) ++Failures;
        for (i = 0; i < 4; ++i) if (Result.Handles[i]) ++Failures;
    }
    printf("SHARED-VIEWS end failures=%u\n", Failures);
    return Failures ? 1 : 0;
}
