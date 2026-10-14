/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for RegisterApplicationRestart, GetApplicationRestartSettings and
 *              UnregisterApplicationRestart
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define ERROR_HR(x) ((HRESULT)(((x) & 0x0000FFFF) | (FACILITY_WIN32 << 16) | 0x80000000))

static HRESULT (WINAPI *pRegisterApplicationRestart)(PCWSTR, DWORD);
static HRESULT (WINAPI *pGetApplicationRestartSettings)(HANDLE, PWSTR, PDWORD, PDWORD);
static HRESULT (WINAPI *pUnregisterApplicationRestart)(void);

START_TEST(ApplicationRestart)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    WCHAR Buffer[64], LongLine[1100];
    DWORD Size, Flags;
    HRESULT hr;
    int i;

    /* A build without the Windows Vista exports in kernel32 has them in kernel32_vista.dll */
    if (!GetProcAddress(hKernel32, "GetApplicationRestartSettings") && !GetProcAddress(hKernel32, "RegisterApplicationRestart"))
    {
        HMODULE hVista = LoadLibraryW(L"kernel32_vista.dll");
        if (hVista)
            hKernel32 = hVista;
    }

    pRegisterApplicationRestart = (void *)GetProcAddress(hKernel32, "RegisterApplicationRestart");
    pGetApplicationRestartSettings = (void *)GetProcAddress(hKernel32, "GetApplicationRestartSettings");
    pUnregisterApplicationRestart = (void *)GetProcAddress(hKernel32, "UnregisterApplicationRestart");
    ok(pRegisterApplicationRestart != NULL, "RegisterApplicationRestart is missing\n");
    ok(pGetApplicationRestartSettings != NULL, "GetApplicationRestartSettings is missing\n");
    ok(pUnregisterApplicationRestart != NULL, "UnregisterApplicationRestart is missing\n");
    if (!pRegisterApplicationRestart || !pGetApplicationRestartSettings || !pUnregisterApplicationRestart)
        return;

    /* Nothing is registered yet */
    Size = 64;
    Flags = 0xdeadbeef;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == ERROR_HR(ERROR_NOT_FOUND), "hr = 0x%lx\n", hr);
    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "hr = 0x%lx\n", hr);

    /* Register with a command line */
    hr = pRegisterApplicationRestart(L"-restarted now", 0);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);

    Size = 64;
    Flags = 0xdeadbeef;
    wcscpy(Buffer, L"junk");
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    ok(Size == 15, "Size = %lu\n", Size);
    ok(!wcscmp(Buffer, L"-restarted now"), "Buffer = %ls\n", Buffer);
    ok(Flags == 0, "Flags = 0x%lx\n", Flags);

    /* The flags come back as given */
    hr = pRegisterApplicationRestart(L"x y", 1 | 4);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    Size = 64;
    Flags = 0xdeadbeef;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    ok(Size == 4, "Size = %lu\n", Size);
    ok(!wcscmp(Buffer, L"x y"), "Buffer = %ls\n", Buffer);
    ok(Flags == 5, "Flags = 0x%lx\n", Flags);

    /* A small buffer gets the needed size */
    Size = 2;
    Flags = 0xdeadbeef;
    wcscpy(Buffer, L"junk");
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == ERROR_HR(ERROR_INSUFFICIENT_BUFFER), "hr = 0x%lx\n", hr);
    ok(Size == 4, "Size = %lu\n", Size);

    /* The size alone */
    Size = 0;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), NULL, &Size, NULL);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    ok(Size == 4, "Size = %lu\n", Size);

    /* The size is required */
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, NULL, &Flags);
    ok(hr == E_INVALIDARG, "hr = 0x%lx\n", hr);

    /* Another registration replaces the first, and a NULL command line is allowed */
    hr = pRegisterApplicationRestart(NULL, 8);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    Size = 64;
    Flags = 0xdeadbeef;
    wcscpy(Buffer, L"junk");
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    ok(Size == 1, "Size = %lu\n", Size);
    ok(Buffer[0] == 0, "Buffer = %ls\n", Buffer);
    ok(Flags == 8, "Flags = 0x%lx\n", Flags);

    /* Flags that are not defined are kept as given */
    hr = pRegisterApplicationRestart(L"z", 0x10);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    Size = 64;
    Flags = 0xdeadbeef;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    ok(Flags == 0x10, "Flags = 0x%lx\n", Flags);

    /* An empty command line */
    hr = pRegisterApplicationRestart(L"", 0);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    Size = 64;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    ok(Size == 1, "Size = %lu\n", Size);

    /* A command line that is too long: at most RESTART_MAX_CMD_LINE characters including the terminator */
    for (i = 0; i < 1023; i++)
        LongLine[i] = L'a';
    LongLine[1023] = 0;
    hr = pRegisterApplicationRestart(LongLine, 0);
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    LongLine[1023] = L'a';
    LongLine[1024] = 0;
    hr = pRegisterApplicationRestart(LongLine, 0);
    ok(hr == E_INVALIDARG, "hr = 0x%lx\n", hr);

    /* Unregistering removes the registration, once */
    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
    Size = 64;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == ERROR_HR(ERROR_NOT_FOUND), "hr = 0x%lx\n", hr);
    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "hr = 0x%lx\n", hr);
}
