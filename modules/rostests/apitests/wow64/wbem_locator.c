/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     A 32-bit WMI client under WoW64: CLSID_WbemLocator is registered
 *              in the 32-bit registry view, and a local query works
 *
 * Built for i386 and amd64: the same executable must give the same answers as
 * the native one (and as Windows) in both bitnesses.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <wbemcli.h>
#include <stdio.h>

/* The GUID is only declared by the headers */
static const GUID ClsidWbemLocator =
    {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};

static unsigned Checks, Failures;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)
#define CHECK_HR(hr) do { ++Checks; if (FAILED(hr)) { ++Failures; printf("FAIL line %u: %s hr=0x%08lx\n", __LINE__, #hr, (ULONG)(hr)); } } while (0)

/* The module of the in-process server must be the one of our own bitness view */
static void CheckRegistration(const WCHAR *Clsid, const WCHAR *Name, REGSAM View)
{
    WCHAR Path[128], Value[MAX_PATH];
    DWORD Size = sizeof(Value), Type;
    HKEY Key;
    LONG Error;

    _snwprintf(Path, ARRAYSIZE(Path), L"CLSID\\%ls\\InprocServer32", Clsid);
    Error = RegOpenKeyExW(HKEY_CLASSES_ROOT, Path, 0, KEY_QUERY_VALUE | View, &Key);
    printf("REGISTRATION %ls view=%s error=%ld", Name, View == KEY_WOW64_32KEY ? "32key" : "default", Error);
    CHECK(Error == ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
    {
        printf("\n");
        return;
    }
    Error = RegQueryValueExW(Key, NULL, NULL, &Type, (BYTE *)Value, &Size);
    CHECK(Error == ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
        printf(" server=%ls", Value);
    printf("\n");
    RegCloseKey(Key);
}

static void CheckQuery(IWbemServices *Services)
{
    IEnumWbemClassObject *Enum = NULL;
    IWbemClassObject *Object = NULL;
    BSTR Language = SysAllocString(L"WQL");
    BSTR Query = SysAllocString(L"SELECT * FROM Win32_OperatingSystem");
    ULONG Returned = 0;
    VARIANT Value;
    HRESULT hr;

    hr = IWbemServices_ExecQuery(Services, Language, Query,
                                 WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                 NULL, &Enum);
    printf("ExecQuery hr=0x%08lx\n", (ULONG)hr);
    CHECK_HR(hr);
    if (SUCCEEDED(hr))
    {
        hr = IEnumWbemClassObject_Next(Enum, WBEM_INFINITE, 1, &Object, &Returned);
        printf("Next hr=0x%08lx returned=%lu\n", (ULONG)hr, Returned);
        CHECK(hr == S_OK && Returned == 1);
        if (hr == S_OK && Returned == 1)
        {
            VariantInit(&Value);
            hr = IWbemClassObject_Get(Object, L"Caption", 0, &Value, NULL, NULL);
            CHECK_HR(hr);
            if (SUCCEEDED(hr))
            {
                CHECK(V_VT(&Value) == VT_BSTR && V_BSTR(&Value) && V_BSTR(&Value)[0]);
                if (V_VT(&Value) == VT_BSTR)
                    printf("Caption=%ls\n", V_BSTR(&Value));
            }
            VariantClear(&Value);
            IWbemClassObject_Release(Object);
        }
        IEnumWbemClassObject_Release(Enum);
    }
    SysFreeString(Language);
    SysFreeString(Query);
}

int wmain(void)
{
    IWbemLocator *Locator = NULL;
    IWbemServices *Services = NULL;
    BSTR Namespace;
    HRESULT hr;

    printf("BITS=%u\n", (unsigned)(sizeof(void *) * 8));
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CHECK_HR(hr);
    /* Not fatal: it may already be set by the runtime, as on Windows */
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    printf("CoInitializeSecurity hr=0x%08lx\n", (ULONG)hr);

    CheckRegistration(L"{4590F811-1D3A-11D0-891F-00AA004B2E24}", L"WbemLocator", 0);
    CheckRegistration(L"{4590F811-1D3A-11D0-891F-00AA004B2E24}", L"WbemLocator", KEY_WOW64_32KEY);
    CheckRegistration(L"{76A64158-CB41-11D1-8B02-00600806D9B6}", L"SWbemLocator", 0);
    CheckRegistration(L"{76A64158-CB41-11D1-8B02-00600806D9B6}", L"SWbemLocator", KEY_WOW64_32KEY);

    hr = CoCreateInstance(&ClsidWbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void **)&Locator);
    printf("CoCreateInstance(WbemLocator) hr=0x%08lx\n", (ULONG)hr);
    CHECK_HR(hr);
    if (SUCCEEDED(hr))
    {
        Namespace = SysAllocString(L"ROOT\\CIMV2");
        hr = IWbemLocator_ConnectServer(Locator, Namespace, NULL, NULL, NULL, 0, NULL, NULL, &Services);
        printf("ConnectServer hr=0x%08lx\n", (ULONG)hr);
        CHECK_HR(hr);
        SysFreeString(Namespace);
        if (SUCCEEDED(hr))
        {
            CheckQuery(Services);
            IWbemServices_Release(Services);
        }
        IWbemLocator_Release(Locator);
    }
    CoUninitialize();
    printf("WBEM_LOCATOR checks=%u failures=%u\n", Checks, Failures);
    return Failures ? 1 : 0;
}
