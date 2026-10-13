/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The no 3D acceleration notice across d3d9.dll unloads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define COBJMACROS
#include <winuser.h>
#include <d3d9.h>
#include <wchar.h>

typedef IDirect3D9 *(WINAPI *PFN_DIRECT3DCREATE9)(UINT);

static BOOL CALLBACK find_notice_proc(HWND hwnd, LPARAM lparam)
{
    WCHAR title[64];
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)
        && GetWindowTextW(hwnd, title, ARRAYSIZE(title))
        && !wcscmp(title, L"ReactOS - no 3D acceleration"))
    {
        *(HWND *)lparam = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND wait_for_notice(DWORD timeout)
{
    DWORD start = GetTickCount();
    HWND notice = NULL;

    do
    {
        EnumWindows(find_notice_proc, (LPARAM)&notice);
        if (notice)
            break;
        Sleep(100);
    } while (GetTickCount() - start < timeout);
    return notice;
}

/* Loads d3d9.dll, calls Direct3DCreate9 once and unloads it again. */
static BOOL create_d3d9_once(void)
{
    PFN_DIRECT3DCREATE9 pDirect3DCreate9;
    IDirect3D9 *d3d9;
    HMODULE module;

    module = LoadLibraryW(L"d3d9.dll");
    ok(module != NULL, "d3d9.dll not loaded, error %lu\n", GetLastError());
    if (!module)
        return FALSE;
    pDirect3DCreate9 = (PFN_DIRECT3DCREATE9)GetProcAddress(module, "Direct3DCreate9");
    ok(pDirect3DCreate9 != NULL, "Direct3DCreate9 not exported\n");
    if (pDirect3DCreate9 && (d3d9 = pDirect3DCreate9(D3D_SDK_VERSION)))
        IDirect3D9_Release(d3d9);
    FreeLibrary(module);
    return pDirect3DCreate9 != NULL;
}

static void close_notice(HWND notice)
{
    DWORD tid, start = GetTickCount();
    HANDLE thread;

    tid = GetWindowThreadProcessId(notice, NULL);
    thread = OpenThread(SYNCHRONIZE, FALSE, tid);
    ok(thread != NULL, "OpenThread failed, error %lu\n", GetLastError());

    PostMessageW(notice, WM_COMMAND, IDOK, 0);
    while (IsWindow(notice) && GetTickCount() - start < 5000)
        Sleep(50);
    ok(!IsWindow(notice), "the notice did not close\n");

    /* The thread returns from MessageBoxW into its module and exits. */
    if (thread)
    {
        ok(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0, "the notice thread did not exit\n");
        CloseHandle(thread);
    }
}

static void test_notice_thread(void)
{
    HWND notice;

    if (!create_d3d9_once())
        return;

    /* ReactOS shows the notice when wined3d finds no OpenGL 2.0 driver; Windows never does. */
    notice = wait_for_notice(10000);
    if (!notice)
    {
        skip("Direct3DCreate9 showed no notice\n");
        return;
    }

    /* The notice thread runs code of d3dwine.dll, which d3d9.dll loaded. */
    ok(GetModuleHandleW(L"d3dwine.dll") != NULL, "d3dwine.dll was unloaded while its notice is shown\n");

    /* Before the fix the thread returned into the unmapped module here. */
    close_notice(notice);

    ok(create_d3d9_once(), "second Direct3DCreate9 round failed\n");
    notice = wait_for_notice(5000);
    ok(notice == NULL, "the notice was shown a second time in one process\n");
    if (notice)
        close_notice(notice);
}

START_TEST(NoticeThread)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    char cmdline[MAX_PATH + 64];
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3)
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        test_notice_thread();
        return;
    }

    /* A child process, so that no other test has d3dwine.dll loaded already
     * and a crash is reported as a failure. */
    sprintf(cmdline, "\"%s\" NoticeThread child", argv[0]);
    ok(CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi),
       "CreateProcessA failed, error %lu\n", GetLastError());
    if (!pi.hProcess)
        return;
    if (WaitForSingleObject(pi.hProcess, 90000) != WAIT_OBJECT_0)
    {
        ok(0, "the child process hung\n");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    else
        wait_child_process(pi.hProcess);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}
