/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for WerRegisterRuntimeExceptionModule and WerUnregisterRuntimeExceptionModule
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef HRESULT (WINAPI *FN_WerRuntimeExceptionModule)(PCWSTR, PVOID);

START_TEST(WerRuntimeExceptionModule)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    FN_WerRuntimeExceptionModule pRegister, pUnregister;
    static const WCHAR Dll[] = L"C:\\nonexistent\\werhandler.dll";
    WCHAR Name[64];
    int Context1, Context2;
    HRESULT hr;
    int i, Registered;

    pRegister = (FN_WerRuntimeExceptionModule)GetProcAddress(Kernel32, "WerRegisterRuntimeExceptionModule");
    pUnregister = (FN_WerRuntimeExceptionModule)GetProcAddress(Kernel32, "WerUnregisterRuntimeExceptionModule");
    if (!pRegister && is_reactos() && !GetProcAddress(Kernel32, "K32EnumProcesses"))
    {
        skip("No NT 6.1 exports\n");
        return;
    }
    ok(pRegister && pUnregister, "WerRegisterRuntimeExceptionModule/WerUnregisterRuntimeExceptionModule are not exported\n");
    if (!pRegister || !pUnregister)
        return;

    hr = pRegister(Dll, &Context1);
    ok(hr == S_OK, "Register returned 0x%lx\n", hr);

    /* Registering the same module and context again does not add a registration */
    hr = pRegister(Dll, &Context1);
    ok(hr == S_OK, "Second register returned 0x%lx\n", hr);

    /* The path is compared case-sensitively and the context is part of the key */
    hr = pUnregister(L"c:\\NONEXISTENT\\werhandler.dll", &Context1);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "Unregister with another case returned 0x%lx\n", hr);
    hr = pUnregister(Dll, NULL);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "Unregister with another context returned 0x%lx\n", hr);

    /* The same module with another context is another registration */
    hr = pRegister(Dll, &Context2);
    ok(hr == S_OK, "Register with a second context returned 0x%lx\n", hr);

    hr = pUnregister(Dll, &Context1);
    ok(hr == S_OK, "Unregister returned 0x%lx\n", hr);

    /* Gone now */
    hr = pUnregister(Dll, &Context1);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "Second unregister returned 0x%lx\n", hr);

    hr = pUnregister(Dll, &Context2);
    ok(hr == S_OK, "Unregister of the second context returned 0x%lx\n", hr);

    /* Never registered */
    hr = pUnregister(L"C:\\nonexistent\\other.dll", NULL);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "Unregister of an unknown module returned 0x%lx\n", hr);

    /* The table holds WER_MAX_REGISTERED_RUNTIME_EXCEPTION_MODULES (16) modules */
    for (i = 0, Registered = 0; i < 20; i++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"C:\\nonexistent\\handler%d.dll", i);
        hr = pRegister(Name, NULL);
        if (i < 16)
            ok(hr == S_OK, "Register %d returned 0x%lx\n", i, hr);
        else
            ok(hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), "Register %d returned 0x%lx\n", i, hr);
        if (SUCCEEDED(hr))
            Registered++;
    }
    ok(Registered == 16, "Registered %d modules\n", Registered);
    for (i = 0; i < 20; i++)
    {
        StringCchPrintfW(Name, ARRAYSIZE(Name), L"C:\\nonexistent\\handler%d.dll", i);
        hr = pUnregister(Name, NULL);
        if (i < 16)
            ok(hr == S_OK, "Unregister %d returned 0x%lx\n", i, hr);
    }
}
