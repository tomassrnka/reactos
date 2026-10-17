/* GUI integration checks with the same PE32 fixture on i386 and WoW64.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ddeml.h>
#include <stdio.h>
#include <stdarg.h>

ULONG __cdecl DbgPrint(const char *, ...);
static void Log(const char *Format, ...)
{
    char Buffer[1024]; va_list Args;
    va_start(Args, Format); _vsnprintf(Buffer, sizeof(Buffer) - 1, Format, Args); va_end(Args);
    Buffer[sizeof(Buffer) - 1] = 0;
    fputs(Buffer, stdout); DbgPrint("W643C: %s", Buffer);
}
#define printf Log

static unsigned Failures, TimerCalls, DialogOpened;
#define CHECK(c) do { if (!(c)) { ++Failures; printf("FAIL line=%u %s error=%lu\n", __LINE__, #c, GetLastError()); } } while (0)

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_TIMER) { ++TimerCalls; return 0; }
    return DefWindowProcW(Window, Message, Wparam, Lparam);
}

static HDDEDATA CALLBACK DdeProc(UINT Type, UINT Format, HCONV Conv, HSZ Topic, HSZ Item,
                               HDDEDATA Data, ULONG_PTR Data1, ULONG_PTR Data2)
{
    return NULL;
}

static UINT_PTR CALLBACK DialogHook(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_INITDIALOG)
    {
        ++DialogOpened;
        printf("DIALOG READY hwnd=%p\n", GetParent(Window));
        SetTimer(Window, 77, 5000, NULL);
    }
    if (Message == WM_TIMER)
    {
        KillTimer(Window, 77);
        PostMessageW(GetParent(Window), WM_COMMAND, IDCANCEL, 0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    INITCOMMONCONTROLSEX Init = {sizeof(Init), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES};
    WNDCLASSW Class = {0};
    HWND Window, Edit, List, Combo;
    HMENU Menu, Popup;
    MENUITEMINFOW MenuItem = {sizeof(MenuItem)};
    MENUINFO MenuInfo = {sizeof(MenuInfo)};
    MENUBARINFO MenuBar = {sizeof(MenuBar)};
    LV_COLUMNW Column = {0}; LV_ITEMW Item = {0};
    SCROLLINFO Scroll = {sizeof(Scroll)};
    COMBOBOXINFO ComboInfo = {sizeof(ComboInfo)};
    HGLOBAL Memory; HANDLE Data; WCHAR Text[128];
    MSG Message; DWORD Start, Dde = 0; HSZ Name; BOOL Wow64 = FALSE;
    BITMAPINFO BitmapInfo = {{sizeof(BITMAPINFOHEADER), 4, 4, 1, 32, BI_RGB}};
    DIBSECTION Section; HBITMAP Bitmap; void *Bits = NULL;
    HICON Icon, Copy;
    OPENFILENAMEW Open = {sizeof(Open)};
    WCHAR File[MAX_PATH] = L"";
    setvbuf(stdout, NULL, _IONBF, 0);
    IsWow64Process(GetCurrentProcess(), &Wow64);
    printf("START gui-controls pointer-size=%u wow64=%u\n", (unsigned)sizeof(void *), Wow64);
    if (argc > 1 && !lstrcmpA(argv[1], "foreign"))
    {
        WNDCLASSW Wide; WNDCLASSA Ansi;
        HWND Foreign = FindWindowW(L"SCROLLBAR", L"WoW64 native scrollbar");
        CHECK(Foreign != NULL);
        CHECK(GetClassInfoW(NULL, L"SCROLLBAR", &Wide));
        CHECK(GetClassInfoA(NULL, "SCROLLBAR", &Ansi));
        CHECK(GetClassLongW(Foreign, GCL_WNDPROC) == (ULONG_PTR)Wide.lpfnWndProc);
        CHECK(GetClassLongA(Foreign, GCL_WNDPROC) == (ULONG_PTR)Ansi.lpfnWndProc);
        SetLastError(0);
        CHECK(GetWindowLongW(Foreign, GWL_WNDPROC) == 0);
        CHECK(GetLastError() == ERROR_ACCESS_DENIED);
        printf("RESULT foreign-class failures=%u\n", Failures);
        return Failures ? 1 : 0;
    }
    CHECK(InitCommonControlsEx(&Init));
    Class.lpfnWndProc = WindowProc; Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"Wow64Controls"; Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    CHECK(RegisterClassW(&Class));
    Menu = CreateMenu(); Popup = CreatePopupMenu(); CHECK(Menu && Popup);
    CHECK(AppendMenuW(Popup, MF_STRING, 101, L"&Open..."));
    CHECK(AppendMenuW(Menu, MF_POPUP, (UINT_PTR)Popup, L"&File"));
    MenuItem.fMask = MIIM_DATA | MIIM_ID; MenuItem.dwItemData = 0x87654321; MenuItem.wID = 102;
    CHECK(SetMenuItemInfoW(Popup, 0, TRUE, &MenuItem));
    MenuItem.dwItemData = 0; CHECK(GetMenuItemInfoW(Popup, 0, TRUE, &MenuItem));
    CHECK(MenuItem.dwItemData == 0x87654321 && MenuItem.wID == 102);
    MenuInfo.fMask = MIM_MENUDATA; MenuInfo.dwMenuData = 0x12345678;
    CHECK(SetMenuInfo(Menu, &MenuInfo)); MenuInfo.dwMenuData = 0;
    CHECK(GetMenuInfo(Menu, &MenuInfo)); CHECK(MenuInfo.dwMenuData == 0x12345678);
    printf("MENU DONE failures=%u\n", Failures);
    Window = CreateWindowW(Class.lpszClassName, L"32-bit common controls", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           50, 50, 620, 430, NULL, Menu, Class.hInstance, NULL);
    CHECK(Window != NULL); if (!Window) return 1;
    CHECK(GetMenuBarInfo(Window, OBJID_MENU, 0, &MenuBar));
    CHECK(MenuBar.hMenu == Menu);
    Edit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | WS_VSCROLL,
                         10, 10, 570, 70, Window, (HMENU)1, Class.hInstance, NULL);
    CHECK(Edit != NULL); CHECK(SetWindowTextW(Edit, L"Typed into the 32-bit edit control"));
    CHECK(GetWindowTextW(Edit, Text, ARRAYSIZE(Text)) > 0);
    CHECK(lstrcmpW(Text, L"Typed into the 32-bit edit control") == 0);
    SendMessageW(Edit, EM_SETSEL, -1, -1); SendMessageW(Edit, EM_REPLACESEL, TRUE, (LPARAM)L"\r\nsecond line");
    CHECK(SendMessageW(Edit, EM_GETLINECOUNT, 0, 0) == 2);
    Scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS; Scroll.nMin = 0; Scroll.nMax = 100; Scroll.nPage = 10; Scroll.nPos = 25;
    SetScrollInfo(Window, SB_VERT, &Scroll, TRUE); Scroll.nPos = 0; CHECK(GetScrollInfo(Window, SB_VERT, &Scroll)); CHECK(Scroll.nPos == 25);
    Combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                          10, 90, 250, 100, Window, (HMENU)3, Class.hInstance, NULL);
    CHECK(Combo != NULL); CHECK(SendMessageW(Combo, CB_ADDSTRING, 0, (LPARAM)L"one") == 0);
    CHECK(SendMessageW(Combo, CB_SETCURSEL, 0, 0) == 0); CHECK(GetComboBoxInfo(Combo, &ComboInfo));
    CHECK(ComboInfo.hwndCombo == Combo && ComboInfo.hwndList != NULL);
    List = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT,
                         10, 130, 570, 220, Window, (HMENU)2, Class.hInstance, NULL);
    CHECK(List != NULL);
    Column.mask = LVCF_TEXT | LVCF_WIDTH; Column.pszText = L"Name"; Column.cx = 350;
    CHECK(SendMessageW(List, LVM_INSERTCOLUMNW, 0, (LPARAM)&Column) == 0);
    Item.mask = LVIF_TEXT | LVIF_PARAM; Item.pszText = L"Explorer-style list view"; Item.lParam = 0x12344321;
    CHECK(SendMessageW(List, LVM_INSERTITEMW, 0, (LPARAM)&Item) == 0);
    CHECK(SendMessageW(List, LVM_GETITEMCOUNT, 0, 0) == 1);
    Item.pszText = Text; Item.cchTextMax = ARRAYSIZE(Text); Item.lParam = 0;
    CHECK(SendMessageW(List, LVM_GETITEMW, 0, (LPARAM)&Item));
    CHECK(lstrcmpW(Text, L"Explorer-style list view") == 0 && Item.lParam == 0x12344321);
    printf("CONTROLS DONE failures=%u\n", Failures);
    Icon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); CHECK(Icon != NULL);
    Copy = CopyImage(Icon, IMAGE_ICON, 16, 16, LR_COPYFROMRESOURCE);
    CHECK(Copy != NULL); if (Copy) CHECK(DestroyIcon(Copy));
    CHECK(OpenClipboard(Window)); CHECK(EmptyClipboard());
    Memory = GlobalAlloc(GMEM_MOVEABLE, 64); CHECK(Memory != NULL);
    if (Memory)
    {
        lstrcpyW(GlobalLock(Memory), L"WoW64 clipboard"); GlobalUnlock(Memory);
        CHECK(SetClipboardData(CF_UNICODETEXT, Memory) != NULL);
        Data = GetClipboardData(CF_UNICODETEXT); CHECK(Data != NULL);
        if (Data) { CHECK(lstrcmpW(GlobalLock(Data), L"WoW64 clipboard") == 0); GlobalUnlock(Data); }
    }
    CHECK(CloseClipboard());
    Bitmap = CreateDIBSection(NULL, &BitmapInfo, DIB_RGB_COLORS, &Bits, NULL, 0);
    CHECK(Bitmap != NULL && Bits != NULL);
    if (Bitmap) { CHECK(GetObjectW(Bitmap, sizeof(Section), &Section) == sizeof(Section)); CHECK(Section.dsBm.bmBits == Bits); CHECK(DeleteObject(Bitmap)); }
    CHECK(DdeInitializeW(&Dde, DdeProc, APPCLASS_STANDARD, 0) == DMLERR_NO_ERROR);
    if (Dde)
    {
        Name = DdeCreateStringHandleW(Dde, L"WoW64Dde", CP_WINUNICODE); CHECK(Name != NULL);
        CHECK(DdeQueryStringW(Dde, Name, Text, ARRAYSIZE(Text), CP_WINUNICODE) == 8);
        CHECK(DdeFreeStringHandle(Dde, Name)); CHECK(DdeUninitialize(Dde));
    }
    CHECK(SetTimer(Window, 1, 50, NULL) != 0);
    Start = GetTickCount();
    while (!TimerCalls && GetTickCount() - Start < 10000)
    {
        while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&Message); DispatchMessageW(&Message); }
        Sleep(10);
    }
    CHECK(TimerCalls > 0); KillTimer(Window, 1); UpdateWindow(Window);
    printf("CONTROLS READY failures=%u hwnd=%p timer=%u\n", Failures, Window, TimerCalls);
    Sleep(5000);
    if (argc == 1 || lstrcmpA(argv[1], "nodialog"))
    {
        Open.hwndOwner = Window; Open.lpstrFile = File; Open.nMaxFile = ARRAYSIZE(File);
        Open.lpstrFilter = L"Text files\0*.txt\0All files\0*.*\0";
        Open.lpfnHook = DialogHook; Open.Flags = OFN_EXPLORER | OFN_ENABLEHOOK;
        CHECK(!GetOpenFileNameW(&Open)); CHECK(CommDlgExtendedError() == 0); CHECK(DialogOpened == 1);
        printf("DIALOG DONE opened=%u failures=%u\n", DialogOpened, Failures);
    }
    CHECK(DestroyWindow(Window)); CHECK(UnregisterClassW(Class.lpszClassName, Class.hInstance));
    printf("RESULT gui-controls failures=%u\n", Failures);
    return Failures ? 1 : 0;
}
