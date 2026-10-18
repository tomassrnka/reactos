/* Owner-drawn menu items of a 32-bit window. win32k sends WM_MEASUREITEM
 * and WM_DRAWITEM from its menu code through the window-procedure callback;
 * the records carry pointer-sized itemData and HWND/HDC/HMENU handles.
 * 32-bit Total Commander owner-draws its menu bar, which had no row at all.
 * Same results on native i386.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>

#define BAR_ID 100
#define POPUP_ID 200
#define BAR_DATA 0x7a5b3c01
#define POPUP_DATA 0x7a5b3c02
#define ITEM_WIDTH 60
#define ITEM_HEIGHT 23
#define SEND_ID 300
#define SEND_DATA 0x7a5b3c03
static unsigned Checks, Failures;
static HMENU Bar, Popup;
static unsigned BarMeasured, PopupMeasured, BadMeasure, BarDrawn, PopupDrawn, BadDraw;
static LONG PopupDrawHeight;
static unsigned SendMeasured, SendDrawn;
static HWND Window, Other;
static HANDLE Ready;
static HDC SendDc;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

static LRESULT CALLBACK WindowProc(HWND Wnd, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_MEASUREITEM)
    {
        MEASUREITEMSTRUCT *Item = (MEASUREITEMSTRUCT *)Lparam;
        if (Item->CtlType == ODT_MENU && Item->itemID == BAR_ID && Item->itemData == BAR_DATA) ++BarMeasured;
        else if (Item->CtlType == ODT_LISTBOX && Item->CtlID == 7 && Item->itemID == SEND_ID &&
                 Item->itemData == SEND_DATA && Wnd == Other)
        {
            /* The whole record is the reply */
            ++SendMeasured;
            Item->itemData = SEND_DATA + 1;
        }
        else if (Item->CtlType == ODT_MENU && Item->itemID == POPUP_ID && Item->itemData == POPUP_DATA) ++PopupMeasured;
        else
        {
            printf("MEASURE type=%u id=%u data=%p\n", Item->CtlType, Item->itemID, (PVOID)Item->itemData);
            ++BadMeasure;
        }
        Item->itemWidth = ITEM_WIDTH;
        Item->itemHeight = ITEM_HEIGHT;
        return TRUE;
    }
    if (Message == WM_DRAWITEM)
    {
        DRAWITEMSTRUCT *Item = (DRAWITEMSTRUCT *)Lparam;
        DWORD Type = GetObjectType(Item->hDC);
        BOOL Sane = Item->CtlType == ODT_MENU && (Type == OBJ_DC || Type == OBJ_MEMDC) &&
                    Item->rcItem.right > Item->rcItem.left && Item->rcItem.bottom > Item->rcItem.top;
        if (Sane && Item->itemID == BAR_ID && Item->itemData == BAR_DATA && (HMENU)Item->hwndItem == Bar)
            ++BarDrawn;
        else if (Item->CtlType == ODT_LISTBOX && Item->CtlID == 7 && Item->itemID == SEND_ID &&
                 Item->itemData == SEND_DATA && Item->hwndItem == Window && Item->hDC == SendDc &&
                 Item->itemAction == ODA_DRAWENTIRE && Item->itemState == ODS_SELECTED &&
                 Item->rcItem.left == 1 && Item->rcItem.top == 2 && Item->rcItem.right == 3 &&
                 Item->rcItem.bottom == 4 && Wnd == Other)
        {
            ++SendDrawn;
            return TRUE;
        }
        else if (Sane && Item->itemID == POPUP_ID && Item->itemData == POPUP_DATA && (HMENU)Item->hwndItem == Popup)
        {
            ++PopupDrawn;
            PopupDrawHeight = Item->rcItem.bottom - Item->rcItem.top;
            EndMenu();
        }
        else
        {
            printf("DRAW type=%u id=%u data=%p menu=%p dc=%p/%lu rect=%ld,%ld,%ld,%ld\n", Item->CtlType,
                   Item->itemID, (PVOID)Item->itemData, Item->hwndItem, Item->hDC, Type,
                   Item->rcItem.left, Item->rcItem.top, Item->rcItem.right, Item->rcItem.bottom);
            ++BadDraw;
        }
        FillRect(Item->hDC, &Item->rcItem, GetSysColorBrush(COLOR_MENU));
        return TRUE;
    }
    if (Message == WM_TIMER)
    {
        /* Never leave the popup open, whatever happens to its items */
        KillTimer(Wnd, 1);
        EndMenu();
        return 0;
    }
    return DefWindowProcA(Wnd, Message, Wparam, Lparam);
}

/* A window on another thread: sends to it go through win32k */
static DWORD WINAPI Receiver(LPVOID Parameter)
{
    MSG Msg;
    Other = CreateWindowA("Wow64OwnerDraw", "other", WS_OVERLAPPED, 0, 0, 50, 50, NULL, NULL,
                          (HINSTANCE)Parameter, NULL);
    SetEvent(Ready);
    if (!Other) return 1;
    while (GetMessageA(&Msg, NULL, 0, 0) > 0) DispatchMessageA(&Msg);
    return 0;
}

