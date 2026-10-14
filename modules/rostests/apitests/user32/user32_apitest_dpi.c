/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Helper for the DpiFunctions test: runs steps against the DPI functions in a
 *              fresh process (the process awareness can be set once) and prints one line per result
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>

typedef HANDLE CTX;

static UINT (WINAPI *pGetDpiForSystem)(void);
static UINT (WINAPI *pGetDpiForWindow)(HWND);
static BOOL (WINAPI *pSetProcessDpiAwarenessContext)(CTX);
static CTX (WINAPI *pGetThreadDpiAwarenessContext)(void);
static CTX (WINAPI *pSetThreadDpiAwarenessContext)(CTX);
static CTX (WINAPI *pGetWindowDpiAwarenessContext)(HWND);
static int (WINAPI *pGetAwarenessFromDpiAwarenessContext)(CTX);
static BOOL (WINAPI *pAreDpiAwarenessContextsEqual)(CTX, CTX);
static BOOL (WINAPI *pIsValidDpiAwarenessContext)(CTX);
static BOOL (WINAPI *pEnableNonClientDpiScaling)(HWND);
static UINT (WINAPI *pGetDpiFromDpiAwarenessContext)(CTX);
static CTX (WINAPI *pGetDpiAwarenessContextForProcess)(HANDLE);
static BOOL (WINAPI *pIsProcessDPIAware)(void);
static BOOL (WINAPI *pSetProcessDPIAware)(void);
static BOOL (WINAPI *pGetProcessDpiAwarenessInternal)(HANDLE, int *);
static BOOL (WINAPI *pSetProcessDpiAwarenessInternal)(int);
static int (WINAPI *pGetThreadDpiHostingBehavior)(void);
static int (WINAPI *pSetThreadDpiHostingBehavior)(int);
static int (WINAPI *pGetWindowDpiHostingBehavior)(HWND);
static HRESULT (WINAPI *pGetProcessDpiAwareness)(HANDLE, int *);
static HRESULT (WINAPI *pSetProcessDpiAwareness)(int);

static UINT SystemDpi;
static DWORD Gle;

#define LOAD(mod, name) p##name = (void *)GetProcAddress(mod, #name)
/* The error is read right after the call, and "-" says that the call left it alone */
#define CALL(var, call) do { SetLastError(0xdead); (var) = (call); Gle = GetLastError(); } while (0)

static void Out(const char *Format, ...)
{
    va_list Args;

    va_start(Args, Format);
    vprintf(Format, Args);
    va_end(Args);
    fflush(stdout);
}

static const char *GleOf(DWORD Value)
{
    static char Buf[4][16];
    static int n;
    char *b = Buf[n++ & 3];

    if (Value == 0xdead)
        return "-";
    sprintf(b, "%lu", Value);
    return b;
}

static const char *GleStr(void)
{
    return GleOf(Gle);
}

/* The system DPI is printed as SYS, so that the output does not depend on it */
static const char *CtxName(ULONG_PTR Ctx, char *Buf)
{
    ULONG Low = (ULONG)Ctx;
    UINT Dpi = (Low >> 8) & 0x1ff;

    if ((INT_PTR)Ctx >= -5 && (INT_PTR)Ctx <= -1)
    {
        sprintf(Buf, "pseudo%d", (int)(INT_PTR)Ctx);
        return Buf;
    }
    /* a 32-bit value is the same whether it was zero-extended or sign-extended to a pointer */
    if (Ctx != Low && (Ctx >> 16 >> 16) != 0xffffffffu)
    {
        sprintf(Buf, "wide%I64x", (unsigned __int64)Ctx);
        return Buf;
    }
    if (Dpi == SystemDpi && (Low & 0xf) == 1)
        sprintf(Buf, "a%lu.v%lu.SYS.f%lx", Low & 0xf, (Low >> 4) & 0xf, Low & 0xfffe0000);
    else
        sprintf(Buf, "a%lu.v%lu.d%u.f%lx", Low & 0xf, (Low >> 4) & 0xf, Dpi, Low & 0xfffe0000);
    return Buf;
}

