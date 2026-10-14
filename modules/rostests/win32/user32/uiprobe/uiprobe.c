/*
 * PROJECT:     ReactOS Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     UI responsiveness probe. Prints machine-readable lines (median and p95 over
 *              N repetitions) for process creation, cross-process window messages, GDI
 *              fill and copy rates, font enumeration and input-to-paint latency. It uses
 *              only Windows 2000 era APIs and loads user32 and gdi32 at run time, so the
 *              same binary runs on ReactOS and on Windows.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define PROBE_VERSION   1
#define SAMPLE_CAP      4096
#define CHILD_WAIT_MS   120000
#define SEND_TIMEOUT_MS 5000
#define INPUT_WAIT_MS   3000
#define BLIT_FRAMES     8

#define USER_FUNCS(X) \
    X(HWND, FindWindowW, (LPCWSTR, LPCWSTR)) \
    X(HWND, GetDesktopWindow, (void)) \
    X(HWND, GetShellWindow, (void)) \
    X(LRESULT, SendMessageTimeoutW, (HWND, UINT, WPARAM, LPARAM, UINT, UINT, PDWORD_PTR)) \
    X(HDC, GetDC, (HWND)) \
    X(int, ReleaseDC, (HWND, HDC)) \
    X(int, GetSystemMetrics, (int)) \
    X(ATOM, RegisterClassW, (const WNDCLASSW *)) \
    X(BOOL, UnregisterClassW, (LPCWSTR, HINSTANCE)) \
    X(HWND, CreateWindowExW, (DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)) \
    X(BOOL, DestroyWindow, (HWND)) \
    X(LRESULT, DefWindowProcW, (HWND, UINT, WPARAM, LPARAM)) \
    X(BOOL, PeekMessageW, (LPMSG, HWND, UINT, UINT, UINT)) \
    X(BOOL, GetMessageW, (LPMSG, HWND, UINT, UINT)) \
    X(BOOL, TranslateMessage, (const MSG *)) \
    X(LRESULT, DispatchMessageW, (const MSG *)) \
    X(BOOL, PostMessageW, (HWND, UINT, WPARAM, LPARAM)) \
    X(void, PostQuitMessage, (int)) \
    X(DWORD, MsgWaitForMultipleObjects, (DWORD, const HANDLE *, BOOL, DWORD, DWORD)) \
    X(BOOL, InvalidateRect, (HWND, const RECT *, BOOL)) \
    X(HDC, BeginPaint, (HWND, LPPAINTSTRUCT)) \
    X(BOOL, EndPaint, (HWND, const PAINTSTRUCT *)) \
    X(int, FillRect, (HDC, const RECT *, HBRUSH)) \
    X(BOOL, GetClientRect, (HWND, LPRECT)) \
    X(UINT, SendInput, (UINT, LPINPUT, int)) \
    X(void, keybd_event, (BYTE, BYTE, DWORD, ULONG_PTR)) \
    X(HWND, GetForegroundWindow, (void)) \
    X(BOOL, SetForegroundWindow, (HWND)) \
    X(HWND, SetFocus, (HWND)) \
    X(HWND, GetFocus, (void)) \
    X(BOOL, ShowWindow, (HWND, int)) \
    X(BOOL, GetCursorPos, (LPPOINT)) \
    X(BOOL, SetCursorPos, (int, int))

#define GDI_FUNCS(X) \
    X(HDC, CreateCompatibleDC, (HDC)) \
    X(HBITMAP, CreateDIBSection, (HDC, const BITMAPINFO *, UINT, VOID **, HANDLE, DWORD)) \
    X(HGDIOBJ, SelectObject, (HDC, HGDIOBJ)) \
    X(BOOL, DeleteObject, (HGDIOBJ)) \
    X(BOOL, DeleteDC, (HDC)) \
    X(BOOL, BitBlt, (HDC, int, int, int, int, HDC, int, int, DWORD)) \
    X(BOOL, PatBlt, (HDC, int, int, int, int, DWORD)) \
    X(HBRUSH, CreateSolidBrush, (COLORREF)) \
    X(BOOL, GdiFlush, (void)) \
    X(int, GetDeviceCaps, (HDC, int)) \
    X(int, EnumFontFamiliesExW, (HDC, LPLOGFONTW, FONTENUMPROCW, LPARAM, DWORD))

static struct
{
#define X(ret, name, args) ret (WINAPI *name) args;
    USER_FUNCS(X)
    GDI_FUNCS(X)
#undef X
} U;

typedef struct
{
    double *v;
    double *t;
    int n;
    int fail;
    int timeout;
} SAMPLES;

static LARGE_INTEGER g_freq, g_start;
static FILE *g_out;
static char g_label[64];
static int g_n = 15, g_round, g_gapMs = 20;
static BOOL g_raw, g_quiet;
static DWORD g_budgetMs = 60000;
static BOOL g_userLoaded;
static WCHAR g_exe[MAX_PATH];

static double NowMs(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - g_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void Out(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 2] = 0;
    if (!g_quiet)
    {
        fputs(buf, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    if (g_out)
    {
        fputs(buf, g_out);
        fputc('\n', g_out);
        fflush(g_out);
    }
}

static void UtcNow(char *s, size_t cb)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    _snprintf(s, cb, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    s[cb - 1] = 0;
}

static int CmpDouble(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static BOOL SamplesInit(SAMPLES *s)
{
    memset(s, 0, sizeof(*s));
    s->v = malloc(SAMPLE_CAP * sizeof(double));
    s->t = malloc(SAMPLE_CAP * sizeof(double));
    return s->v != NULL && s->t != NULL;
}

static void SamplesAdd(SAMPLES *s, double v)
{
    if (s->n < SAMPLE_CAP)
    {
        s->t[s->n] = NowMs() / 1000.0;
        s->v[s->n++] = v;
    }
}

static void Emit(const char *metric, const char *unit, SAMPLES *s, const char *extra)
{
    char labelField[96] = "";
    double med = 0, p95 = 0, mn = 0, mx = 0;

    if (g_label[0])
        _snprintf(labelField, sizeof(labelField), " label=%s", g_label);
    if (g_raw)
    {
        int k;
        for (k = 0; k < s->n; k++)
            Out("UIPROBE-SAMPLE metric=%s t=%.2f v=%.2f round=%d", metric, s->t[k], s->v[k], g_round);
    }
    if (s->n > 0)
    {
        int rank;
        qsort(s->v, s->n, sizeof(double), CmpDouble);
        med = (s->n & 1) ? s->v[s->n / 2] : (s->v[s->n / 2 - 1] + s->v[s->n / 2]) / 2.0;
        rank = (int)((95.0 * s->n + 99.0) / 100.0);
        if (rank < 1)
            rank = 1;
        p95 = s->v[rank - 1];
        mn = s->v[0];
        mx = s->v[s->n - 1];
    }
    Out("UIPROBE metric=%s unit=%s n=%d fail=%d timeout=%d median=%.2f p95=%.2f min=%.2f max=%.2f%s round=%d t=%.1f%s",
        metric, unit, s->n, s->fail, s->timeout, med, p95, mn, mx, extra ? extra : "",
        g_round, NowMs() / 1000.0, labelField);
    free(s->v);
    free(s->t);
    s->v = s->t = NULL;
}

static void EmitOne(const char *metric, double ms)
{
    SAMPLES s;

    if (!SamplesInit(&s))
        return;
    SamplesAdd(&s, ms);
    Emit(metric, "ms", &s, " phase=setup");
}

static BOOL OverBudget(double t0)
{
    return (NowMs() - t0) > (double)g_budgetMs;
}

static BOOL LoadUserGdi(void)
{
    HMODULE hu, hg;
    int missing = 0;

    if (g_userLoaded)
        return TRUE;
    hu = LoadLibraryW(L"user32.dll");
    hg = LoadLibraryW(L"gdi32.dll");
    if (!hu || !hg)
        return FALSE;
#define X(ret, name, args) \
    U.name = (void *)GetProcAddress(hu, #name); if (!U.name) { Out("UIPROBE-ERROR missing user32!%s", #name); missing++; }
    USER_FUNCS(X)
#undef X
#define X(ret, name, args) \
    U.name = (void *)GetProcAddress(hg, #name); if (!U.name) { Out("UIPROBE-ERROR missing gdi32!%s", #name); missing++; }
    GDI_FUNCS(X)
#undef X
    g_userLoaded = (missing == 0);
    return g_userLoaded;
}

static void Pump(DWORD ms)
{
    double t0 = NowMs();
    MSG msg;

    for (;;)
    {
        while (U.PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            U.TranslateMessage(&msg);
            U.DispatchMessageW(&msg);
        }
        if (NowMs() - t0 >= (double)ms)
            break;
        U.MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    }
}

/* (a) process creation round trip */
static void MeasureCreateProc(const char *metric, const WCHAR *childArg, DWORD flags)
{
    SAMPLES total, create;
    WCHAR cmd[MAX_PATH + 32];
    double t0 = NowMs();
    char extra[64];
    int i;

    if (!SamplesInit(&total) || !SamplesInit(&create))
        return;
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%s\" %s", g_exe, childArg);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    for (i = 0; i < g_n && !OverBudget(t0); i++)
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        double a, b, c;
        DWORD w;

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        a = NowMs();
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi))
        {
            total.fail++;
            create.fail++;
            continue;
        }
        b = NowMs();
        w = WaitForSingleObject(pi.hProcess, CHILD_WAIT_MS);
        c = NowMs();
        if (w != WAIT_OBJECT_0)
        {
            total.timeout++;
            TerminateProcess(pi.hProcess, 1);
        }
        else
        {
            SamplesAdd(&total, c - a);
        }
        SamplesAdd(&create, b - a);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Sleep(g_gapMs);
    }
    _snprintf(extra, sizeof(extra), " phase=roundtrip");
    Emit(metric, "ms", &total, extra);
    _snprintf(extra, sizeof(extra), " phase=create_return");
    {
        char name[64];
        _snprintf(name, sizeof(name), "%s_create", metric);
        name[sizeof(name) - 1] = 0;
        Emit(name, "ms", &create, extra);
    }
}

