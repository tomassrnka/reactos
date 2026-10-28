/*
 * PROJECT:     ReactOS runtime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NtUser PFN registration
 * COPYRIGHT:   Copyright 2026 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

#include <rtl.h>
#include <ndk/rtlfuncs.h>

#define NDEBUG
#include <debug.h>

/*
 * Fork: the 32-bit WoW64 guest ntdll (SYSWOW64_GUEST) works with the wide PFNCLIENT that the guest user32 shares
 * with the 64-bit win32k, where every pointer is 64 bits wide (as it is for a 32-bit process on Windows 10 WoW64).
 * So all tables here have 64-bit entries holding zero-extended addresses; see ntuserdisp.s for the thunks and
 * the stub table. Native i386 and amd64 builds keep pointer-sized entries.
 */
#ifdef SYSWOW64_GUEST
typedef ULONG64 NTUSER_PFN_ENTRY;
#else
typedef PVOID NTUSER_PFN_ENTRY;
#endif

typedef struct _NTUSER_PFN
{
    NTUSER_PFN_ENTRY WndProcsA[RTLP_NUMBER_OF_NTUSER_WNDPROCS];
    NTUSER_PFN_ENTRY WndProcsW[RTLP_NUMBER_OF_NTUSER_WNDPROCS];
    NTUSER_PFN_ENTRY WorkerProcs[RTLP_NUMBER_OF_NTUSER_WORKERPROCS];
} NTUSER_PFN, *PNTUSER_PFN;

const NTUSER_PFN RtlpNtUserPfns = { 0 };

extern const NTUSER_PFN NtDllUserStubs;

static BOOLEAN NtUserPfnsInitialized;

NTSTATUS
NTAPI
RtlInitializeNtUserPfn(
    _In_reads_bytes_(SizeOfWndProcsA) LPCVOID WndProcsA,
    _In_ SIZE_T SizeOfWndProcsA,
    _In_reads_bytes_(SizeOfWndProcsW) LPCVOID WndProcsW,
    _In_ SIZE_T SizeOfWndProcsW,
    _In_reads_bytes_(SizeOfWorkerProcs) LPCVOID WorkerProcs,
    _In_ SIZE_T SizeOfWorkerProcs)
{
    ULONG OldProtect;
    PVOID BaseAddress;
    SIZE_T RegionSize;
    NTSTATUS Status;

    /* Validate parameters */
    if ((SizeOfWndProcsA != sizeof(RtlpNtUserPfns.WndProcsA)) ||
        (SizeOfWndProcsW != sizeof(RtlpNtUserPfns.WndProcsW)) ||
        (SizeOfWorkerProcs != sizeof(RtlpNtUserPfns.WorkerProcs)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Check if already initialized */
    if (NtUserPfnsInitialized != FALSE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Change page protection, so we can set up the PFNs */
    BaseAddress = (PVOID)&RtlpNtUserPfns;
    RegionSize = sizeof(RtlpNtUserPfns);
    Status = NtProtectVirtualMemory(NtCurrentProcess(),
                                    &BaseAddress,
                                    &RegionSize,
                                    PAGE_READWRITE,
                                    &OldProtect);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    RtlCopyMemory((PVOID)RtlpNtUserPfns.WndProcsA, WndProcsA, SizeOfWndProcsA);
    RtlCopyMemory((PVOID)RtlpNtUserPfns.WndProcsW, WndProcsW, SizeOfWndProcsW);
    RtlCopyMemory((PVOID)RtlpNtUserPfns.WorkerProcs, WorkerProcs, SizeOfWorkerProcs);

    /* Restore original page protection */
    Status = NtProtectVirtualMemory(NtCurrentProcess(),
                                    &BaseAddress,
                                    &RegionSize,
                                    OldProtect,
                                    &OldProtect);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    NtUserPfnsInitialized = TRUE;

    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
RtlRetrieveNtUserPfn(
    _Outptr_ const VOID** NtdllWndProcsA,
    _Outptr_ const VOID** NtdllWndProcsW,
    _Outptr_ const VOID** NtdllWorkerProcs)
{
    if (NtUserPfnsInitialized == FALSE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *NtdllWndProcsA = NtDllUserStubs.WndProcsA;
    *NtdllWndProcsW = NtDllUserStubs.WndProcsW;
    *NtdllWorkerProcs = NtDllUserStubs.WorkerProcs;

    return STATUS_SUCCESS;
}