static CTX Token(const char *s)
{
    if (!strcmp(s, "U")) return (CTX)(INT_PTR)-1;
    if (!strcmp(s, "S")) return (CTX)(INT_PTR)-2;
    if (!strcmp(s, "P")) return (CTX)(INT_PTR)-3;
    if (!strcmp(s, "P2")) return (CTX)(INT_PTR)-4;
    if (!strcmp(s, "G")) return (CTX)(INT_PTR)-5;
    if (!strcmp(s, "N")) return NULL;
    if (!strcmp(s, "Cur")) return pGetThreadDpiAwarenessContext ? pGetThreadDpiAwarenessContext() : NULL;
    if (!strncmp(s, "raw:", 4)) return (CTX)(ULONG_PTR)strtoul(s + 4, NULL, 16);
    /* a 32-bit value that was sign-extended to a pointer, as LongToHandle does */
    if (!strncmp(s, "sx:", 3)) return (CTX)(INT_PTR)(LONG)strtoul(s + 3, NULL, 16);
    if (!strncmp(s, "sys:", 4))
    {
        ULONG Flags = strtoul(s + 4, NULL, 16);
        return (CTX)(ULONG_PTR)(1 | (1 << 4) | (SystemDpi << 8) | Flags);
    }
    if (!strncmp(s, "dpi:", 4))
    {
        ULONG Dpi = strtoul(s + 4, NULL, 10);
        return (CTX)(ULONG_PTR)(1 | (1 << 4) | (Dpi << 8));
    }
    return (CTX)(INT_PTR)-99;
}

static void PrintCtx(const char *Label, CTX Ctx)
{
    char Buf[64];
    Out("%s=%s\n", Label, CtxName((ULONG_PTR)Ctx, Buf));
}

static void StepGet(void)
{
    int Aw1 = -77, Aw2 = -77, Aw3 = -77, Aw4 = -77;
    BOOL Ret1 = -1, Ret2 = -1, IsAware = -1;
    HRESULT hr1 = -1, hr2 = -1;
    CTX Thread, Ctx1, Ctx2;
    char B1[64], B2[64], B3[64];

    /* All the calls leave the last error alone */
    SetLastError(0xdead);
    Thread = pGetThreadDpiAwarenessContext();
    Ctx1 = pGetDpiAwarenessContextForProcess ? pGetDpiAwarenessContextForProcess(NULL) : NULL;
    Ctx2 = pGetDpiAwarenessContextForProcess ? pGetDpiAwarenessContextForProcess(GetCurrentProcess()) : NULL;
    IsAware = pIsProcessDPIAware();
    Ret1 = pGetProcessDpiAwarenessInternal(NULL, &Aw1);
    Ret2 = pGetProcessDpiAwarenessInternal(GetCurrentProcess(), &Aw2);
    Gle = GetLastError();
    hr1 = pGetProcessDpiAwareness(NULL, &Aw3);
    hr2 = pGetProcessDpiAwareness(GetCurrentProcess(), &Aw4);
    Out("get: thread=%s proc=%s/%s IsProcessDPIAware=%d internal=%d:%d/%d:%d shcore=%08lx:%d/%08lx:%d gle=%s\n",
        CtxName((ULONG_PTR)Thread, B1), CtxName((ULONG_PTR)Ctx1, B2), CtxName((ULONG_PTR)Ctx2, B3), IsAware,
        Ret1, Aw1, Ret2, Aw2, (ULONG)hr1, Aw3, (ULONG)hr2, Aw4, GleStr());
}

