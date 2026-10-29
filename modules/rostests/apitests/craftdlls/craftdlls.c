/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     windows.data.pdf.dll and prntvpt.dll load, export what WordCraft links, and fail cleanly
 *
 * WordCraft (a Rust word processor) imports PdfCreateRenderer from windows.data.pdf.dll and six PT*
 * functions from prntvpt.dll statically (raw-dylib), so both DLLs must load and resolve the names.
 * Both libraries are loaded by name and every function is bound with GetProcAddress: nothing here links them.
 */

#include <apitest.h>
#include <windows.h>
#include <objbase.h>

static const char *pdf_exports[] =
{
    "PdfCreateRenderer", "DllGetActivationFactory", "DllGetClassObject", "DllCanUnloadNow",
};

static const char *prn_exports[] =
{
    "PTOpenProvider", "PTCloseProvider", "PTGetPrintCapabilities", "PTMergeAndValidatePrintTicket",
    "PTConvertPrintTicketToDevMode", "PTReleaseMemory",
    /* the rest of the documented surface */
    "PTOpenProviderEx", "PTQuerySchemaVersionSupport", "PTConvertDevModeToPrintTicket",
    "PTGetPrintDeviceCapabilities", "PTGetPrintDeviceResources",
    "DllGetClassObject", "DllCanUnloadNow", "DllRegisterServer", "DllUnregisterServer",
};

static void check_exports(HMODULE mod, const char *dll, const char **names, int count)
{
    int i;
    for (i = 0; i < count; i++)
        ok(GetProcAddress(mod, names[i]) != NULL, "%s!%s is not exported\n", dll, names[i]);
}

static void test_pdf(void)
{
    typedef HRESULT (WINAPI *PdfCreateRenderer_t)(void *, void **);
    typedef HRESULT (WINAPI *DllGetActivationFactory_t)(void *, void **);
    typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);
    PdfCreateRenderer_t pPdfCreateRenderer;
    DllGetActivationFactory_t pDllGetActivationFactory;
    DllCanUnloadNow_t pDllCanUnloadNow;
    HMODULE mod;
    void *obj;
    HRESULT hr;

    mod = LoadLibraryA("windows.data.pdf.dll");
    ok(mod != NULL, "LoadLibrary(windows.data.pdf.dll) failed, error %lu\n", GetLastError());
    if (!mod)
        return;

    check_exports(mod, "windows.data.pdf.dll", pdf_exports, ARRAYSIZE(pdf_exports));

    pPdfCreateRenderer = (PdfCreateRenderer_t)GetProcAddress(mod, "PdfCreateRenderer");
    if (pPdfCreateRenderer)
    {
        obj = (void *)(ULONG_PTR)0xdeadbeef;
        hr = pPdfCreateRenderer(NULL, &obj);
        ok(FAILED(hr), "PdfCreateRenderer(NULL) returned %08lx\n", hr);
        ok(obj == NULL, "renderer = %p\n", obj);
        hr = pPdfCreateRenderer(NULL, NULL);
        ok(FAILED(hr), "PdfCreateRenderer(NULL, NULL) returned %08lx\n", hr);
    }

    pDllGetActivationFactory = (DllGetActivationFactory_t)GetProcAddress(mod, "DllGetActivationFactory");
    if (pDllGetActivationFactory)
    {
        obj = (void *)(ULONG_PTR)0xdeadbeef;
        hr = pDllGetActivationFactory(NULL, &obj);
        ok(FAILED(hr), "DllGetActivationFactory(NULL) returned %08lx\n", hr);
        ok(obj == NULL, "factory = %p\n", obj);
    }

    pDllCanUnloadNow = (DllCanUnloadNow_t)GetProcAddress(mod, "DllCanUnloadNow");
    if (pDllCanUnloadNow)
    {
        hr = pDllCanUnloadNow();
        ok(hr == S_OK || hr == S_FALSE, "DllCanUnloadNow returned %08lx\n", hr);
    }

    FreeLibrary(mod);
}

