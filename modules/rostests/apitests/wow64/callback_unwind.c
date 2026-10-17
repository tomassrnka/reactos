/* Exercise exceptions across real win32k callbacks on native i386 and WoW64.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <pseh/pseh2.h>

#define TEST_EXCEPTION 0xe042643c
static HWND Inner;
static unsigned Mode, Armed, Entered, LocalCaught, OuterCaught, Finalized;

static LONG WINAPI Unhandled(EXCEPTION_POINTERS *Exception)
{
    printf("UNHANDLED mode=%u code=%08lx entered=%u finalized=%u\n",
           Mode, Exception->ExceptionRecord->ExceptionCode, Entered, Finalized);
    fflush(stdout);
    ExitProcess(70);
    return EXCEPTION_EXECUTE_HANDLER;
}

static VOID RaiseTest(void)
{
    if (Mode == 3) *(volatile ULONG *)0 = 1;
    else RaiseException(TEST_EXCEPTION, 0, 0, NULL);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_WINDOWPOSCHANGING && Armed)
    {
        ++Entered;
        _SEH2_TRY
        {
            if (Mode == 0)
            {
                _SEH2_TRY { RaiseTest(); }
                _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { ++LocalCaught; }
                _SEH2_END;
            }
            else if (Mode == 2 && Window != Inner)
                SetWindowPos(Inner, NULL, 30, 30, 100, 100, SWP_NOZORDER | SWP_NOACTIVATE);
            else RaiseTest();
        }
        _SEH2_FINALLY { ++Finalized; }
        _SEH2_END;
    }
    return DefWindowProcW(Window, Message, Wparam, Lparam);
}

static int Child(unsigned TestMode)
{
    WNDCLASSW Class = {0};
    HWND Window;
    BOOL Wow64 = FALSE, Returned = FALSE;
    DWORD Code = 0;
    Mode = TestMode;
    SetUnhandledExceptionFilter(Unhandled);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    IsWow64Process(GetCurrentProcess(), &Wow64);
    printf("START callback-unwind mode=%u pointer-size=%u wow64=%u\n", Mode, (unsigned)sizeof(void *), Wow64);
    Class.lpfnWndProc = WindowProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"Wow64CallbackUnwind";
    if (!RegisterClassW(&Class)) return 10;
    Window = CreateWindowW(Class.lpszClassName, L"outer", WS_OVERLAPPEDWINDOW,
                           10, 10, 150, 150, NULL, NULL, Class.hInstance, NULL);
    Inner = CreateWindowW(Class.lpszClassName, L"inner", WS_OVERLAPPEDWINDOW,
                          20, 20, 150, 150, NULL, NULL, Class.hInstance, NULL);
    if (!Window || !Inner) return 11;
    Armed = 1;
    _SEH2_TRY
    {
        SetWindowPos(Window, NULL, 40, 40, 160, 160, SWP_NOZORDER | SWP_NOACTIVATE);
        Returned = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Code = _SEH2_GetExceptionCode();
        ++OuterCaught;
    }
    _SEH2_END;
    Armed = 0;
    printf("RESULT mode=%u returned=%u entered=%u local=%u outer=%u finalized=%u code=%08lx\n",
           Mode, Returned, Entered, LocalCaught, OuterCaught, Finalized, Code);
    /* Reenter win32k twice after unwinding. A stale simulation/SEH frame may
     * otherwise remain invisible until a later syscall or callback. */
    if (!SetWindowPos(Window, NULL, 50, 50, 180, 180, SWP_NOZORDER | SWP_NOACTIVATE) ||
        !DestroyWindow(Inner) || !DestroyWindow(Window) ||
        !UnregisterClassW(Class.lpszClassName, Class.hInstance)) return 12;
    printf("RECOVERY mode=%u passed\n", Mode);
    if (!Entered || (Mode == 0 && (!LocalCaught || OuterCaught || !Returned))) return 13;
    if (Mode != 0 && (!OuterCaught || Returned)) return 15;
    if (OuterCaught && (Code != (Mode == 3 ? EXCEPTION_ACCESS_VIOLATION : TEST_EXCEPTION) ||
                       Finalized != (Mode == 2 ? 2 : 1))) return 14;
    return 0;
}

int main(int argc, char **argv)
{
    char Path[MAX_PATH], Command[MAX_PATH + 64];
    unsigned i, Failures = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2) return Child(atoi(argv[1]));
    GetModuleFileNameA(NULL, Path, sizeof(Path));
    for (i = 0; i < 4; ++i)
    {
        STARTUPINFOA Startup = {sizeof(Startup)};
        PROCESS_INFORMATION Process;
        DWORD ExitCode;
        sprintf(Command, "\"%s\" %u", Path, i);
        if (!CreateProcessA(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Process))
        { printf("FAIL launch mode=%u error=%lu\n", i, GetLastError()); ++Failures; continue; }
        if (WaitForSingleObject(Process.hProcess, 30000) != WAIT_OBJECT_0)
        { printf("TIMEOUT mode=%u\n", i); TerminateProcess(Process.hProcess, 99); WaitForSingleObject(Process.hProcess, 5000); }
        GetExitCodeProcess(Process.hProcess, &ExitCode);
        printf("CHILD mode=%u exit=%08lx\n", i, ExitCode);
        if (ExitCode) ++Failures;
        CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
    }
    printf("SUMMARY callback-unwind failures=%u\n", Failures);
    return Failures ? 1 : 0;
}
