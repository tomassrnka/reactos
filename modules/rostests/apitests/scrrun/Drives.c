/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for Scripting.FileSystemObject Drives: Item, enumeration, identity, Path
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define COBJMACROS
#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <ole2.h>
#include <oleauto.h>

#define CTL_E_DEVICEUNAVAILABLE_HR ((HRESULT)0x800A0044)

/* Late bound call; the HRESULT of a failed method, also when it comes in an EXCEPINFO. */
static HRESULT
Call(IDispatch *Object, PCWSTR Name, WORD Flags, VARIANT *Args, UINT ArgCount, VARIANT *Result)
{
    DISPPARAMS Params = { Args, NULL, ArgCount, 0 };
    EXCEPINFO Excep;
    UINT ArgErr = 0;
    DISPID Id;
    HRESULT hr;
    PWSTR NameCopy = (PWSTR)Name;

    hr = IDispatch_GetIDsOfNames(Object, &IID_NULL, &NameCopy, 1, LOCALE_USER_DEFAULT, &Id);
    if (FAILED(hr))
        return hr;
    memset(&Excep, 0, sizeof(Excep));
    VariantInit(Result);
    hr = IDispatch_Invoke(Object, Id, &IID_NULL, LOCALE_USER_DEFAULT, Flags, &Params, Result, &Excep, &ArgErr);
    if (hr == DISP_E_EXCEPTION)
    {
        hr = Excep.scode ? Excep.scode : HRESULT_FROM_WIN32(Excep.wCode);
        SysFreeString(Excep.bstrSource);
        SysFreeString(Excep.bstrDescription);
        SysFreeString(Excep.bstrHelpFile);
    }
    return hr;
}

static IUnknown *
Identity(IUnknown *Object)
{
    IUnknown *Unknown = NULL;
    IUnknown_QueryInterface(Object, &IID_IUnknown, (void **)&Unknown);
    if (Unknown)
        IUnknown_Release(Unknown);
    return Unknown;
}

static HRESULT
ItemOf(IDispatch *Drives, VARIANT *Key, IDispatch **Drive)
{
    VARIANT Result;
    HRESULT hr;

    *Drive = NULL;
    hr = Call(Drives, L"Item", DISPATCH_METHOD | DISPATCH_PROPERTYGET, Key, 1, &Result);
    if (SUCCEEDED(hr) && V_VT(&Result) == VT_DISPATCH)
        *Drive = V_DISPATCH(&Result);
    else if (SUCCEEDED(hr))
        VariantClear(&Result);
    return hr;
}

static HRESULT
ItemOfString(IDispatch *Drives, PCWSTR Spec, IDispatch **Drive)
{
    VARIANT Key;
    HRESULT hr;

    V_VT(&Key) = VT_BSTR;
    V_BSTR(&Key) = SysAllocString(Spec);
    hr = ItemOf(Drives, &Key, Drive);
    SysFreeString(V_BSTR(&Key));
    return hr;
}

static void
TestItemErrors(IDispatch *Drives, WCHAR Missing)
{
    static const PCWSTR Invalid[] = { L"", L"CC", L"1", L"C:\\Windows", L"C;", L"C:x" };
    IDispatch *Drive;
    VARIANT Key;
    WCHAR Spec[3] = { Missing, 0, 0 };
    HRESULT hr;
    int i;

    for (i = 0; i < ARRAYSIZE(Invalid); i++)
    {
        hr = ItemOfString(Drives, Invalid[i], &Drive);
        ok(hr == E_INVALIDARG, "Item(\"%ls\") returned 0x%08lx\n", Invalid[i], hr);
        ok(Drive == NULL, "Item(\"%ls\") returned a drive\n", Invalid[i]);
    }

    /* a number is no drive spec */
    V_VT(&Key) = VT_I4;
    V_I4(&Key) = 2;
    hr = ItemOf(Drives, &Key, &Drive);
    ok(hr == E_INVALIDARG, "Item(2) returned 0x%08lx\n", hr);
    V_VT(&Key) = VT_EMPTY;
    hr = ItemOf(Drives, &Key, &Drive);
    ok(hr == E_INVALIDARG, "Item(empty) returned 0x%08lx\n", hr);

    /* a letter that is no drive */
    if (Missing)
    {
        hr = ItemOfString(Drives, Spec, &Drive);
        ok(hr == CTL_E_DEVICEUNAVAILABLE_HR, "Item(\"%ls\") returned 0x%08lx\n", Spec, hr);
        ok(Drive == NULL, "Item of a missing drive returned a drive\n");
    }
}