static void StepFuncs(void)
{
    static const char *Tokens[] = { "U", "S", "P", "P2", "G", "N", "raw:1", "raw:6010", "sys:0", "raw:12", "raw:22",
                                    "raw:40006010", "raw:80006010", "raw:80000012", "raw:6011", "raw:10", "raw:7811",
                                    "raw:32", "raw:20", "raw:40000012", "raw:2", "raw:1010", "raw:30", "dpi:95",
                                    "dpi:100", "dpi:192", "dpi:255", "dpi:480", "dpi:511", "dpi:0", "dpi:1", "raw:20011",
                                    "raw:100012", "raw:2006010", "raw:40000022", "raw:80000022", "raw:c0006010",
                                    "raw:c0000012", "raw:6012", "raw:6020", "raw:7010", "raw:6030", "raw:5010", "raw:6110",
                                    "sx:80006010", "sx:80000012", "sx:c0006010", "sx:6010" };
    static const char *EqTokens[] = { "U", "S", "P", "P2", "G", "sys:0", "raw:6010", "raw:80006010", "raw:12",
                                      "raw:80000012", "raw:40006010", "raw:c0006010", "raw:7811" };
    UINT i, j;
    char Buf[64];
    int Aw, Equal;
    BOOL Valid;
    UINT Dpi;

    for (i = 0; i < ARRAYSIZE(Tokens); i++)
    {
        CTX c = Token(Tokens[i]);
        char Line[200];

        Line[0] = 0;
        if (pIsValidDpiAwarenessContext)
        {
            CALL(Valid, pIsValidDpiAwarenessContext(c));
            sprintf(Line + strlen(Line), " valid=%d/%s", Valid, GleStr());
        }
        if (pGetAwarenessFromDpiAwarenessContext)
        {
            CALL(Aw, pGetAwarenessFromDpiAwarenessContext(c));
            sprintf(Line + strlen(Line), " aw=%d/%s", Aw, GleStr());
        }
        if (pGetDpiFromDpiAwarenessContext)
        {
            CALL(Dpi, pGetDpiFromDpiAwarenessContext(c));
            sprintf(Line + strlen(Line), " dpi=%u/%s", Dpi, GleStr());
        }
        Out("ctx %s (%s):%s\n", Tokens[i], CtxName((ULONG_PTR)c, Buf), Line);
    }

    if (!pAreDpiAwarenessContextsEqual)
        return;
    for (i = 0; i < ARRAYSIZE(EqTokens); i++)
    {
        char Row[64];

        for (j = 0; j < ARRAYSIZE(EqTokens); j++)
        {
            CALL(Equal, pAreDpiAwarenessContextsEqual(Token(EqTokens[i]), Token(EqTokens[j])));
            Row[j] = (Equal ? '1' : '0');
        }
        Row[j] = 0;
        Out("equal %s: %s\n", EqTokens[i], Row);
    }
    CALL(Equal, pAreDpiAwarenessContextsEqual(Token("U"), Token("raw:7")));
    Out("equal U invalid=%d/%s\n", Equal, GleStr());
    CALL(Equal, pAreDpiAwarenessContextsEqual(Token("raw:7"), Token("U")));
    Out("equal invalid U=%d/%s\n", Equal, GleStr());
    CALL(Equal, pAreDpiAwarenessContextsEqual(Token("raw:7"), Token("raw:7")));
    Out("equal invalid invalid=%d/%s\n", Equal, GleStr());
    CALL(Equal, pAreDpiAwarenessContextsEqual(NULL, NULL));
    Out("equal NULL NULL=%d/%s\n", Equal, GleStr());
}

static void StepSetProcess(const char *Arg)
{
    BOOL Ret;

    CALL(Ret, pSetProcessDpiAwarenessContext(Token(Arg)));
    Out("SetProcessDpiAwarenessContext(%s)=%d gle=%s\n", Arg, Ret, GleStr());
}

static void StepSetThread(const char *Arg)
{
    CTX Prev;

    CALL(Prev, pSetThreadDpiAwarenessContext(Token(Arg)));
    Out("SetThreadDpiAwarenessContext(%s) gle=%s ", Arg, GleStr());
    PrintCtx("prev", Prev);
}

