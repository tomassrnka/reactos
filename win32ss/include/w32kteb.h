/*
 * PROJECT:     ReactOS Win32 subsystem
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Access the TEB/PEB in which win32k maintains client state
 */
#ifndef _W32KTEB_H
#define _W32KTEB_H

#include <ndk/pstypes.h>
#include "w32kwide.h"

#ifdef W32K_WIDE_ACTIVE

typedef TEB64 W32TEB, *PW32TEB;

/* MmCreateTeb places the guest TEB immediately after the page-rounded native
 * TEB. Both TEBs and the native PEB are below 2 GB in a WoW64 process. Do not
 * redirect NtCurrentTeb itself: last error, TLS, loader state and callbacks
 * still belong to the guest runtime. Pin the allocation contract here. */
C_ASSERT(((sizeof(TEB64) + 0xfff) & ~0xfff) == KTEB64_BIAS);
C_ASSERT(FIELD_OFFSET(TEB64, Win32ThreadInfo) == 0x78);
C_ASSERT(FIELD_OFFSET(TEB64, Win32ClientInfo) == 0x800);
C_ASSERT(FIELD_OFFSET(PEB64, GdiHandleBuffer) == 0x140);

FORCEINLINE PTEB64 W32GetCurrentTeb(VOID)
{
    return (PTEB64)((PBYTE)NtCurrentTeb() - KTEB64_BIAS);
}

FORCEINLINE PPEB64 W32GetCurrentPeb(VOID)
{
    ASSERT((W32GetCurrentTeb()->ProcessEnvironmentBlock >> 32) == 0);
    return (PPEB64)(ULONG_PTR)W32GetCurrentTeb()->ProcessEnvironmentBlock;
}

/* Opaque nonpaged-pool identity: retain all 64 bits for pointer comparisons. */
#define W32GetCurrentThreadInfo() (W32GetCurrentTeb()->Win32ThreadInfo)

/* GDI handles, unlike USER/NT handles, are issued zero-extended by win32k. */
#define W32GdiHandle(h) ((ULONG64)(ULONG_PTR)(h))
#define W32GetGdiSharedHandleTable() ((PVOID)(ULONG_PTR)W32GetCurrentPeb()->GdiSharedHandleTable)

#else

typedef TEB W32TEB, *PW32TEB;
#define W32GetCurrentTeb() NtCurrentTeb()
#define W32GetCurrentPeb() (NtCurrentTeb()->ProcessEnvironmentBlock)
#define W32GetCurrentThreadInfo() (NtCurrentTeb()->Win32ThreadInfo)
#define W32GdiHandle(h) (h)
#define W32GetGdiSharedHandleTable() (NtCurrentTeb()->ProcessEnvironmentBlock->GdiSharedHandleTable)

#endif
#endif
