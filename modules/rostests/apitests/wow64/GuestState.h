/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Also permits the same fixture sources to build against the pre-fix headers. */
#ifndef _W32KTEB_H
#define EXPLICIT_64BIT
#include <ndk/peb_teb.h>
#undef EXPLICIT_64BIT
#endif

enum
{
#define WOW64_SYSCALL(Name, Source, Target, SourceSlots, TargetSlots, Flags) Native_##Name = Target,
#include <reactos/wow64/win32k32to64.inc>
#undef WOW64_SYSCALL
};

ULONG GuestStateCall(ULONG Number, ULONGLONG *Args);
PTEB64 __cdecl GuestStateNativeTeb(void);
void GuestStateUser(PTEB64 Teb, PVOID Connection);
void GuestStateGdi(PTEB64 Teb);
ULONG_PTR GuestStateImmThread(void);
PVOID GuestStateImmClient(void);