static BOOL NcEnable;
static char NcCtx[64];

static LRESULT CALLBACK DpiWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        char Buf[64];

        NcEnable = pEnableNonClientDpiScaling(hwnd);
        strcpy(NcCtx, CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(hwnd), Buf));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI WindowThread(PVOID Arg)
{
    char Buf[64];

    Out("new thread: ctx=%s\n", CtxName((ULONG_PTR)pGetThreadDpiAwarenessContext(), Buf));
    return 0;
}

/* One window of a style, and what the system says of it while it is made and after */
static HWND MakeWindow(const char *Label, DWORD Style, DWORD ExStyle, HWND Parent)
{
    HWND hwnd;
    char Buf[64], After[64];
    BOOL Ret;
    UINT Dpi;

    NcEnable = -1;
    NcCtx[0] = 0;
    hwnd = CreateWindowExW(ExStyle, L"DpiTestClass", L"dpi", Style, 10, 10, 300, 200, Parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd)
    {
        Out("%s: not created\n", Label);
        return NULL;
    }
    strcpy(After, CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(hwnd), Buf));
    CALL(Dpi, pGetDpiForWindow(hwnd));
    CALL(Ret, pEnableNonClientDpiScaling(hwnd));
    Out("%s: nccreate enable=%d ctx=%s, later ctx=%s dpi=%s enable=%d/%s\n", Label, NcEnable, NcCtx, After,
        Dpi == SystemDpi ? "SYS" : "other", Ret, GleStr());
    return hwnd;
}

static void StepWin(const char *Arg)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd, child, popup, other;
    char Buf[64];
    CTX Cur = pGetThreadDpiAwarenessContext(), Ctx, Prev;
    HANDLE hThread;
    BOOL Ret;
    UINT Dpi;
    int Behavior;

    wc.lpfnWndProc = DpiWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"DpiTestClass";
    RegisterClassW(&wc);

    Out("windows under %s\n", CtxName((ULONG_PTR)Cur, Buf));
    hwnd = MakeWindow("overlapped", WS_OVERLAPPEDWINDOW, 0, NULL);
    if (!hwnd)
        return;
    popup = MakeWindow("popup", WS_POPUP, 0, NULL);
    DestroyWindow(popup);
    popup = MakeWindow("owned popup", WS_POPUP, 0, hwnd);
    DestroyWindow(popup);
    popup = MakeWindow("child", WS_CHILD, 0, hwnd);
    DestroyWindow(popup);
    popup = MakeWindow("tool window", WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, NULL);
    DestroyWindow(popup);
    child = CreateWindowExW(0, L"static", L"c", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, NULL, wc.hInstance, NULL);
    Out("static child: ctx=%s\n", CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(child), Buf));

    /* a window made under another thread context keeps the context of its creation */
    if (pSetThreadDpiAwarenessContext && Arg && Arg[0])
    {
        Prev = pSetThreadDpiAwarenessContext(Token(Arg));
        Out("windows under %s\n", Arg);
        other = MakeWindow("overlapped", WS_OVERLAPPEDWINDOW, 0, NULL);
        pSetThreadDpiAwarenessContext(Prev);
        {
            char B1[64], B2[64];

            Out("first window again: ctx=%s, thread ctx=%s\n", CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(hwnd), B1),
                CtxName((ULONG_PTR)pGetThreadDpiAwarenessContext(), B2));
        }
        DestroyWindow(other);
    }

    {
        char L1[64], L2[64];
        UINT D1, D2, D3;
        DWORD G1, G2, G3;
        CTX C1, C2;
        BOOL E1, E2;
        DWORD G4, G5, G6, G7;

        CALL(D1, pGetDpiForWindow(NULL)); G1 = Gle;
        CALL(D2, pGetDpiForWindow((HWND)(ULONG_PTR)0x1234567)); G2 = Gle;
        DestroyWindow(hwnd);
        CALL(D3, pGetDpiForWindow(hwnd)); G3 = Gle;
        Out("GetDpiForWindow NULL=%u/%s bad=%u/%s destroyed=%u/%s\n", D1, GleOf(G1), D2, GleOf(G2), D3, GleOf(G3));
        CALL(C1, pGetWindowDpiAwarenessContext(NULL)); G4 = Gle;
        CALL(C2, pGetWindowDpiAwarenessContext((HWND)(ULONG_PTR)0x1234567)); G5 = Gle;
        CALL(E1, pEnableNonClientDpiScaling(NULL)); G6 = Gle;
        CALL(E2, pEnableNonClientDpiScaling((HWND)(ULONG_PTR)0x1234567)); G7 = Gle;
        Out("GetWindowDpiAwarenessContext NULL=%s/%s bad=%s/%s, EnableNonClientDpiScaling NULL=%d/%s bad=%d/%s\n",
            CtxName((ULONG_PTR)C1, L1), GleOf(G4), CtxName((ULONG_PTR)C2, L2), GleOf(G5), E1, GleOf(G6), E2, GleOf(G7));
    }
    CALL(Ctx, pGetWindowDpiAwarenessContext(GetDesktopWindow()));
    CALL(Dpi, pGetDpiForWindow(GetDesktopWindow()));
    Out("desktop: ctx=%s dpi=%s\n", CtxName((ULONG_PTR)Ctx, Buf), Dpi == SystemDpi ? "SYS" : "other");
    if (pGetWindowDpiHostingBehavior)
    {
        int B2;
        DWORD G;

        CALL(Behavior, pGetWindowDpiHostingBehavior(GetDesktopWindow()));
        CALL(B2, pGetWindowDpiHostingBehavior(NULL));
        G = Gle;
        Out("GetWindowDpiHostingBehavior desktop=%d NULL=%d/%s\n", Behavior, B2, GleOf(G));
    }

    hThread = CreateThread(NULL, 0, WindowThread, NULL, 0, NULL);
    WaitForSingleObject(hThread, 10000);
    CloseHandle(hThread);
}

