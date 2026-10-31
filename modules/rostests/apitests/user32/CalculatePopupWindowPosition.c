/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for CalculatePopupWindowPosition
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef TPM_WORKAREA
#define TPM_WORKAREA 0x00010000
#endif

typedef BOOL (WINAPI *PFN_CalculatePopupWindowPosition)(const POINT *, const SIZE *, UINT, RECT *, RECT *);

/*
 * Every position is given relative to the work area of the primary monitor, so that the rows hold for any
 * resolution: an offset from the left, top, right or bottom edge or from the centre. The expected rectangles are
 * the results of Windows 10 22H2 on a 1024x768 monitor, expressed in the same way (they were also taken on
 * 1280x800 to tell which edge each coordinate follows).
 */
typedef enum { AL, AT, AC, AM, AR, AB } BASE;
typedef struct { BASE Base; int Offset; } REL;
typedef struct { REL X, Y; } RELPOINT;
typedef struct { REL Left, Top, Right, Bottom; } RELRECT;
typedef enum { SW_ABS, SW_REL } SIZEKIND_W;
typedef enum { SH_ABS, SH_REL } SIZEKIND_H;
typedef struct { SIZEKIND_W WKind; int W; SIZEKIND_H HKind; int H; } RELSIZE;

static const struct
{
    PCSTR Name;
    RELPOINT Anchor;
    RELSIZE Size;
    UINT Flags;
    BOOL HasExclude;
    RELRECT Exclude;
    RELRECT Expected;
} Rows[] =
{
#include "CalculatePopupWindowPosition.inc"
};

static LONG Resolve(REL Rel, const RECT *Area)
{
    switch (Rel.Base)
    {
        case AL: return Area->left + Rel.Offset;
        case AT: return Area->top + Rel.Offset;
        case AR: return Area->right + Rel.Offset;
        case AB: return Area->bottom + Rel.Offset;
        case AC: return Area->left + (Area->right - Area->left) / 2 + Rel.Offset;
        default: return Area->top + (Area->bottom - Area->top) / 2 + Rel.Offset;
    }
}

static void ResolveRect(const RELRECT *Rel, const RECT *Area, RECT *Rect)
{
    Rect->left = Resolve(Rel->Left, Area);
    Rect->top = Resolve(Rel->Top, Area);
    Rect->right = Resolve(Rel->Right, Area);
    Rect->bottom = Resolve(Rel->Bottom, Area);
}

static void TestRows(PFN_CalculatePopupWindowPosition pCalc, const RECT *Area)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Rows); i++)
    {
        POINT Anchor;
        SIZE Size;
        RECT Exclude, ExcludeCopy, Out = { 0x11, 0x22, 0x33, 0x44 }, Expected;
        BOOL Ret;

        Anchor.x = Resolve(Rows[i].Anchor.X, Area);
        Anchor.y = Resolve(Rows[i].Anchor.Y, Area);
        Size.cx = Rows[i].Size.WKind == SW_ABS ? Rows[i].Size.W : (Area->right - Area->left) + Rows[i].Size.W;
        Size.cy = Rows[i].Size.HKind == SH_ABS ? Rows[i].Size.H : (Area->bottom - Area->top) + Rows[i].Size.H;
        ResolveRect(&Rows[i].Exclude, Area, &Exclude);
        ExcludeCopy = Exclude;
        ResolveRect(&Rows[i].Expected, Area, &Expected);

        SetLastError(0xdeadbeef);
        Ret = pCalc(&Anchor, &Size, Rows[i].Flags, Rows[i].HasExclude ? &Exclude : NULL, &Out);
        ok(Ret, "%s: failed with %lu\n", Rows[i].Name, GetLastError());
        ok(EqualRect(&Out, &Expected), "%s: got (%ld,%ld,%ld,%ld), expected (%ld,%ld,%ld,%ld)\n", Rows[i].Name,
           Out.left, Out.top, Out.right, Out.bottom, Expected.left, Expected.top, Expected.right, Expected.bottom);
        ok(EqualRect(&Exclude, &ExcludeCopy), "%s: the exclude rectangle was changed\n", Rows[i].Name);
    }
}

