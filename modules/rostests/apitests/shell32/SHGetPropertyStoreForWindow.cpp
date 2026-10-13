/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for SHGetPropertyStoreForWindow
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"
#include <propsys.h>

typedef HRESULT (WINAPI *FN_SHGetPropertyStoreForWindow)(HWND, REFIID, void **);

START_TEST(SHGetPropertyStoreForWindow)
{
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    FN_SHGetPropertyStoreForWindow pSHGetPropertyStoreForWindow;
    IPropertyStore *pStore;
    HWND hwnd;
    HRESULT hr;

    pSHGetPropertyStoreForWindow = (FN_SHGetPropertyStoreForWindow)GetProcAddress(hShell32, "SHGetPropertyStoreForWindow");
    if (!pSHGetPropertyStoreForWindow && is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcesses"))
    {
        skip("No NT 6.1 exports\n");
        return;
    }
    ok(pSHGetPropertyStoreForWindow != NULL, "SHGetPropertyStoreForWindow is not exported\n");
    if (!pSHGetPropertyStoreForWindow)
        return;

    hwnd = CreateWindowExW(0, L"STATIC", L"SHGetPropertyStoreForWindow", WS_OVERLAPPEDWINDOW,
                           0, 0, 100, 100, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());

    pStore = (IPropertyStore *)(ULONG_PTR)0xdeadbeef;
    hr = pSHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore, (void **)&pStore);
    if (is_reactos())
    {
        /* No per-window property stores: an honest failure */
        ok(hr == E_NOTIMPL, "hr = 0x%lx\n", hr);
        ok(pStore == NULL, "pStore = %p\n", pStore);
    }
    else
    {
        ok(hr == S_OK, "hr = 0x%lx\n", hr);
        ok(pStore != NULL, "No store\n");
        if (SUCCEEDED(hr) && pStore)
            pStore->Release();
    }

    DestroyWindow(hwnd);
}
