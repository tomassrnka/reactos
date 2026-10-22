/* ReactOS WoW64 GDI drawing, font and device-context translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

typedef struct { LONG x, y; ULONG n, lpstr; UINT uiFlags; RECT rcl; ULONG pdx; } POLYTEXT32;
typedef struct { DWORD lStructSize; ULONG lpOutString, lpOrder, lpDx, lpCaretPos, lpClass, lpGlyphs; UINT nGlyphs, nMaxFit; } GCP32;
typedef struct { BYTE Head[200]; ULONG FamilyName, FaceName, StyleName, FullName; } OTM32;
typedef struct { LONG nXLeft, nYLeft, nWidth, nHeight; ULONG hBrush; } POLYPATBLT32;
C_ASSERT(sizeof(POLYTEXT32) == 40 && sizeof(POLYTEXTW) == 56);
C_ASSERT(sizeof(GCP32) == 36 && sizeof(GCP_RESULTSW) == 64);
C_ASSERT(sizeof(OTM32) == 216 && sizeof(OUTLINETEXTMETRICW) == 232);
C_ASSERT(offsetof(OUTLINETEXTMETRICW, otmpFamilyName) == sizeof(((OTM32 *)0)->Head));
C_ASSERT(sizeof(POLYPATBLT32) == 20 && sizeof(POLYPATBLT) == 24);
C_ASSERT(sizeof(LOGBRUSH32) == 12 && sizeof(LOGBRUSH) == 16);

/* A guest pointer plus a length must stay below 4 GiB, or native memory above it becomes reachable. */
static BOOL GuestRange(ULONG Ptr, ULONGLONG Length)
{
    return (ULONGLONG)Ptr + Length <= 0x100000000ULL;
}

static ULONG_PTR Fault(VOID)
{
    RtlSetLastWin32Error(ERROR_NOACCESS);
    return 0;
}

/* Reads one byte per page so an unmapped guest array faults before any allocation or drawing. */
static VOID GuestTouch(ULONG Ptr, ULONGLONG Length)
{
    volatile const BYTE *Bytes = P32(Ptr);
    ULONGLONG Offset;

    if (!Length) return;
    for (Offset = 0; Offset < Length; Offset += 0x1000) (VOID)Bytes[Offset];
    (VOID)Bytes[Length - 1];
}