/* (b) cross-process window round trip */
typedef struct
{
    HWND hwnd;
    HANDLE ready;
} HELPER;

static LRESULT CALLBACK HelperProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE)
    {
        U.DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        U.PostQuitMessage(0);
        return 0;
    }
    return U.DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI HelperThread(LPVOID p)
{
    HELPER *h = p;
    MSG msg;

    h->hwnd = U.CreateWindowExW(0, L"UiProbeHelper", L"uiprobe-helper", WS_POPUP,
                                0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetEvent(h->ready);
    if (!h->hwnd)
        return 1;
    while (U.GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        U.TranslateMessage(&msg);
        U.DispatchMessageW(&msg);
    }
    return 0;
}

static void MeasureSend(const char *metric, int which, HWND fixed)
{
    SAMPLES s;
    double t0 = NowMs();
    DWORD lastErr = 0;
    char extra[96];
    int i;

    if (!SamplesInit(&s))
        return;
    for (i = 0; i < g_n && !OverBudget(t0); i++)
    {
        HWND h = fixed;
        DWORD_PTR res = 0;
        double a, b;
        LRESULT r;

        if (which == 1)
            h = U.FindWindowW(L"Shell_TrayWnd", NULL);
        else if (which == 2)
            h = U.GetDesktopWindow();
        else if (which == 3)
            h = U.GetShellWindow();
        if (!h)
        {
            s.fail++;
            lastErr = ERROR_INVALID_WINDOW_HANDLE;
            continue;
        }
        SetLastError(0);
        a = NowMs();
        r = U.SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, SEND_TIMEOUT_MS, &res);
        b = NowMs();
        if (r)
        {
            SamplesAdd(&s, b - a);
        }
        else
        {
            DWORD e = GetLastError();
            if (e == ERROR_TIMEOUT)
            {
                s.timeout++;
                SamplesAdd(&s, b - a);
            }
            else
            {
                s.fail++;
                lastErr = e;
            }
        }
        Sleep(g_gapMs);
    }
    _snprintf(extra, sizeof(extra), " lasterr=%lu", lastErr);
    Emit(metric, "ms", &s, extra);
}

