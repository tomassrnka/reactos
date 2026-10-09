/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtGdiDdCreateSurface with bad caller pointers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

/* Never mapped */
#define BAD_USER_ADDRESS ((PVOID)(ULONG_PTR)8)
/* Never user memory: the last page of the address space. It is usually
   unmapped, so these cases show that bad pointers are caught; they cannot
   tell a probe from an exception handler alone */
#define KERNEL_ADDRESS ((PVOID)(ULONG_PTR)-4096)

static
DWORD
CreateSurface(HANDLE hDirectDraw, PDD_SURFACE_LOCAL pLocal, PDD_CREATESURFACEDATA pData)
{
    DDSURFACEDESC Desc;
    DD_SURFACE_GLOBAL Global;
    DD_SURFACE_MORE More;
    HANDLE hSurface = NULL, huSurface = NULL;

    ZeroMemory(&Desc, sizeof(Desc));
    Desc.dwSize = sizeof(Desc);
    ZeroMemory(&Global, sizeof(Global));
    ZeroMemory(&More, sizeof(More));

    return NtGdiDdCreateSurface(hDirectDraw, &hSurface, &Desc, &Global, pLocal,
                                &More, pData, &huSurface);
}

START_TEST(NtGdiDdCreateSurface)
{
    DD_SURFACE_LOCAL Local;
    DD_CREATESURFACEDATA Data;
    HANDLE hDirectDraw;
    HDC hdc;

    ZeroMemory(&Local, sizeof(Local));
    Local.ddsCaps.dwCaps = DDSCAPS_VISIBLE;
    ZeroMemory(&Data, sizeof(Data));
    Data.dwSCnt = 1;

    /* The create data is not caller memory */
    ok_long(CreateSurface(NULL, &Local, (PDD_CREATESURFACEDATA)BAD_USER_ADDRESS), DDHAL_DRIVER_NOTHANDLED);
    ok_long(CreateSurface(NULL, &Local, (PDD_CREATESURFACEDATA)KERNEL_ADDRESS), DDHAL_DRIVER_NOTHANDLED);

    /* The surface local data is not caller memory. Without a DirectDraw
       object these pass even if the data is never read; only the cases
       with an object below exercise the reads */
    ok_long(CreateSurface(NULL, (PDD_SURFACE_LOCAL)BAD_USER_ADDRESS, &Data), DDHAL_DRIVER_NOTHANDLED);
    ok_long(CreateSurface(NULL, (PDD_SURFACE_LOCAL)KERNEL_ADDRESS, &Data), DDHAL_DRIVER_NOTHANDLED);

    /* The same with a DirectDraw object, where the local data is used */
    hdc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
    ok(hdc != NULL, "CreateDCW() failed\n");
    if (hdc == NULL)
    {
        skip("No DC\n");
        return;
    }

    hDirectDraw = NtGdiDdCreateDirectDrawObject(hdc);
    if (hDirectDraw == NULL)
    {
        skip("No DirectDraw object\n");
        DeleteDC(hdc);
        return;
    }

    ok_long(CreateSurface(hDirectDraw, &Local, (PDD_CREATESURFACEDATA)BAD_USER_ADDRESS), DDHAL_DRIVER_NOTHANDLED);
    ok_long(CreateSurface(hDirectDraw, &Local, (PDD_CREATESURFACEDATA)KERNEL_ADDRESS), DDHAL_DRIVER_NOTHANDLED);
    ok_long(CreateSurface(hDirectDraw, (PDD_SURFACE_LOCAL)BAD_USER_ADDRESS, &Data), DDHAL_DRIVER_NOTHANDLED);
    ok_long(CreateSurface(hDirectDraw, (PDD_SURFACE_LOCAL)KERNEL_ADDRESS, &Data), DDHAL_DRIVER_NOTHANDLED);

    NtGdiDdDeleteDirectDrawObject(hDirectDraw);
    DeleteDC(hdc);
}