static ULONG_PTR PolyText(ULONG *a, BOOL Console)
{
    const POLYTEXT32 *Guest = P(1);
    POLYTEXTW *Native = NULL;
    ULONG Count = a[2], i;
    ULONG_PTR Ret = 0;

    if (!GuestRange(a[1], (ULONGLONG)Count * sizeof(*Guest)) || (Console && !GuestRange(a[3], sizeof(RECTL))))
        return Fault();
    GuestTouch(a[1], (ULONGLONG)Count * sizeof(*Guest));
    if (Count)
    {
        Native = RtlAllocateHeap(RtlGetProcessHeap(), 0, (SIZE_T)Count * sizeof(*Native));
        if (!Native)
        {
            RtlSetLastWin32Error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
    }
    _SEH2_TRY
    {
        for (i = 0; i < Count; ++i)
        {
            ULONG n = Guest[i].n, Flags = Guest[i].uiFlags, Pdx = Guest[i].pdx;
            if (!GuestRange(Guest[i].lpstr, (ULONGLONG)n * sizeof(WCHAR)) ||
                !GuestRange(Pdx, (ULONGLONG)n * ((Flags & ETO_PDY) ? 8 : 4)))
                break;
            Native[i].x = Guest[i].x; Native[i].y = Guest[i].y; Native[i].n = n;
            Native[i].lpstr = P32(Guest[i].lpstr); Native[i].uiFlags = Flags;
            Native[i].rcl = Guest[i].rcl; Native[i].pdx = P32(Pdx);
        }
        if (i < Count) Ret = Fault();
        else if (Console) Ret = NtGdiConsoleTextOut(G(0), Native, Count, P(3));
        else Ret = NtGdiPolyTextOutW(G(0), Native, Count, a[3]);
    }
    _SEH2_FINALLY
    {
        if (Native) RtlFreeHeap(RtlGetProcessHeap(), 0, Native);
    }
    _SEH2_END;
    return Ret;
}

static ULONG_PTR PolyPat(ULONG *a)
{
    const POLYPATBLT32 *Guest = P(2);
    POLYPATBLT *Native = NULL;
    ULONG Count = a[3], i;
    ULONG_PTR Ret;

    if (!GuestRange(a[2], (ULONGLONG)Count * sizeof(*Guest))) return Fault();
    GuestTouch(a[2], (ULONGLONG)Count * sizeof(*Guest));
    if (Count)
    {
        Native = RtlAllocateHeap(RtlGetProcessHeap(), 0, (SIZE_T)Count * sizeof(*Native));
        if (!Native) { RtlSetLastWin32Error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    }
    _SEH2_TRY
    {
        for (i = 0; i < Count; ++i)
        {
            Native[i].nXLeft = Guest[i].nXLeft; Native[i].nYLeft = Guest[i].nYLeft;
            Native[i].nWidth = Guest[i].nWidth; Native[i].nHeight = Guest[i].nHeight;
            Native[i].hBrush = (HBRUSH)(ULONG_PTR)Guest[i].hBrush;
        }
        Ret = NtGdiPolyPatBlt(G(0), a[1], Native, Count, a[4]);
    }
    _SEH2_FINALLY
    {
        if (Native) RtlFreeHeap(RtlGetProcessHeap(), 0, Native);
    }
    _SEH2_END;
    return Ret;
}

static ULONG_PTR CharPlacement(ULONG *a)
{
    GCP32 *Guest = P(4);
    GCP_RESULTSW Native = {0};
    ULONGLONG Count = a[2];
    ULONG_PTR Ret;

    if ((INT)a[2] < 0) { RtlSetLastWin32Error(ERROR_INVALID_PARAMETER); return 0; }
    if (!GuestRange(a[1], Count * sizeof(WCHAR))) return Fault();
    if (!a[4]) return NtGdiGetCharacterPlacementW(G(0), P(1), a[2], (INT)a[3], NULL, a[5]);
    if (!GuestRange(a[4], sizeof(*Guest))) return Fault();
    if (!GuestRange(Guest->lpOutString, Count * sizeof(WCHAR)) || !GuestRange(Guest->lpOrder, Count * sizeof(UINT)) ||
        !GuestRange(Guest->lpDx, Count * sizeof(INT)) || !GuestRange(Guest->lpCaretPos, Count * sizeof(INT)) ||
        !GuestRange(Guest->lpClass, Count) || !GuestRange(Guest->lpGlyphs, (ULONGLONG)Guest->nGlyphs * sizeof(WCHAR)))
        return Fault();
    Native.lStructSize = Guest->lStructSize == sizeof(*Guest) ? sizeof(Native) : Guest->lStructSize;
    Native.lpOutString = P32(Guest->lpOutString); Native.lpOrder = P32(Guest->lpOrder);
    Native.lpDx = P32(Guest->lpDx); Native.lpCaretPos = P32(Guest->lpCaretPos);
    Native.lpClass = P32(Guest->lpClass); Native.lpGlyphs = P32(Guest->lpGlyphs);
    Native.nGlyphs = Guest->nGlyphs; Native.nMaxFit = Guest->nMaxFit;
    Ret = NtGdiGetCharacterPlacementW(G(0), P(1), a[2], (INT)a[3], &Native, a[5]);
    Guest->nGlyphs = Native.nGlyphs; Guest->nMaxFit = Native.nMaxFit;
    return Ret;
}


static ULONG_PTR EnumGdiObjects(ULONG *a)
{
    LOGBRUSH *Native;
    LOGBRUSH32 *Guest = P(3);
    ULONG Count = min(a[2] / sizeof(LOGBRUSH32), MAXULONG / sizeof(LOGBRUSH)), i;
    ULONG_PTR Ret = 0;

    if (!a[3]) return NtGdiEnumObjects(G(0), (INT)a[1], a[2], NULL);
    if (!GuestRange(a[3], a[2])) return Fault();
    /* LOGPEN is identical on both targets. */
    if ((INT)a[1] == OBJ_PEN) return NtGdiEnumObjects(G(0), OBJ_PEN, a[2], P(3));
    if ((INT)a[1] != OBJ_BRUSH) { RtlSetLastWin32Error(ERROR_INVALID_PARAMETER); return 0; }
    GuestTouch(a[3], (ULONGLONG)Count * sizeof(*Guest));
    Native = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)Count + 1) * sizeof(*Native));
    if (!Native) { RtlSetLastWin32Error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    _SEH2_TRY
    {
        /* The kernel's return unit for a filled buffer is unverified; it is passed through unscaled. */
        Ret = NtGdiEnumObjects(G(0), OBJ_BRUSH, Count * sizeof(*Native), Native);
        if (Ret)
        {
            for (i = 0; i < Count; ++i)
            {
                Guest[i].lbStyle = Native[i].lbStyle; Guest[i].lbColor = Native[i].lbColor;
                Guest[i].lbHatch = (ULONG)Native[i].lbHatch;
            }
        }
    }
    _SEH2_FINALLY
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Native);
    }
    _SEH2_END;
    return Ret;
}

