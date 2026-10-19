/* A window of one process is moved over a window of another process while that
 * other process holds a DC of its own window. Whatever the holder draws through
 * the DC afterwards is clipped to what is still visible: it must not appear on
 * the window that now covers it. win32k brings the visible region of a DC up to
 * date when a window moves, but it could not lock a DC that another process
 * owned, so the holder's DC kept the visible region from before and the holder
 * drew over the window that had been moved over it. 32-bit Total Commander
 * dragged over 64-bit Total Commander left copies of its path bars on the
 * window this way: the 64-bit process painted while the 32-bit window moved,
 * and every drag step copied the leftovers along.
 * Three kinds of DC are held: a cache DC from GetDC, the DC of a paint in
 * progress (BeginPaint) and the permanent DC of a CS_OWNDC window class. The
 * moved window covers the left of the victim's window only: the drawing must
 * be missing there and still show on the rest, so a DC that is cut off
 * altogether does not pass either. The window that was covered must receive the
 * repaint of what is uncovered.
 * Not WoW64-specific: the PE32 fixture also runs natively and on Windows.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define VICTIM_X 40
#define VICTIM_Y 40
#define VICTIM_W 240
#define VICTIM_H 160
#define MOVER_W  130    /* the moved window covers the left of the victim's: columns 0-2 of the sample grid */
#define AWAY_X   420
#define AWAY_Y   40

#define COLOR_VICTIM RGB(255, 0, 0)
#define COLOR_MOVER  RGB(0, 0, 255)
#define COLOR_DRAWN  RGB(0, 255, 0)

enum { SCENARIO_GETDC = 1, SCENARIO_BEGINPAINT = 2, SCENARIO_OWNDC = 3, SCENARIO_COUNT = 3 };
static const char *const ScenarioName[] = { "", "GetDC", "BeginPaint", "CS_OWNDC" };

static unsigned Checks, Failures;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

typedef struct
{
    LONG Left, Top, Right, Bottom;
    LONG Paints;
} PAINT_REPORT;

/* ---- shared by both processes ---- */

static void ObjectName(char *Buffer, size_t Size, const char *Kind, DWORD Parent, int Scenario)
{
    _snprintf(Buffer, Size, "dc_cover_%s_%lu_%d", Kind, (unsigned long)Parent, Scenario);
    Buffer[Size - 1] = 0;
}

static HANDLE OpenOrCreateEvent(const char *Kind, DWORD Parent, int Scenario)
{
    char Name[80];
    ObjectName(Name, sizeof(Name), Kind, Parent, Scenario);
    return CreateEventA(NULL, FALSE, FALSE, Name);
}

/* Wait for a handle while the thread's windows keep working */
static BOOL WaitPump(HANDLE Handle, DWORD Milliseconds)
{
    DWORD Start = GetTickCount();
    for (;;)
    {
        MSG Msg;
        DWORD Elapsed = GetTickCount() - Start, Wait;
        if (Elapsed >= Milliseconds) return FALSE;
        Wait = MsgWaitForMultipleObjects(1, &Handle, FALSE, Milliseconds - Elapsed, QS_ALLINPUT);
        if (Wait == WAIT_OBJECT_0) return TRUE;
        if (Wait == WAIT_FAILED) return FALSE;
        while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&Msg);
    }
}

static void Pump(DWORD Milliseconds)
{
    DWORD Start = GetTickCount();
    while (GetTickCount() - Start < Milliseconds)
    {
        MSG Msg;
        while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&Msg);
        Sleep(10);
    }
}

/* ---- the process that is covered ---- */

static HWND VictimWindow;
static HANDLE EventHeld, EventMoved, EventDone, EventQuit, EventHold;
static int VictimScenario;
static BOOL HoldInPaint;
static PAINT_REPORT *Report;

static void FillClient(HDC Dc, COLORREF Color)
{
    RECT Client;
    HBRUSH Brush = CreateSolidBrush(Color);
    GetClientRect(VictimWindow, &Client);
    FillRect(Dc, &Client, Brush);
    GdiFlush();
    DeleteObject(Brush);
}

static void ResetReport(void)
{
    if (Report) memset(Report, 0, sizeof(*Report));
}

