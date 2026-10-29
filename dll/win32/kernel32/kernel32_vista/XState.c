/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     InitializeContext, CopyContext and the XState functions (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Thin wrappers around the Rtl*ExtendedContext functions of ntdll, as in Wine's kernelbase. The x87 and SSE state
 * (features 0 and 1) lives in the CONTEXT itself, the features from 2 up in the XSTATE area behind the XCONTEXT_EX.
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

#if defined(_M_IX86) || defined(_M_AMD64)

#include <reactos/xcontext.h>

#define XSTATE_CONTEXT_FLAG 0x40UL

#if defined(_M_AMD64)
#define K32_CONTEXT_ARCH            CONTEXT_AMD64
#define K32_CONTEXT_LEGACY_FLOAT    CONTEXT_FLOATING_POINT
#define K32_XMM_COUNT               16
#else
#define K32_CONTEXT_ARCH            CONTEXT_i386
#define K32_CONTEXT_LEGACY_FLOAT    CONTEXT_EXTENDED_REGISTERS
#define K32_XMM_COUNT               8
#endif

static
BOOL
K32SetNtStatus(
    _In_ NTSTATUS Status)
{
    if (Status != STATUS_SUCCESS)
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    return TRUE;
}

DWORD64
WINAPI
GetEnabledXStateFeatures(VOID)
{
    return RtlGetEnabledExtendedFeatures(~0ULL);
}

/*
 * ContextLength is the size of Buffer, and returns the size the context needs. Without extended state on the system,
 * CONTEXT_XSTATE is dropped from the flags. With a Buffer that is NULL or too small
 * the call fails with ERROR_INSUFFICIENT_BUFFER and *Context is not touched.
 */
BOOL
WINAPI
InitializeContext(
    _Out_writes_bytes_opt_(*ContextLength) PVOID Buffer,
    _In_ DWORD ContextFlags,
    _Out_opt_ PCONTEXT *Context,
    _Inout_ PDWORD ContextLength)
{
    PXCONTEXT_EX ContextEx;
    NTSTATUS Status;
    ULONG Needed;
    DWORD Available;

    if (!ContextLength)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Available = *ContextLength;
    Status = RtlGetExtendedContextLength(ContextFlags, &Needed);
    if (Status == STATUS_NOT_SUPPORTED && (ContextFlags & XSTATE_CONTEXT_FLAG))
    {
        /* A system without extended state makes a context without it, as Windows does */
        ContextFlags &= ~XSTATE_CONTEXT_FLAG;
        Status = RtlGetExtendedContextLength(ContextFlags, &Needed);
    }
    if (!K32SetNtStatus(Status))
        return FALSE;
    *ContextLength = Needed;

    if (!Buffer || Available < Needed)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (!Context)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!K32SetNtStatus(RtlInitializeExtendedContext(Buffer, ContextFlags, &ContextEx)))
        return FALSE;

    *Context = (PCONTEXT)((PUCHAR)ContextEx + ContextEx->Legacy.Offset);
    return TRUE;
}

/*
 * InitializeContext with the XSAVE compaction mask of the context given (Windows 10): a compacted XSTATE area then
 * has room for those features only. A system without extended state drops CONTEXT_XSTATE as InitializeContext does.
 */
BOOL
WINAPI
InitializeContext2(
    _Out_writes_bytes_opt_(*ContextLength) PVOID Buffer,
    _In_ DWORD ContextFlags,
    _Out_opt_ PCONTEXT *Context,
    _Inout_ PDWORD ContextLength,
    _In_ ULONG64 XStateCompactionMask)
{
    PXCONTEXT_EX ContextEx;
    NTSTATUS Status;
    ULONG Needed;
    DWORD Available;

    if (!ContextLength)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Available = *ContextLength;
    Status = RtlGetExtendedContextLength2(ContextFlags, &Needed, XStateCompactionMask);
    if (Status == STATUS_NOT_SUPPORTED && (ContextFlags & XSTATE_CONTEXT_FLAG))
    {
        ContextFlags &= ~XSTATE_CONTEXT_FLAG;
        Status = RtlGetExtendedContextLength2(ContextFlags, &Needed, XStateCompactionMask);
    }
    if (!K32SetNtStatus(Status))
        return FALSE;
    *ContextLength = Needed;

    if (!Buffer || Available < Needed)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (!Context)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!K32SetNtStatus(RtlInitializeExtendedContext2(Buffer, ContextFlags, &ContextEx, XStateCompactionMask)))
        return FALSE;

    *Context = (PCONTEXT)((PUCHAR)ContextEx + ContextEx->Legacy.Offset);
    return TRUE;
}

BOOL
WINAPI
CopyContext(
    _Inout_ PCONTEXT Destination,
    _In_ DWORD ContextFlags,
    _In_ PCONTEXT Source)
{
    return K32SetNtStatus(RtlCopyContext(Destination, ContextFlags, Source));
}

/* Feature 0 is the x87 state and the control part of the SSE state, feature 1 the XMM registers */
PVOID
WINAPI
LocateXStateFeature(
    _In_ PCONTEXT Context,
    _In_ DWORD FeatureId,
    _Out_opt_ PDWORD Length)
{
    if (!Context || (Context->ContextFlags & K32_CONTEXT_ARCH) != K32_CONTEXT_ARCH)
        return NULL;

    if (FeatureId >= 2)
    {
        if ((Context->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE)
            return NULL;
        return RtlLocateExtendedFeature((PXCONTEXT_EX)(Context + 1), FeatureId, Length);
    }

#if defined(_M_AMD64)
    PUCHAR Area = (PUCHAR)&Context->FltSave;
#else
    PUCHAR Area = (PUCHAR)&Context->ExtendedRegisters;
#endif

    if (FeatureId == 1)
    {
        if (Length)
            *Length = sizeof(M128A) * K32_XMM_COUNT;
        return Area + FIELD_OFFSET(XSAVE_FORMAT, XmmRegisters);
    }

    if (Length)
        *Length = FIELD_OFFSET(XSAVE_FORMAT, XmmRegisters);
    return Area;
}

/* The x87 and SSE bits come from the context flags, the others from the XSTATE header (Windows 10) */
BOOL
WINAPI
GetXStateFeaturesMask(
    _In_ PCONTEXT Context,
    _Out_ PDWORD64 FeatureMask)
{
    if (!Context || !FeatureMask || (Context->ContextFlags & K32_CONTEXT_ARCH) != K32_CONTEXT_ARCH)
        return FALSE;

    *FeatureMask = ((Context->ContextFlags & K32_CONTEXT_LEGACY_FLOAT) == K32_CONTEXT_LEGACY_FLOAT) ? 3 : 0;
    if ((Context->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE)
        *FeatureMask |= RtlGetExtendedFeaturesMask((PXCONTEXT_EX)(Context + 1));
    return TRUE;
}

/* The x87 and SSE bits go to the context flags, the others to the XSTATE header; they need CONTEXT_XSTATE */
BOOL
WINAPI
SetXStateFeaturesMask(
    _Inout_ PCONTEXT Context,
    _In_ DWORD64 FeatureMask)
{
    if (!Context || (Context->ContextFlags & K32_CONTEXT_ARCH) != K32_CONTEXT_ARCH)
        return FALSE;

    if (FeatureMask & 0x3)
        Context->ContextFlags |= K32_CONTEXT_LEGACY_FLOAT;

    if ((Context->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE)
        return !(FeatureMask & ~3ULL);

    RtlSetExtendedFeaturesMask((PXCONTEXT_EX)(Context + 1), FeatureMask);
    return TRUE;
}

#endif /* _M_IX86 || _M_AMD64 */
