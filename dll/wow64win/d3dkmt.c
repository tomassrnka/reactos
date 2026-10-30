/* ReactOS WoW64 D3DKMT (NtGdiDdDDI*) translation.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The native win32k has no display-driver adapter interface registered, so
 * every NtGdiDdDDI service that would reach a miniport answers
 * STATUS_PROCEDURE_NOT_FOUND. The native gdi32_vista wrappers use that status
 * to fall back to their own software adapter emulation (D3DKMTCreateDevice and
 * friends), which is how WPF and wined3d run without a GPU. The guest
 * services therefore have to hand the native service a native-layout copy of
 * the guest structure and return the native status unchanged. "Unsupported
 * service" (STATUS_NOT_IMPLEMENTED) is the wrong answer: it is not the status
 * the fallback looks for.
 *
 * The structures translated here carry pointers or handles that are wider in
 * the native layout. Structures that only hold 32-bit values are dispatched
 * from generated.inc. The remaining ones (allocation, context, lock, present,
 * render, ... ) nest pointer arrays inside pointer arrays; translating them
 * only matters when a display driver registers adapter callbacks, which the
 * native side does not do, so they report the status the native side gives
 * today instead of passing an untranslated structure to a future callback. */
#include "wow64win.h"

typedef struct { ULONG hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } OPENADAPTERFROMHDC32;
typedef struct { ULONG pDeviceName; UINT hAdapter; LUID AdapterLuid; } OPENADAPTERFROMDEVICENAME32;
typedef struct
{
    UINT hAdapter; UINT Flags; UINT hDevice;
    ULONG pCommandBuffer; UINT CommandBufferSize;
    ULONG pAllocationList; UINT AllocationListSize;
    ULONG pPatchLocationList; UINT PatchLocationListSize;
} CREATEDEVICE32;
typedef struct { UINT hDevice; ULONG pType; ULONG pVidPnSourceId; UINT VidPnSourceCount; } SETVIDPNSOURCEOWNER32;
typedef struct { UINT hAdapter; UINT Type; ULONG pPrivateDriverData; UINT PrivateDriverDataSize; } QUERYADAPTERINFO32;
typedef struct { ULONG hWindow; } CHECKOCCLUSION32;
typedef struct
{
    ULONG pMemory; UINT Format, Width, Height, Pitch;
    ULONG hDeviceDc, pColorTable, hDc, hBitmap;
} CREATEDCFROMMEMORY32;
typedef struct { ULONG hDc, hBitmap; } DESTROYDCFROMMEMORY32;

C_ASSERT(sizeof(OPENADAPTERFROMHDC32) == 20 && sizeof(D3DKMT_OPENADAPTERFROMHDC) == 24);
C_ASSERT(sizeof(OPENADAPTERFROMDEVICENAME32) == 16 && sizeof(D3DKMT_OPENADAPTERFROMDEVICENAME) == 24);
C_ASSERT(sizeof(CREATEDEVICE32) == 36 && sizeof(D3DKMT_CREATEDEVICE) == 64);
C_ASSERT(sizeof(SETVIDPNSOURCEOWNER32) == 16 && sizeof(D3DKMT_SETVIDPNSOURCEOWNER) == 32);
C_ASSERT(sizeof(QUERYADAPTERINFO32) == 16 && sizeof(D3DKMT_QUERYADAPTERINFO) == 24);
C_ASSERT(sizeof(CHECKOCCLUSION32) == 4 && sizeof(D3DKMT_CHECKOCCLUSION) == 8);
C_ASSERT(sizeof(CREATEDCFROMMEMORY32) == 36 && sizeof(D3DKMT_CREATEDCFROMMEMORY) == 56);
C_ASSERT(sizeof(DESTROYDCFROMMEMORY32) == 8 && sizeof(D3DKMT_DESTROYDCFROMMEMORY) == 16);

/* GDI handles are 32-bit values in the guest; the native handle is the same value zero-extended. */
#define G32(v) ((HANDLE)(ULONG_PTR)(ULONG)(v))
C_ASSERT(sizeof(D3DKMT_CREATEDEVICEFLAGS) == sizeof(UINT));

/* Largest element array or private-data block a guest may hand in. */
#define D3DKMT_MAX_ARRAY_BYTES 0x10000

static VOID KmtProbe(ULONG Address, ULONG Size, BOOLEAN Write)
{
    volatile BYTE *Bytes = P32(Address);
    ULONG i;

    if (Address > MAXULONG - Size) RtlRaiseStatus(STATUS_ACCESS_VIOLATION);
    for (i = 0; i < Size; i += 0x1000) { if (Write) Bytes[i] = Bytes[i]; else (VOID)Bytes[i]; }
    if (Size) { if (Write) Bytes[Size - 1] = Bytes[Size - 1]; else (VOID)Bytes[Size - 1]; }
}