static void
TestDrive(IDispatch *Drives, IDispatch *Fso, WCHAR Letter, IDispatch *Enumerated)
{
    WCHAR Spec[4], Expected[4];
    IDispatch *Drive;
    VARIANT Result, Arg, Byref;
    BSTR Text;
    HRESULT hr;
    int i;

    /* every spelling of the drive gives the one object, and the enumeration and GetDrive too */
    for (i = 0; i < 5; i++)
    {
        Spec[0] = (i == 1) ? (WCHAR)(Letter | 0x20) : Letter;
        Spec[1] = (i >= 2) ? L':' : 0;
        Spec[2] = (i == 3) ? L'\\' : 0;
        Spec[3] = 0;
        if (i == 4)
        {
            Spec[0] = (WCHAR)(Letter | 0x20);
            Spec[1] = L':';
            Spec[2] = 0;
        }
        hr = ItemOfString(Drives, Spec, &Drive);
        ok(hr == S_OK, "Item(\"%ls\") returned 0x%08lx\n", Spec, hr);
        if (hr != S_OK)
            continue;
        ok(Identity((IUnknown *)Drive) == Identity((IUnknown *)Enumerated), "Item(\"%ls\") is not the enumerated drive\n", Spec);
        IDispatch_Release(Drive);
    }

    V_VT(&Arg) = VT_BSTR;
    V_BSTR(&Arg) = SysAllocString(Spec);
    hr = Call(Fso, L"GetDrive", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &Arg, 1, &Result);
    ok(hr == S_OK, "GetDrive returned 0x%08lx\n", hr);
    if (hr == S_OK)
    {
        ok(Identity((IUnknown *)V_DISPATCH(&Result)) == Identity((IUnknown *)Enumerated), "GetDrive is not the enumerated drive\n");
        VariantClear(&Result);
    }
    SysFreeString(V_BSTR(&Arg));

    /* the key has to be a string itself, a reference to one is refused */
    V_VT(&Arg) = VT_BSTR;
    V_BSTR(&Arg) = SysAllocString(L"c");
    V_BSTR(&Arg)[0] = (WCHAR)(Letter | 0x20);
    V_VT(&Byref) = VT_BSTR | VT_BYREF;
    V_BSTRREF(&Byref) = &V_BSTR(&Arg);
    hr = ItemOf(Drives, &Byref, &Drive);
    ok(hr == E_INVALIDARG, "Item(byref string) returned 0x%08lx\n", hr);
    if (hr == S_OK)
        IDispatch_Release(Drive);
    SysFreeString(V_BSTR(&Arg));

    /* Path is "X:", DriveLetter "X" */
    Expected[0] = Letter;
    Expected[1] = L':';
    Expected[2] = 0;
    hr = Call(Enumerated, L"Path", DISPATCH_PROPERTYGET, NULL, 0, &Result);
    ok(hr == S_OK, "Path returned 0x%08lx\n", hr);
    if (hr == S_OK && V_VT(&Result) == VT_BSTR)
        ok(!wcscmp(V_BSTR(&Result), Expected), "Path is \"%ls\", expected \"%ls\"\n", V_BSTR(&Result), Expected);
    VariantClear(&Result);

    hr = Call(Enumerated, L"DriveLetter", DISPATCH_PROPERTYGET, NULL, 0, &Result);
    ok(hr == S_OK, "DriveLetter returned 0x%08lx\n", hr);
    if (hr == S_OK && V_VT(&Result) == VT_BSTR)
        ok(V_BSTR(&Result)[0] == Letter && V_BSTR(&Result)[1] == 0, "DriveLetter is \"%ls\"\n", V_BSTR(&Result));
    VariantClear(&Result);

    /* RootFolder is "X:\" */
    hr = Call(Enumerated, L"RootFolder", DISPATCH_PROPERTYGET, NULL, 0, &Result);
    ok(hr == S_OK, "RootFolder returned 0x%08lx\n", hr);
    if (hr == S_OK && V_VT(&Result) == VT_DISPATCH)
    {
        VARIANT PathResult;

        hr = Call(V_DISPATCH(&Result), L"Path", DISPATCH_PROPERTYGET, NULL, 0, &PathResult);
        ok(hr == S_OK, "RootFolder.Path returned 0x%08lx\n", hr);
        Text = (hr == S_OK && V_VT(&PathResult) == VT_BSTR) ? V_BSTR(&PathResult) : NULL;
        if (Text)
        {
            Expected[2] = L'\\';
            Expected[3] = 0;
            ok(!wcsicmp(Text, Expected), "RootFolder.Path is \"%ls\", expected \"%ls\"\n", Text, Expected);
        }
        VariantClear(&PathResult);
    }
    VariantClear(&Result);
}