static void MeasureSends(void)
{
    HELPER h = { NULL, NULL };
    WNDCLASSW wc;
    HANDLE thr;
    double t0;

    MeasureSend("send_tray", 1, NULL);
    MeasureSend("send_desktop", 2, NULL);
    MeasureSend("send_shell", 3, NULL);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HelperProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"UiProbeHelper";
    U.RegisterClassW(&wc);
    h.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    t0 = NowMs();
    thr = CreateThread(NULL, 0, HelperThread, &h, 0, NULL);
    if (thr)
    {
        WaitForSingleObject(h.ready, 10000);
        EmitOne("setup_helper_thread", NowMs() - t0);
        if (h.hwnd)
        {
            MeasureSend("send_ownthread", 0, h.hwnd);
            t0 = NowMs();
            U.PostMessageW(h.hwnd, WM_CLOSE, 0, 0);
        }
        WaitForSingleObject(thr, 5000);
        if (h.hwnd)
            EmitOne("teardown_helper_thread", NowMs() - t0);
        CloseHandle(thr);
    }
    CloseHandle(h.ready);
    U.UnregisterClassW(L"UiProbeHelper", wc.hInstance);
}

/* (c) GDI fill and copy rates */
static BOOL RateOp(HDC dst, HDC src, int w, int h, BOOL isBlit)
{
    if (isBlit)
        return U.BitBlt(dst, 0, 0, w, h, src, 0, 0, SRCCOPY);
    return U.PatBlt(dst, 0, 0, w, h, PATCOPY);
}