static void AddReport(const RECT *Rect)
{
    if (!Report) return;
    if (Report->Paints == 0)
    {
        Report->Left = Rect->left; Report->Top = Rect->top; Report->Right = Rect->right; Report->Bottom = Rect->bottom;
    }
    else
    {
        if (Rect->left < Report->Left) Report->Left = Rect->left;
        if (Rect->top < Report->Top) Report->Top = Rect->top;
        if (Rect->right > Report->Right) Report->Right = Rect->right;
        if (Rect->bottom > Report->Bottom) Report->Bottom = Rect->bottom;
    }
    ++Report->Paints;
}

static LRESULT CALLBACK VictimProc(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_ERASEBKGND) return 1;
    if (Message == WM_PAINT)
    {
        PAINTSTRUCT Paint;
        HDC Dc = BeginPaint(Window, &Paint);
        AddReport(&Paint.rcPaint);
        if (HoldInPaint)
        {
            /* The paint of the scenario: it holds its DC until the other window
             * has been moved over this one, then draws */
            HoldInPaint = FALSE;
            SetEvent(EventHeld);
            /* keep the thread serving the other process's messages while it holds the DC */
            WaitPump(EventMoved, 15000);
            FillClient(Dc, COLOR_DRAWN);
            EndPaint(Window, &Paint);
            ResetReport();
            SetEvent(EventDone);
            return 0;
        }
        FillClient(Dc, COLOR_VICTIM);
        EndPaint(Window, &Paint);
        return 0;
    }
    return DefWindowProcA(Window, Message, Wparam, Lparam);
}

static int RunVictim(DWORD Parent, int Scenario)
{
    WNDCLASSA Class;
    HANDLE Mapping;
    char Name[80];
    HDC Held = NULL;
    HANDLE Events[3];
    BOOL Holding = FALSE;
    MSG Msg;

    VictimScenario = Scenario;
    EventHold = OpenOrCreateEvent("hold", Parent, Scenario);
    EventHeld = OpenOrCreateEvent("held", Parent, Scenario);
    EventMoved = OpenOrCreateEvent("moved", Parent, Scenario);
    EventDone = OpenOrCreateEvent("done", Parent, Scenario);
    EventQuit = OpenOrCreateEvent("quit", Parent, Scenario);
    ObjectName(Name, sizeof(Name), "report", Parent, Scenario);
    Mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(PAINT_REPORT), Name);
    Report = Mapping ? MapViewOfFile(Mapping, FILE_MAP_WRITE, 0, 0, sizeof(PAINT_REPORT)) : NULL;
    if (!EventHold || !EventHeld || !EventMoved || !EventDone || !EventQuit || !Report) return 2;
    memset(Report, 0, sizeof(*Report));

    memset(&Class, 0, sizeof(Class));
    Class.style = Scenario == SCENARIO_OWNDC ? CS_OWNDC : 0;
    Class.lpfnWndProc = VictimProc;
    Class.hInstance = GetModuleHandleA(NULL);
    Class.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    Class.lpszClassName = "dc_cover_victim";
    if (!RegisterClassA(&Class)) return 2;
    VictimWindow = CreateWindowA(Class.lpszClassName, "dc_cover victim", WS_POPUP | WS_VISIBLE,
                                 VICTIM_X, VICTIM_Y, VICTIM_W, VICTIM_H, NULL, NULL, Class.hInstance, NULL);
    if (!VictimWindow) return 2;
    UpdateWindow(VictimWindow);
    Pump(300);
    {
        char Ready[80];
        HANDLE ReadyEvent;
        ObjectName(Ready, sizeof(Ready), "ready", Parent, Scenario);
        ReadyEvent = CreateEventA(NULL, FALSE, FALSE, Ready);
        SetEvent(ReadyEvent);
    }

    Events[0] = EventHold; Events[1] = EventMoved; Events[2] = EventQuit;
    for (;;)
    {
        DWORD Wait = MsgWaitForMultipleObjects(3, Events, FALSE, 30000, QS_ALLINPUT);
        if (Wait == WAIT_OBJECT_0 && !Holding)
        {
            Holding = TRUE;
            if (Scenario == SCENARIO_BEGINPAINT)
            {
                /* the next paint is the one that holds its DC; it returns
                 * after the other window has moved and the draw is done */
                HoldInPaint = TRUE;
                InvalidateRect(VictimWindow, NULL, FALSE);
                UpdateWindow(VictimWindow);
                Holding = FALSE;
                continue;
            }
            Held = GetDC(VictimWindow);
            SetEvent(EventHeld);
        }
        else if (Wait == WAIT_OBJECT_0 + 1 && Held)
        {
            FillClient(Held, COLOR_DRAWN);
            if (Scenario != SCENARIO_OWNDC) ReleaseDC(VictimWindow, Held);
            Held = NULL;
            ResetReport();
            SetEvent(EventDone);
        }
        else if (Wait == WAIT_OBJECT_0 + 2 || Wait == WAIT_TIMEOUT || Wait == WAIT_FAILED) break;
        while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&Msg);
    }
    return 0;
}