static void Pump(void)
{
    MSG Msg;
    DWORD Start = GetTickCount();
    while (GetTickCount() - Start < 500)
    {
        while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&Msg);
        Sleep(10);
    }
}

int main(void)
{
    WNDCLASSA Class = {0};
    MEASUREITEMSTRUCT Measure = {0};
    DRAWITEMSTRUCT Draw = {0};
    HANDLE Thread;
    RECT WindowRect, ItemRect;
    POINT Client = {0, 0};
    BOOL Wow64 = FALSE;
    LONG Top;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* A PE32 fixture: WoW64 on amd64, the native control on i386 */
    CHECK(sizeof(void *) == 4);
    CHECK(IsWow64Process(GetCurrentProcess(), &Wow64));
    printf("START ownerdraw wow64=%u\n", Wow64);
    Class.lpfnWndProc = WindowProc;
    Class.hInstance = GetModuleHandleA(NULL);
    Class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    Class.lpszClassName = "Wow64OwnerDraw";
    CHECK(RegisterClassA(&Class) != 0);
    Bar = CreateMenu();
    Popup = CreatePopupMenu();
    CHECK(Bar && Popup);
    CHECK(AppendMenuA(Popup, MF_OWNERDRAW, POPUP_ID, (LPCSTR)(ULONG_PTR)POPUP_DATA));
    CHECK(AppendMenuA(Bar, MF_OWNERDRAW, BAR_ID, (LPCSTR)(ULONG_PTR)BAR_DATA));
    CHECK(AppendMenuA(Bar, MF_POPUP | MF_STRING, (UINT_PTR)Popup, "Popup"));
    Window = CreateWindowA(Class.lpszClassName, "ownerdraw", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           40, 40, 300, 200, NULL, Bar, Class.hInstance, NULL);
    CHECK(Window != NULL);
    if (!Window) goto Done;
    UpdateWindow(Window);
    DrawMenuBar(Window);
    Pump();

    /* The bar takes a row between the caption and the client area */
    CHECK(GetWindowRect(Window, &WindowRect));
    CHECK(ClientToScreen(Window, &Client));
    Top = Client.y - WindowRect.top;
    printf("BAR client-top=%ld caption+frame=%d menu=%d\n", Top,
           GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME), GetSystemMetrics(SM_CYMENU));
    CHECK(Top >= GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYMENU));
    CHECK(GetMenuItemRect(Window, Bar, 0, &ItemRect));
    printf("BAR item=%ld,%ld,%ld,%ld\n", ItemRect.left, ItemRect.top, ItemRect.right, ItemRect.bottom);
    CHECK(ItemRect.right - ItemRect.left >= ITEM_WIDTH);
    CHECK(ItemRect.bottom > ItemRect.top);
    CHECK(BarMeasured != 0);
    CHECK(BarDrawn != 0);

    /* A popup item is as high as measured */
    SetTimer(Window, 1, 3000, NULL);
    TrackPopupMenuEx(Popup, TPM_RETURNCMD, 60, 60, Window, NULL);
    KillTimer(Window, 1);
    printf("POPUP measured=%u drawn=%u height=%ld\n", PopupMeasured, PopupDrawn, PopupDrawHeight);
    CHECK(PopupMeasured != 0);
    CHECK(PopupDrawn != 0);
    CHECK(PopupDrawHeight == ITEM_HEIGHT);
    CHECK(!BadMeasure && !BadDraw);

    /* Guest sends across threads: widened for win32k, the size comes back */
    Ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, Receiver, Class.hInstance, 0, NULL);
    CHECK(Thread != NULL);
    if (Thread)
    {
        WaitForSingleObject(Ready, 30000);
        CHECK(Other != NULL);
        Measure.CtlType = ODT_LISTBOX; Measure.CtlID = 7; Measure.itemID = SEND_ID;
        Measure.itemData = SEND_DATA;
        CHECK(SendMessageA(Other, WM_MEASUREITEM, 7, (LPARAM)&Measure) == TRUE);
        CHECK(SendMeasured == 1);
        CHECK(Measure.itemWidth == ITEM_WIDTH && Measure.itemHeight == ITEM_HEIGHT);
        CHECK(Measure.itemData == SEND_DATA + 1);
        SendDc = GetDC(Window);
        Draw.CtlType = ODT_LISTBOX; Draw.CtlID = 7; Draw.itemID = SEND_ID;
        Draw.itemAction = ODA_DRAWENTIRE; Draw.itemState = ODS_SELECTED;
        Draw.hwndItem = Window; Draw.hDC = SendDc; SetRect(&Draw.rcItem, 1, 2, 3, 4);
        Draw.itemData = SEND_DATA;
        CHECK(SendMessageA(Other, WM_DRAWITEM, 7, (LPARAM)&Draw) == TRUE);
        CHECK(SendDrawn == 1);
        ReleaseDC(Window, SendDc);
        PostMessageA(Other, WM_QUIT, 0, 0);
        CHECK(WaitForSingleObject(Thread, 30000) == WAIT_OBJECT_0);
    }
    printf("SEND measured=%u drawn=%u size=%ux%u\n", SendMeasured, SendDrawn, Measure.itemWidth, Measure.itemHeight);
    DestroyWindow(Window);
Done:
    printf("OWNERDRAW: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
