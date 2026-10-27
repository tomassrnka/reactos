/*
 * PROJECT:     ReactOS
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Extended (XSTATE) context: the CONTEXT_EX layout and the ntdll functions that handle it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The PSDK headers show CONTEXT_EX to kernel mode only, so the user mode code that needs it uses these names for the
 * same layout.
 */

#pragma once

typedef struct _XCONTEXT_CHUNK
{
    LONG Offset;
    ULONG Length;
} XCONTEXT_CHUNK;

/* Directly behind the CONTEXT; the chunk offsets count from the XCONTEXT_EX itself */
typedef struct _XCONTEXT_EX
{
    XCONTEXT_CHUNK All;
    XCONTEXT_CHUNK Legacy;
    XCONTEXT_CHUNK XState;
} XCONTEXT_EX, *PXCONTEXT_EX;

NTSYSAPI ULONG64 NTAPI RtlGetEnabledExtendedFeatures(_In_ ULONG64 FeatureMask);
NTSYSAPI NTSTATUS NTAPI RtlGetExtendedContextLength(_In_ ULONG ContextFlags, _Out_ PULONG ContextLength);
NTSYSAPI NTSTATUS NTAPI RtlGetExtendedContextLength2(_In_ ULONG ContextFlags, _Out_ PULONG ContextLength, _In_ ULONG64 CompactionMask);
NTSYSAPI NTSTATUS NTAPI RtlInitializeExtendedContext(_Out_ PVOID Buffer, _In_ ULONG ContextFlags, _Out_ PXCONTEXT_EX *ContextEx);
NTSYSAPI NTSTATUS NTAPI RtlInitializeExtendedContext2(_Out_ PVOID Buffer, _In_ ULONG ContextFlags, _Out_ PXCONTEXT_EX *ContextEx, _In_ ULONG64 CompactionMask);
NTSYSAPI PVOID NTAPI RtlLocateExtendedFeature(_In_ PXCONTEXT_EX ContextEx, _In_ ULONG FeatureId, _Out_opt_ PULONG Length);
NTSYSAPI PVOID NTAPI RtlLocateExtendedFeature2(_In_ PXCONTEXT_EX ContextEx, _In_ ULONG FeatureId, _In_ PXSTATE_CONFIGURATION XStateConfig, _Out_opt_ PULONG Length);
NTSYSAPI PVOID NTAPI RtlLocateLegacyContext(_In_ PXCONTEXT_EX ContextEx, _Out_opt_ PULONG Length);
NTSYSAPI VOID NTAPI RtlSetExtendedFeaturesMask(_In_ PXCONTEXT_EX ContextEx, _In_ ULONG64 FeatureMask);
NTSYSAPI ULONG64 NTAPI RtlGetExtendedFeaturesMask(_In_ PXCONTEXT_EX ContextEx);
NTSYSAPI NTSTATUS NTAPI RtlCopyContext(_Inout_ PCONTEXT Destination, _In_ ULONG ContextFlags, _In_ PCONTEXT Source);
NTSYSAPI NTSTATUS NTAPI RtlCopyExtendedContext(_Inout_ PXCONTEXT_EX Destination, _In_ ULONG ContextFlags, _In_ PXCONTEXT_EX Source);