static void TestParameters(PFN_CalculatePopupWindowPosition pCalc, const RECT *Area)
{
    POINT Anchor = { Area->left + 100, Area->top + 100 };
    SIZE Size = { 50, 40 };
    RECT Out, Expected;
    BOOL Ret;
    ULONG i;
    /* every TPM_* bit but 0x200 is accepted, none above TPM_WORKAREA */
    static const UINT Valid[] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x400, 0x800, 0x1000, 0x2000,
                                  0x4000, 0x8000, 0x10000 };
    static const UINT Invalid[] = { 0x200, 0x20000, 0x40000, 0x100000, 0x01000000, 0x80000000, 0xFFFFFFFF };

    for (i = 0; i < RTL_NUMBER_OF(Valid); i++)
    {
        SetLastError(0xdeadbeef);
        Ret = pCalc(&Anchor, &Size, Valid[i], NULL, &Out);
        ok(Ret, "flags %x: failed with %lu\n", Valid[i], GetLastError());
    }

    for (i = 0; i < RTL_NUMBER_OF(Invalid); i++)
    {
        SetRect(&Out, 1, 2, 3, 4);
        SetLastError(0xdeadbeef);
        Ret = pCalc(&Anchor, &Size, Invalid[i], NULL, &Out);
        ok(!Ret && GetLastError() == ERROR_INVALID_FLAGS, "flags %x: ret %d, error %lu\n", Invalid[i], Ret, GetLastError());
        ok(Out.left == 1 && Out.top == 2 && Out.right == 3 && Out.bottom == 4, "flags %x: the result was written\n", Invalid[i]);
    }

    /* a negative size is rejected, a zero size is a point */
    Size.cx = -5;
    Size.cy = 40;
    SetRect(&Out, 1, 2, 3, 4);
    SetLastError(0xdeadbeef);
    Ret = pCalc(&Anchor, &Size, 0, NULL, &Out);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "negative width: ret %d, error %lu\n", Ret, GetLastError());
    ok(Out.left == 1 && Out.top == 2 && Out.right == 3 && Out.bottom == 4, "negative width: the result was written\n");

    Size.cx = 50;
    Size.cy = -5;
    SetLastError(0xdeadbeef);
    Ret = pCalc(&Anchor, &Size, 0, NULL, &Out);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "negative height: ret %d, error %lu\n", Ret, GetLastError());

    Size.cx = 0;
    Size.cy = 0;
    SetLastError(0xdeadbeef);
    Ret = pCalc(&Anchor, &Size, 0, NULL, &Out);
    SetRect(&Expected, Anchor.x, Anchor.y, Anchor.x, Anchor.y);
    ok(Ret && EqualRect(&Out, &Expected), "zero size: ret %d, (%ld,%ld,%ld,%ld)\n", Ret, Out.left, Out.top, Out.right, Out.bottom);

    /* an empty window has nothing to move out of the way */
    {
        RECT Exclude = { Anchor.x - 10, Anchor.y - 10, Anchor.x + 10, Anchor.y + 10 };
        SetLastError(0xdeadbeef);
        Ret = pCalc(&Anchor, &Size, 0, &Exclude, &Out);
        ok(Ret && EqualRect(&Out, &Expected), "zero size with an exclude rectangle: ret %d, (%ld,%ld,%ld,%ld)\n",
           Ret, Out.left, Out.top, Out.right, Out.bottom);
    }

    Size.cx = 50;
    Size.cy = 40;
    SetLastError(0xdeadbeef);
    Ret = pCalc(&Anchor, &Size, 0, NULL, NULL);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "no result rectangle: ret %d, error %lu\n", Ret, GetLastError());
}

START_TEST(CalculatePopupWindowPosition)
{
    PFN_CalculatePopupWindowPosition pCalc;
    MONITORINFO Info = { sizeof(Info) };
    POINT Origin = { 0, 0 };
    HMONITOR Monitor;

    /* the first user32 call turns the thread into a GUI thread */
    GetSystemMetrics(SM_CXSCREEN);

    pCalc = (PFN_CalculatePopupWindowPosition)GetProcAddress(GetModuleHandleW(L"user32.dll"), "CalculatePopupWindowPosition");
    if (!pCalc)
    {
        ok(!is_reactos(), "CalculatePopupWindowPosition is not exported by user32.dll\n");
        skip("CalculatePopupWindowPosition is not available\n");
        return;
    }

    Monitor = MonitorFromPoint(Origin, MONITOR_DEFAULTTOPRIMARY);
    if (!Monitor || !GetMonitorInfoW(Monitor, &Info))
    {
        skip("no primary monitor\n");
        return;
    }

    /* the rows need room for the windows and rectangles they place around the centre */
    if (Info.rcWork.right - Info.rcWork.left < 500 || Info.rcWork.bottom - Info.rcWork.top < 400)
    {
        skip("the work area is too small\n");
        return;
    }

    TestRows(pCalc, &Info.rcWork);
    TestParameters(pCalc, &Info.rcWork);
}
