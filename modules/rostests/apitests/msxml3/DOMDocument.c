/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     MSXML3 COM class registration: the DOMDocument coclasses must be
 *              creatable through CoCreateInstance, as installers do.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <ole2.h>
#include <oleauto.h>

#define DEFINE_CLSID(name, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    static const CLSID name = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }

DEFINE_CLSID(TEST_DOMDocument30, 0xF5078F32, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_CLSID(TEST_DOMDocument,   0xF6D90F11, 0x9C73, 0x11D3, 0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4);
DEFINE_CLSID(TEST_FreeThreadedDOMDocument30, 0xF5078F33, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_CLSID(TEST_FreeThreadedDOMDocument,   0xF6D90F12, 0x9C73, 0x11D3, 0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4);
DEFINE_CLSID(TEST_XMLHTTP30,     0xF5078F35, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_CLSID(TEST_XMLSchemaCache30, 0xF5078F34, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_CLSID(TEST_XSLTemplate30, 0xF5078F36, 0xC551, 0x11D3, 0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21);
DEFINE_CLSID(TEST_SAXXMLReader30, 0x3124C396, 0xFB13, 0x4836, 0xA6, 0xAD, 0x13, 0x17, 0xF1, 0x71, 0x36, 0x88);
DEFINE_CLSID(TEST_MXXMLWriter30, 0x3D813DFE, 0x6C91, 0x4A4E, 0x8F, 0x41, 0x04, 0x34, 0x6A, 0x84, 0x1D, 0x9C);
DEFINE_CLSID(TEST_SAXAttributes30, 0x3E784A01, 0xF3AE, 0x4DC0, 0x93, 0x54, 0x95, 0x26, 0xB9, 0x37, 0x0E, 0xBA);

static const struct
{
    const CLSID *clsid;
    const char *name;
} classes[] =
{
    { &TEST_DOMDocument30, "DOMDocument30" },
    { &TEST_DOMDocument, "DOMDocument" },
    { &TEST_FreeThreadedDOMDocument30, "FreeThreadedDOMDocument30" },
    { &TEST_FreeThreadedDOMDocument, "FreeThreadedDOMDocument" },
    { &TEST_XMLHTTP30, "XMLHTTP30" },
    { &TEST_XMLSchemaCache30, "XMLSchemaCache30" },
    { &TEST_XSLTemplate30, "XSLTemplate30" },
    { &TEST_SAXXMLReader30, "SAXXMLReader30" },
    { &TEST_MXXMLWriter30, "MXXMLWriter30" },
    { &TEST_SAXAttributes30, "SAXAttributes30" },
};

static HRESULT Invoke(IDispatch *Disp, PCWSTR Name, WORD Flags, VARIANT *Args, UINT ArgCount, VARIANT *Result)
{
    DISPID Id;
    DISPPARAMS Params = { Args, NULL, ArgCount, 0 };
    DISPID Named = DISPID_PROPERTYPUT;
    HRESULT hr;
    PWSTR Names[1];

    Names[0] = (PWSTR)Name;
    hr = Disp->lpVtbl->GetIDsOfNames(Disp, &IID_NULL, Names, 1, LOCALE_USER_DEFAULT, &Id);
    if (FAILED(hr))
        return hr;
    if (Flags == DISPATCH_PROPERTYPUT)
    {
        Params.rgdispidNamedArgs = &Named;
        Params.cNamedArgs = 1;
    }
    return Disp->lpVtbl->Invoke(Disp, Id, &IID_NULL, LOCALE_USER_DEFAULT, Flags, &Params, Result, NULL, NULL);
}

static void TestDocument(void)
{
    CLSID ProgIdClsid;
    IDispatch *Doc = NULL, *Root = NULL;
    VARIANT Arg, Result;
    HRESULT hr;

    hr = CLSIDFromProgID(L"Msxml2.DOMDocument.3.0", &ProgIdClsid);
    ok(hr == S_OK, "CLSIDFromProgID(Msxml2.DOMDocument.3.0) returned 0x%lx\n", hr);
    if (hr == S_OK)
        ok(IsEqualCLSID(&ProgIdClsid, &TEST_DOMDocument30), "ProgID maps to the wrong CLSID\n");

    hr = CoCreateInstance(&TEST_DOMDocument30, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch, (void **)&Doc);
    ok(hr == S_OK, "CoCreateInstance(DOMDocument30) returned 0x%lx\n", hr);
    if (hr != S_OK)
        return;

    /* Load a trivial document and read the root element name back. */
    VariantInit(&Arg);
    VariantInit(&Result);
    V_VT(&Arg) = VT_BSTR;
    V_BSTR(&Arg) = SysAllocString(L"<root><child>1</child></root>");
    hr = Invoke(Doc, L"loadXML", DISPATCH_METHOD, &Arg, 1, &Result);
    ok(hr == S_OK, "loadXML returned 0x%lx\n", hr);
    ok(V_VT(&Result) == VT_BOOL && V_BOOL(&Result) == VARIANT_TRUE, "loadXML result vt=%u\n", V_VT(&Result));
    SysFreeString(V_BSTR(&Arg));
    VariantClear(&Result);

    hr = Invoke(Doc, L"documentElement", DISPATCH_PROPERTYGET, NULL, 0, &Result);
    ok(hr == S_OK, "documentElement returned 0x%lx\n", hr);
    if (hr == S_OK && V_VT(&Result) == VT_DISPATCH && V_DISPATCH(&Result))
    {
        Root = V_DISPATCH(&Result);
        VariantInit(&Result);
        hr = Invoke(Root, L"nodeName", DISPATCH_PROPERTYGET, NULL, 0, &Result);
        ok(hr == S_OK, "nodeName returned 0x%lx\n", hr);
        ok(V_VT(&Result) == VT_BSTR && V_BSTR(&Result) && !lstrcmpW(V_BSTR(&Result), L"root"),
           "nodeName vt=%u\n", V_VT(&Result));
        VariantClear(&Result);
        Root->lpVtbl->Release(Root);
    }
    else
    {
        ok(0, "documentElement vt=%u\n", V_VT(&Result));
    }
    Doc->lpVtbl->Release(Doc);
}

START_TEST(DOMDocument)
{
    HRESULT hr;
    ULONG i;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(SUCCEEDED(hr), "CoInitializeEx returned 0x%lx\n", hr);

    for (i = 0; i < ARRAYSIZE(classes); i++)
    {
        IUnknown *Unk = NULL;

        hr = CoCreateInstance(classes[i].clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&Unk);
        ok(hr == S_OK, "CoCreateInstance(%s) returned 0x%lx\n", classes[i].name, hr);
        if (Unk)
            Unk->lpVtbl->Release(Unk);
    }

    TestDocument();
    CoUninitialize();
}
