/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared declarations
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <winuser.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>
#include <pseh/pseh2.h>

#ifndef MEM_LARGE_PAGES
#define MEM_LARGE_PAGES 0x20000000
#endif

#define MMT_MAX_SLOTS 128
#define MMT_SHARED_NAME L"MmTortureShared"
#define MMT_STOP_EVENT L"MmTortureStop"

typedef struct _MMT_SLOT
{
    volatile LONG Ops;
    volatile LONG LastOpTick;
    LONG ProcessId;
    LONG InUse;
    CHAR Name[24];
} MMT_SLOT;

typedef struct _MMT_SHARED
{
    volatile LONG SlotCount;
    volatile LONG Failures;
    volatile LONG Children;
    volatile LONG ChildFailures;
    MMT_SLOT Slot[MMT_MAX_SLOTS];
} MMT_SHARED;

typedef struct _MMT_RNG
{
    ULONGLONG State;
} MMT_RNG;

extern MMT_SHARED *MmtShared;
extern HANDLE MmtStopEvent;
extern WCHAR MmtExePath[MAX_PATH];

/* common.c */
VOID MmtLog(const char *Format, ...);
VOID MmtFail(const char *Format, ...);
BOOL MmtOpenShared(BOOL Create);
LONG MmtAllocSlot(const char *Name);
VOID MmtProgress(LONG Slot);
BOOL MmtShouldStop(VOID);
VOID MmtRngInit(MMT_RNG *Rng, ULONGLONG Seed);
ULONG MmtRand(MMT_RNG *Rng);
ULONG MmtRandRange(MMT_RNG *Rng, ULONG Low, ULONG High);
ULONGLONG MmtPatternWord(ULONGLONG Seed, SIZE_T Page, SIZE_T Word);
VOID MmtFillPage(PVOID Page, ULONGLONG Seed, SIZE_T PageIndex);
BOOL MmtCheckPage(const VOID *Page, ULONGLONG Seed, SIZE_T PageIndex, BOOL Full, const char *What);
BOOL MmtIsZeroPage(const VOID *Page);
BOOL MmtSpawn(PCWSTR Arguments, BOOL Wait, DWORD TimeoutMs, PDWORD ExitCode, PHANDLE ProcessHandle);
VOID MmtSetupProcess(VOID);
ULONG MmtArgUlong(int argc, char **argv, int Index, ULONG Default);

/* workloads */
int MmtMonitorMain(int argc, char **argv);
int MmtPressureMain(int argc, char **argv);
int MmtProcsMain(int argc, char **argv);
int MmtChildMain(int argc, char **argv);
int MmtMappingMain(int argc, char **argv);
int MmtVmApiMain(int argc, char **argv);
int MmtPoolMain(int argc, char **argv);
int MmtProbeMain(int argc, char **argv);
