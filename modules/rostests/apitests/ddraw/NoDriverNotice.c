/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DirectX without a 3D driver fails quietly: no message box in the calling process
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define COBJMACROS
#include <wingdi.h>
#include <winuser.h>
#include <ddraw.h>
#include <d3d9.h>
#include <stdio.h>
#include <wchar.h>

typedef IDirect3D9 *(WINAPI *PFN_DIRECT3DCREATE9)(UINT);
typedef HRESULT (WINAPI *PFN_D3D11CREATEDEVICE)(IUnknown *, UINT, HMODULE, UINT, const UINT *, UINT, UINT,
                                                IUnknown **, UINT *, IUnknown **);
typedef HRESULT (WINAPI *PFN_DIRECTDRAWCREATEEX)(GUID *, void **, REFIID, IUnknown *);

#define D3D_DRIVER_TYPE_HARDWARE 1
#define D3D11_SDK_VERSION 7

static int CountDialogs(void)
{
    HWND hwnd = NULL;
    WCHAR Class[16];
    DWORD Pid;
    int Count = 0;

    while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)))
    {
        GetWindowThreadProcessId(hwnd, &Pid);
        if (Pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
            continue;
        if (GetClassNameW(hwnd, Class, ARRAYSIZE(Class)) && !wcscmp(Class, L"#32770"))
        {
            WCHAR Title[128] = L"";
            GetWindowTextW(hwnd, Title, ARRAYSIZE(Title));
            trace("Dialog %p '%S'\n", hwnd, Title);
            Count++;
        }
    }
    return Count;
}

static void CloseDialogs(void)
{
    HWND hwnd = NULL;
    WCHAR Class[16];
    DWORD Pid;

    while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)))
    {
        GetWindowThreadProcessId(hwnd, &Pid);
        if (Pid == GetCurrentProcessId() && GetClassNameW(hwnd, Class, ARRAYSIZE(Class)) && !wcscmp(Class, L"#32770"))
            PostMessageW(hwnd, WM_COMMAND, IDOK, 0);
    }
}

/* Waits so that a message box shown from another thread has time to appear */
static int DialogsAfterWait(void)
{
    int Count = 0, i;

    for (i = 0; i < 30 && !Count; i++)
    {
        Sleep(100);
        Count = CountDialogs();
    }
    return Count;
}

static void ProbeDirect3D9(HWND hwnd)
{
    PFN_DIRECT3DCREATE9 pDirect3DCreate9;
    D3DPRESENT_PARAMETERS Params;
    IDirect3DDevice9 *Device = NULL;
    IDirect3D9 *D3D9;
    HMODULE hD3D9;
    HRESULT hr;

    hD3D9 = LoadLibraryW(L"d3d9.dll");
    ok(hD3D9 != NULL, "d3d9.dll not loaded: %lu\n", GetLastError());
    if (!hD3D9)
        return;
    pDirect3DCreate9 = (PFN_DIRECT3DCREATE9)GetProcAddress(hD3D9, "Direct3DCreate9");
    ok(pDirect3DCreate9 != NULL, "No Direct3DCreate9\n");
    if (pDirect3DCreate9 && (D3D9 = pDirect3DCreate9(D3D_SDK_VERSION)))
    {
        ZeroMemory(&Params, sizeof(Params));
        Params.Windowed = TRUE;
        Params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        Params.BackBufferFormat = D3DFMT_UNKNOWN;
        Params.hDeviceWindow = hwnd;
        hr = IDirect3D9_CreateDevice(D3D9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &Params, &Device);
        trace("Direct3D 9: %u adapters, HAL device hr 0x%lx\n", IDirect3D9_GetAdapterCount(D3D9), hr);
        if (SUCCEEDED(hr))
            IDirect3DDevice9_Release(Device);
        IDirect3D9_Release(D3D9);
    }
    else
    {
        trace("Direct3D 9: Direct3DCreate9 returned NULL\n");
    }
    FreeLibrary(hD3D9);
}