/* Copies a guest structure once, so a racing guest thread cannot change a field between check and use. */
static VOID KmtCapture(PVOID Local, ULONG Address, ULONG Size, BOOLEAN Write)
{
    const volatile ULONG *Guest = P32(Address);
    ULONG i;

    KmtProbe(Address, Size, Write);
    for (i = 0; i < Size / sizeof(ULONG); ++i) ((ULONG *)Local)[i] = Guest[i];
}

static ULONG KmtHandle(HANDLE Handle)
{
    return PtrToUlong(Handle);
}

static ULONG_PTR OpenAdapterFromHdc(ULONG *a)
{
    OPENADAPTERFROMHDC32 In;
    D3DKMT_OPENADAPTERFROMHDC Native = {0};
    NTSTATUS Status;

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDIOpenAdapterFromHdc(NULL);
    KmtCapture(&In, a[0], sizeof(In), TRUE);
    Native.hDc = G32(In.hDc);
    Native.hAdapter = In.hAdapter;
    Native.AdapterLuid = In.AdapterLuid;
    Native.VidPnSourceId = In.VidPnSourceId;
    Status = NtGdiDdDDIOpenAdapterFromHdc(&Native);
    if (NT_SUCCESS(Status))
    {
        OPENADAPTERFROMHDC32 *Out = P(0);
        Out->hAdapter = Native.hAdapter;
        Out->AdapterLuid = Native.AdapterLuid;
        Out->VidPnSourceId = Native.VidPnSourceId;
    }
    return (ULONG_PTR)Status;
}

static ULONG_PTR OpenAdapterFromDeviceName(ULONG *a)
{
    OPENADAPTERFROMDEVICENAME32 In;
    D3DKMT_OPENADAPTERFROMDEVICENAME Native = {0};
    NTSTATUS Status;

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDIOpenAdapterFromDeviceName(NULL);
    KmtCapture(&In, a[0], sizeof(In), TRUE);
    Native.pDeviceName = P32(In.pDeviceName);
    Native.hAdapter = In.hAdapter;
    Native.AdapterLuid = In.AdapterLuid;
    Status = NtGdiDdDDIOpenAdapterFromDeviceName(&Native);
    if (NT_SUCCESS(Status))
    {
        OPENADAPTERFROMDEVICENAME32 *Out = P(0);
        Out->hAdapter = Native.hAdapter;
        Out->AdapterLuid = Native.AdapterLuid;
    }
    return (ULONG_PTR)Status;
}

static ULONG_PTR CreateDevice(ULONG *a)
{
    CREATEDEVICE32 In;
    D3DKMT_CREATEDEVICE Native = {{0}};
    NTSTATUS Status;

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDICreateDevice(NULL);
    KmtCapture(&In, a[0], sizeof(In), TRUE);
    /* The guest passes an adapter handle; the kernel-mode pAdapter form is not reachable from user mode. */
    Native.hAdapter = In.hAdapter;
    Native.Flags = *(D3DKMT_CREATEDEVICEFLAGS *)&In.Flags;
    Native.hDevice = In.hDevice;
    Native.pCommandBuffer = P32(In.pCommandBuffer);
    Native.CommandBufferSize = In.CommandBufferSize;
    Native.pAllocationList = P32(In.pAllocationList);
    Native.AllocationListSize = In.AllocationListSize;
    Native.pPatchLocationList = P32(In.pPatchLocationList);
    Native.PatchLocationListSize = In.PatchLocationListSize;
    Status = NtGdiDdDDICreateDevice(&Native);
    if (NT_SUCCESS(Status))
    {
        CREATEDEVICE32 *Out = P(0);
        /* A buffer above 4 GiB cannot be named by a guest. */
        if ((ULONG_PTR)Native.pCommandBuffer > MAXULONG || (ULONG_PTR)Native.pAllocationList > MAXULONG ||
            (ULONG_PTR)Native.pPatchLocationList > MAXULONG)
        {
            D3DKMT_DESTROYDEVICE Destroy = { Native.hDevice };
            NtGdiDdDDIDestroyDevice(&Destroy);
            return (ULONG_PTR)STATUS_NOT_SUPPORTED;
        }
        Out->hDevice = Native.hDevice;
        Out->pCommandBuffer = PtrToUlong(Native.pCommandBuffer);
        Out->CommandBufferSize = Native.CommandBufferSize;
        Out->pAllocationList = PtrToUlong(Native.pAllocationList);
        Out->AllocationListSize = Native.AllocationListSize;
        Out->pPatchLocationList = PtrToUlong(Native.pPatchLocationList);
        Out->PatchLocationListSize = Native.PatchLocationListSize;
    }
    return (ULONG_PTR)Status;
}