/* ---- the process that moves ---- */

static HWND MoverWindow;

static LRESULT CALLBACK MoverProc(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_ERASEBKGND) return 1;
    if (Message == WM_PAINT)
    {
        PAINTSTRUCT Paint;
        RECT Client;
        HBRUSH Brush = CreateSolidBrush(COLOR_MOVER);
        HDC Dc = BeginPaint(Window, &Paint);
        GetClientRect(Window, &Client);
        FillRect(Dc, &Client, Brush);
        EndPaint(Window, &Paint);
        DeleteObject(Brush);
        return 0;
    }
    return DefWindowProcA(Window, Message, Wparam, Lparam);
}

typedef enum { PIXEL_OTHER, PIXEL_VICTIM, PIXEL_MOVER, PIXEL_DRAWN } PIXEL_KIND;

static PIXEL_KIND Classify(DWORD Color)
{
    unsigned Red = (Color >> 16) & 0xff, Green = (Color >> 8) & 0xff, Blue = Color & 0xff;
    if (Red > 160 && Green < 100 && Blue < 100) return PIXEL_VICTIM;
    if (Blue > 160 && Red < 100 && Green < 100) return PIXEL_MOVER;
    if (Green > 160 && Red < 100 && Blue < 100) return PIXEL_DRAWN;
    return PIXEL_OTHER;
}

/* Read the screen over the victim's area and classify the pixels at a grid of points */
static BOOL SampleScreen(PIXEL_KIND *Kinds, int Columns, int Rows)
{
    BITMAPINFO Info = {{sizeof(BITMAPINFOHEADER), VICTIM_W, -VICTIM_H, 1, 32, BI_RGB}};
    DWORD *Bits = NULL;
    HDC Screen = GetDC(NULL), Memory = CreateCompatibleDC(Screen);
    HBITMAP Bitmap = CreateDIBSection(Screen, &Info, DIB_RGB_COLORS, (void **)&Bits, NULL, 0);
    HGDIOBJ Old = Bitmap ? SelectObject(Memory, Bitmap) : NULL;
    BOOL Ok = FALSE;
    int Column, Row;
    /* A failed capture leaves every point as not recognised */
    for (Row = 0; Row < Rows * Columns; ++Row) Kinds[Row] = PIXEL_OTHER;
    if (Bits) memset(Bits, 0x80, VICTIM_W * VICTIM_H * 4);
    if (Old && BitBlt(Memory, 0, 0, VICTIM_W, VICTIM_H, Screen, VICTIM_X, VICTIM_Y, SRCCOPY))
    {
        GdiFlush();
        for (Row = 0; Row < Rows; ++Row)
            for (Column = 0; Column < Columns; ++Column)
            {
                int X = 10 + Column * (VICTIM_W - 20) / (Columns - 1), Y = 10 + Row * (VICTIM_H - 20) / (Rows - 1);
                Kinds[Row * Columns + Column] = Classify(Bits[Y * VICTIM_W + X]);
            }
        Ok = TRUE;
    }
    if (Old) SelectObject(Memory, Old);
    if (Bitmap) DeleteObject(Bitmap);
    DeleteDC(Memory);
    ReleaseDC(NULL, Screen);
    return Ok;
}

#define COLUMNS 5
#define ROWS 4

/* With a compositor (Windows Vista and later) every window draws into a surface of its own, a
 * covered window included: nothing is repainted when the covering window leaves. */
static BOOL Composited(void)
{
    HRESULT (WINAPI *IsEnabled)(BOOL *);
    HMODULE Dwm = LoadLibraryA("dwmapi.dll");
    BOOL Enabled = FALSE;
    if (!Dwm) return FALSE;
    IsEnabled = (HRESULT (WINAPI *)(BOOL *))GetProcAddress(Dwm, "DwmIsCompositionEnabled");
    if (!IsEnabled || FAILED(IsEnabled(&Enabled))) Enabled = FALSE;
    FreeLibrary(Dwm);
    return Enabled;
}

