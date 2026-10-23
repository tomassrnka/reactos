/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Contract tests for the user mode pointer handling of NtGdiCreateDIBSection (T58)
 *
 * Defensive only: every case hands the kernel a bad but harmless pointer and
 * expects a clean failure (NULL handle, no bugcheck, *ppvBits untouched).
 * Split in several tests so that a misbehaving kernel is attributable to one case.
 */

#include "../win32nt.h"

static
PBYTE
AllocGuarded(SIZE_T *pcbReadable)
{
    SYSTEM_INFO si;
    PBYTE pb;
    DWORD dwOld;

    GetSystemInfo(&si);
    pb = VirtualAlloc(NULL, 2 * si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pb) return NULL;
    VirtualProtect(pb + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &dwOld);
    *pcbReadable = si.dwPageSize;
    return pb;
}

static
void
FillHeader(BITMAPINFOHEADER *pbih, WORD bpp)
{
    ZeroMemory(pbih, sizeof(*pbih));
    pbih->biSize = sizeof(BITMAPINFOHEADER);
    pbih->biWidth = 4;
    pbih->biHeight = 4;
    pbih->biPlanes = 1;
    pbih->biBitCount = bpp;
    pbih->biCompression = BI_RGB;
}

/* Valid calls keep working: info header with and without colour table, core header must not crash */
START_TEST(NtGdiCreateDIBSectionCaptureValid)
{
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bmi;
    BITMAPCOREINFO core;
    PVOID pvBits;
    HBITMAP hbmp;
    HDC hdc = GetDC(0);

    FillHeader(&bmi.h, 8);
    ZeroMemory(bmi.c, sizeof(bmi.c));
    pvBits = NULL;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bmi, DIB_RGB_COLORS, sizeof(bmi), 0, 0, &pvBits);
    TEST(hbmp != 0);
    TEST(pvBits != NULL);
    if (hbmp) DeleteObject(hbmp);

    FillHeader(&bmi.h, 24);
    pvBits = NULL;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bmi, DIB_RGB_COLORS, sizeof(bmi.h), 0, 0, &pvBits);
    TEST(hbmp != 0);
    TEST(pvBits != NULL);
    if (hbmp) DeleteObject(hbmp);

    /* 16 bpp BI_BITFIELDS: masks follow the header */
    FillHeader(&bmi.h, 16);
    bmi.h.biCompression = BI_BITFIELDS;
    ((DWORD*)bmi.c)[0] = 0xF800; ((DWORD*)bmi.c)[1] = 0x07E0; ((DWORD*)bmi.c)[2] = 0x001F;
    pvBits = NULL;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bmi, DIB_RGB_COLORS, sizeof(bmi), 0, 0, &pvBits);
    TEST(hbmp != 0);
    if (hbmp) DeleteObject(hbmp);

    /* Core header: outcome is not specified, it only has to be survived */
    ZeroMemory(&core, sizeof(core));
    core.bmciHeader.bcSize = sizeof(BITMAPCOREHEADER);
    core.bmciHeader.bcWidth = 4;
    core.bmciHeader.bcHeight = 4;
    core.bmciHeader.bcPlanes = 1;
    core.bmciHeader.bcBitCount = 24;
    pvBits = NULL;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&core, DIB_RGB_COLORS, sizeof(core), 0, 0, &pvBits);
    if (hbmp) DeleteObject(hbmp);

    ReleaseDC(0, hdc);
}

