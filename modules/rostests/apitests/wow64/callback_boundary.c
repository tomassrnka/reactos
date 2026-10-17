/* Callback stack growth, floating-point environment and flat message tests.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_NO_STATUS
#include <windows.h>
#include <stdio.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>
#include <imm.h>
#include <immdev.h>
#include <imm32_undoc.h>
#include <ntusrtyp.h>
#include <ntuser.h>

BOOL WINAPI CallbackBoundary(ULONG Stack, HWND Window, UINT Message, PVOID Buffer, LRESULT *Result);
static unsigned Failures, Calls, Settings, TimerCalls;
static HINSTANCE Instance;
static USHORT Control;
static ULONG Mxcsr;
#define CHECK(c) do { if (!(c)) { ++Failures; printf("FAIL line=%u %s error=%lu\n", __LINE__, #c, GetLastError()); } } while (0)
C_ASSERT(FNID_SENDMESSAGE == 0x2b1);

static LRESULT CALLBACK Proc(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_GETTEXT)
    { ++Calls; lstrcpyW((WCHAR *)Lparam, L"boundary"); return 8; }
    if (Message == WM_SETTINGCHANGE)
    { CHECK(lstrcmpW((WCHAR *)Lparam, L"Environment") == 0); ++Settings; return 42; }
    if (Message == WM_APP)
    {
        __asm__ __volatile__("fldcw %0" : : "m"(Control));
        __asm__ __volatile__("ldmxcsr %0" : : "m"(Mxcsr));
        return 1;
    }
    return DefWindowProcW(Window, Message, Wparam, Lparam);
}

static DWORD WINAPI Sweep(PVOID Parameter)
{
    ULONG Offset = (ULONG_PTR)Parameter;
    WCHAR Buffer[1024] = {0}; LRESULT Result = 0;
    HWND Window = CreateWindowW(L"Wow64Boundary", L"", 0, 0, 0, 100, 100, NULL, NULL, Instance, NULL);
    ULONG Limit = (ULONG_PTR)NtCurrentTeb()->NtTib.StackLimit;
    CHECK(Window != NULL);
    if (!Window) return 1;
    CHECK(CallbackBoundary(Limit + Offset, Window, WM_GETTEXT, Buffer, &Result));
    CHECK(Result == 8 && lstrcmpW(Buffer, L"boundary") == 0);
    printf("SWEEP offset=%lu limit=%08lx after=%p result=%ld\n", Offset, Limit, NtCurrentTeb()->NtTib.StackLimit, Result);
    CHECK(DestroyWindow(Window));
    return 0;
}

static void PointerMessages(HWND Window, BOOL Wow64)
{
    BYTE *Page = VirtualAlloc(Wow64 ? (PVOID)0x90000000 : NULL, 4096,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    LRESULT Result = 0;
    MSG Message = {0};
    /* Authored i386 TIMERPROC: mov eax,&TimerCalls; inc dword [eax];
     * xor eax,eax; ret 16. No imported code is inspected or copied. */
    const BYTE Code[] = {0xb8, 0, 0, 0, 0, 0xff, 0x00, 0x31, 0xc0, 0xc2, 0x10, 0x00};
    CHECK(Page != NULL);
    if (!Page) return;
    if (Wow64) CHECK((ULONG_PTR)Page >= 0x80000000);
    CopyMemory(Page, Code, sizeof(Code));
    *(ULONG *)(Page + 1) = (ULONG_PTR)&TimerCalls;
    FlushInstructionCache(GetCurrentProcess(), Page, sizeof(Code));
    lstrcpyW((WCHAR *)(Page + 64), L"Environment");
    CHECK(NtUserMessageCall(Window, WM_SETTINGCHANGE, 0, (LPARAM)(Page + 64),
                           (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE));
    CHECK(Result == 42 && Settings == 2);
    CHECK(NtUserSetTimer(Window, 91, 60000, (TIMERPROC)Page) == 91);
    Message.hwnd = Window; Message.message = WM_TIMER; Message.wParam = 91;
    Message.lParam = (LPARAM)Page; Message.time = GetTickCount();
    NtUserDispatchMessage(&Message);
    CHECK(TimerCalls == 1); CHECK(KillTimer(Window, 91));
    printf("POINTERS address=%p settings=%u timers=%u\n", Page, Settings, TimerCalls);
    CHECK(VirtualFree(Page, 0, MEM_RELEASE));
}

int main(void)
{
    WNDCLASSW Class = {0}; ULONG Offset, OldMxcsr, NewMxcsr;
    USHORT OldControl, NewControl;
    HWND Window; LRESULT Result = 0; BOOL Wow64 = FALSE;
    setvbuf(stdout, NULL, _IONBF, 0);
    IsWow64Process(GetCurrentProcess(), &Wow64);
    printf("START callback-boundary wow64=%u\n", Wow64);
    Instance = GetModuleHandleW(NULL); Class.lpfnWndProc = Proc; Class.hInstance = Instance; Class.lpszClassName = L"Wow64Boundary";
    CHECK(RegisterClassW(&Class));
    for (Offset = 128; Offset <= 4096; Offset += 128)
    {
        HANDLE Thread = CreateThread(NULL, 0, Sweep, (PVOID)(ULONG_PTR)Offset, 0, NULL);
        CHECK(Thread != NULL);
        if (Thread) { CHECK(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0); CloseHandle(Thread); }
    }
    CHECK(Calls == 32);
    Window = CreateWindowW(Class.lpszClassName, L"", 0, 0, 0, 100, 100, NULL, NULL, Instance, NULL);
    CHECK(Window != NULL);
    __asm__ __volatile__("fnstcw %0" : "=m"(OldControl));
    __asm__ __volatile__("stmxcsr %0" : "=m"(OldMxcsr));
    Control = (OldControl & ~0x0c00) | 0x0800;
    Mxcsr = (OldMxcsr & ~0x6000) | 0x4000;
    CHECK(NtUserMessageCall(Window, WM_APP, 0, 0, (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE));
    __asm__ __volatile__("fnstcw %0" : "=m"(NewControl));
    __asm__ __volatile__("stmxcsr %0" : "=m"(NewMxcsr));
    CHECK((NewControl & 0x0c00) == 0x0800); CHECK((NewMxcsr & 0x6000) == 0x4000);
    __asm__ __volatile__("fldcw %0" : : "m"(OldControl));
    __asm__ __volatile__("ldmxcsr %0" : : "m"(OldMxcsr));
    CHECK(NtUserMessageCall(Window, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE));
    CHECK(Result == 42 && Settings == 1);
    PointerMessages(Window, Wow64);
    CHECK(DestroyWindow(Window)); CHECK(UnregisterClassW(Class.lpszClassName, Instance));
    printf("RESULT callback-boundary failures=%u calls=%u settings=%u\n", Failures, Calls, Settings);
    return Failures ? 1 : 0;
}