static void MeasureRate(const char *metric, HDC dst, HDC src, int w, int h, BOOL isBlit)
{
    SAMPLES s;
    double t0 = NowMs();
    double fps, first;
    char extra[112];
    int i;

    if (!SamplesInit(&s))
        return;
    /* one cold call before the loop (first touch of the DIB pages); its time is reported, not sampled */
    first = NowMs();
    RateOp(dst, src, w, h, isBlit);
    U.GdiFlush();
    first = NowMs() - first;
    for (i = 0; i < g_n && !OverBudget(t0); i++)
    {
        BOOL ok = TRUE;
        int f, done = 0;
        double a = NowMs();

        for (f = 0; f < BLIT_FRAMES; f++)
        {
            ok = RateOp(dst, src, w, h, isBlit);
            done++;
            if (!ok || NowMs() - a > 5000.0)
                break;
        }
        U.GdiFlush();
        if (!ok)
            s.fail++;
        SamplesAdd(&s, (NowMs() - a) / (double)done);
    }
    if (s.n > 0)
    {
        double *tmp = malloc(s.n * sizeof(double));
        double med;

        memcpy(tmp, s.v, s.n * sizeof(double));
        qsort(tmp, s.n, sizeof(double), CmpDouble);
        med = (s.n & 1) ? tmp[s.n / 2] : (tmp[s.n / 2 - 1] + tmp[s.n / 2]) / 2.0;
        fps = med > 0.0 ? 1000.0 / med : 0.0;
        free(tmp);
    }
    else
    {
        fps = 0.0;
    }
    _snprintf(extra, sizeof(extra), " fps=%.2f mpix_s=%.0f size=%dx%d first_ms=%.2f", fps, fps * w * h / 1e6, w, h, first);
    Emit(metric, "ms_per_frame", &s, extra);
}