static unsigned Count(const PIXEL_KIND *Kinds, PIXEL_KIND Kind)
{
    unsigned i, Total = 0;
    for (i = 0; i < COLUMNS * ROWS; ++i) if (Kinds[i] == Kind) ++Total;
    return Total;
}

/* Columns 0-2 of the grid lie under the moved window, columns 3 and 4 do not */
#define COVERED_COLUMNS 3

static BOOL ColumnsAre(const PIXEL_KIND *Kinds, PIXEL_KIND Covered, PIXEL_KIND Open)
{
    int Row, Column;
    for (Row = 0; Row < ROWS; ++Row)
        for (Column = 0; Column < COLUMNS; ++Column)
            if (Kinds[Row * COLUMNS + Column] != (Column < COVERED_COLUMNS ? Covered : Open)) return FALSE;
    return TRUE;
}

/* One letter per sampled point: V victim, M moved window, D victim's drawing, . anything else */
static const char *Grid(const PIXEL_KIND *Kinds)
{
    static char Text[COLUMNS * ROWS + ROWS + 1];
    int Row, Column, At = 0;
    for (Row = 0; Row < ROWS; ++Row)
    {
        for (Column = 0; Column < COLUMNS; ++Column)
            Text[At++] = ".VMD"[Kinds[Row * COLUMNS + Column]];
        Text[At++] = '/';
    }
    Text[At] = 0;
    return Text;
}

/* The screen shows a window only after that window has painted: sample again for a
 * while until all points are of the wanted kind. The result tells whether the capture
 * worked, not whether the points are of the wanted kind: the caller counts them. */
static BOOL Settle(PIXEL_KIND *Kinds, PIXEL_KIND Wanted)
{
    int Try;
    for (Try = 0; Try < 8; ++Try)
    {
        if (SampleScreen(Kinds, COLUMNS, ROWS) && Count(Kinds, Wanted) == COLUMNS * ROWS) return TRUE;
        Pump(400);
    }
    return SampleScreen(Kinds, COLUMNS, ROWS);
}

/* The same, until the columns under the moved window and the open ones show the wanted kinds */
static BOOL SettleColumns(PIXEL_KIND *Kinds, PIXEL_KIND Covered, PIXEL_KIND Open)
{
    int Try;
    for (Try = 0; Try < 8; ++Try)
    {
        if (SampleScreen(Kinds, COLUMNS, ROWS) && ColumnsAre(Kinds, Covered, Open)) return TRUE;
        Pump(400);
    }
    return SampleScreen(Kinds, COLUMNS, ROWS);
}

