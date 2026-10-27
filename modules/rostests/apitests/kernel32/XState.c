/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for InitializeContext, CopyContext, the XState functions and the ideal processor
 *              functions with PROCESSOR_NUMBER (all Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The CONTEXT_EX types are declared here because the headers show them for Windows 7 only.
 */

#include "precomp.h"

typedef DWORD64 (WINAPI *PFN_GetEnabledXStateFeatures)(void);
typedef BOOL (WINAPI *PFN_InitializeContext)(PVOID, DWORD, PCONTEXT *, PDWORD);
typedef BOOL (WINAPI *PFN_CopyContext)(PCONTEXT, DWORD, PCONTEXT);
typedef PVOID (WINAPI *PFN_LocateXStateFeature)(PCONTEXT, DWORD, PDWORD);
typedef BOOL (WINAPI *PFN_SetXStateFeaturesMask)(PCONTEXT, DWORD64);
typedef BOOL (WINAPI *PFN_GetThreadIdealProcessorEx)(HANDLE, PPROCESSOR_NUMBER);
typedef BOOL (WINAPI *PFN_SetThreadIdealProcessorEx)(HANDLE, PPROCESSOR_NUMBER, PPROCESSOR_NUMBER);

static PFN_GetEnabledXStateFeatures pGetEnabledXStateFeatures;
static PFN_InitializeContext pInitializeContext;
static PFN_CopyContext pCopyContext;
static PFN_LocateXStateFeature pLocateXStateFeature;
static PFN_SetXStateFeaturesMask pSetXStateFeaturesMask;
static PFN_GetThreadIdealProcessorEx pGetThreadIdealProcessorEx;
static PFN_SetThreadIdealProcessorEx pSetThreadIdealProcessorEx;

/* ntdll, for the contexts with a restricted compaction mask */
typedef LONG (WINAPI *PFN_RtlGetExtendedContextLength2)(ULONG, PULONG, ULONG64);
typedef LONG (WINAPI *PFN_RtlInitializeExtendedContext2)(PVOID, ULONG, PVOID *, ULONG64);
typedef PVOID (WINAPI *PFN_RtlLocateExtendedFeature)(PVOID, ULONG, PULONG);
static PFN_RtlGetExtendedContextLength2 pRtlGetExtendedContextLength2;
static PFN_RtlInitializeExtendedContext2 pRtlInitializeExtendedContext2;
static PFN_RtlLocateExtendedFeature pRtlLocateExtendedFeature;

/* what the system enables; 0 when it has no extended state, then CONTEXT_XSTATE is dropped by InitializeContext */
static DWORD64 g_Enabled;

#if defined(_M_IX86) || defined(_M_AMD64)

typedef struct _XS_CHUNK
{
    LONG Offset;
    ULONG Length;
} XS_CHUNK;

typedef struct _XS_CONTEXT_EX
{
    XS_CHUNK All;
    XS_CHUNK Legacy;
    XS_CHUNK XState;
} XS_CONTEXT_EX;

typedef struct _XS_HEADER
{
    ULONG64 Mask;
    ULONG64 CompactionMask;
    ULONG64 Reserved[6];
} XS_HEADER;

#define XS_XSTATE_BIT 0x40UL
#define XS_FEATURES 64
#define XS_HEADER_SIZE 64
#define XS_LEGACY_SSE_OFFSET 160

#if defined(_M_AMD64)
#define XS_ARCH 0x100000UL
#define XS_FLOAT_FLAG 0x8UL     /* CONTEXT_FLOATING_POINT */
#define XS_CONTEXT_ALIGN 16
#define XS_XMM_BYTES 256
#define XS_AX Rax
#define XS_BX Rbx
#define XS_IP Rip
#else
#define XS_ARCH 0x10000UL
#define XS_FLOAT_FLAG 0x20UL    /* CONTEXT_EXTENDED_REGISTERS */
#define XS_CONTEXT_ALIGN 4
#define XS_XMM_BYTES 128
#define XS_AX Eax
#define XS_BX Ebx
#define XS_IP Eip
#endif

#define XS_CONTROL (XS_ARCH | 0x1UL)
#define XS_INTEGER (XS_ARCH | 0x2UL)
#define XS_XSTATE (XS_ARCH | XS_XSTATE_BIT)
#define XS_FULL (XS_ARCH | 0x1UL | 0x2UL | 0x8UL)
#define XS_ALL_TESTED (XS_FULL | XS_ARCH | XS_FLOAT_FLAG | XS_XSTATE_BIT)

#define XS_COMPACTED (1ULL << 63)

static
void
Cpuid(unsigned Leaf, unsigned Subleaf, unsigned Regs[4])
{
#if defined(__GNUC__)
    __asm__ __volatile__("cpuid" : "=a"(Regs[0]), "=b"(Regs[1]), "=c"(Regs[2]), "=d"(Regs[3]) : "a"(Leaf), "c"(Subleaf));
#else
    __cpuidex((int *)Regs, Leaf, Subleaf);
#endif
}

typedef struct _XS_BUFFER
{
    PBYTE Base;
    PBYTE Start;
    DWORD Length;
    PCONTEXT Context;
    XS_CONTEXT_EX *Ex;
} XS_BUFFER;

#define GUARD_BYTES 32

/* A buffer of exactly the length InitializeContext asks for, starting Shift bytes after an allocation boundary */
static
BOOL
CreateContext(XS_BUFFER *Buffer, DWORD Flags, ULONG Shift)
{
    DWORD Length = 0;
    BOOL Result;

    ZeroMemory(Buffer, sizeof(*Buffer));
    Result = pInitializeContext(NULL, Flags, NULL, &Length);
    if (Result || GetLastError() != ERROR_INSUFFICIENT_BUFFER || Length == 0)
        return FALSE;

    Buffer->Base = HeapAlloc(GetProcessHeap(), 0, Length + Shift + GUARD_BYTES);
    if (!Buffer->Base)
        return FALSE;
    memset(Buffer->Base, 0xCC, Length + Shift + GUARD_BYTES);
    Buffer->Start = Buffer->Base + Shift;
    Buffer->Length = Length;
    Result = pInitializeContext(Buffer->Start, Flags, &Buffer->Context, &Length);
    if (!Result)
        return FALSE;
    Buffer->Ex = (XS_CONTEXT_EX *)(Buffer->Context + 1);
    return TRUE;
}

static
void
FreeContext(XS_BUFFER *Buffer)
{
    ULONG i;
    BOOL Intact = TRUE;

    if (!Buffer->Base)
        return;
    for (i = 0; i < GUARD_BYTES; i++)
        Intact &= (Buffer->Start[Buffer->Length + i] == 0xCC);
    ok(Intact, "InitializeContext wrote past the length it asked for\n");
    HeapFree(GetProcessHeap(), 0, Buffer->Base);
    ZeroMemory(Buffer, sizeof(*Buffer));
}

static
XS_HEADER *
HeaderOf(const XS_BUFFER *Buffer)
{
    return (XS_HEADER *)((PBYTE)Buffer->Ex + Buffer->Ex->XState.Offset);
}

static
void
test_GetEnabledXStateFeatures(void)
{
    DWORD64 Features = pGetEnabledXStateFeatures();

    trace("enabled XState features: 0x%I64x\n", Features);
    /* A system without extended state has no mask at all */
    ok(Features == 0 || (Features & 3) == 3, "x87 and SSE are missing in 0x%I64x\n", Features);
#if defined(_M_AMD64)
    ok(Features != 0, "no feature is enabled\n");
#endif
    ok(pGetEnabledXStateFeatures() == Features, "the mask is not stable\n");
    /* feature 63 is the compaction bit, never a feature */
    ok(!(Features & (1ULL << 63)), "bit 63 is set in 0x%I64x\n", Features);
}

static
void
test_InitializeContext_errors(void)
{
    PCONTEXT Context = (PCONTEXT)(ULONG_PTR)0x1234;
    DWORD Length, Plain, Extended;
    BYTE Small[16];
    BOOL Result;

    SetLastError(0xdeadbeef);
    Result = pInitializeContext(NULL, XS_FULL, &Context, NULL);
    ok(!Result, "no length succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    Length = 0;
    SetLastError(0xdeadbeef);
    Result = pInitializeContext(NULL, XS_FULL, &Context, &Length);
    ok(!Result, "no buffer succeeded\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got error %lu\n", GetLastError());
    ok(Length >= sizeof(CONTEXT) + sizeof(XS_CONTEXT_EX), "length is only %lu\n", Length);
    ok(Context == (PCONTEXT)(ULONG_PTR)0x1234, "the context pointer was written\n");
    Plain = Length;

    Length = sizeof(Small);
    SetLastError(0xdeadbeef);
    Result = pInitializeContext(Small, XS_FULL, &Context, &Length);
    ok(!Result, "small buffer succeeded\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got error %lu\n", GetLastError());
    ok(Length == Plain, "length %lu, expected %lu\n", Length, Plain);

    Length = 0;
    SetLastError(0xdeadbeef);
    Result = pInitializeContext(NULL, XS_FULL | XS_XSTATE_BIT, &Context, &Length);
    ok(!Result, "no buffer succeeded\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got error %lu\n", GetLastError());
    Extended = Length;
    if (g_Enabled)
        ok(Extended >= Plain + XS_HEADER_SIZE, "extended length %lu, plain %lu\n", Extended, Plain);
    else
        ok(Extended == Plain, "extended length %lu, plain %lu\n", Extended, Plain);

    /* No architecture in the flags, then bits no context has */
    Length = 0;
    SetLastError(0xdeadbeef);
    Result = pInitializeContext(NULL, 0x7, &Context, &Length);
    ok(!Result, "flags without architecture succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    Length = 0;
    SetLastError(0xdeadbeef);
    Result = pInitializeContext(NULL, XS_FULL | 0x00400000UL, &Context, &Length);
    ok(!Result, "flags with unknown bits succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
}

static
void
test_InitializeContext_layout(void)
{
    static const DWORD FlagSets[] = { XS_CONTROL, XS_FULL, XS_ALL_TESTED, XS_FULL | XS_XSTATE_BIT };
    ULONG Set, Shift;

    for (Set = 0; Set < ARRAYSIZE(FlagSets); Set++)
    {
        DWORD Flags = FlagSets[Set];
        DWORD Asked = Flags;

        for (Shift = 0; Shift < 16; Shift++)
        {
            XS_BUFFER Buffer;
            PBYTE End;

            if (!CreateContext(&Buffer, Asked, Shift))
            {
                ok(0, "no context for 0x%lx at shift %lu, error %lu\n", Asked, Shift, GetLastError());
                FreeContext(&Buffer);
                continue;
            }
            End = Buffer.Start + Buffer.Length;

            ok((PBYTE)Buffer.Context >= Buffer.Start, "context before the buffer\n");
            ok(((ULONG_PTR)Buffer.Context % XS_CONTEXT_ALIGN) == 0, "context %p is not aligned\n", Buffer.Context);
            Flags = g_Enabled ? Asked : (Asked & ~XS_XSTATE_BIT);
            ok(Buffer.Context->ContextFlags == Flags, "flags 0x%lx, expected 0x%lx\n",
               Buffer.Context->ContextFlags, Flags);
            ok(Buffer.Ex->Legacy.Offset == -(LONG)sizeof(CONTEXT), "legacy offset %ld\n", Buffer.Ex->Legacy.Offset);
#if defined(_M_IX86)
            /* the extended registers belong to the legacy chunk only when they are asked for */
            ok(Buffer.Ex->Legacy.Length == ((Flags & 0x20) ? sizeof(CONTEXT) : 0xcc), "legacy length %lu\n",
               Buffer.Ex->Legacy.Length);
#else
            ok(Buffer.Ex->Legacy.Length == sizeof(CONTEXT), "legacy length %lu\n", Buffer.Ex->Legacy.Length);
#endif
            ok(Buffer.Ex->All.Offset == -(LONG)sizeof(CONTEXT), "all offset %ld\n", Buffer.Ex->All.Offset);
            ok((PBYTE)(Buffer.Ex + 1) <= End, "the CONTEXT_EX is out of the buffer\n");

            if (!(Flags & XS_XSTATE_BIT))
            {
                ok(Buffer.Ex->XState.Length == 0, "xstate length %lu\n", Buffer.Ex->XState.Length);
                ok(Buffer.Ex->All.Length == sizeof(CONTEXT) + sizeof(XS_CONTEXT_EX),
                   "all length %lu\n", Buffer.Ex->All.Length);
            }
            else
            {
                XS_HEADER *Header = HeaderOf(&Buffer);

                ok(Buffer.Ex->XState.Offset >= (LONG)sizeof(XS_CONTEXT_EX), "xstate offset %ld\n",
                   Buffer.Ex->XState.Offset);
                ok(Buffer.Ex->XState.Length >= XS_HEADER_SIZE, "xstate length %lu\n", Buffer.Ex->XState.Length);
                ok(((ULONG_PTR)Header % 64) == 0, "header %p is not aligned\n", Header);
                ok((PBYTE)Header + Buffer.Ex->XState.Length <= End, "the XSTATE area is out of the buffer\n");
                ok(Buffer.Ex->All.Length == sizeof(CONTEXT) + Buffer.Ex->XState.Offset + Buffer.Ex->XState.Length,
                   "all length %lu\n", Buffer.Ex->All.Length);
                ok(Header->Mask == 0, "mask 0x%I64x\n", Header->Mask);
            }
            FreeContext(&Buffer);
        }
    }
}

static
BOOL
HasXState(const XS_BUFFER *Buffer)
{
    return (Buffer->Context->ContextFlags & XS_XSTATE_BIT) != 0;
}

typedef struct _XS_CPU
{
    unsigned Size[XS_FEATURES];
    unsigned Offset[XS_FEATURES];   /* standard format, from the start of the XSAVE area */
    BOOL Aligned[XS_FEATURES];      /* 64 byte aligned in the compacted format */
} XS_CPU;

/* CPUID leaf 0xD describes the features: size in EAX, standard offset in EBX, alignment in bit 1 of ECX */
static
void
ReadCpu(XS_CPU *Cpu)
{
    unsigned Regs[4], i;

    ZeroMemory(Cpu, sizeof(*Cpu));
    for (i = 2; i < 63; i++)
    {
        Cpuid(0xd, i, Regs);
        Cpu->Size[i] = Regs[0];
        Cpu->Offset[i] = Regs[1];
        Cpu->Aligned[i] = (Regs[2] >> 1) & 1;
    }
}

/*
 * Offset from the header of feature Id in the compacted format for Mask: sizes of the features before it that the
 * mask has, and after every feature the next one is aligned when it asks for it, in the mask or not.
 */
static
ULONG
CompactedOffset(const XS_CPU *Cpu, ULONG64 Mask, ULONG Id)
{
    ULONG Offset = XS_HEADER_SIZE, i;

    for (i = 2; i < Id; i++)
    {
        if (Mask & (1ULL << i))
            Offset += Cpu->Size[i];
        if (Cpu->Aligned[i + 1])
            Offset = (Offset + 63) & ~63UL;
    }
    return Offset;
}

static
ULONG
HighestFeature(ULONG64 Mask)
{
    ULONG i, Highest = 0;

    for (i = 2; i < XS_FEATURES; i++)
        if (Mask & (1ULL << i))
            Highest = i;
    return Highest;
}

/* A context made by the ntdll functions with a compaction mask of its own */
static
BOOL
CreateRestricted(XS_BUFFER *Buffer, DWORD Flags, ULONG64 Mask)
{
    ULONG Length = 0;
    PVOID Ex;

    ZeroMemory(Buffer, sizeof(*Buffer));
    if (pRtlGetExtendedContextLength2(Flags, &Length, Mask) != 0)
        return FALSE;
    Buffer->Base = HeapAlloc(GetProcessHeap(), 0, Length + GUARD_BYTES);
    if (!Buffer->Base)
        return FALSE;
    memset(Buffer->Base, 0xCC, Length + GUARD_BYTES);
    Buffer->Start = Buffer->Base;
    Buffer->Length = Length;
    if (pRtlInitializeExtendedContext2(Buffer->Start, Flags, &Ex, Mask) != 0)
        return FALSE;
    Buffer->Ex = Ex;
    Buffer->Context = (PCONTEXT)((PBYTE)Ex + Buffer->Ex->Legacy.Offset);
    return TRUE;
}

/*
 * The feature offsets, from numbers that do not come from the code under test: AVX is at 576 of the XSAVE area
 * (64 behind the header) in both formats, and CPUID leaf 0xD gives the rest.
 */
static
void
test_FeatureLayout(void)
{
    XS_BUFFER Buffer;
    XS_CPU Cpu;
    BOOL Compacted;
    ULONG i, Length, Top;
    PBYTE Header;
    PVOID Feature;

    if (!g_Enabled)
    {
        skip("The system has no extended state\n");
        return;
    }
    if (!CreateContext(&Buffer, XS_FULL | XS_XSTATE_BIT, 0))
    {
        ok(0, "no extended context\n");
        return;
    }
    Header = (PBYTE)HeaderOf(&Buffer);
    Compacted = (HeaderOf(&Buffer)->CompactionMask & XS_COMPACTED) != 0;
    trace("the XSAVE area is %s, %lu bytes behind the header\n", Compacted ? "compacted" : "standard",
          Buffer.Ex->XState.Length);
    if (Compacted)
        ok(HeaderOf(&Buffer)->CompactionMask == (XS_COMPACTED | g_Enabled), "compaction mask 0x%I64x\n",
           HeaderOf(&Buffer)->CompactionMask);
    else
        ok(HeaderOf(&Buffer)->CompactionMask == 0, "compaction mask 0x%I64x\n", HeaderOf(&Buffer)->CompactionMask);

    if (g_Enabled & 4)
    {
        Length = 0;
        Feature = pLocateXStateFeature(Buffer.Context, 2, &Length);
        ok(Feature == Header + 64, "AVX at %p, expected %p\n", Feature, Header + 64);
        ok(Length == 256, "AVX is %lu bytes\n", Length);
    }

    ReadCpu(&Cpu);
    if (g_Enabled & 4)
        ok(Cpu.Offset[2] == 576 && Cpu.Size[2] == 256, "CPUID has AVX at %u, %u bytes\n", Cpu.Offset[2], Cpu.Size[2]);

    for (i = 2; i < XS_FEATURES; i++)
    {
        ULONG Expected;

        if (!(g_Enabled & (1ULL << i)))
            continue;
        Expected = Compacted ? CompactedOffset(&Cpu, g_Enabled, i) : Cpu.Offset[i] - 512;
        Length = 0;
        Feature = pLocateXStateFeature(Buffer.Context, i, &Length);
        ok(Feature == Header + Expected, "feature %lu at %p, expected %p\n", i, Feature, Header + Expected);
        ok(Length == Cpu.Size[i], "feature %lu is %lu bytes, CPUID says %u\n", i, Length, Cpu.Size[i]);
    }

    Top = HighestFeature(g_Enabled);
    if (Top)
    {
        Length = (Compacted ? CompactedOffset(&Cpu, g_Enabled, Top) : Cpu.Offset[Top] - 512) + Cpu.Size[Top];
        ok(Buffer.Ex->XState.Length == Length, "chunk is %lu bytes, expected %lu\n", Buffer.Ex->XState.Length, Length);
    }
    /* x87, SSE and AVX only: 64 header bytes and 256 of AVX */
    if (g_Enabled == 7)
        ok(Buffer.Ex->XState.Length == 0x140, "chunk is %lu bytes\n", Buffer.Ex->XState.Length);

    FreeContext(&Buffer);
}

/* Contexts for a part of the features: shorter, and in the compacted format the features move up */
static
void
test_RestrictedMask(void)
{
    ULONG64 Masks[3], Mask;
    XS_BUFFER Buffer;
    XS_CPU Cpu;
    ULONG Set, i, Length, Top, Full = 0;
    BOOL Compacted;
    PBYTE Header;
    PVOID Feature;

    if (!g_Enabled || !pRtlGetExtendedContextLength2 || !pRtlInitializeExtendedContext2)
    {
        skip("No extended state, or no Rtl functions\n");
        return;
    }
    pRtlGetExtendedContextLength2(XS_FULL | XS_XSTATE_BIT, &Full, ~0ULL);
    ReadCpu(&Cpu);

    Masks[0] = 0;                       /* nothing: the header alone */
    Masks[1] = g_Enabled & 4;           /* AVX alone */
    Masks[2] = g_Enabled & ~7ULL;       /* everything but x87, SSE and AVX */

    for (Set = 0; Set < 3; Set++)
    {
        Mask = Masks[Set];
        if (Set > 0 && !Mask)
            continue;
        if (!CreateRestricted(&Buffer, XS_FULL | XS_XSTATE_BIT, Mask))
        {
            ok(0, "no context for mask 0x%I64x\n", Mask);
            FreeContext(&Buffer);
            continue;
        }
        Header = (PBYTE)HeaderOf(&Buffer);
        Compacted = (HeaderOf(&Buffer)->CompactionMask & XS_COMPACTED) != 0;
        Top = HighestFeature(Mask);

        if (Compacted)
            ok(HeaderOf(&Buffer)->CompactionMask == (XS_COMPACTED | Mask), "mask 0x%I64x: compaction mask 0x%I64x\n",
               Mask, HeaderOf(&Buffer)->CompactionMask);
        else
            ok(HeaderOf(&Buffer)->CompactionMask == 0, "mask 0x%I64x: compaction mask 0x%I64x\n",
               Mask, HeaderOf(&Buffer)->CompactionMask);

        if (!Top)
            Length = XS_HEADER_SIZE;
        else
            Length = (Compacted ? CompactedOffset(&Cpu, Mask, Top) : Cpu.Offset[Top] - 512) + Cpu.Size[Top];
        ok(Buffer.Ex->XState.Length == Length, "mask 0x%I64x: chunk is %lu bytes, expected %lu\n",
           Mask, Buffer.Ex->XState.Length, Length);
        if (Mask == 4)
            ok(Buffer.Ex->XState.Length == 0x140, "AVX alone: chunk is %lu bytes\n", Buffer.Ex->XState.Length);
        ok(Buffer.Length <= Full, "mask 0x%I64x: %lu bytes, the full context needs %lu\n", Mask, Buffer.Length, Full);

        for (i = 2; i < XS_FEATURES; i++)
        {
            ULONG Expected;

            if (!(g_Enabled & (1ULL << i)))
                continue;
            Feature = pRtlLocateExtendedFeature(Buffer.Ex, i, &Length);
            if (Compacted)
            {
                if (!(Mask & (1ULL << i)))
                {
                    ok(Feature == NULL, "mask 0x%I64x: feature %lu is outside of the mask, found at %p\n", Mask, i, Feature);
                    continue;
                }
                Expected = CompactedOffset(&Cpu, Mask, i);
            }
            else
            {
                /* the standard format keeps its places; what lies behind the chunk is not there */
                Expected = Cpu.Offset[i] - 512;
                if (Expected + Cpu.Size[i] > Buffer.Ex->XState.Length)
                {
                    ok(Feature == NULL, "mask 0x%I64x: feature %lu beyond the chunk, found at %p\n", Mask, i, Feature);
                    continue;
                }
            }
            ok(Feature == Header + Expected, "mask 0x%I64x: feature %lu at %p, expected %p\n",
               Mask, i, Feature, Header + Expected);
        }
        FreeContext(&Buffer);
    }
}

static
void
test_LocateXStateFeature(void)
{
    XS_BUFFER Buffer, Plain;
    PBYTE Area, Previous[XS_FEATURES];
    DWORD Length, PreviousLength[XS_FEATURES];
    ULONG i, j;
    PVOID Feature;

    ok(CreateContext(&Buffer, XS_ALL_TESTED, 3), "no extended context\n");
    ok(CreateContext(&Plain, XS_ALL_TESTED & ~XS_XSTATE_BIT, 5), "no plain context\n");
    if (!Buffer.Base || !Plain.Base)
        goto Done;

#if defined(_M_AMD64)
    Area = (PBYTE)&Buffer.Context->FltSave;
#else
    Area = (PBYTE)&Buffer.Context->ExtendedRegisters;
#endif

    Length = 0;
    Feature = pLocateXStateFeature(Buffer.Context, 0, &Length);
    ok(Feature == Area, "feature 0 at %p, expected %p\n", Feature, Area);
    ok(Length == XS_LEGACY_SSE_OFFSET, "feature 0 length %lu\n", Length);
    Length = 0;
    Feature = pLocateXStateFeature(Buffer.Context, 1, &Length);
    ok(Feature == Area + XS_LEGACY_SSE_OFFSET, "feature 1 at %p\n", Feature);
    ok(Length == XS_XMM_BYTES, "feature 1 length %lu\n", Length);
    ok(pLocateXStateFeature(Buffer.Context, 1, NULL) != NULL, "feature 1 without length\n");
    ok(pLocateXStateFeature(Plain.Context, 0, &Length) != NULL, "feature 0 of a plain context\n");

    ok(pLocateXStateFeature(NULL, 0, &Length) == NULL, "NULL context\n");
    ok(pLocateXStateFeature(Buffer.Context, XS_FEATURES, &Length) == NULL, "feature 64\n");
    ok(pLocateXStateFeature(Buffer.Context, 0xffffffff, &Length) == NULL, "feature -1\n");

    for (i = 2; i < XS_FEATURES; i++)
    {
        Previous[i] = NULL;
        Length = 0;
        Feature = pLocateXStateFeature(Buffer.Context, i, &Length);
        ok(pLocateXStateFeature(Plain.Context, i, &Length) == NULL, "feature %lu of a plain context\n", i);
        if (!HasXState(&Buffer) || !(g_Enabled & (1ULL << i)))
        {
            ok(Feature == NULL, "disabled feature %lu found at %p\n", i, Feature);
            continue;
        }
        {
            PBYTE Header = (PBYTE)HeaderOf(&Buffer);

            ok(Feature != NULL, "enabled feature %lu not found\n", i);
            if (!Feature)
                continue;
            ok(Length != 0, "feature %lu has no length\n", i);
            ok((PBYTE)Feature >= Header + XS_HEADER_SIZE &&
               (PBYTE)Feature + Length <= Header + Buffer.Ex->XState.Length,
               "feature %lu [%p, +%lu) is outside of the area at %p, %lu bytes\n",
               i, Feature, Length, Header, Buffer.Ex->XState.Length);
        }
        for (j = 2; j < i; j++)
        {
            if (!Previous[j])
                continue;
            ok((PBYTE)Feature >= Previous[j] + PreviousLength[j] || Previous[j] >= (PBYTE)Feature + Length,
               "features %lu and %lu overlap\n", j, i);
        }
        Previous[i] = Feature;
        PreviousLength[i] = Length;
    }

Done:
    FreeContext(&Buffer);
    FreeContext(&Plain);
}

static
void
test_SetXStateFeaturesMask(void)
{
    DWORD64 Enabled = g_Enabled & ~3ULL;
    XS_BUFFER Buffer, Plain;
    BOOL Result;

    ok(CreateContext(&Buffer, XS_CONTROL | XS_XSTATE_BIT, 0), "no extended context\n");
    ok(CreateContext(&Plain, XS_CONTROL, 0), "no plain context\n");
    if (!Buffer.Base || !Plain.Base)
        goto Done;

    ok(!(Buffer.Context->ContextFlags & XS_FLOAT_FLAG), "the context has the floating point flag already\n");
    Result = pSetXStateFeaturesMask(Buffer.Context, 3);
    ok(Result, "setting the legacy bits failed\n");
    ok((Buffer.Context->ContextFlags & XS_FLOAT_FLAG) == XS_FLOAT_FLAG,
       "legacy bits did not set the flag, flags 0x%lx\n", Buffer.Context->ContextFlags);

    if (HasXState(&Buffer))
    {
        /* the legacy bits are not stored in the header */
        ok(HeaderOf(&Buffer)->Mask == 0, "mask 0x%I64x\n", HeaderOf(&Buffer)->Mask);

        Result = pSetXStateFeaturesMask(Buffer.Context, Enabled | 3);
        ok(Result, "setting 0x%I64x failed\n", Enabled | 3);
        ok(HeaderOf(&Buffer)->Mask == Enabled, "mask 0x%I64x, expected 0x%I64x\n", HeaderOf(&Buffer)->Mask, Enabled);

        /* what the system does not enable is not stored: it could fault on a restore */
        Result = pSetXStateFeaturesMask(Buffer.Context, ~0ULL);
        ok(Result, "setting all bits failed\n");
        ok(HeaderOf(&Buffer)->Mask == Enabled, "mask 0x%I64x, expected 0x%I64x\n", HeaderOf(&Buffer)->Mask, Enabled);
        Result = pSetXStateFeaturesMask(Buffer.Context, 1ULL << 62);
        ok(Result, "setting bit 62 failed\n");
        ok(HeaderOf(&Buffer)->Mask == 0, "mask 0x%I64x\n", HeaderOf(&Buffer)->Mask);

        if (Enabled & 4)
        {
            Result = pSetXStateFeaturesMask(Buffer.Context, 4);
            ok(Result, "setting AVX failed\n");
            ok(HeaderOf(&Buffer)->Mask == 4, "mask 0x%I64x\n", HeaderOf(&Buffer)->Mask);
        }
        Result = pSetXStateFeaturesMask(Buffer.Context, 0);
        ok(Result, "clearing failed\n");
        ok(HeaderOf(&Buffer)->Mask == 0, "mask 0x%I64x\n", HeaderOf(&Buffer)->Mask);
    }

    /* Without extended state only the legacy bits can be set */
    Result = pSetXStateFeaturesMask(Plain.Context, 3);
    ok(Result, "setting the legacy bits of a plain context failed\n");
    ok((Plain.Context->ContextFlags & XS_FLOAT_FLAG) == XS_FLOAT_FLAG, "flags 0x%lx\n", Plain.Context->ContextFlags);
    Result = pSetXStateFeaturesMask(Plain.Context, 4);
    ok(!Result, "setting AVX of a plain context succeeded\n");

    Result = pSetXStateFeaturesMask(NULL, 3);
    ok(!Result, "NULL context succeeded\n");

Done:
    FreeContext(&Buffer);
    FreeContext(&Plain);
}

static
void
test_CopyContext(void)
{
    DWORD64 Enabled = g_Enabled & ~3ULL;
    XS_BUFFER Source, Destination, Control, Plain;
    ULONG i;
    DWORD Length, SourceLength;
    PBYTE From, To;
    BOOL Result;

    ok(CreateContext(&Source, XS_ALL_TESTED, 0), "no source\n");
    ok(CreateContext(&Destination, XS_ALL_TESTED, 7), "no destination\n");
    ok(CreateContext(&Control, XS_CONTROL, 1), "no control context\n");
    ok(CreateContext(&Plain, XS_ALL_TESTED & ~XS_XSTATE_BIT, 2), "no plain context\n");
    if (!Source.Base || !Destination.Base || !Control.Base || !Plain.Base)
        goto Done;

    Source.Context->XS_AX = 0x11111111;
    Source.Context->XS_BX = 0x22222222;
    Source.Context->XS_IP = 0x33333333;
    Destination.Context->XS_AX = 0xaaaaaaaa;
    Destination.Context->XS_BX = 0xbbbbbbbb;
    Destination.Context->XS_IP = 0xcccccccc;

    SetLastError(0xdeadbeef);
    Result = pCopyContext(Destination.Context, XS_INTEGER, Source.Context);
    ok(Result, "copy of the integer registers failed, error %lu\n", GetLastError());
    ok(Destination.Context->XS_AX == 0x11111111, "Ax is 0x%lx\n", (ULONG)Destination.Context->XS_AX);
    ok(Destination.Context->XS_BX == 0x22222222, "Bx is 0x%lx\n", (ULONG)Destination.Context->XS_BX);
    ok(Destination.Context->XS_IP == 0xcccccccc, "Ip was copied: 0x%lx\n", (ULONG)Destination.Context->XS_IP);
    ok(Destination.Context->ContextFlags == (g_Enabled ? XS_ALL_TESTED : (XS_ALL_TESTED & ~XS_XSTATE_BIT)),
       "flags 0x%lx\n", Destination.Context->ContextFlags);

    Result = pCopyContext(Destination.Context, XS_CONTROL, Source.Context);
    ok(Result, "copy of the control registers failed, error %lu\n", GetLastError());
    ok(Destination.Context->XS_IP == 0x33333333, "Ip is 0x%lx\n", (ULONG)Destination.Context->XS_IP);

    /* What the source does not have is left out, and the call succeeds */
    Destination.Context->XS_AX = 0xaaaaaaaa;
    Destination.Context->XS_IP = 0xcccccccc;
    Control.Context->XS_IP = 0x44444444;
    SetLastError(0xdeadbeef);
    Result = pCopyContext(Destination.Context, XS_FULL, Control.Context);
    ok(Result, "copy of more than the source has failed, error %lu\n", GetLastError());
    ok(Destination.Context->XS_IP == 0x44444444, "Ip is 0x%lx\n", (ULONG)Destination.Context->XS_IP);
    ok(Destination.Context->XS_AX == 0xaaaaaaaa, "Ax was copied: 0x%lx\n", (ULONG)Destination.Context->XS_AX);

    /* The flags that are copied are added to the destination */
    ok(!(Control.Context->ContextFlags & XS_INTEGER & ~XS_ARCH), "the control context has integer registers\n");
    Result = pCopyContext(Control.Context, XS_INTEGER, Source.Context);
    ok(Result, "copy into a context without integer registers failed, error %lu\n", GetLastError());
    ok((Control.Context->ContextFlags & XS_INTEGER) == XS_INTEGER, "flags 0x%lx\n", Control.Context->ContextFlags);
    ok(Control.Context->XS_AX == 0x11111111, "Ax is 0x%lx\n", (ULONG)Control.Context->XS_AX);

    SetLastError(0xdeadbeef);
    Result = pCopyContext(Destination.Context, 0x7, Source.Context);
    ok(!Result, "flags without architecture succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    ok(!pCopyContext(NULL, XS_INTEGER, Source.Context), "NULL destination succeeded\n");
    ok(!pCopyContext(Destination.Context, XS_INTEGER, NULL), "NULL source succeeded\n");

    if (!g_Enabled)
    {
        SetLastError(0xdeadbeef);
        Result = pCopyContext(Destination.Context, XS_XSTATE, Source.Context);
        ok(!Result, "copy of extended state without any succeeded\n");
        ok(GetLastError() == ERROR_NOT_SUPPORTED, "got error %lu\n", GetLastError());
    }
    else
    {
        /* A destination without room for extended state is the one case that fails for a missing part */
        SetLastError(0xdeadbeef);
        Result = pCopyContext(Plain.Context, XS_XSTATE, Source.Context);
        ok(!Result, "copy of extended state into a plain context succeeded\n");
        ok(GetLastError() == ERROR_MORE_DATA, "got error %lu\n", GetLastError());
        /* a source without it is left out */
        Result = pCopyContext(Destination.Context, XS_XSTATE, Plain.Context);
        ok(Result, "copy of extended state from a plain context failed, error %lu\n", GetLastError());
    }

    /* x87 and SSE */
    From = pLocateXStateFeature(Source.Context, 1, &Length);
    To = pLocateXStateFeature(Destination.Context, 1, &Length);
    ok(From && To, "no SSE state\n");
    if (From && To)
    {
        memset(From, 0x5a, Length);
        memset(To, 0x00, Length);
        Result = pCopyContext(Destination.Context, XS_ARCH | XS_FLOAT_FLAG, Source.Context);
        ok(Result, "copy of the floating point state failed, error %lu\n", GetLastError());
        ok(!memcmp(From, To, Length), "the SSE registers differ\n");
    }

    /* The extended features, with the mask */
    if (g_Enabled)
    {
        ok(pSetXStateFeaturesMask(Source.Context, Enabled), "no mask\n");
        for (i = 2; i < XS_FEATURES; i++)
        {
            From = pLocateXStateFeature(Source.Context, i, &SourceLength);
            if (From)
                memset(From, 0x30 + i, SourceLength);
            To = pLocateXStateFeature(Destination.Context, i, &Length);
            if (To)
                memset(To, 0xee, Length);
        }
        ok(pSetXStateFeaturesMask(Destination.Context, 0), "no mask\n");
        Result = pCopyContext(Destination.Context, XS_XSTATE, Source.Context);
        ok(Result, "copy of the extended state failed, error %lu\n", GetLastError());
        ok(HeaderOf(&Destination)->Mask == Enabled, "mask 0x%I64x, expected 0x%I64x\n",
           HeaderOf(&Destination)->Mask, Enabled);
        ok((HeaderOf(&Destination)->CompactionMask & XS_COMPACTED) == (HeaderOf(&Source)->CompactionMask & XS_COMPACTED),
           "compaction 0x%I64x and 0x%I64x\n", HeaderOf(&Destination)->CompactionMask, HeaderOf(&Source)->CompactionMask);
        for (i = 2; i < XS_FEATURES; i++)
        {
            From = pLocateXStateFeature(Source.Context, i, &SourceLength);
            To = pLocateXStateFeature(Destination.Context, i, &Length);
            ok((To != NULL) == (From != NULL), "feature %lu is in one context only\n", i);
            if (From && To)
                ok(Length == SourceLength && !memcmp(From, To, Length), "feature %lu differs\n", i);
        }
    }

Done:
    FreeContext(&Source);
    FreeContext(&Destination);
    FreeContext(&Control);
    FreeContext(&Plain);
}

#endif /* _M_IX86 || _M_AMD64 */

static
void
test_IdealProcessorEx(void)
{
    HANDLE Thread = GetCurrentThread();
    PROCESSOR_NUMBER Original, Number, Previous;
    SYSTEM_INFO Info;
    BOOL Result;

    GetSystemInfo(&Info);

    SetLastError(0xdeadbeef);
    Result = pGetThreadIdealProcessorEx(Thread, &Original);
    ok(Result, "GetThreadIdealProcessorEx failed, error %lu\n", GetLastError());
    if (!Result)
        return;
    trace("ideal processor: group %u number %u of %lu\n", Original.Group, Original.Number, Info.dwNumberOfProcessors);
    ok(Original.Group == 0, "group %u\n", Original.Group);
    ok(Original.Number < Info.dwNumberOfProcessors, "number %u\n", Original.Number);

    /* the same processor again, with and without the previous one */
    memset(&Previous, 0xaa, sizeof(Previous));
    Number = Original;
    Result = pSetThreadIdealProcessorEx(Thread, &Number, &Previous);
    ok(Result, "SetThreadIdealProcessorEx failed, error %lu\n", GetLastError());
    ok(Previous.Group == Original.Group && Previous.Number == Original.Number, "previous %u/%u\n",
       Previous.Group, Previous.Number);
    Result = pSetThreadIdealProcessorEx(Thread, &Number, NULL);
    ok(Result, "SetThreadIdealProcessorEx without previous failed, error %lu\n", GetLastError());

    /* another one: it is the next one that is read back, and the old one comes out as previous */
    if (Info.dwNumberOfProcessors > 1)
    {
        Number = Original;
        Number.Number = (Original.Number + 1) % Info.dwNumberOfProcessors;
        Result = pSetThreadIdealProcessorEx(Thread, &Number, &Previous);
        ok(Result, "setting processor %u failed, error %lu\n", Number.Number, GetLastError());
        ok(Previous.Number == Original.Number, "previous %u, expected %u\n", Previous.Number, Original.Number);
        memset(&Previous, 0xaa, sizeof(Previous));
        Result = pGetThreadIdealProcessorEx(Thread, &Previous);
        ok(Result, "GetThreadIdealProcessorEx failed, error %lu\n", GetLastError());
        ok(Previous.Group == 0 && Previous.Number == Number.Number, "got %u/%u, expected 0/%u\n",
           Previous.Group, Previous.Number, Number.Number);

        Result = pSetThreadIdealProcessorEx(Thread, &Original, &Previous);
        ok(Result, "restoring the processor failed, error %lu\n", GetLastError());
        ok(Previous.Number == Number.Number, "previous %u, expected %u\n", Previous.Number, Number.Number);
    }

    /* a group that does not exist leaves the previous one alone */
    memset(&Previous, 0xaa, sizeof(Previous));
    Number = Original;
    Number.Group = 1;
    SetLastError(0xdeadbeef);
    Result = pSetThreadIdealProcessorEx(Thread, &Number, &Previous);
    ok(!Result, "group 1 succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    ok(Previous.Group == 0xaaaa && Previous.Number == 0xaa, "previous was written\n");

    SetLastError(0xdeadbeef);
    Result = pSetThreadIdealProcessorEx(Thread, NULL, NULL);
    ok(!Result, "NULL processor succeeded\n");

    SetLastError(0xdeadbeef);
    Result = pGetThreadIdealProcessorEx((HANDLE)(ULONG_PTR)0xdeadbee0, &Number);
    ok(!Result, "bad handle succeeded\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE, "got error %lu\n", GetLastError());

    pGetThreadIdealProcessorEx(Thread, &Number);
    ok(Number.Group == Original.Group && Number.Number == Original.Number, "the processor was not restored\n");
}

START_TEST(XState)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");

    pGetEnabledXStateFeatures = (PVOID)GetProcAddress(Kernel32, "GetEnabledXStateFeatures");
    pInitializeContext = (PVOID)GetProcAddress(Kernel32, "InitializeContext");
    pCopyContext = (PVOID)GetProcAddress(Kernel32, "CopyContext");
    pLocateXStateFeature = (PVOID)GetProcAddress(Kernel32, "LocateXStateFeature");
    pSetXStateFeaturesMask = (PVOID)GetProcAddress(Kernel32, "SetXStateFeaturesMask");
    pGetThreadIdealProcessorEx = (PVOID)GetProcAddress(Kernel32, "GetThreadIdealProcessorEx");
    pSetThreadIdealProcessorEx = (PVOID)GetProcAddress(Kernel32, "SetThreadIdealProcessorEx");

    if (pGetThreadIdealProcessorEx && pSetThreadIdealProcessorEx)
        test_IdealProcessorEx();
    else
        skip("The ideal processor functions are not exported\n");

#if defined(_M_IX86) || defined(_M_AMD64)
    /* ReactOS has them from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pGetEnabledXStateFeatures || !pInitializeContext || !pCopyContext ||
        !pLocateXStateFeature || !pSetXStateFeaturesMask)
    {
        ok(!(is_reactos() && GetProcAddress(Kernel32, "K32EnumProcessModules")), "the XState functions are not all exported\n");
        skip("The XState functions are not exported\n");
        return;
    }

    {
        HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");

        pRtlGetExtendedContextLength2 = (PVOID)GetProcAddress(Ntdll, "RtlGetExtendedContextLength2");
        pRtlInitializeExtendedContext2 = (PVOID)GetProcAddress(Ntdll, "RtlInitializeExtendedContext2");
        pRtlLocateExtendedFeature = (PVOID)GetProcAddress(Ntdll, "RtlLocateExtendedFeature");
        g_Enabled = pGetEnabledXStateFeatures();
    }

    test_GetEnabledXStateFeatures();
    test_InitializeContext_errors();
    test_InitializeContext_layout();
    test_FeatureLayout();
    test_RestrictedMask();
    test_LocateXStateFeature();
    test_SetXStateFeaturesMask();
    test_CopyContext();
#endif
}
