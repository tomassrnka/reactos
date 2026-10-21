/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for NtGdiPolyPolyDraw
 * PROGRAMMERS:
 */

#include "../win32nt.h"


static
void
Test_Params(void)
{
    ULONG_PTR ret;
    ULONG Count1[4] = {3, 2, 4, 3};
    ULONG Count2[2] = {0, 3};
    ULONG Count3[2] = {0, 0};
    ULONG Count4[2] = {1, 3};
    ULONG Count5[2] = {0x80000001, 0x80000001};
    POINT Points[6] = {{0,0}, {1,1}, {3,-3}, {-2,2}, {4,2}, {2,4}};
    HDC hDC;

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(NULL, NULL, NULL, 0, 0);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(NULL, NULL, NULL, 0, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(NULL, NULL, NULL, 0, 2);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(NULL, NULL, NULL, 0, 3);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(NULL, NULL, NULL, 0, 4);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(NULL, NULL, NULL, 0, 5);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(NULL, NULL, NULL, 0, 6);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

////////////////////////////////////////////////////////////////////////////////

    /* Test with an invalid DC */

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(0, Points, Count1, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_HANDLE);

    hDC = (HDC)0x12345;

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 0);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_HANDLE);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 2);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_HANDLE);

    SetLastError(0xdeadbeef);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 3);
    TEST(ret == 0);
    ok_long(GetLastError(), 0xdeadbeef);

    SetLastError(0xdeadbeef);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 4);
    TEST(ret == 0);
    ok_long(GetLastError(), 0xdeadbeef);

    SetLastError(0xdeadbeef);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 5);
    TEST(ret == 0);
    ok_long(GetLastError(), 0xdeadbeef);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 6);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw((HDC)1, Points, Count1, 1, 6);
    TEST((ret & GDI_HANDLE_BASETYPE_MASK) == GDI_OBJECT_TYPE_REGION);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw((HDC)0, Points, Count1, 1, 6);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 0, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count2, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_HANDLE);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, NULL, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, (PVOID)(LONG_PTR)0x81000000, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, NULL, Count1, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, (PVOID)(LONG_PTR)0x81000000, Count1, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 0);


////////////////////////////////////////////////////////////////////////////////

    /* Test with a valid DC */

    hDC = CreateCompatibleDC(NULL);
    ASSERT(hDC);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 0);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 1);
    TEST(ret == 1);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 2);
    TEST(ret == 1);
    TEST(GetLastError() == 0);

#if 0
    SetLastError(0);
    // better don't do this on win xp!!! (random crashes)
//    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 3);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_PARAMETER);

    SetLastError(0);
    // better don't do this on win xp!!! (random crashes)
//    ret = NtGdiPolyPolyDraw(hDC, Points, Count1, 2, 4);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_PARAMETER);

#endif

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count2, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_PARAMETER);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count3, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 0);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count4, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == ERROR_INVALID_PARAMETER);

    SetLastError(0);
    ret = NtGdiPolyPolyDraw(hDC, Points, Count5, 2, 1);
    TEST(ret == 0);
    TEST(GetLastError() == 87);

}


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

/*
 * Size math / capture contract: sums of the counts that overflow, huge Count
 * values and buffers shorter than the counts claim must be rejected cleanly,
 * and valid calls keep working.
 */
static void
Test_Overflow(void)
{
    PVOID pBasePts, pBaseCnt;
    POINT *pts;
    ULONG *cnt;
    ULONG_PTR ret;
    HDC hDC = CreateCompatibleDC(NULL);
    ULONG i;

    ok(hDC != NULL, "CreateCompatibleDC failed\n");

    /* 4 points, buffer ends at the guard page */
    pts = (POINT *)AllocBeforeGuard(4 * sizeof(POINT), &pBasePts);
    for (i = 0; i < 4; i++) { pts[i].x = i * 3; pts[i].y = (i & 1) * 5; }
    cnt = (ULONG *)AllocBeforeGuard(2 * sizeof(ULONG), &pBaseCnt);

    /* Valid */
    cnt[0] = 2; cnt[1] = 2;
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 2, GdiPolyPolyLine);
    ok(ret != 0, "valid polyline failed\n");
    ret = NtGdiPolyPolyDraw((HDC)1, pts, cnt, 2, GdiPolyPolyRgn);
    ok(ret != 0, "valid region failed\n");
    if (ret) DeleteObject((HRGN)ret);

    /* Sum of counts wraps around 32 bit to a small value */
    cnt[0] = 0x80000000; cnt[1] = 0x80000002;
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 2, GdiPolyPolygon);
    ok(ret == 0, "wrapping sum (to 2) must fail\n");

    /* Sum does not wrap, but sum * sizeof(POINT) does */
    cnt[0] = 0x20000001; cnt[1] = 0x20000001;
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 2, GdiPolyPolygon);
    ok(ret == 0, "size wrap (points) must fail\n");
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 2, GdiPolyPolyLine);
    ok(ret == 0, "size wrap (points, polyline) must fail\n");
    ret = NtGdiPolyPolyDraw((HDC)1, pts, cnt, 2, GdiPolyPolyRgn);
    ok(ret == 0, "size wrap (points, region) must fail\n");
    cnt[0] = 0x1FFFFFFF; cnt[1] = 0x20000003;
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 2, GdiPolyPolygon);
    ok(ret == 0, "size wrap (points, 2) must fail\n");

    /* Counts claim more points than the (probed) buffer holds */
    cnt[0] = 3; cnt[1] = 3;
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 2, GdiPolyPolyLine);
    ok(ret == 0, "more points than mapped must fail\n");
    cnt[0] = 0x00100000; cnt[1] = 2;
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 2, GdiPolyPolyLine);
    ok(ret == 0, "far more points than mapped must fail\n");

    /* Count larger than the counts buffer / overflowing Count * sizeof(ULONG) */
    cnt[0] = 2; cnt[1] = 2;
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 1000, GdiPolyPolyLine);
    ok(ret == 0, "Count beyond the mapped counts must fail\n");
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 0x40000000, GdiPolyPolyLine);
    ok(ret == 0, "Count * 4 wrap must fail\n");
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 0x20000000, GdiPolyPolyLine);
    ok(ret == 0, "Count * 8 wrap must fail\n");
    ret = NtGdiPolyPolyDraw(hDC, pts, cnt, 0xFFFFFFFF, GdiPolyPolyLine);
    ok(ret == 0, "Count 0xFFFFFFFF must fail\n");

    VirtualFree(pBasePts, 0, MEM_RELEASE);
    VirtualFree(pBaseCnt, 0, MEM_RELEASE);
    if (hDC) DeleteDC(hDC);
}


START_TEST(NtGdiPolyPolyDraw)
{
    Test_Params();
    Test_Overflow();

}

