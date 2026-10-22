/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Native DirectDraw implementation
 * FILE:             win32ss/reactx/ntddraw/d3dkmt.c
 * PROGRAMER:        Sebastian Gasiorek (sebastian.gasiorek@reactos.com)
 */

#include <win32k.h>

DWORD
APIENTRY
NtGdiDdDDICreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc)
{
    PSURFACE psurf;
    HDC hDC;

    const struct d3dddi_format_info
    {
        D3DDDIFORMAT format;
        unsigned int bit_count;
        DWORD compression;
        unsigned int palette_size;
        DWORD mask_r, mask_g, mask_b;
    } *format = NULL;
    unsigned int i;

    static const struct d3dddi_format_info format_info[] =
    {
        { D3DDDIFMT_R8G8B8,   24, BI_RGB,       0,   0x00000000, 0x00000000, 0x00000000 },
        { D3DDDIFMT_A8R8G8B8, 32, BI_RGB,       0,   0x00000000, 0x00000000, 0x00000000 },
        { D3DDDIFMT_X8R8G8B8, 32, BI_RGB,       0,   0x00000000, 0x00000000, 0x00000000 },
        { D3DDDIFMT_R5G6B5,   16, BI_BITFIELDS, 0,   0x0000f800, 0x000007e0, 0x0000001f },
        { D3DDDIFMT_X1R5G5B5, 16, BI_BITFIELDS, 0,   0x00007c00, 0x000003e0, 0x0000001f },
        { D3DDDIFMT_A1R5G5B5, 16, BI_BITFIELDS, 0,   0x00007c00, 0x000003e0, 0x0000001f },
        { D3DDDIFMT_A4R4G4B4, 16, BI_BITFIELDS, 0,   0x00000f00, 0x000000f0, 0x0000000f },
        { D3DDDIFMT_X4R4G4B4, 16, BI_BITFIELDS, 0,   0x00000f00, 0x000000f0, 0x0000000f },
        { D3DDDIFMT_P8,       8,  BI_RGB,       256, 0x00000000, 0x00000000, 0x00000000 },
    };

    D3DKMT_CREATEDCFROMMEMORY Desc;
    ULONGLONG BufferSize;
    HBITMAP hBitmap;
    NTSTATUS Status;

    if (!desc)
        return STATUS_INVALID_PARAMETER;

    /* The descriptor is caller memory: capture it once */
    _SEH2_TRY
    {
        ProbeForWrite(desc, sizeof(*desc), 1);
        Desc = *desc;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (!Desc.pMemory)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < sizeof(format_info) / sizeof(*format_info); ++i)
    {
        if (format_info[i].format == Desc.Format)
        {
            format = &format_info[i];
            break;
        }
    }

    if (!format)
        return STATUS_INVALID_PARAMETER;

    if (Desc.Width > (UINT_MAX & ~3) / (format->bit_count / 8) ||
        !Desc.Pitch || Desc.Pitch < ((((ULONGLONG)Desc.Width * format->bit_count + 31) >> 3) & ~3) ||
        !Desc.Height || Desc.Height > UINT_MAX / Desc.Pitch)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* The surface converts the pitch to bits in 32 bits and draws with
       signed 32-bit offsets, and it rounds its stride up to 4 bytes */
    if (Desc.Pitch > MAXULONG / 8)
        return STATUS_INVALID_PARAMETER;
    BufferSize = (((ULONGLONG)Desc.Pitch + 3) & ~3ULL) * Desc.Height;
    if (BufferSize > MAXLONG)
        return STATUS_INVALID_PARAMETER;

    /* The surface bits stay in the caller's memory: it must be user memory */
    _SEH2_TRY
    {
        ProbeForRead(Desc.pMemory, (SIZE_T)BufferSize, 1);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (!Desc.hDeviceDc || !(hDC = NtGdiCreateCompatibleDC(Desc.hDeviceDc)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Allocate a surface */
    psurf = SURFACE_AllocSurface(STYPE_BITMAP,
                                 Desc.Width,
                                 Desc.Height,
                                 BitmapFormat(format->bit_count, format->compression),
                                 BMF_TOPDOWN | BMF_NOZEROINIT,
                                 Desc.Pitch,
                                 (ULONG)BufferSize,
                                 Desc.pMemory);
    if (!psurf)
    {
        NtGdiDeleteObjectApp(hDC);
        return STATUS_NO_MEMORY;
    }

    /* Mark as API bitmap */
    psurf->flags |= (DDB_SURFACE | API_BITMAP);

    /* Get the handle for the bitmap */
    hBitmap = (HBITMAP)psurf->SurfObj.hsurf;

    /* Allocate a palette for this surface */
    if (format->bit_count <= 8)
    {
        PPALETTE palette = PALETTE_AllocPalette(PAL_INDEXED, 1 << format->bit_count, NULL, 0, 0, 0);
        if (palette)
        {
            SURFACE_vSetPalette(psurf, palette);
            PALETTE_ShareUnlockPalette(palette);
        }
    }

    /* Unlock the surface and return */
    SURFACE_UnlockSurface(psurf);

    NtGdiSelectBitmap(hDC, hBitmap);

    Status = STATUS_SUCCESS;
    _SEH2_TRY
    {
        desc->hDc = hDC;
        desc->hBitmap = hBitmap;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        /* The caller cannot learn the handles, so do not leak them, and do
           not leave a deleted handle behind if the first write went through */
        NtGdiDeleteObjectApp(hDC);
        NtGdiDeleteObjectApp(hBitmap);
        _SEH2_TRY
        {
            desc->hDc = Desc.hDc;
            desc->hBitmap = Desc.hBitmap;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            NOTHING;
        }
        _SEH2_END;
    }

    return Status;
}

DWORD
APIENTRY
NtGdiDdDDIDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *desc)
{
    D3DKMT_DESTROYDCFROMMEMORY Desc;

    if (!desc)
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        ProbeForRead(desc, sizeof(*desc), 1);
        Desc = *desc;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (GDI_HANDLE_GET_TYPE(Desc.hDc) != GDI_OBJECT_TYPE_DC ||
        GDI_HANDLE_GET_TYPE(Desc.hBitmap) != GDI_OBJECT_TYPE_BITMAP)
        return STATUS_INVALID_PARAMETER;

    NtGdiDeleteObjectApp(Desc.hBitmap);
    NtGdiDeleteObjectApp(Desc.hDc);

    return STATUS_SUCCESS;
}