static ULONG_PTR SetVidPnSourceOwner(ULONG *a)
{
    SETVIDPNSOURCEOWNER32 In;
    D3DKMT_SETVIDPNSOURCEOWNER Native = {0};
    PVOID Types = NULL, Sources = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDISetVidPnSourceOwner(NULL);
    KmtCapture(&In, a[0], sizeof(In), FALSE);
    Native.hDevice = In.hDevice;
    Native.VidPnSourceCount = In.VidPnSourceCount;
    /* The arrays are only read, and only when the count is not zero (zero releases ownership). Both are
     * arrays of 32-bit values, so their layout is the same in the guest. */
    if (In.VidPnSourceCount)
    {
        ULONG Bytes;

        if (In.VidPnSourceCount > D3DKMT_MAX_ARRAY_BYTES / sizeof(ULONG)) return (ULONG_PTR)STATUS_INVALID_PARAMETER;
        Bytes = In.VidPnSourceCount * sizeof(ULONG);
        _SEH2_TRY
        {
            if (In.pType)
            {
                KmtProbe(In.pType, Bytes, FALSE);
                Types = RtlAllocateHeap(RtlGetProcessHeap(), 0, Bytes);
                if (!Types) RtlRaiseStatus(STATUS_NO_MEMORY);
                RtlCopyMemory(Types, P32(In.pType), Bytes);
            }
            if (In.pVidPnSourceId)
            {
                KmtProbe(In.pVidPnSourceId, Bytes, FALSE);
                Sources = RtlAllocateHeap(RtlGetProcessHeap(), 0, Bytes);
                if (!Sources) RtlRaiseStatus(STATUS_NO_MEMORY);
                RtlCopyMemory(Sources, P32(In.pVidPnSourceId), Bytes);
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }
    if (NT_SUCCESS(Status))
    {
        Native.pType = Types;
        Native.pVidPnSourceId = Sources;
        Status = NtGdiDdDDISetVidPnSourceOwner(&Native);
    }
    if (Types) RtlFreeHeap(RtlGetProcessHeap(), 0, Types);
    if (Sources) RtlFreeHeap(RtlGetProcessHeap(), 0, Sources);
    return (ULONG_PTR)Status;
}

static ULONG_PTR QueryAdapterInfo(ULONG *a)
{
    QUERYADAPTERINFO32 In;
    D3DKMT_QUERYADAPTERINFO Native = {0};
    PVOID Data = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDIQueryAdapterInfo(NULL);
    KmtCapture(&In, a[0], sizeof(In), TRUE);
    Native.hAdapter = In.hAdapter;
    Native.Type = In.Type;
    Native.PrivateDriverDataSize = In.PrivateDriverDataSize;
    if (In.pPrivateDriverData && In.PrivateDriverDataSize)
    {
        if (In.PrivateDriverDataSize > D3DKMT_MAX_ARRAY_BYTES) return (ULONG_PTR)STATUS_INVALID_PARAMETER;
        /* The data block is in and out: some query types read their request from it. Its layout is
         * per query type; the types a display driver answers are plain 32-bit data. */
        _SEH2_TRY
        {
            KmtProbe(In.pPrivateDriverData, In.PrivateDriverDataSize, TRUE);
            Data = RtlAllocateHeap(RtlGetProcessHeap(), 0, In.PrivateDriverDataSize);
            if (!Data) RtlRaiseStatus(STATUS_NO_MEMORY);
            RtlCopyMemory(Data, P32(In.pPrivateDriverData), In.PrivateDriverDataSize);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }
    if (NT_SUCCESS(Status))
    {
        Native.pPrivateDriverData = Data;
        Status = NtGdiDdDDIQueryAdapterInfo(&Native);
        if (NT_SUCCESS(Status) && Data)
        {
            _SEH2_TRY
            {
                RtlCopyMemory(P32(In.pPrivateDriverData), Data, In.PrivateDriverDataSize);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
        }
    }
    if (Data) RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
    return (ULONG_PTR)Status;
}

static ULONG_PTR CheckOcclusion(ULONG *a)
{
    CHECKOCCLUSION32 In;
    D3DKMT_CHECKOCCLUSION Native = {0};

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDICheckOcclusion(NULL);
    KmtCapture(&In, a[0], sizeof(In), FALSE);
    Native.hWindow = H32(In.hWindow);
    return (ULONG_PTR)NtGdiDdDDICheckOcclusion(&Native);
}

static ULONG_PTR CreateDCFromMemory(ULONG *a)
{
    CREATEDCFROMMEMORY32 In;
    D3DKMT_CREATEDCFROMMEMORY Native = {0};
    NTSTATUS Status;

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDICreateDCFromMemory(NULL);
    KmtCapture(&In, a[0], sizeof(In), TRUE);
    Native.pMemory = P32(In.pMemory);
    Native.Format = In.Format;
    Native.Width = In.Width;
    Native.Height = In.Height;
    Native.Pitch = In.Pitch;
    Native.hDeviceDc = G32(In.hDeviceDc);
    Native.pColorTable = P32(In.pColorTable);
    Native.hDc = G32(In.hDc);
    Native.hBitmap = G32(In.hBitmap);
    Status = NtGdiDdDDICreateDCFromMemory(&Native);
    if (NT_SUCCESS(Status))
    {
        CREATEDCFROMMEMORY32 *Out = P(0);
        Out->hDc = KmtHandle(Native.hDc);
        Out->hBitmap = KmtHandle(Native.hBitmap);
    }
    return (ULONG_PTR)Status;
}

static ULONG_PTR DestroyDCFromMemory(ULONG *a)
{
    DESTROYDCFROMMEMORY32 In;
    D3DKMT_DESTROYDCFROMMEMORY Native;

    if (!a[0]) return (ULONG_PTR)NtGdiDdDDIDestroyDCFromMemory(NULL);
    KmtCapture(&In, a[0], sizeof(In), FALSE);
    Native.hDc = G32(In.hDc);
    Native.hBitmap = G32(In.hBitmap);
    return (ULONG_PTR)NtGdiDdDDIDestroyDCFromMemory(&Native);
}

static BOOLEAN D3dkmtDispatch(ULONG Number, ULONG *a, ULONG_PTR *Result)
{
    switch (Number)
    {
    case W64_NtGdiDdDDIOpenAdapterFromHdc: *Result = OpenAdapterFromHdc(a); return TRUE;
    case W64_NtGdiDdDDIOpenAdapterFromDeviceName: *Result = OpenAdapterFromDeviceName(a); return TRUE;
    case W64_NtGdiDdDDICreateDevice: *Result = CreateDevice(a); return TRUE;
    case W64_NtGdiDdDDISetVidPnSourceOwner: *Result = SetVidPnSourceOwner(a); return TRUE;
    case W64_NtGdiDdDDIQueryAdapterInfo: *Result = QueryAdapterInfo(a); return TRUE;
    case W64_NtGdiDdDDICheckOcclusion: *Result = CheckOcclusion(a); return TRUE;
    case W64_NtGdiDdDDICreateDCFromMemory: *Result = CreateDCFromMemory(a); return TRUE;
    case W64_NtGdiDdDDIDestroyDCFromMemory: *Result = DestroyDCFromMemory(a); return TRUE;
    /* Pointer-array structures that only an adapter callback would read: see the file comment. */
    case W64_NtGdiDdDDICreateAllocation:
    case W64_NtGdiDdDDICreateContext:
    case W64_NtGdiDdDDICreateOverlay:
    case W64_NtGdiDdDDIDestroyAllocation:
    case W64_NtGdiDdDDIEscape:
    case W64_NtGdiDdDDIFlipOverlay:
    case W64_NtGdiDdDDIGetDisplayModeList:
    case W64_NtGdiDdDDIGetMultisampleMethodList:
    case W64_NtGdiDdDDIGetPresentHistory:
    case W64_NtGdiDdDDIGetRuntimeData:
    case W64_NtGdiDdDDIInvalidateActiveVidPn:
    case W64_NtGdiDdDDILock:
    case W64_NtGdiDdDDIOpenResource:
    case W64_NtGdiDdDDIPresent:
    case W64_NtGdiDdDDIQueryAllocationResidency:
    case W64_NtGdiDdDDIQueryResourceInfo:
    case W64_NtGdiDdDDIQueryStatistics:
    case W64_NtGdiDdDDIRender:
    case W64_NtGdiDdDDISetAllocationPriority:
    case W64_NtGdiDdDDISetGammaRamp:
    case W64_NtGdiDdDDIUnlock:
    case W64_NtGdiDdDDIUpdateOverlay:
        *Result = (ULONG_PTR)STATUS_PROCEDURE_NOT_FOUND;
        return TRUE;
    default:
        return FALSE;
    }
}

/* A guest pointer that faults answers with the exception status, as the native services do; the services
 * that return a plain DWORD are not in failure.inc, so the status is returned here for all of them. */
ULONG_PTR Wow64WinD3dkmtService(ULONG Number, ULONG *a)
{
    ULONG_PTR Result = 0;
    BOOLEAN Handled = FALSE;

    _SEH2_TRY
    {
        Handled = D3dkmtDispatch(Number, a, &Result);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Handled = TRUE;
        Result = (ULONG_PTR)(LONG_PTR)(NTSTATUS)_SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Handled ? Result : Wow64WinUserInputService(Number, a);
}
