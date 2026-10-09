/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for D3DKMTCreateDCFromMemory and D3DKMTDestroyDCFromMemory with bad pointers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <ntstatus.h>
#include <d3dkmthk.h>

typedef NTSTATUS (APIENTRY *PFN_CREATEDCFROMMEMORY)(D3DKMT_CREATEDCFROMMEMORY *);
typedef NTSTATUS (APIENTRY *PFN_DESTROYDCFROMMEMORY)(const D3DKMT_DESTROYDCFROMMEMORY *);

static PFN_CREATEDCFROMMEMORY pD3DKMTCreateDCFromMemory;
static PFN_DESTROYDCFROMMEMORY pD3DKMTDestroyDCFromMemory;

/* The last page of the address space: kernel memory, or unmapped under WOW64 */
#define KERNEL_ADDRESS ((PVOID)(ULONG_PTR)-4096)
/* Never mapped */
#define BAD_USER_ADDRESS ((PVOID)(ULONG_PTR)8)

#define ok_bad_pointer_status(Status) \
    ok((Status) == STATUS_ACCESS_VIOLATION || (Status) == STATUS_INVALID_PARAMETER, \
       "Got unexpected status 0x%lx\n", (Status))

static BOOL InitFunctions(void)
{
    HMODULE hMod = GetModuleHandleW(L"gdi32.dll");

    pD3DKMTCreateDCFromMemory = (PFN_CREATEDCFROMMEMORY)GetProcAddress(hMod, "D3DKMTCreateDCFromMemory");
    pD3DKMTDestroyDCFromMemory = (PFN_DESTROYDCFROMMEMORY)GetProcAddress(hMod, "D3DKMTDestroyDCFromMemory");
    if (!pD3DKMTCreateDCFromMemory || !pD3DKMTDestroyDCFromMemory)
    {
        /* ReactOS NT 5.x builds export them from gdi32_vista */
        hMod = LoadLibraryW(L"gdi32_vista.dll");
        if (!hMod)
            return FALSE;
        pD3DKMTCreateDCFromMemory = (PFN_CREATEDCFROMMEMORY)GetProcAddress(hMod, "D3DKMTCreateDCFromMemory");
        pD3DKMTDestroyDCFromMemory = (PFN_DESTROYDCFROMMEMORY)GetProcAddress(hMod, "D3DKMTDestroyDCFromMemory");
    }
    return pD3DKMTCreateDCFromMemory && pD3DKMTDestroyDCFromMemory;
}

static void InitDesc(D3DKMT_CREATEDCFROMMEMORY *Desc, PVOID Memory, HDC hDeviceDc)
{
    ZeroMemory(Desc, sizeof(*Desc));
    Desc->pMemory = Memory;
    Desc->Format = D3DDDIFMT_X8R8G8B8;
    Desc->Width = 4;
    Desc->Height = 4;
    Desc->Pitch = 16;
    Desc->hDeviceDc = hDeviceDc;
    Desc->hDc = (HDC)(ULONG_PTR)0x010baade;
    Desc->hBitmap = (HANDLE)(ULONG_PTR)0x020baade;
}