/* screen to DIB: reads the display surface back; also fills DIB 1, the restore image */
static void MeasureReadback(HDC screen, HDC mem, int w, int h, void *bits)
{
    SAMPLES s;
    char extra[64];
    double med;
    int i, reps = g_n < 3 ? g_n : 3;

    if (!SamplesInit(&s))
        return;
    for (i = 0; i < reps; i++)
    {
        double a = NowMs();

        if (!U.BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY))
        {
            s.fail++;
            continue;
        }
        U.GdiFlush();
        SamplesAdd(&s, NowMs() - a);
    }
    if (s.fail == reps)
        memset(bits, 0x40, (size_t)w * h * 4);
    med = s.n ? s.v[s.n / 2] : 0.0;
    _snprintf(extra, sizeof(extra), " mpix_s=%.1f size=%dx%d", med > 0.0 ? (double)w * h / 1e3 / med : 0.0, w, h);
    Emit("bitblt_screen_to_dib", "ms_per_frame", &s, extra);
}

static void MeasureGdi(void)
{
    HDC screen, mem1, mem2;
    HBITMAP bm1, bm2, old1, old2;
    BITMAPINFO bmi;
    void *bits1 = NULL, *bits2 = NULL;
    HBRUSH brush, oldBrush;
    double t0;
    int w, h;

    t0 = NowMs();
    screen = U.GetDC(NULL);
    if (!screen)
    {
        Out("UIPROBE-ERROR GetDC(NULL) failed %lu", GetLastError());
        return;
    }
    w = U.GetSystemMetrics(SM_CXSCREEN);
    h = U.GetSystemMetrics(SM_CYSCREEN);
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    mem1 = U.CreateCompatibleDC(screen);
    mem2 = U.CreateCompatibleDC(screen);
    bm1 = U.CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits1, NULL, 0);
    bm2 = U.CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits2, NULL, 0);
    if (!mem1 || !mem2 || !bm1 || !bm2)
    {
        Out("UIPROBE-ERROR DIB setup failed %lu", GetLastError());
    }
    else
    {
        old1 = U.SelectObject(mem1, bm1);
        old2 = U.SelectObject(mem2, bm2);
        /* keep the desktop: capture it into DIB 1, which is then the blit source and the restore image */
        EmitOne("setup_gdi_dibs", NowMs() - t0);
        MeasureReadback(screen, mem1, w, h, bits1);
        brush = U.CreateSolidBrush(RGB(32, 64, 128));
        oldBrush = U.SelectObject(screen, brush);
        MeasureRate("bitblt_dib_to_screen", screen, mem1, w, h, TRUE);
        MeasureRate("patblt_screen", screen, NULL, w, h, FALSE);
        U.BitBlt(screen, 0, 0, w, h, mem1, 0, 0, SRCCOPY);
        U.SelectObject(screen, oldBrush);
        U.SelectObject(mem2, brush);
        MeasureRate("bitblt_dib_to_dib", mem2, mem1, w, h, TRUE);
        MeasureRate("patblt_dib", mem2, NULL, w, h, FALSE);
        U.SelectObject(mem2, old2);
        U.DeleteObject(brush);
        U.SelectObject(mem1, old1);
        {
            char info[96];
            _snprintf(info, sizeof(info), "UIPROBE-GDI screen=%dx%d screen_bpp=%d dib_bpp=32",
                      w, h, U.GetDeviceCaps(screen, BITSPIXEL) * U.GetDeviceCaps(screen, PLANES));
            Out("%s", info);
        }
    }
    if (bm1) U.DeleteObject(bm1);
    if (bm2) U.DeleteObject(bm2);
    if (mem1) U.DeleteDC(mem1);
    if (mem2) U.DeleteDC(mem2);
    U.ReleaseDC(NULL, screen);
}

/* (d) font enumeration */
#define MAX_FAMILIES 4096
typedef struct
{
    WCHAR (*names)[LF_FACESIZE];
    int families;
    int faces;
} FONTCTX;

static int CALLBACK FontProc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    FONTCTX *c = (FONTCTX *)lp;
    int i;

    c->faces++;
    for (i = 0; i < c->families; i++)
        if (!wcscmp(c->names[i], lf->lfFaceName))
            return 1;
    if (c->families < MAX_FAMILIES)
    {
        wcsncpy(c->names[c->families], lf->lfFaceName, LF_FACESIZE - 1);
        c->names[c->families][LF_FACESIZE - 1] = 0;
        c->families++;
    }
    return 1;
}

