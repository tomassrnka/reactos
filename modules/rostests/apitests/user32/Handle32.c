/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Public-API handle extension and message transport measurements
 */

#include "precomp.h"

static HBRUSH MessageBrush;
static BOOL SignedResult;

/* Bound each reuse search in the ordinary suite. Report an inconclusive
 * measurement if the host cannot reach a bit-31 handle within this budget. */
#define REUSE_TIMEOUT_MS 5000

static HANDLE ZeroHandle(HANDLE Handle)
{
    return (HANDLE)(ULONG_PTR)(ULONG)(ULONG_PTR)Handle;
}

static HANDLE SignHandle(HANDLE Handle)
{
    return (HANDLE)(LONG_PTR)(LONG)(ULONG_PTR)Handle;
}

static BOOL Bit31(HANDLE Handle)
{
    return ((ULONG_PTR)Handle & 0x80000000UL) != 0;
}

static LRESULT CALLBACK HandleWndProc(HWND Wnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    if (Msg == WM_CTLCOLORSTATIC)
        return (LRESULT)(SignedResult ? SignHandle(MessageBrush) : ZeroHandle(MessageBrush));
    return DefWindowProcW(Wnd, Msg, WParam, LParam);
}

static void TestUser(void)
{
    HMENU Menu = NULL;
    DWORD Start = GetTickCount(), Iteration;
    BOOL Z, S;

    /* Recycle an owned object, never invent a generation or borrow a handle. */
    for (Iteration = 0; Iteration < 400000; ++Iteration)
    {
        Menu = CreateMenu();
        if (!Menu || Bit31(Menu) || Iteration + 1 == 400000 || GetTickCount() - Start >= REUSE_TIMEOUT_MS)
            break;
        if (!DestroyMenu(Menu))
        {
            ok(FALSE, "DestroyMenu failed during reuse\n");
            return;
        }
        Menu = NULL;
    }
    ok(Menu != NULL, "CreateMenu failed\n");
    if (!Menu) return;
    trace("USER issued=%p zero=%p sign=%p iterations=%lu bit31=%d\n",
          Menu, ZeroHandle(Menu), SignHandle(Menu), Iteration, Bit31(Menu));
    if (!Bit31(Menu)) skip("USER: no bit-31 handle; forms are identical, result is inconclusive\n");
    Z = IsMenu(ZeroHandle(Menu));
    S = IsMenu(SignHandle(Menu));
    trace("USER IsMenu zero=%d sign=%d\n", Z, S);
    ok(Z && S, "IsMenu rejected a form: zero=%d sign=%d\n", Z, S);
    ok_int(GetMenuItemCount(ZeroHandle(Menu)), 0);
    ok_int(GetMenuItemCount(SignHandle(Menu)), 0);
    ok(DestroyMenu(Menu), "DestroyMenu failed\n");
    ok(!IsMenu(ZeroHandle(Menu)) && !IsMenu(SignHandle(Menu)), "Stale menu accepted\n");
}

static HGDIOBJ HighGdi(BOOL Brush)
{
    HGDIOBJ Object = NULL;
    DWORD Start = GetTickCount(), Iteration;

    for (Iteration = 0; Iteration < 65536; ++Iteration)
    {
        Object = Brush ? (HGDIOBJ)CreateSolidBrush(RGB(0x12, 0x34, 0x56)) :
                         (HGDIOBJ)CreateBitmap(1, 1, 1, 1, NULL);
        if (!Object || Bit31(Object) || Iteration + 1 == 65536 || GetTickCount() - Start >= REUSE_TIMEOUT_MS)
            break;
        if (!DeleteObject(Object))
        {
            ok(FALSE, "DeleteObject failed during reuse\n");
            return NULL;
        }
        GdiFlush();
        Object = NULL;
    }
    trace("GDI %s issued=%p zero=%p sign=%p iterations=%lu bit31=%d\n",
          Brush ? "brush" : "bitmap", Object, ZeroHandle(Object), SignHandle(Object),
          Iteration, Bit31(Object));
    if (Object && !Bit31(Object)) skip("GDI: no bit-31 handle; forms are identical\n");
    return Object;
}