/* Garbage, oversized, unmapped and NULL bmi pointers are rejected cleanly */
START_TEST(NtGdiCreateDIBSectionCaptureInvalid)
{
    BITMAPINFOHEADER bih;
    PBYTE pb;
    SIZE_T cb;
    PVOID pvBits;
    HBITMAP hbmp;
    HDC hdc = GetDC(0);
    DWORD adwSizes[] = { 0, 1, 11, 13, 39, 0x1001, 0x10000, 0x7FFFFFF0, 0xFFFFFFFF };
    ULONG i;

    pvBits = (PVOID)-1;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, NULL, DIB_RGB_COLORS, 0, 0, 0, &pvBits);
    TEST(hbmp == 0);
    TEST(pvBits == (PVOID)-1);

    /* Unmapped pointers */
    pvBits = (PVOID)-1;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)(ULONG_PTR)0x10, DIB_RGB_COLORS, 64, 0, 0, &pvBits);
    TEST(hbmp == 0);
    TEST(pvBits == (PVOID)-1);

    pb = VirtualAlloc(NULL, 4096, MEM_RESERVE, PAGE_NOACCESS);
    if (pb)
    {
        hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)pb, DIB_RGB_COLORS, 64, 0, 0, &pvBits);
        TEST(hbmp == 0);
        TEST(pvBits == (PVOID)-1);
        VirtualFree(pb, 0, MEM_RELEASE);
    }

    /* Kernel address */
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)(ULONG_PTR)(~(ULONG_PTR)0 - 0xFFF), DIB_RGB_COLORS, 64, 0, 0, &pvBits);
    TEST(hbmp == 0);
    TEST(pvBits == (PVOID)-1);

    /* Bogus biSize values: none may create a bitmap or touch ppvBits */
    pb = AllocGuarded(&cb);
    if (pb)
    {
        for (i = 0; i < ARRAYSIZE(adwSizes); i++)
        {
            FillHeader(&bih, 8);
            bih.biSize = adwSizes[i];
            RtlCopyMemory(pb, &bih, sizeof(bih));
            pvBits = (PVOID)-1;
            hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)pb, DIB_RGB_COLORS, sizeof(bih), 0, 0, &pvBits);
            ok(hbmp == 0, "biSize 0x%lx created a bitmap\n", adwSizes[i]);
            ok(pvBits == (PVOID)-1, "biSize 0x%lx touched ppvBits\n", adwSizes[i]);
            if (hbmp) DeleteObject(hbmp);
        }

        /* Colour table running into the guard page */
        FillHeader(&bih, 8);
        RtlCopyMemory(pb + cb - sizeof(bih), &bih, sizeof(bih));
        pvBits = (PVOID)-1;
        hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)(pb + cb - sizeof(bih)), DIB_RGB_COLORS, 1064, 0, 0, &pvBits);
        TEST(hbmp == 0);
        TEST(pvBits == (PVOID)-1);
        if (hbmp) DeleteObject(hbmp);

        VirtualFree(pb, 0, MEM_RELEASE);
    }

    /* Garbage header: any outcome but a crash is acceptable, a bitmap must be deletable */
    {
        BYTE ab[2048];
        for (i = 0; i < sizeof(ab); i++) ab[i] = (BYTE)(i * 131 + 7);
        pvBits = NULL;
        hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)ab, DIB_RGB_COLORS, sizeof(ab), 0, 0, &pvBits);
        if (hbmp) DeleteObject(hbmp);
    }

    ReleaseDC(0, hdc);
}

/* The header straddles a guard page but passes a ProbeForRead of biSize bytes. With biBitCount == 0
   the old code never sized the colour table, so nothing touched the missing bytes under SEH
   before DIB_CreateDIBSection read them raw in kernel mode. */
START_TEST(NtGdiCreateDIBSectionCaptureStraddle)
{
    BITMAPINFOHEADER bih;
    PBYTE pb, pbHdr;
    SIZE_T cb;
    PVOID pvBits = (PVOID)-1;
    HBITMAP hbmp;
    HDC hdc = GetDC(0);

    pb = AllocGuarded(&cb);
    ok(pb != NULL, "VirtualAlloc failed\n");
    if (pb)
    {
        FillHeader(&bih, 0);
        pbHdr = pb + cb - 16; /* biCompression and later are in the guard page */
        RtlCopyMemory(pbHdr, &bih, 16);
        hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)pbHdr, DIB_RGB_COLORS, sizeof(bih), 0, 0, &pvBits);
        TEST(hbmp == 0);
        TEST(pvBits == (PVOID)-1);
        if (hbmp) DeleteObject(hbmp);
        VirtualFree(pb, 0, MEM_RELEASE);
    }
    ReleaseDC(0, hdc);
}