static void MeasureFonts(void)
{
    SAMPLES s;
    FONTCTX ctx;
    HDC dc = U.GetDC(NULL);
    double t0 = NowMs();
    int families = 0, faces = 0, i;
    char extra[64];

    if (!dc || !SamplesInit(&s))
        return;
    ctx.names = malloc(MAX_FAMILIES * sizeof(*ctx.names));
    for (i = 0; i < g_n && !OverBudget(t0); i++)
    {
        LOGFONTW lf;
        double a;

        memset(&lf, 0, sizeof(lf));
        lf.lfCharSet = DEFAULT_CHARSET;
        ctx.families = ctx.faces = 0;
        a = NowMs();
        U.EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)FontProc, (LPARAM)&ctx, 0);
        SamplesAdd(&s, NowMs() - a);
        families = ctx.families;
        faces = ctx.faces;
        Sleep(g_gapMs);
    }
    free(ctx.names);
    U.ReleaseDC(NULL, dc);
    _snprintf(extra, sizeof(extra), " families=%d faces=%d", families, faces);
    Emit("enum_fonts", "ms", &s, extra);
}

/* (e) input to paint */
static volatile int g_armKey, g_armMouse, g_gotKey, g_gotMouse, g_gotPaint, g_colour;
static volatile double g_tKey, g_tMouse, g_tPaint;

static LRESULT CALLBACK ProbeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_KEYDOWN:
            if (g_armKey && !g_gotKey)
            {
                g_tKey = NowMs();
                g_gotKey = 1;
                U.InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (g_armMouse && !g_gotMouse)
            {
                g_tMouse = NowMs();
                g_gotMouse = 1;
                U.InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            RECT rc;
            HBRUSH b;
            HDC dc = U.BeginPaint(hwnd, &ps);

            U.GetClientRect(hwnd, &rc);
            g_colour ^= 0x00FFFFFF;
            b = U.CreateSolidBrush(g_colour & 0x00FF8040);
            U.FillRect(dc, &rc, b);
            U.DeleteObject(b);
            U.EndPaint(hwnd, &ps);
            if ((g_armKey && g_gotKey) || (g_armMouse && g_gotMouse))
                if (!g_gotPaint)
                {
                    g_tPaint = NowMs();
                    g_gotPaint = 1;
                }
            return 0;
        }
    }
    return U.DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL WaitFlag(volatile int *flag, DWORD ms)
{
    double t0 = NowMs();
    MSG msg;

    while (NowMs() - t0 < (double)ms)
    {
        while (U.PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            U.TranslateMessage(&msg);
            U.DispatchMessageW(&msg);
        }
        if (*flag)
            return TRUE;
        U.MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
    }
    return *flag != 0;
}