START_TEST(D3DKMTCreateDCFromMemory)
{
    D3DKMT_CREATEDCFROMMEMORY Desc, *pDesc;
    D3DKMT_DESTROYDCFROMMEMORY DestroyDesc;
    DWORD Bits[16];
    HDC hDeviceDc;
    NTSTATUS Status;
    DWORD OldProtect;

    if (!InitFunctions())
    {
        skip("D3DKMTCreateDCFromMemory is not available\n");
        return;
    }

    hDeviceDc = CreateCompatibleDC(NULL);
    ok(hDeviceDc != NULL, "CreateCompatibleDC failed\n");

    /* A valid call works and its objects can be destroyed */
    InitDesc(&Desc, Bits, hDeviceDc);
    Status = pD3DKMTCreateDCFromMemory(&Desc);
    ok(Status == STATUS_SUCCESS, "Got unexpected status 0x%lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        ok(GetObjectType(Desc.hDc) == OBJ_MEMDC, "Got unexpected dc %p\n", Desc.hDc);
        DestroyDesc.hDc = Desc.hDc;
        DestroyDesc.hBitmap = Desc.hBitmap;
        Status = pD3DKMTDestroyDCFromMemory(&DestroyDesc);
        ok(Status == STATUS_SUCCESS, "Got unexpected status 0x%lx\n", Status);
    }

    /* The descriptor is not readable */
    Status = pD3DKMTCreateDCFromMemory((D3DKMT_CREATEDCFROMMEMORY *)BAD_USER_ADDRESS);
    ok_bad_pointer_status(Status);
    Status = pD3DKMTCreateDCFromMemory((D3DKMT_CREATEDCFROMMEMORY *)KERNEL_ADDRESS);
    ok_bad_pointer_status(Status);
    Status = pD3DKMTDestroyDCFromMemory((const D3DKMT_DESTROYDCFROMMEMORY *)BAD_USER_ADDRESS);
    ok_bad_pointer_status(Status);
    Status = pD3DKMTDestroyDCFromMemory((const D3DKMT_DESTROYDCFROMMEMORY *)KERNEL_ADDRESS);
    ok_bad_pointer_status(Status);

    /* The descriptor is readable but not writable: rejected before any object is made */
    pDesc = VirtualAlloc(NULL, sizeof(*pDesc), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok(pDesc != NULL, "VirtualAlloc failed\n");
    if (pDesc)
    {
        InitDesc(pDesc, Bits, hDeviceDc);
        ok(VirtualProtect(pDesc, sizeof(*pDesc), PAGE_READONLY, &OldProtect), "VirtualProtect failed\n");
        Status = pD3DKMTCreateDCFromMemory(pDesc);
        ok_bad_pointer_status(Status);
        ok(pDesc->hDc == (HDC)(ULONG_PTR)0x010baade, "Got unexpected dc %p\n", pDesc->hDc);
        VirtualFree(pDesc, 0, MEM_RELEASE);
    }

    /* The width overflows the pitch computation: the bits would be too small */
    InitDesc(&Desc, Bits, hDeviceDc);
    Desc.Width = 0x20000001;
    Desc.Height = 1;
    Desc.Pitch = 4;
    Status = pD3DKMTCreateDCFromMemory(&Desc);
    /* Windows (Server 2008 R2 and Windows 10) does not check the size and
       creates the DC; ReactOS rejects it */
    ok(Status == STATUS_INVALID_PARAMETER || (!is_reactos() && Status == STATUS_SUCCESS),
       "Got unexpected status 0x%lx\n", Status);
    if (Status != STATUS_SUCCESS)
        ok(Desc.hDc == (HDC)(ULONG_PTR)0x010baade, "Got unexpected dc %p\n", Desc.hDc);
    if (Status == STATUS_SUCCESS)
    {
        DestroyDesc.hDc = Desc.hDc;
        DestroyDesc.hBitmap = Desc.hBitmap;
        pD3DKMTDestroyDCFromMemory(&DestroyDesc);
    }

    /* The surface would exceed signed 32-bit offsets, or the pitch in bits wraps */
    InitDesc(&Desc, Bits, hDeviceDc);
    Desc.Width = 1;
    Desc.Pitch = 0x1FFFFFFC;
    Desc.Height = 8;
    Status = pD3DKMTCreateDCFromMemory(&Desc);
    ok(Status == STATUS_INVALID_PARAMETER, "Got unexpected status 0x%lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        DestroyDesc.hDc = Desc.hDc;
        DestroyDesc.hBitmap = Desc.hBitmap;
        pD3DKMTDestroyDCFromMemory(&DestroyDesc);
    }
    InitDesc(&Desc, Bits, hDeviceDc);
    Desc.Width = 1;
    Desc.Pitch = 0x20000000;
    Desc.Height = 1;
    Status = pD3DKMTCreateDCFromMemory(&Desc);
    ok(Status == STATUS_INVALID_PARAMETER, "Got unexpected status 0x%lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        DestroyDesc.hDc = Desc.hDc;
        DestroyDesc.hBitmap = Desc.hBitmap;
        pD3DKMTDestroyDCFromMemory(&DestroyDesc);
    }

    /* The bits are not user memory */
    InitDesc(&Desc, KERNEL_ADDRESS, hDeviceDc);
    Status = pD3DKMTCreateDCFromMemory(&Desc);
    ok_bad_pointer_status(Status);
    ok(Desc.hDc == (HDC)(ULONG_PTR)0x010baade, "Got unexpected dc %p\n", Desc.hDc);
    if (Status == STATUS_SUCCESS)
    {
        DestroyDesc.hDc = Desc.hDc;
        DestroyDesc.hBitmap = Desc.hBitmap;
        pD3DKMTDestroyDCFromMemory(&DestroyDesc);
    }

    DeleteDC(hDeviceDc);
}
