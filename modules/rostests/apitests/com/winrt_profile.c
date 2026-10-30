/*
 * PROJECT:         ReactOS API Tests
 * LICENSE:         LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:         Tests for the Windows.System.Profile.PlatformDiagnosticsAndUsageDataSettings WinRT class
 * COPYRIGHT:       Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "com_apitest.h"

typedef struct IStatics IStatics;

typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IStatics *This, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IStatics *This);
    ULONG (STDMETHODCALLTYPE *Release)(IStatics *This);
    HRESULT (STDMETHODCALLTYPE *GetIids)(IStatics *This, ULONG *count, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IStatics *This, PVOID *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IStatics *This, int *level);
    HRESULT (STDMETHODCALLTYPE *get_CollectionLevel)(IStatics *This, INT32 *value);
    HRESULT (STDMETHODCALLTYPE *add_CollectionLevelChanged)(IStatics *This, void *handler, INT64 *token);
    HRESULT (STDMETHODCALLTYPE *remove_CollectionLevelChanged)(IStatics *This, INT64 token);
    HRESULT (STDMETHODCALLTYPE *CanCollectDiagnostics)(IStatics *This, INT32 level, BOOLEAN *result);
} StaticsVtbl;

struct IStatics
{
    const StaticsVtbl *lpVtbl;
};

/* IPlatformDiagnosticsAndUsageDataSettingsStatics */
static const GUID IID_IStatics =
    {0xb6e24c1b, 0x7b1c, 0x4b32, {0x8c, 0x62, 0xa6, 0x65, 0x97, 0xce, 0x72, 0x3a}};
/* IActivationFactory */
static const GUID IID_IActivationFactoryTest =
    {0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

typedef HRESULT (WINAPI *PRO_GET_ACTIVATION_FACTORY)(PVOID, REFIID, void **);
typedef HRESULT (WINAPI *PWINDOWS_CREATE_STRING)(LPCWSTR, UINT32, PVOID *);
typedef HRESULT (WINAPI *PWINDOWS_DELETE_STRING)(PVOID);

START_TEST(winrt_profile)
{
    static const WCHAR ClassName[] = L"Windows.System.Profile.PlatformDiagnosticsAndUsageDataSettings";
    HMODULE hCombase = LoadLibraryW(L"combase.dll");
    PRO_GET_ACTIVATION_FACTORY pRoGetActivationFactory;
    PWINDOWS_CREATE_STRING pWindowsCreateString;
    PWINDOWS_DELETE_STRING pWindowsDeleteString;
    PVOID Class = NULL;
    IStatics *Statics = NULL;
    IUnknown *Factory = NULL;
    INT32 Level = -1, Previous;
    BOOLEAN Can;
    INT64 Token;
    HRESULT hr;
    int i;

    if (!hCombase)
    {
        skip("No combase.dll\n");
        return;
    }
    pRoGetActivationFactory = (PRO_GET_ACTIVATION_FACTORY)GetProcAddress(hCombase, "RoGetActivationFactory");
    pWindowsCreateString = (PWINDOWS_CREATE_STRING)GetProcAddress(hCombase, "WindowsCreateString");
    pWindowsDeleteString = (PWINDOWS_DELETE_STRING)GetProcAddress(hCombase, "WindowsDeleteString");
    if (!pRoGetActivationFactory || !pWindowsCreateString || !pWindowsDeleteString)
    {
        skip("No WinRT activation\n");
        return;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(SUCCEEDED(hr), "CoInitializeEx returned 0x%08lx\n", hr);
    hr = pWindowsCreateString(ClassName, ARRAYSIZE(ClassName) - 1, &Class);
    ok(hr == S_OK, "WindowsCreateString returned 0x%08lx\n", hr);

    /* PowerShell asks for the statics of this class to learn whether it may send telemetry */
    hr = pRoGetActivationFactory(Class, &IID_IStatics, (void **)&Statics);
    ok(hr == S_OK, "RoGetActivationFactory returned 0x%08lx\n", hr);
    if (hr != S_OK || !Statics)
        goto done;

    hr = Statics->lpVtbl->get_CollectionLevel(Statics, &Level);
    ok(hr == S_OK, "get_CollectionLevel returned 0x%08lx\n", hr);
    ok(Level >= 0 && Level <= 3, "Collection level %ld\n", (long)Level);

    /* a level can be collected when it does not exceed the current one */
    for (i = -1; i <= 4; i++)
    {
        Can = 0x55;
        hr = Statics->lpVtbl->CanCollectDiagnostics(Statics, i, &Can);
        ok(hr == S_OK, "CanCollectDiagnostics(%d) returned 0x%08lx\n", i, hr);
        ok(Can == (i <= Level), "CanCollectDiagnostics(%d) is %d, level is %ld\n", i, Can, (long)Level);
    }

    Token = 7;
    hr = Statics->lpVtbl->add_CollectionLevelChanged(Statics, NULL, &Token);
    ok(hr == E_INVALIDARG, "add_CollectionLevelChanged(NULL) returned 0x%08lx\n", hr);

    Previous = Level;
    hr = Statics->lpVtbl->get_CollectionLevel(Statics, &Level);
    ok(hr == S_OK && Level == Previous, "Level changed from %ld to %ld\n", (long)Previous, (long)Level);

    /* the class has no instances: its factory refuses to activate one */
    hr = Statics->lpVtbl->QueryInterface(Statics, &IID_IActivationFactoryTest, (void **)&Factory);
    ok(hr == S_OK, "QueryInterface(IActivationFactory) returned 0x%08lx\n", hr);
    if (Factory)
    {
        PVOID Instance = (PVOID)(ULONG_PTR)0x1234;
        /* ActivateInstance is the seventh method: IUnknown (3), IInspectable (3), then it */
        HRESULT (STDMETHODCALLTYPE *ActivateInstance)(IUnknown *, PVOID *) =
            ((HRESULT (STDMETHODCALLTYPE **)(IUnknown *, PVOID *))*(void ***)Factory)[6];
        hr = ActivateInstance(Factory, &Instance);
        ok(hr == E_NOTIMPL, "ActivateInstance returned 0x%08lx\n", hr);
        ok(Instance == NULL, "Instance is %p\n", Instance);
        Factory->lpVtbl->Release(Factory);
    }
    Statics->lpVtbl->Release(Statics);

done:
    if (Class)
        pWindowsDeleteString(Class);
    CoUninitialize();
}