static void MeasureInput(void)
{
    WNDCLASSW wc;
    HWND hwnd;
    HINSTANCE inst = GetModuleHandleW(NULL);
    SAMPLES kd, kp, md, mp;
    POINT oldPos;
    double t0;
    int i, focus = 0, sw = U.GetSystemMetrics(SM_CXSCREEN), sh = U.GetSystemMetrics(SM_CYSCREEN);
    int x0 = 40, y0 = 40, ww = 320, wh = 240;
    char extra[48];

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"UiProbeInput";
    U.RegisterClassW(&wc);
    t0 = NowMs();
    hwnd = U.CreateWindowExW(WS_EX_TOPMOST, L"UiProbeInput", L"uiprobe", WS_POPUP | WS_VISIBLE,
                             x0, y0, ww, wh, NULL, NULL, inst, NULL);
    EmitOne("setup_window_create", NowMs() - t0);
    if (!hwnd)
    {
        Out("UIPROBE-ERROR CreateWindow failed %lu", GetLastError());
        return;
    }
    if (!SamplesInit(&kd) || !SamplesInit(&kp) || !SamplesInit(&md) || !SamplesInit(&mp))
        return;
    U.GetCursorPos(&oldPos);
    U.ShowWindow(hwnd, SW_SHOW);
    U.keybd_event(VK_MENU, 0, 0, 0);
    U.keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    U.SetForegroundWindow(hwnd);
    U.SetFocus(hwnd);
    Pump(300);
    focus = (U.GetForegroundWindow() == hwnd);
    t0 = NowMs();

    for (i = 0; i < g_n && !OverBudget(t0); i++)
    {
        INPUT in[2];
        double a;
        int px = x0 + (i & 1 ? 40 : 200), py = y0 + (i & 1 ? 40 : 160);

        g_armKey = g_armMouse = 0;
        U.SetCursorPos(x0 + 20, y0 + 20);
        Pump(60);
        /* mouse: absolute move inside the probe window */
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dx = (px * 65535) / (sw > 1 ? sw - 1 : 1);
        in[0].mi.dy = (py * 65535) / (sh > 1 ? sh - 1 : 1);
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        g_gotMouse = g_gotPaint = 0;
        g_armMouse = 1;
        a = NowMs();
        if (U.SendInput(1, in, sizeof(INPUT)) != 1)
        {
            md.fail++;
            mp.fail++;
        }
        else if (!WaitFlag(&g_gotMouse, INPUT_WAIT_MS))
        {
            md.timeout++;
            mp.timeout++;
        }
        else
        {
            SamplesAdd(&md, g_tMouse - a);
            if (WaitFlag(&g_gotPaint, INPUT_WAIT_MS))
                SamplesAdd(&mp, g_tPaint - a);
            else
                mp.timeout++;
        }
        g_armMouse = 0;
        Pump(60);

        /* keyboard: needs the probe window in the foreground; F24 triggers no shell action */
        if (!focus && i >= 3)
        {
            kd.fail++;
            kp.fail++;
            continue;
        }
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_F24;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = VK_F24;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        g_gotKey = g_gotPaint = 0;
        g_armKey = 1;
        a = NowMs();
        if (U.SendInput(2, in, sizeof(INPUT)) != 2)
        {
            kd.fail++;
            kp.fail++;
        }
        else if (!WaitFlag(&g_gotKey, INPUT_WAIT_MS))
        {
            kd.timeout++;
            kp.timeout++;
        }
        else
        {
            SamplesAdd(&kd, g_tKey - a);
            if (WaitFlag(&g_gotPaint, INPUT_WAIT_MS))
                SamplesAdd(&kp, g_tPaint - a);
            else
                kp.timeout++;
        }
        g_armKey = 0;
        Pump(g_gapMs);
    }
    _snprintf(extra, sizeof(extra), " focus=%d", focus);
    Emit("input_mouse_to_event", "ms", &md, extra);
    Emit("input_mouse_to_paint", "ms", &mp, extra);
    Emit("input_key_to_event", "ms", &kd, extra);
    Emit("input_key_to_paint", "ms", &kp, extra);
    U.SetCursorPos(oldPos.x, oldPos.y);
    t0 = NowMs();
    U.DestroyWindow(hwnd);
    EmitOne("teardown_window_destroy", NowMs() - t0);
    Pump(50);
    U.UnregisterClassW(L"UiProbeInput", inst);
}

static BOOL RtlVersion(DWORD *maj, DWORD *min, DWORD *build)
{
    typedef LONG (WINAPI *PFN)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi;
    PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!fn || fn(&vi) != 0)
        return FALSE;
    *maj = vi.dwMajorVersion;
    *min = vi.dwMinorVersion;
    *build = vi.dwBuildNumber;
    return TRUE;
}

static void Usage(void)
{
    puts("uiprobe [-n N] [-only abcde] [-loop SECONDS] [-label NAME] [-o FILE] [-gap MS] [-budget MS] [-raw] [-q]\n"
         "  a process creation (detached console-less child, GUI-thread child)\n"
         "  b SendMessageTimeout(WM_NULL, SMTO_ABORTIFHUNG, 5000) to tray, desktop, shell, own thread\n"
         "  c BitBlt and PatBlt rates (screen and DIB-to-DIB)\n"
         "  d EnumFontFamiliesEx over all fonts\n"
         "  e input to event and input to paint (SendInput mouse and key)\n"
         "  -raw also prints every sample as UIPROBE-SAMPLE; -q writes the -o file only (no console output).\n"
         "  Summary lines start with UIPROBE;\n"
         "  the bad tail is p95 (ms metrics) or max; rate metrics carry an fps field.");
}