static void TestGdi(HWND Wnd)
{
    HDC DC;
    HBITMAP Bitmap, OldBitmap, DeleteBitmap;
    HBRUSH OldBrush, Returned;
    LOGBRUSH ZBrush, SBrush;
    BITMAP DeletedInfo;
    RECT Rect = {0, 0, 2, 2};
    INT Z, S, Form;
    BOOL Deleted;
    DWORD Error;

    MessageBrush = HighGdi(TRUE);
    ok(MessageBrush != NULL, "CreateSolidBrush failed\n");
    if (!MessageBrush) return;
    Z = GetObjectW(ZeroHandle(MessageBrush), sizeof(ZBrush), &ZBrush);
    S = GetObjectW(SignHandle(MessageBrush), sizeof(SBrush), &SBrush);
    trace("GDI GetObject brush zero=%d sign=%d\n", Z, S);
    ok_int(Z, sizeof(ZBrush));
    ok_int(S, sizeof(SBrush));
    if (Z == sizeof(ZBrush) && S == sizeof(SBrush))
        ok(ZBrush.lbColor == SBrush.lbColor, "Brush forms describe different colors\n");

    DC = CreateCompatibleDC(NULL);
    Bitmap = CreateBitmap(2, 2, 1, 32, NULL);
    ok(DC && Bitmap, "Cannot create drawing fixture\n");
    if (DC && Bitmap)
    {
        OldBitmap = SelectObject(DC, Bitmap);
        /* Typed DC_ATTR path: the guest must write the zero-extended handle. */
        OldBrush = SelectObject(DC, ZeroHandle(MessageBrush));
        ok(OldBrush != NULL, "SelectObject failed\n");
        ok(PatBlt(DC, 0, 0, 2, 2, PATCOPY), "Typed brush PatBlt failed\n");
        ok_hex(GetPixel(DC, 0, 0), RGB(0x12, 0x34, 0x56));
        SelectObject(DC, OldBrush);
        for (Form = 0; Form < 2; ++Form)
        {
            SignedResult = Form;
            Returned = (HBRUSH)SendMessageW(Wnd, WM_CTLCOLORSTATIC, (WPARAM)DC, (LPARAM)Wnd);
            trace("GDI WM_CTLCOLORSTATIC form=%d returned=%p\n", Form, Returned);
            ok_ptr(Returned, Form ? SignHandle(MessageBrush) : ZeroHandle(MessageBrush));
            SetPixel(DC, 0, 0, RGB(0, 0, 0));
            ok(FillRect(DC, &Rect, Returned), "FillRect failed for form %d\n", Form);
            ok_hex(GetPixel(DC, 0, 0), RGB(0x12, 0x34, 0x56));
        }
        SelectObject(DC, OldBitmap);
    }
    if (Bitmap) DeleteObject(Bitmap);
    if (DC) DeleteDC(DC);
    DeleteObject(MessageBrush);
    MessageBrush = NULL;
    GdiFlush();

    /* Bitmap deletion bypasses the client brush-deletion batch and exercises
     * win32k's object-owner check. Test stale handles and clean up on failure. */
    DeleteBitmap = HighGdi(FALSE);
    ok(DeleteBitmap != NULL, "CreateBitmap failed\n");
    if (!DeleteBitmap) return;
    ok_int(GetObjectType(ZeroHandle(DeleteBitmap)), OBJ_BITMAP);
    ok_int(GetObjectType(SignHandle(DeleteBitmap)), OBJ_BITMAP);
    SetLastError(0xdeadbeef);
    Deleted = DeleteObject(SignHandle(DeleteBitmap));
    Error = GetLastError();
    trace("GDI DeleteObject signed bitmap=%d error=%lu\n", Deleted, Error);
#if defined(_WIN64) && !defined(BUILD_WOW64_ENABLED)
    /* Default amd64 still compares the full-width unique value in the owner
     * check. Keep the Windows expectation; enabled builds must pass it. */
    todo_ros_if(SignHandle(DeleteBitmap) != ZeroHandle(DeleteBitmap))
#endif
    ok(Deleted, "Signed bitmap deletion failed, error %lu\n", Error);
    if (!Deleted) ok(DeleteObject(ZeroHandle(DeleteBitmap)), "Zero bitmap deletion failed\n");
    GdiFlush();
    /* GetObjectType can report the encoded type even after deletion on Windows. */
    ok_int(GetObjectW(ZeroHandle(DeleteBitmap), sizeof(DeletedInfo), &DeletedInfo), 0);
    ok_int(GetObjectW(SignHandle(DeleteBitmap), sizeof(DeletedInfo), &DeletedInfo), 0);
}