static ULONG_PTR FontFileInfo(ULONG *a)
{
    ULONG *Out = P(4);
    SIZE_T Actual = 0;
    ULONG_PTR Ret;

    /* FONT_FILE_INFO has the same layout on both targets; the variable path follows it. */
    if (!GuestRange(a[2], a[3]) || (a[4] && !GuestRange(a[4], sizeof(ULONG)))) return Fault();
    if (Out) Actual = *Out;
    Ret = NtGdiGetFontFileInfo(a[0], a[1], P(2), a[3], Out ? &Actual : NULL);
    if (Out) *Out = Actual > MAXULONG ? MAXULONG : (ULONG)Actual;
    return Ret;
}

static ULONG_PTR EscapeCall(ULONG *a)
{
    /* The escape code defines the payload; it is forwarded as opaque bytes, bounded below 4 GiB. */
    if (((INT)a[2] > 0 && !GuestRange(a[1], (ULONGLONG)a[2] * sizeof(WCHAR))) ||
        ((INT)a[4] > 0 && !GuestRange(a[5], a[4])) || ((INT)a[6] > 0 && !GuestRange(a[7], a[6])))
    {
        RtlSetLastWin32Error(ERROR_NOACCESS);
        return (ULONG_PTR)-1;
    }
    /* The native service leaves its result uninitialised when a buffer probe fails, so fault here instead. */
    if ((INT)a[4] > 0) GuestTouch(a[5], a[4]);
    if ((INT)a[6] > 0) GuestTouch(a[7], a[6]);
    return (ULONG_PTR)NtGdiExtEscape(G(0), P(1), (INT)a[2], (INT)a[3], (INT)a[4], P(5), (INT)a[6], P(7));
}

ULONG_PTR Wow64WinGdiExtraService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    case W64_NtGdiCreateColorSpace:
        if (!GuestRange(a[0], sizeof(LOGCOLORSPACEEXW))) return Fault();
        return (ULONG_PTR)NtGdiCreateColorSpace(P(0));
    case W64_NtGdiExtEscape: return EscapeCall(a);
    case W64_NtGdiConsoleTextOut: return PolyText(a, TRUE);
    case W64_NtGdiPolyTextOutW: return PolyText(a, FALSE);
    case W64_NtGdiPolyPatBlt: return PolyPat(a);
    case W64_NtGdiGetCharacterPlacementW: return CharPlacement(a);
    case W64_NtGdiEnumObjects: return EnumGdiObjects(a);
    case W64_NtGdiGetFontFileInfo: return FontFileInfo(a);
    /* dxg.sys never reads the untyped buffers; the native path always answers DDHAL_DRIVER_NOTHANDLED. */
    case W64_NtGdiDxgGenericThunk: return DDHAL_DRIVER_NOTHANDLED;
    default:
        return Wow64WinUserInputService(Number, a);
    }
}