static void ProbeDirect3D11(void)
{
    PFN_D3D11CREATEDEVICE pD3D11CreateDevice;
    IUnknown *Device = NULL, *Context = NULL;
    UINT Level = 0;
    HMODULE hD3D11;
    HRESULT hr;

    hD3D11 = LoadLibraryW(L"d3d11.dll");
    if (!hD3D11)
    {
        skip("d3d11.dll not loaded: %lu\n", GetLastError());
        return;
    }
    pD3D11CreateDevice = (PFN_D3D11CREATEDEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    ok(pD3D11CreateDevice != NULL, "No D3D11CreateDevice\n");
    if (pD3D11CreateDevice)
    {
        hr = pD3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                &Device, &Level, &Context);
        trace("Direct3D 11: hardware device hr 0x%lx level 0x%x\n", hr, Level);
        if (Context)
            IUnknown_Release(Context);
        if (Device)
            IUnknown_Release(Device);
    }
    FreeLibrary(hD3D11);
}

static void ProbeDirectDraw7(HWND hwnd)
{
    PFN_DIRECTDRAWCREATEEX pDirectDrawCreateEx;
    IDirectDraw7 *DDraw = NULL;
    HMODULE hDDraw;
    HRESULT hr;

    hDDraw = LoadLibraryW(L"ddraw.dll");
    ok(hDDraw != NULL, "ddraw.dll not loaded: %lu\n", GetLastError());
    if (!hDDraw)
        return;
    pDirectDrawCreateEx = (PFN_DIRECTDRAWCREATEEX)GetProcAddress(hDDraw, "DirectDrawCreateEx");
    if (pDirectDrawCreateEx)
    {
        hr = pDirectDrawCreateEx(NULL, (void **)&DDraw, &IID_IDirectDraw7, NULL);
        trace("DirectDraw 7: hr 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            IDirectDraw7_SetCooperativeLevel(DDraw, hwnd, DDSCL_NORMAL);
            IDirectDraw7_Release(DDraw);
        }
    }
    FreeLibrary(hDDraw);
}

static void TestChild(void)
{
    HWND hwnd;
    int Round, Dialogs;

    hwnd = CreateWindowExA(0, "static", "ddraw_apitest", WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowExA failed: %lu\n", GetLastError());

    /* What a browser's graphics probe does, twice: each round loads and unloads the DLLs */
    for (Round = 1; Round <= 2; Round++)
    {
        ProbeDirectDraw7(hwnd);
        Dialogs = DialogsAfterWait();
        ok(Dialogs == 0, "Round %d: DirectDraw showed %d message boxes\n", Round, Dialogs);
        CloseDialogs();

        ProbeDirect3D9(hwnd);
        Dialogs = DialogsAfterWait();
        ok(Dialogs == 0, "Round %d: Direct3D 9 showed %d message boxes\n", Round, Dialogs);
        CloseDialogs();

        ProbeDirect3D11();
        Dialogs = DialogsAfterWait();
        ok(Dialogs == 0, "Round %d: Direct3D 11 showed %d message boxes\n", Round, Dialogs);
        CloseDialogs();
    }

    if (hwnd)
        DestroyWindow(hwnd);
    /* Let a message box thread return before the process exits */
    Sleep(1000);
}

START_TEST(NoDriverNotice)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo = { 0 };
    char CommandLine[MAX_PATH + 64];
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3)
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        TestChild();
        return;
    }

    /* A child process, so that no other test has the DirectX DLLs loaded already */
    sprintf(CommandLine, "\"%s\" NoDriverNotice child", argv[0]);
    ok(CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo),
       "CreateProcessA failed: %lu\n", GetLastError());
    if (!ProcessInfo.hProcess)
        return;
    if (WaitForSingleObject(ProcessInfo.hProcess, 120000) != WAIT_OBJECT_0)
    {
        ok(0, "The child process hung\n");
        TerminateProcess(ProcessInfo.hProcess, 1);
        WaitForSingleObject(ProcessInfo.hProcess, 10000);
    }
    else
    {
        wait_child_process(ProcessInfo.hProcess);
    }
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
}
