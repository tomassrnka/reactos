/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Contract test for NtGdiExtCreateRegion size handling
 */

#include "../win32nt.h"

/* Returns a buffer of cb bytes that ends exactly at an inaccessible page */
static PBYTE
AllocBeforeGuard(SIZE_T cb, PVOID *ppBase)
{
    DWORD dwOld;
    BOOL bOk;
    PBYTE pb = VirtualAlloc(NULL, 2 * 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    ok(pb != NULL, "VirtualAlloc failed\n");
    bOk = VirtualProtect(pb + 0x1000, 0x1000, PAGE_NOACCESS, &dwOld);
    ok(bOk, "VirtualProtect failed\n");
    *ppBase = pb;
    return pb + 0x1000 - cb;
}

START_TEST(NtGdiExtCreateRegion)
{
    PVOID pBase;
    PRGNDATA prd;
    SIZE_T cb = sizeof(RGNDATAHEADER) + 2 * sizeof(RECT);
    HRGN hrgn;
    RECT rc;
    XFORM xf = {1, 0, 0, 1, 0, 0};

    prd = (PRGNDATA)AllocBeforeGuard(cb, &pBase);
    memset(prd, 0, cb);
    prd->rdh.dwSize = sizeof(RGNDATAHEADER);
    prd->rdh.iType = RDH_RECTANGLES;
    prd->rdh.nCount = 2;
    prd->rdh.nRgnSize = 2 * sizeof(RECT);
    SetRect((RECT *)prd->Buffer, 0, 0, 10, 10);
    SetRect((RECT *)prd->Buffer + 1, 20, 0, 30, 10);

    /* Valid */
    hrgn = NtGdiExtCreateRegion(NULL, cb, prd);
    ok(hrgn != NULL, "valid call failed\n");
    if (hrgn)
    {
        ok(GetRgnBox(hrgn, &rc) == COMPLEXREGION, "region is not complex\n");
        ok(rc.right == 30 && rc.bottom == 10, "bad box\n");
        DeleteObject(hrgn);
    }

    hrgn = NtGdiExtCreateRegion(&xf, cb, prd);
    ok(hrgn != NULL, "valid call with xform failed\n");
    if (hrgn) DeleteObject(hrgn);

    /* Length too small for the header (and zero length) */
    ok(NtGdiExtCreateRegion(NULL, 0, prd) == NULL, "Count == 0 must fail\n");
    ok(NtGdiExtCreateRegion(NULL, sizeof(RGNDATAHEADER) - 1, prd) == NULL,
       "short header must fail\n");

    /* Length too small for nCount rects */
    ok(NtGdiExtCreateRegion(NULL, cb - 1, prd) == NULL, "short rects must fail\n");

    /* nCount values for which nCount * sizeof(RECT) wraps in 32 bits */
    prd->rdh.nCount = 0x10000000;
    ok(NtGdiExtCreateRegion(NULL, cb, prd) == NULL, "nCount 0x10000000 must fail\n");
    prd->rdh.nCount = 0x10000001;
    ok(NtGdiExtCreateRegion(NULL, cb, prd) == NULL, "nCount 0x10000001 must fail\n");
    prd->rdh.nCount = 0xFFFFFFFF;
    ok(NtGdiExtCreateRegion(NULL, cb, prd) == NULL, "nCount 0xFFFFFFFF must fail\n");
    prd->rdh.nCount = 0x0FFFFFFF;
    ok(NtGdiExtCreateRegion(NULL, cb, prd) == NULL, "nCount 0x0FFFFFFF must fail\n");
    prd->rdh.nCount = 0x80000000;
    ok(NtGdiExtCreateRegion(NULL, 0xFFFFFFFF, prd) == NULL, "huge Count/nCount must fail\n");

    /* Wrong header fields */
    prd->rdh.nCount = 2;
    prd->rdh.iType = 0;
    ok(NtGdiExtCreateRegion(NULL, cb, prd) == NULL, "bad iType must fail\n");
    prd->rdh.iType = RDH_RECTANGLES;
    prd->rdh.dwSize = 0;
    ok(NtGdiExtCreateRegion(NULL, cb, prd) == NULL, "bad dwSize must fail\n");
    prd->rdh.dwSize = sizeof(RGNDATAHEADER);

    /* Invalid pointers must fail cleanly */
    ok(NtGdiExtCreateRegion(NULL, cb, (PRGNDATA)(LONG_PTR)0x81000000) == NULL,
       "kernel pointer must fail\n");
    ok(NtGdiExtCreateRegion((LPXFORM)(LONG_PTR)0x81000000, cb, prd) == NULL,
       "kernel XFORM pointer must fail\n");
    /* Count is only an upper bound for what is read (nCount rects), so a
       larger Count must neither crash nor read behind the rects */
    hrgn = NtGdiExtCreateRegion(NULL, cb + 0x1000, prd);
    if (hrgn) DeleteObject(hrgn);

    VirtualFree(pBase, 0, MEM_RELEASE);
}