/* ppvBits is a user pointer too: a NOACCESS target must fail the call, not fault in kernel mode */
START_TEST(NtGdiCreateDIBSectionCaptureBitsPtr)
{
    BITMAPINFOHEADER bih;
    PBYTE pb;
    SIZE_T cb;
    HBITMAP hbmp;
    HDC hdc = GetDC(0);
    PVOID pvBits;

    FillHeader(&bih, 24);
    pb = AllocGuarded(&cb);
    ok(pb != NULL, "VirtualAlloc failed\n");
    if (pb)
    {
        hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bih, DIB_RGB_COLORS, sizeof(bih), 0, 0, (PVOID*)(pb + cb));
        TEST(hbmp == 0);
        if (hbmp) DeleteObject(hbmp);
        VirtualFree(pb, 0, MEM_RELEASE);
    }

    /* Sanity: a good target still works afterwards */
    pvBits = NULL;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bih, DIB_RGB_COLORS, sizeof(bih), 0, 0, &pvBits);
    TEST(hbmp != 0);
    TEST(pvBits != NULL);
    if (hbmp) DeleteObject(hbmp);
    ReleaseDC(0, hdc);
}

/* Contract for the size arithmetic in DIB_CreateDIBSection (T62): dimensions whose pitch or
   pitch * height does not fit 32 bits must fail cleanly, not wrap into an undersized buffer.
   Every case here is expected to be refused, so no large allocation is ever made. */
START_TEST(NtGdiCreateDIBSectionSizeMath)
{
    struct { LONG w; LONG h; WORD bpp; } aCases[] =
    {
        { 0x7FFFFFFF, 2, 8 },           /* pitch ~2GB, *2 wraps ULONG */
        { 0x7FFFFFFF, 0x7FFFFFFF, 1 },
        { 0x40000000, 4, 32 },          /* width * bpp overflows 32 bits */
        { 0x20000000, 4, 8 },           /* pitch 512MB: (pitch << 3) wraps in the surface code */
        { 0x10000, 0x10000, 32 },       /* pitch 256KB * height 64K = 2^34 */
        { 0x10000, 0x10000, 8 },        /* 2^32 exactly */
        { 0x7FFFFFFF, 1, 0xFFFF },      /* absurd bit count */
        { 0x01000001, 0x100, 32 },
        { 4, MINLONG, 8 },              /* -MINLONG is MINLONG */
        { 4, -0x7FFFFFFF - 1, 24 },
        { MINLONG, 4, 8 },              /* negative width */
        { -1, 4, 8 },
    };
    BITMAPINFOHEADER bih;
    PVOID pvBits;
    HBITMAP hbmp;
    HDC hdc = GetDC(0);
    ULONG i;

    for (i = 0; i < ARRAYSIZE(aCases); i++)
    {
        FillHeader(&bih, aCases[i].bpp);
        bih.biWidth = aCases[i].w;
        bih.biHeight = aCases[i].h;
        pvBits = (PVOID)-1;
        hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bih, DIB_RGB_COLORS, sizeof(bih), 0, 0, &pvBits);
        ok(hbmp == 0, "case %lu (%ld x %ld x %u) created a bitmap\n", i, aCases[i].w, aCases[i].h, aCases[i].bpp);
        ok(pvBits == (PVOID)-1, "case %lu touched ppvBits\n", i);
        if (hbmp) DeleteObject(hbmp);

        /* A top-down header must be judged the same way */
        if (aCases[i].h > 0)
        {
            bih.biHeight = -aCases[i].h;
            pvBits = (PVOID)-1;
            hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bih, DIB_RGB_COLORS, sizeof(bih), 0, 0, &pvBits);
            ok(hbmp == 0, "case %lu top-down created a bitmap\n", i);
            if (hbmp) DeleteObject(hbmp);
        }
    }

    /* Ordinary sizes keep working, including a top-down one and an odd width */
    FillHeader(&bih, 24);
    bih.biWidth = 33; bih.biHeight = -17;
    pvBits = NULL;
    hbmp = NtGdiCreateDIBSection(hdc, NULL, 0, (PBITMAPINFO)&bih, DIB_RGB_COLORS, sizeof(bih), 0, 0, &pvBits);
    TEST(hbmp != 0);
    TEST(pvBits != NULL);
    if (hbmp) DeleteObject(hbmp);

    ReleaseDC(0, hdc);
}
