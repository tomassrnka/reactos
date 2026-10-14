/* ReactOS WoW64 private definitions. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>
#include <pseh/pseh2.h>
#include <reactos/wow64context.h>

#define PTR32(Value) ((PVOID)(ULONG_PTR)(ULONG)(Value))
#define HANDLE32(Value) ((HANDLE)(LONG_PTR)(LONG)(Value))

typedef struct { ULONG Status, Information; } WOW64_IOSB;
typedef struct { USHORT Length, MaximumLength; ULONG Buffer; } WOW64_STRING;
typedef struct
{
    ULONG Length, RootDirectory, ObjectName, Attributes;
    ULONG SecurityDescriptor, SecurityQualityOfService;
} WOW64_OBJECT_ATTRIBUTES;

typedef struct { WOW64_STRING DosPath; ULONG Handle; } WOW64_CURDIR;
typedef struct { USHORT Flags, Length; ULONG TimeStamp; WOW64_STRING DosPath; } WOW64_DRIVE;
typedef struct
{
    ULONG MaximumLength, Length, Flags, DebugFlags;
    ULONG ConsoleHandle, ConsoleFlags, StandardInput, StandardOutput, StandardError;
    WOW64_CURDIR CurrentDirectory;
    WOW64_STRING DllPath, ImagePathName, CommandLine;
    ULONG Environment;
    ULONG StartingX, StartingY, CountX, CountY, CountCharsX, CountCharsY;
    ULONG FillAttribute, WindowFlags, ShowWindowFlags;
    WOW64_STRING WindowTitle, DesktopInfo, ShellInfo, RuntimeData;
    WOW64_DRIVE CurrentDirectories[32];
    ULONG EnvironmentSize;
} WOW64_PARAMETERS;

NTSTATUS NTAPI Wow64SystemService(ULONG Number, ULONG *Arguments);
NTSTATUS Wow64Allocate(SIZE_T Size, PVOID *Base);

NTSTATUS Wow64CopyString(WOW64_STRING *To, const UNICODE_STRING *From);
NTSTATUS Wow64CsrDispatch(ULONG Operation, ULONG *Arguments);
extern PVOID Wow64ExceptionDispatcher;
NTSTATUS Wow64Continue(const WOW64_CONTEXT *Context, BOOLEAN TestAlert);
NTSTATUS Wow64InitializeRedirection(VOID);
NTSTATUS Wow64RedirectFile(OBJECT_ATTRIBUTES *Attributes, UNICODE_STRING *Name, PVOID *Buffer);

typedef struct
{
    ULONG Reserved, TimerResolution, PageSize, NumberOfPhysicalPages;
    ULONG LowestPhysicalPageNumber, HighestPhysicalPageNumber, AllocationGranularity;
    ULONG MinimumUserModeAddress, MaximumUserModeAddress, ActiveProcessorsAffinityMask;
    CCHAR NumberOfProcessors;
} WOW64_SYSTEM_BASIC_INFORMATION;

NTSTATUS Wow64CreateEnvironment(PVOID *Environment);

VOID Wow64InitializeTeb(TEB32 *, const TEB *, PTEB, const INITIAL_TEB *);
VOID Wow64AbortThread(HANDLE, HANDLE, NTSTATUS);
NTSTATUS Wow64CreateThread(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, HANDLE,
                           CLIENT_ID *, const WOW64_CONTEXT *, const ULONG *, BOOLEAN);
NTSTATUS Wow64QueryThread(HANDLE, ULONG, PVOID, ULONG, PULONG);

extern PVOID Wow64ApcDispatcher;

NTSTATUS Wow64RaiseException(const EXCEPTION_RECORD32 *, const WOW64_CONTEXT *, BOOLEAN);