static void StepHosting(void)
{
    int Behavior;

    Out("hosting: get=%d\n", pGetThreadDpiHostingBehavior());
    CALL(Behavior, pSetThreadDpiHostingBehavior(1));
    Out("hosting: set(1)=%d gle=%s\n", Behavior, GleStr());
    Out("hosting: get=%d\n", pGetThreadDpiHostingBehavior());
    CALL(Behavior, pSetThreadDpiHostingBehavior(7));
    Out("hosting: set(7)=%d gle=%s\n", Behavior, GleStr());
    CALL(Behavior, pSetThreadDpiHostingBehavior(-1));
    Out("hosting: set(-1)=%d gle=%s\n", Behavior, GleStr());
    CALL(Behavior, pSetThreadDpiHostingBehavior(0));
    Out("hosting: set(0)=%d gle=%s\n", Behavior, GleStr());
    Out("hosting: get=%d\n", pGetThreadDpiHostingBehavior());
}

/* A window made with a parent or an owner while the thread has another context, and the hosting
 * behaviour that windows get */
static void StepNest(const char *Arg)
{
    WNDCLASSW wc = { 0 };
    HWND Parent, Child, Popup, Overlapped, Top;
    CTX Prev;
    int Prev1;
    char B1[64], B2[64], B3[64];
    BOOL EnableChild, EnablePopup, EnableOverlapped;

    wc.lpfnWndProc = DpiWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"DpiTestClass";
    RegisterClassW(&wc);

    Parent = CreateWindowExW(0, L"DpiTestClass", L"p", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    Prev = pSetThreadDpiAwarenessContext(Token(Arg));
    Child = CreateWindowExW(0, L"DpiTestClass", L"c", WS_CHILD, 0, 0, 10, 10, Parent, NULL, wc.hInstance, NULL);
    EnableChild = NcEnable;
    Popup = CreateWindowExW(0, L"DpiTestClass", L"t", WS_POPUP, 0, 0, 10, 10, Parent, NULL, wc.hInstance, NULL);
    EnablePopup = NcEnable;
    Overlapped = CreateWindowExW(0, L"DpiTestClass", L"o", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, Parent, NULL, wc.hInstance, NULL);
    EnableOverlapped = NcEnable;
    Out("parent under default, thread %s: parent ctx=%s, child ctx=%s enable=%d, owned popup ctx=%s enable=%d\n", Arg,
        CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Parent), B1), CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Child), B2),
        EnableChild, CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Popup), B3), EnablePopup);
    Out("  owned overlapped ctx=%s enable=%d\n", CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Overlapped), B1), EnableOverlapped);
    DestroyWindow(Child);
    DestroyWindow(Popup);
    DestroyWindow(Overlapped);
    DestroyWindow(Parent);
    pSetThreadDpiAwarenessContext(Prev);

    Prev = pSetThreadDpiAwarenessContext(Token(Arg));
    Parent = CreateWindowExW(0, L"DpiTestClass", L"p", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    pSetThreadDpiAwarenessContext(Prev);
    Child = CreateWindowExW(0, L"DpiTestClass", L"c", WS_CHILD, 0, 0, 10, 10, Parent, NULL, wc.hInstance, NULL);
    Overlapped = CreateWindowExW(0, L"DpiTestClass", L"o", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, Parent, NULL, wc.hInstance, NULL);
    Out("parent under %s, thread default: parent ctx=%s, child ctx=%s, owned overlapped ctx=%s enable=%d\n", Arg,
        CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Parent), B1), CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Child), B2),
        CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Overlapped), B3), NcEnable);
    DestroyWindow(Overlapped);
    DestroyWindow(Child);

    /* Mixed hosting changes nothing for a window with a parent; the behaviour of a window is the one of its thread */
    Prev1 = pSetThreadDpiHostingBehavior(1);
    Prev = pSetThreadDpiAwarenessContext(Token("U"));
    Child = CreateWindowExW(0, L"DpiTestClass", L"c", WS_CHILD, 0, 0, 10, 10, Parent, NULL, wc.hInstance, NULL);
    Top = CreateWindowExW(0, L"DpiTestClass", L"t", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    pSetThreadDpiAwarenessContext(Prev);
    Out("mixed hosting, thread unaware: child ctx=%s, top ctx=%s\n", CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Child), B1),
        CtxName((ULONG_PTR)pGetWindowDpiAwarenessContext(Top), B2));
    Out("hosting of the window made under mixed: %d, of its parent: %d, of the child: %d\n", pGetWindowDpiHostingBehavior(Top),
        pGetWindowDpiHostingBehavior(Parent), pGetWindowDpiHostingBehavior(Child));
    pSetThreadDpiHostingBehavior(Prev1);
    DestroyWindow(Top);
    Top = CreateWindowExW(0, L"DpiTestClass", L"t", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    Out("hosting of a window made after: %d\n", pGetWindowDpiHostingBehavior(Top));
    DestroyWindow(Top);
    DestroyWindow(Child);
    DestroyWindow(Parent);
}