static void test_prntvpt(void)
{
    typedef HRESULT (WINAPI *PTOpenProvider_t)(const WCHAR *, DWORD, HANDLE *);
    typedef HRESULT (WINAPI *PTCloseProvider_t)(HANDLE);
    typedef HRESULT (WINAPI *PTReleaseMemory_t)(void *);
    typedef HRESULT (WINAPI *PTGetPrintCapabilities_t)(HANDLE, void *, void *, BSTR *);
    typedef HRESULT (WINAPI *PTMergeAndValidatePrintTicket_t)(HANDLE, void *, void *, int, void *, BSTR *);
    typedef HRESULT (WINAPI *PTConvertPrintTicketToDevMode_t)(HANDLE, void *, int, int, ULONG *, void **, BSTR *);
    PTOpenProvider_t pPTOpenProvider;
    PTCloseProvider_t pPTCloseProvider;
    PTReleaseMemory_t pPTReleaseMemory;
    PTGetPrintCapabilities_t pPTGetPrintCapabilities;
    PTMergeAndValidatePrintTicket_t pPTMerge;
    PTConvertPrintTicketToDevMode_t pPTConvert;
    HMODULE mod;
    HANDLE prov;
    HRESULT hr;

    mod = LoadLibraryA("prntvpt.dll");
    ok(mod != NULL, "LoadLibrary(prntvpt.dll) failed, error %lu\n", GetLastError());
    if (!mod)
        return;

    check_exports(mod, "prntvpt.dll", prn_exports, ARRAYSIZE(prn_exports));

    pPTOpenProvider = (PTOpenProvider_t)GetProcAddress(mod, "PTOpenProvider");
    pPTCloseProvider = (PTCloseProvider_t)GetProcAddress(mod, "PTCloseProvider");
    pPTReleaseMemory = (PTReleaseMemory_t)GetProcAddress(mod, "PTReleaseMemory");
    pPTGetPrintCapabilities = (PTGetPrintCapabilities_t)GetProcAddress(mod, "PTGetPrintCapabilities");
    pPTMerge = (PTMergeAndValidatePrintTicket_t)GetProcAddress(mod, "PTMergeAndValidatePrintTicket");
    pPTConvert = (PTConvertPrintTicketToDevMode_t)GetProcAddress(mod, "PTConvertPrintTicketToDevMode");
    if (!pPTOpenProvider || !pPTCloseProvider || !pPTReleaseMemory || !pPTGetPrintCapabilities || !pPTMerge || !pPTConvert)
    {
        FreeLibrary(mod);
        return;
    }

    /* An unsupported schema version and a printer that does not exist fail with an HRESULT. */
    prov = (HANDLE)(ULONG_PTR)0xdeadbeef;
    hr = pPTOpenProvider(L"No such printer (apitest)", 2, &prov);
    ok(hr == E_INVALIDARG, "PTOpenProvider(version 2) returned %08lx\n", hr);
    ok(prov == (HANDLE)(ULONG_PTR)0xdeadbeef, "provider changed to %p\n", prov);

    prov = NULL;
    hr = pPTOpenProvider(L"No such printer (apitest)", 1, &prov);
    ok(FAILED(hr), "PTOpenProvider(missing printer) returned %08lx\n", hr);
    ok(prov == NULL, "provider = %p\n", prov);

    /* A handle that was never opened is rejected by every function, and does not crash. */
    hr = pPTCloseProvider(NULL);
    ok(hr == E_HANDLE, "PTCloseProvider(NULL) returned %08lx\n", hr);
    hr = pPTGetPrintCapabilities(NULL, NULL, NULL, NULL);
    ok(FAILED(hr), "PTGetPrintCapabilities(NULL) returned %08lx\n", hr);
    hr = pPTMerge(NULL, NULL, NULL, 0, NULL, NULL);
    ok(FAILED(hr), "PTMergeAndValidatePrintTicket(NULL) returned %08lx\n", hr);
    hr = pPTConvert(NULL, NULL, 0, 0, NULL, NULL, NULL);
    ok(FAILED(hr), "PTConvertPrintTicketToDevMode(NULL) returned %08lx\n", hr);

    hr = pPTReleaseMemory(NULL);
    ok(hr == S_OK, "PTReleaseMemory(NULL) returned %08lx\n", hr);

    FreeLibrary(mod);
}

START_TEST(craftdlls)
{
    test_pdf();
    test_prntvpt();
}