START_TEST(ScrrunDrives)
{
    IEnumVARIANT *Enum = NULL;
    IDispatch *Fso = NULL, *Drives = NULL;
    IUnknown *Unknown = NULL;
    VARIANT Result, Element;
    CLSID Clsid;
    DWORD Mask = GetLogicalDrives();
    WCHAR Missing = 0, FirstLetter = 0;
    LONG Count = -1;
    HRESULT hr;
    ULONG Fetched;
    int Bits = 0, Seen = 0, i;

    hr = CoInitialize(NULL);
    ok(hr == S_OK || hr == S_FALSE, "CoInitialize: 0x%08lx\n", hr);

    hr = CLSIDFromProgID(L"Scripting.FileSystemObject", &Clsid);
    if (FAILED(hr))
    {
        skip("Scripting.FileSystemObject is not registered: 0x%08lx\n", hr);
        goto done;
    }
    hr = CoCreateInstance(&Clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch, (void **)&Fso);
    if (FAILED(hr))
    {
        skip("Scripting.FileSystemObject can not be created: 0x%08lx\n", hr);
        goto done;
    }

    for (i = 0; i < 26; i++)
    {
        if (Mask & (1u << i))
            Bits++;
        else if (!Missing)
            Missing = (WCHAR)(L'A' + i);
    }

    hr = Call(Fso, L"Drives", DISPATCH_PROPERTYGET, NULL, 0, &Result);
    ok(hr == S_OK, "Drives returned 0x%08lx\n", hr);
    if (hr != S_OK || V_VT(&Result) != VT_DISPATCH)
        goto done;
    Drives = V_DISPATCH(&Result);

    hr = Call(Drives, L"Count", DISPATCH_PROPERTYGET, NULL, 0, &Result);
    ok(hr == S_OK, "Count returned 0x%08lx\n", hr);
    if (hr == S_OK)
        Count = V_I4(&Result);
    ok(Count == Bits, "Count is %ld, GetLogicalDrives has %d\n", Count, Bits);

    TestItemErrors(Drives, Missing);

    /* the enumeration is an IEnumVARIANT of the drives in letter order */
    hr = Call(Drives, L"_NewEnum", DISPATCH_METHOD | DISPATCH_PROPERTYGET, NULL, 0, &Result);
    ok(hr == S_OK, "_NewEnum returned 0x%08lx\n", hr);
    if (hr != S_OK)
        goto done;
    Unknown = (V_VT(&Result) == VT_UNKNOWN) ? V_UNKNOWN(&Result) : (V_VT(&Result) == VT_DISPATCH ? (IUnknown *)V_DISPATCH(&Result) : NULL);
    ok(Unknown != NULL, "_NewEnum returned vt %d\n", V_VT(&Result));
    if (!Unknown)
        goto done;
    hr = IUnknown_QueryInterface(Unknown, &IID_IEnumVARIANT, (void **)&Enum);
    ok(hr == S_OK, "no IEnumVARIANT: 0x%08lx\n", hr);
    if (hr != S_OK)
        goto done;

    for (i = 0; i < 26; i++)
    {
        IDispatch *Drive;
        WCHAR Letter = (WCHAR)(L'A' + i);

        if (!(Mask & (1u << i)))
            continue;
        VariantInit(&Element);
        Fetched = 0;
        hr = IEnumVARIANT_Next(Enum, 1, &Element, &Fetched);
        ok(hr == S_OK && Fetched == 1, "Next for %lc returned 0x%08lx, %lu fetched\n", Letter, hr, Fetched);
        if (hr != S_OK)
            break;
        ok(V_VT(&Element) == VT_DISPATCH, "element vt %d\n", V_VT(&Element));
        Drive = V_VT(&Element) == VT_DISPATCH ? V_DISPATCH(&Element) : NULL;
        if (Drive)
        {
            if (!FirstLetter)
                FirstLetter = Letter;
            Seen++;
            TestDrive(Drives, Fso, Letter, Drive);
        }
        VariantClear(&Element);
    }
    ok(Seen == Bits, "enumerated %d drives of %d\n", Seen, Bits);

    VariantInit(&Element);
    hr = IEnumVARIANT_Next(Enum, 1, &Element, &Fetched);
    ok(hr == S_FALSE && Fetched == 0, "Next at the end returned 0x%08lx, %lu\n", hr, Fetched);

    /* Reset and Skip */
    hr = IEnumVARIANT_Reset(Enum);
    ok(hr == S_OK, "Reset: 0x%08lx\n", hr);
    if (Bits > 1)
    {
        IEnumVARIANT *Clone = NULL;

        hr = IEnumVARIANT_Skip(Enum, 1);
        ok(hr == S_OK, "Skip: 0x%08lx\n", hr);
        hr = IEnumVARIANT_Clone(Enum, &Clone);
        ok(hr == S_OK, "Clone: 0x%08lx\n", hr);
        if (hr == S_OK)
        {
            VariantInit(&Element);
            hr = IEnumVARIANT_Next(Clone, 1, &Element, &Fetched);
            ok(hr == S_OK && Fetched == 1, "Next on the clone: 0x%08lx\n", hr);
            VariantClear(&Element);
            IEnumVARIANT_Release(Clone);
        }
    }

done:
    if (Enum)
        IEnumVARIANT_Release(Enum);
    if (Unknown)
        IUnknown_Release(Unknown);
    if (Drives)
        IDispatch_Release(Drives);
    if (Fso)
        IDispatch_Release(Fso);
    CoUninitialize();
}