/* A process handle that is none */
static void StepBadProcess(void)
{
    int Aw = -77;
    BOOL Ret;
    CTX Ctx;
    char B[64];

    CALL(Ret, pGetProcessDpiAwarenessInternal((HANDLE)(ULONG_PTR)0x4567, &Aw));
    Out("GetProcessDpiAwarenessInternal(bad)=%d aw=%d gle=%s\n", Ret, Aw, GleStr());
    CALL(Ctx, pGetDpiAwarenessContextForProcess((HANDLE)(ULONG_PTR)0x4567));
    Out("GetDpiAwarenessContextForProcess(bad)=%s gle=%s\n", CtxName((ULONG_PTR)Ctx, B), GleStr());
    CALL(Ctx, pGetDpiAwarenessContextForProcess(INVALID_HANDLE_VALUE));
    Out("GetDpiAwarenessContextForProcess(-1)=%s gle=%s\n", CtxName((ULONG_PTR)Ctx, B), GleStr());
}

static void StepLegacy(const char *Arg)
{
    BOOL Ret;
    HRESULT hr;

    if (!strcmp(Arg, "setaware"))
    {
        CALL(Ret, pSetProcessDPIAware());
        Out("SetProcessDPIAware=%d gle=%s\n", Ret, GleStr());
    }
    else if (!strncmp(Arg, "shset", 5))
    {
        hr = pSetProcessDpiAwareness(atoi(Arg + 5));
        Out("SetProcessDpiAwareness(%d)=0x%08lx\n", atoi(Arg + 5), (ULONG)hr);
    }
    else if (!strncmp(Arg, "intset", 6))
    {
        CALL(Ret, pSetProcessDpiAwarenessInternal(atoi(Arg + 6)));
        Out("SetProcessDpiAwarenessInternal(%d)=%d gle=%s\n", atoi(Arg + 6), Ret, GleStr());
    }
    else
        Out("unknown step %s\n", Arg);
}

