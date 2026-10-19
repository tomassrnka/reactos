/* Native UI driver for the installed PE32 applications.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>

static DWORD TargetPid;
static HWND Found;
static BOOL WantDialog;
static const WCHAR *WantClass;

static BOOL CALLBACK FindTop(HWND Window, LPARAM Param)
{
    DWORD Pid; WCHAR Class[128];
    GetWindowThreadProcessId(Window, &Pid); GetClassNameW(Window, Class, ARRAYSIZE(Class));
    if (Pid == TargetPid && IsWindowVisible(Window) &&
        (WantDialog ? !lstrcmpW(Class, L"#32770") : lstrcmpW(Class, L"#32770") != 0))
    { Found = Window; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK FindChild(HWND Window, LPARAM Param)
{
    WCHAR Class[128]; GetClassNameW(Window, Class, ARRAYSIZE(Class));
    if (!lstrcmpiW(Class, WantClass)) { Found = Window; return FALSE; }
    return TRUE;
}

static HWND WaitWindow(HANDLE Process, BOOL Dialog)
{
    DWORD Start = GetTickCount();
    WantDialog = Dialog;
    do
    {
        Found = NULL; EnumWindows(FindTop, 0);
        if (Found) return Found;
        if (WaitForSingleObject(Process, 100) == WAIT_OBJECT_0) break;
    } while (GetTickCount() - Start < 20000);
    return NULL;
}

static VOID Key(WORD Vk, BOOL Up)
{
    INPUT Input = {0}; Input.type = INPUT_KEYBOARD; Input.ki.wVk = Vk;
    Input.ki.dwFlags = Up ? KEYEVENTF_KEYUP : 0; SendInput(1, &Input, sizeof(Input));
}

static int ForeignClass(void)
{
    WCHAR Command[] = L"C:\\harness\\wow64_gui_controls.exe foreign";
    STARTUPINFOW Startup = {sizeof(Startup)}; PROCESS_INFORMATION Process;
    HWND Window = CreateWindowW(L"SCROLLBAR", L"WoW64 native scrollbar", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    DWORD Start = GetTickCount(), Code = 1; MSG Message;
    if (!Window) return 1;
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    Startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    Startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Process))
    { DestroyWindow(Window); return 1; }
    while (WaitForSingleObject(Process.hProcess, 10) == WAIT_TIMEOUT && GetTickCount() - Start < 20000)
        while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
        { TranslateMessage(&Message); DispatchMessageW(&Message); }
    GetExitCodeProcess(Process.hProcess, &Code);
    if (Code == STILL_ACTIVE) { TerminateProcess(Process.hProcess, 1); Code = 1; }
    CloseHandle(Process.hThread); CloseHandle(Process.hProcess); DestroyWindow(Window);
    printf("RESULT native-owner child-exit=%lu\n", Code);
    return Code;
}

int main(int argc, char **argv)
{
    WCHAR Path[MAX_PATH], Command[MAX_PATH + 8], Text[256];
    STARTUPINFOW Startup = {sizeof(Startup)}; PROCESS_INFORMATION Process;
    HWND Window, Edit, Dialog; DWORD Code, Tid; DWORD_PTR Result;
    BOOL Regedit = argc > 1 && !lstrcmpA(argv[1], "regedit"), Wow64 = FALSE;
    const WCHAR *Typed = L"WoW64 round 2 typing";
    unsigned i, Failures = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !lstrcmpA(argv[1], "foreign")) return ForeignClass();
    GetWindowsDirectoryW(Path, ARRAYSIZE(Path));
    lstrcatW(Path, argc > 2 ? L"\\system32\\" : L"\\SysWOW64\\");
    lstrcatW(Path, Regedit ? L"regedit.exe" : L"notepad.exe");
    wsprintfW(Command, L"\"%s\"", Path);
    if (!CreateProcessW(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Process))
    { printf("FAIL app launch error=%lu\n", GetLastError()); return 1; }
    TargetPid = Process.dwProcessId; IsWow64Process(Process.hProcess, &Wow64);
    printf("APP START %s pid=%lu wow64=%u\n", Regedit ? "regedit" : "notepad", TargetPid, Wow64);
    Window = WaitWindow(Process.hProcess, FALSE);
    if (!Window) { ++Failures; printf("FAIL no main window\n"); goto Cleanup; }
    Tid = GetWindowThreadProcessId(Window, NULL);
    SetForegroundWindow(Window); ShowWindow(Window, SW_RESTORE);
    printf("APP OPEN hwnd=%p menus=%d\n", Window, GetMenuItemCount(GetMenu(Window)));
    if (!GetMenu(Window) || GetMenuItemCount(GetMenu(Window)) < 1) ++Failures;
    WantClass = L"Edit"; Found = NULL; EnumChildWindows(Window, FindChild, 0); Edit = Found;
    if (!Edit) { ++Failures; printf("FAIL no edit control\n"); }
    else
    {
        AttachThreadInput(GetCurrentThreadId(), Tid, TRUE); SetFocus(Edit);
        SendMessageW(Edit, EM_SETSEL, 0, -1);
        for (i = 0; Typed[i]; ++i)
        {
            INPUT Input[2] = {{0}, {0}};
            Input[0].type = Input[1].type = INPUT_KEYBOARD;
            Input[0].ki.wScan = Input[1].ki.wScan = Typed[i];
            Input[0].ki.dwFlags = KEYEVENTF_UNICODE; Input[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            SendInput(2, Input, sizeof(INPUT));
        }
        AttachThreadInput(GetCurrentThreadId(), Tid, FALSE); Sleep(1000);
        Text[0] = 0;
        if (!SendMessageTimeoutW(Edit, WM_GETTEXT, ARRAYSIZE(Text), (LPARAM)Text, SMTO_ABORTIFHUNG, 5000, &Result) || lstrcmpW(Text, Typed))
        { ++Failures; printf("FAIL typed text mismatch result=%Iu error=%lu chars=", Result, GetLastError()); for (i=0; Text[i] && i<64; ++i) printf("%04x ", Text[i]); printf("\n"); }
        SendMessageW(Edit, EM_SETMODIFY, FALSE, 0);
        printf("APP TYPED hwnd=%p failures=%u\n", Edit, Failures);
    }
    Sleep(4000);
    SetForegroundWindow(Window); Key(VK_MENU, FALSE); Key('F', FALSE); Key('F', TRUE); Key(VK_MENU, TRUE);
    printf("APP MENU READY\n"); Sleep(3000); Key(VK_ESCAPE, FALSE); Key(VK_ESCAPE, TRUE); Sleep(250);
    PostMessageW(Window, WM_COMMAND, Regedit ? 32789 : 0x102, 0);
    Dialog = WaitWindow(Process.hProcess, TRUE);
    if (!Dialog) { ++Failures; printf("FAIL no file dialog\n"); }
    else
    {
        printf("APP DIALOG READY hwnd=%p\n", Dialog); Sleep(5000);
        PostMessageW(Dialog, WM_COMMAND, IDCANCEL, 0); Sleep(1000);
    }
    PostMessageW(Window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(Process.hProcess, 10000) != WAIT_OBJECT_0)
    { ++Failures; printf("FAIL app did not close\n"); }
Cleanup:
    GetExitCodeProcess(Process.hProcess, &Code);
    if (Code == STILL_ACTIVE) { TerminateProcess(Process.hProcess, 99); WaitForSingleObject(Process.hProcess, 5000); }
    else if (Code) { ++Failures; printf("FAIL app exit=%08lx\n", Code); }
    printf("RESULT app=%s failures=%u exit=%08lx\n", Regedit ? "regedit" : "notepad", Failures, Code);
    CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
    return Failures ? 1 : 0;
}