static void RunScenario(const char *Self, int Scenario)
{
    DWORD Me = GetCurrentProcessId();
    HANDLE Ready, Hold, Held, Moved, Done, Quit, Mapping;
    PAINT_REPORT *Report32 = NULL;
    char Name[80], Command[MAX_PATH + 40];
    STARTUPINFOA Startup;
    PROCESS_INFORMATION Process;
    PIXEL_KIND Kinds[COLUMNS * ROWS];
    BOOL Started;

    Ready = OpenOrCreateEvent("ready", Me, Scenario);
    Hold = OpenOrCreateEvent("hold", Me, Scenario);
    Held = OpenOrCreateEvent("held", Me, Scenario);
    Moved = OpenOrCreateEvent("moved", Me, Scenario);
    Done = OpenOrCreateEvent("done", Me, Scenario);
    Quit = OpenOrCreateEvent("quit", Me, Scenario);
    ObjectName(Name, sizeof(Name), "report", Me, Scenario);
    Mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(PAINT_REPORT), Name);
    Report32 = Mapping ? MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, sizeof(PAINT_REPORT)) : NULL;
    CHECK(Ready && Hold && Held && Moved && Done && Quit && Report32);
    if (!(Ready && Hold && Held && Moved && Done && Quit && Report32)) return;

    SetWindowPos(MoverWindow, HWND_TOPMOST, AWAY_X, AWAY_Y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    /* An earlier scenario that went wrong may have left the other process's drawing in this
     * window, and moving a window carries its pixels along: start from a repainted window */
    RedrawWindow(MoverWindow, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    _snprintf(Command, sizeof(Command), "\"%s\" victim %lu %d", Self, (unsigned long)Me, Scenario);
    Command[sizeof(Command) - 1] = 0;
    memset(&Startup, 0, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    Started = CreateProcessA(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Process);
    CHECK(Started);
    if (!Started) return;

    CHECK(WaitPump(Ready, 20000));
    Pump(200);
    CHECK(Settle(Kinds, PIXEL_VICTIM));
    printf("%s: victim alone: %u of %u points are the victim's [%s]\n", ScenarioName[Scenario], Count(Kinds, PIXEL_VICTIM), COLUMNS * ROWS, Grid(Kinds));
    CHECK(Count(Kinds, PIXEL_VICTIM) == COLUMNS * ROWS);

    /* The victim takes its DC; this window moves over the victim's window */
    SetEvent(Hold);
    CHECK(WaitPump(Held, 15000));
    SetWindowPos(MoverWindow, HWND_TOPMOST, VICTIM_X, VICTIM_Y, MOVER_W, VICTIM_H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(MoverWindow, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    Pump(300);
    SettleColumns(Kinds, PIXEL_MOVER, PIXEL_VICTIM);
    printf("%s: window moved over the left of the victim [%s]\n", ScenarioName[Scenario], Grid(Kinds));
    CHECK(ColumnsAre(Kinds, PIXEL_MOVER, PIXEL_VICTIM));

    /* The victim now draws through the DC it took before the move */
    SetEvent(Moved);
    CHECK(WaitPump(Done, 15000));
    Pump(500);
    CHECK(SampleScreen(Kinds, COLUMNS, ROWS));
    printf("%s: after the victim drew through its DC [%s]\n", ScenarioName[Scenario], Grid(Kinds));
    /* its drawing must not be on the moved window (M), and must be on what is not covered (D) */
    CHECK(ColumnsAre(Kinds, PIXEL_MOVER, PIXEL_DRAWN));

    /* The moved window leaves: the victim gets the repaint of what was uncovered */
    SetWindowPos(MoverWindow, NULL, AWAY_X, AWAY_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    Pump(800);
    SettleColumns(Kinds, PIXEL_VICTIM, PIXEL_DRAWN);
    printf("%s: window moved away [%s]; victim painted %ld time(s), update rectangle %ld,%ld-%ld,%ld\n",
           ScenarioName[Scenario], Grid(Kinds), Report32->Paints, Report32->Left, Report32->Top, Report32->Right, Report32->Bottom);
    if (Composited())
    {
        /* what shows is the drawing the victim made into its own surface; the part that was
         * covered may be that or the window's own colour */
        CHECK(Count(Kinds, PIXEL_VICTIM) + Count(Kinds, PIXEL_DRAWN) == COLUMNS * ROWS);
    }
    else
    {
        /* the covered part of the window is painted again, all of it; the open part keeps
         * the drawing that was visible there */
        CHECK(ColumnsAre(Kinds, PIXEL_VICTIM, PIXEL_DRAWN));
        CHECK(Report32->Paints >= 1);
        CHECK(Report32->Left <= 10 && Report32->Top <= 10 && Report32->Right >= MOVER_W - 10 && Report32->Bottom >= VICTIM_H - 10);
    }

    SetEvent(Quit);
    if (WaitForSingleObject(Process.hProcess, 10000) != WAIT_OBJECT_0) TerminateProcess(Process.hProcess, 3);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    UnmapViewOfFile(Report32);
    CloseHandle(Mapping);
    CloseHandle(Ready); CloseHandle(Hold); CloseHandle(Held); CloseHandle(Moved); CloseHandle(Done); CloseHandle(Quit);
}

int main(int argc, char **argv)
{
    char Self[MAX_PATH];
    WNDCLASSA Class;
    int Scenario;

    if (argc == 4 && !strcmp(argv[1], "victim"))
        return RunVictim(strtoul(argv[2], NULL, 10), atoi(argv[3]));

    GetModuleFileNameA(NULL, Self, sizeof(Self));
    memset(&Class, 0, sizeof(Class));
    Class.lpfnWndProc = MoverProc;
    Class.hInstance = GetModuleHandleA(NULL);
    Class.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    Class.lpszClassName = "dc_cover_mover";
    CHECK(RegisterClassA(&Class));
    MoverWindow = CreateWindowA(Class.lpszClassName, "dc_cover mover", WS_POPUP | WS_VISIBLE, AWAY_X, AWAY_Y,
                                MOVER_W, VICTIM_H, NULL, NULL, Class.hInstance, NULL);
    CHECK(MoverWindow != NULL);
    if (!MoverWindow) return 1;
    Pump(300);

    for (Scenario = 1; Scenario <= SCENARIO_COUNT; ++Scenario)
        RunScenario(Self, Scenario);

    DestroyWindow(MoverWindow);
    printf("dc_cover: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