int main(int argc, char **argv)
{
    HMODULE hUser = LoadLibraryW(L"user32.dll");
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    LOAD(hUser, GetDpiForSystem);
    LOAD(hUser, GetDpiForWindow);
    LOAD(hUser, SetProcessDpiAwarenessContext);
    LOAD(hUser, GetThreadDpiAwarenessContext);
    LOAD(hUser, SetThreadDpiAwarenessContext);
    LOAD(hUser, GetWindowDpiAwarenessContext);
    LOAD(hUser, GetAwarenessFromDpiAwarenessContext);
    LOAD(hUser, AreDpiAwarenessContextsEqual);
    LOAD(hUser, IsValidDpiAwarenessContext);
    LOAD(hUser, EnableNonClientDpiScaling);
    LOAD(hUser, GetDpiFromDpiAwarenessContext);
    LOAD(hUser, GetDpiAwarenessContextForProcess);
    LOAD(hUser, IsProcessDPIAware);
    LOAD(hUser, SetProcessDPIAware);
    LOAD(hUser, GetProcessDpiAwarenessInternal);
    LOAD(hUser, SetProcessDpiAwarenessInternal);
    LOAD(hUser, GetThreadDpiHostingBehavior);
    LOAD(hUser, SetThreadDpiHostingBehavior);
    LOAD(hUser, GetWindowDpiHostingBehavior);
    if (hShcore)
    {
        LOAD(hShcore, GetProcessDpiAwareness);
        LOAD(hShcore, SetProcessDpiAwareness);
    }

    if (!pGetDpiForSystem || !pGetThreadDpiAwarenessContext)
    {
        Out("MISSING exports\n");
        return 2;
    }
    {
        HDC hdc = GetDC(NULL);
        SystemDpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);
    }
    Out("GetDpiForSystem==LOGPIXELSY: %d\n", pGetDpiForSystem() == SystemDpi);

    for (i = 1; i < argc; i++)
    {
        Out("# %s\n", argv[i]);
        if (!strcmp(argv[i], "get")) StepGet();
        else if (!strcmp(argv[i], "funcs")) StepFuncs();
        else if (!strncmp(argv[i], "setp:", 5)) StepSetProcess(argv[i] + 5);
        else if (!strncmp(argv[i], "sett:", 5)) StepSetThread(argv[i] + 5);
        else if (!strncmp(argv[i], "win", 3)) StepWin(argv[i][3] == ':' ? argv[i] + 4 : "");
        else if (!strcmp(argv[i], "hosting")) StepHosting();
        else if (!strncmp(argv[i], "nest:", 5)) StepNest(argv[i] + 5);
        else if (!strcmp(argv[i], "badproc")) StepBadProcess();
        else StepLegacy(argv[i]);
    }
    return 0;
}