int _cdecl wmain(int argc, wchar_t **argv)
{
    const char *only = "abcde";
    DWORD loopSec = 0, maj = 0, min = 0, build = 0;
    char utc[40], onlyBuf[16], label[64] = "";
    double tLoop;
    int i;

    if (argc >= 2 && !wcscmp(argv[1], L"-child"))
        return 0;
    if (argc >= 2 && !wcscmp(argv[1], L"-childgui"))
    {
        MSG msg;
        if (!LoadUserGdi())
            return 3;
        U.PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
        return 0;
    }

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    GetModuleFileNameW(NULL, g_exe, ARRAYSIZE(g_exe));
    for (i = 1; i < argc; i++)
    {
        const wchar_t *a = argv[i];
        const wchar_t *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (!wcscmp(a, L"-n") && v)
            g_n = _wtoi(argv[++i]);
        else if (!wcscmp(a, L"-loop") && v)
            loopSec = (DWORD)_wtoi(argv[++i]);
        else if (!wcscmp(a, L"-gap") && v)
            g_gapMs = _wtoi(argv[++i]);
        else if (!wcscmp(a, L"-budget") && v)
            g_budgetMs = (DWORD)_wtoi(argv[++i]);
        else if (!wcscmp(a, L"-raw"))
            g_raw = TRUE;
        else if (!wcscmp(a, L"-q"))
            g_quiet = TRUE;
        else if (!wcscmp(a, L"-label") && v)
        {
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, label, sizeof(label) - 1, NULL, NULL);
            label[sizeof(label) - 1] = 0;
        }
        else if (!wcscmp(a, L"-only") && v)
        {
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, onlyBuf, sizeof(onlyBuf) - 1, NULL, NULL);
            onlyBuf[sizeof(onlyBuf) - 1] = 0;
            only = onlyBuf;
        }
        else if (!wcscmp(a, L"-o") && v)
        {
            g_out = _wfopen(argv[++i], L"a");
            if (!g_out)
                return 2;
        }
        else
        {
            Usage();
            return 2;
        }
    }
    if (g_n < 1 || g_n > SAMPLE_CAP)
    {
        Usage();
        return 2;
    }
    strcpy(g_label, label);

    UtcNow(utc, sizeof(utc));
    RtlVersion(&maj, &min, &build);
    Out("UIPROBE-INFO version=%d os=%lu.%lu.%lu arch=%s n=%d only=%s loop=%lu started=%s%s%s",
        PROBE_VERSION, maj, min, build,
#ifdef _WIN64
        "x64",
#else
        "x86",
#endif
        g_n, only, loopSec, utc, g_label[0] ? " label=" : "", g_label);

    if (!LoadUserGdi())
    {
        Out("UIPROBE-ERROR cannot load user32 and gdi32 functions");
        return 3;
    }

    tLoop = NowMs();
    do
    {
        g_round++;
        if (strchr(only, 'a'))
        {
            MeasureCreateProc("createproc_nogui", L"-child", DETACHED_PROCESS);
            MeasureCreateProc("createproc_guithread", L"-childgui", DETACHED_PROCESS);
        }
        if (strchr(only, 'b'))
            MeasureSends();
        if (strchr(only, 'c'))
            MeasureGdi();
        if (strchr(only, 'd'))
            MeasureFonts();
        if (strchr(only, 'e'))
            MeasureInput();
    }
    while (loopSec && (NowMs() - tLoop) < loopSec * 1000.0);

    UtcNow(utc, sizeof(utc));
    Out("UIPROBE-END rounds=%d elapsed_s=%.1f ended=%s", g_round, NowMs() / 1000.0, utc);
    if (g_out)
        fclose(g_out);
    return 0;
}