static void TestLayout(HWND Wnd)
{
    HKL Original = GetKeyboardLayout(0), Layout, Previous, Before[256];
    INT Count, Index, Form;
    BOOL WasLoaded = FALSE, RejectZero;
    UINT Z, S;

    Count = GetKeyboardLayoutList(ARRAYSIZE(Before), Before);
    /* US Dvorak has a layout identifier in the high half (normally f0020409). */
    Layout = LoadKeyboardLayoutW(L"00010409", KLF_NOTELLSHELL);
    if (!Layout)
    {
        skip("Dvorak unavailable, error %lu; no high HKL measurement\n", GetLastError());
        return;
    }
    for (Index = 0; Index < Count; ++Index)
        if ((ULONG_PTR)ZeroHandle(Before[Index]) == (ULONG_PTR)ZeroHandle(Layout)) WasLoaded = TRUE;
    trace("HKL issued=%p zero=%p sign=%p bit31=%d original=%p\n",
          Layout, ZeroHandle(Layout), SignHandle(Layout), Bit31(Layout), Original);
    ok_ptr(Layout, SignHandle(Layout));
    RejectZero = ZeroHandle(Layout) != SignHandle(Layout);
    if (!Bit31(Layout)) skip("HKL bit 31 clear; forms are identical\n");
    Z = MapVirtualKeyExW('Q', MAPVK_VK_TO_VSC, ZeroHandle(Layout));
    S = MapVirtualKeyExW('Q', MAPVK_VK_TO_VSC, SignHandle(Layout));
    trace("HKL MapVirtualKeyEx zero=%u sign=%u\n", Z, S);
    ok(S != 0, "MapVirtualKeyEx rejected the signed HKL\n");
    ok_int(Z, RejectZero ? 0 : S);
    for (Form = 0; Form < 2; ++Form)
    {
        ActivateKeyboardLayout(Original, 0);
        ok_ptr(GetKeyboardLayout(0), Original);
        SetLastError(0xdeadbeef);
        Previous = ActivateKeyboardLayout(Form ? SignHandle(Layout) : ZeroHandle(Layout), 0);
        trace("HKL Activate form=%d previous=%p current=%p error=%lu\n",
              Form, Previous, GetKeyboardLayout(0), GetLastError());
        ok_ptr(Previous, !Form && RejectZero ? NULL : Original);
        ok_ptr(GetKeyboardLayout(0), !Form && RejectZero ? Original : Layout);
        ActivateKeyboardLayout(Original, 0);
        ok_ptr(GetKeyboardLayout(0), Original);
        SendMessageW(Wnd, WM_INPUTLANGCHANGEREQUEST, 0,
                     (LPARAM)(Form ? SignHandle(Layout) : ZeroHandle(Layout)));
        trace("HKL message form=%d current=%p\n", Form, GetKeyboardLayout(0));
        ok_ptr(GetKeyboardLayout(0), !Form && RejectZero ? Original : Layout);
    }
    ActivateKeyboardLayout(Original, 0);
    if (!WasLoaded) UnloadKeyboardLayout(Layout);
}

START_TEST(Handle32)
{
    WNDCLASSW Class = {0};
    HWND Wnd;
    DWORD BatchLimit;

    trace("Handle32 process_bits=%u\n", (unsigned)(8 * sizeof(void *)));
    Class.lpfnWndProc = HandleWndProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"Handle32Test";
    if (!RegisterClassW(&Class))
    {
        ok(FALSE, "RegisterClass failed\n");
        return;
    }
    Wnd = CreateWindowW(Class.lpszClassName, L"", 0, 0, 0, 16, 16,
                        NULL, NULL, Class.hInstance, NULL);
    ok(Wnd != NULL, "CreateWindow failed\n");
    if (Wnd)
    {
        BatchLimit = GdiSetBatchLimit(1);
        TestUser();
        TestGdi(Wnd);
        TestLayout(Wnd);
        GdiSetBatchLimit(BatchLimit);
        DestroyWindow(Wnd);
    }
    UnregisterClassW(Class.lpszClassName, Class.hInstance);
}
