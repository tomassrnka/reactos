/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for ICustomDestinationList (jump lists) and IObjectCollection
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellutils.h>

static void TestCollection(void)
{
    CComPtr<IObjectCollection> pColl, pColl2;
    CComPtr<IObjectArray> pArray;
    CComPtr<IShellLinkW> pLink1, pLink2, pLinkOut;
    CComPtr<IUnknown> pUnk;
    UINT cObjects;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_EnumerableObjectCollection, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IObjectCollection, &pColl));
    ok(hr == S_OK, "CoCreateInstance(EnumerableObjectCollection) returned 0x%lx\n", hr);
    if (!pColl)
        return;

    hr = pColl->QueryInterface(IID_PPV_ARG(IObjectArray, &pArray));
    ok(hr == S_OK, "QueryInterface(IObjectArray) returned 0x%lx\n", hr);

    cObjects = 77;
    hr = pColl->GetCount(&cObjects);
    ok(hr == S_OK, "GetCount returned 0x%lx\n", hr);
    ok(cObjects == 0, "GetCount gave %u\n", cObjects);

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &pLink1));
    ok(hr == S_OK, "CoCreateInstance(ShellLink) returned 0x%lx\n", hr);
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &pLink2));
    ok(hr == S_OK, "CoCreateInstance(ShellLink) returned 0x%lx\n", hr);
    if (!pLink1 || !pLink2)
        return;
    pLink1->SetPath(L"C:\\Windows\\notepad.exe");
    pLink2->SetPath(L"C:\\Windows\\system32\\cmd.exe");

    hr = pColl->AddObject(pLink1);
    ok(hr == S_OK, "AddObject returned 0x%lx\n", hr);
    hr = pColl->AddObject(pLink2);
    ok(hr == S_OK, "AddObject returned 0x%lx\n", hr);
    hr = pColl->GetCount(&cObjects);
    ok(hr == S_OK && cObjects == 2, "GetCount returned 0x%lx, %u\n", hr, cObjects);

    hr = pColl->GetAt(1, IID_PPV_ARG(IShellLinkW, &pLinkOut));
    ok(hr == S_OK, "GetAt(1) returned 0x%lx\n", hr);
    if (pLinkOut)
    {
        CComPtr<IUnknown> pA, pB;
        pLinkOut->QueryInterface(IID_PPV_ARG(IUnknown, &pA));
        pLink2->QueryInterface(IID_PPV_ARG(IUnknown, &pB));
        ok(pA == pB, "GetAt(1) returned a different object\n");
    }

    hr = pColl->GetAt(2, IID_PPV_ARG(IUnknown, &pUnk));
    ok(FAILED(hr), "GetAt(2) returned 0x%lx\n", hr);
    ok(pUnk == NULL, "GetAt(2) gave %p\n", (void *)pUnk);

    hr = CoCreateInstance(CLSID_EnumerableObjectCollection, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IObjectCollection, &pColl2));
    ok(hr == S_OK, "CoCreateInstance returned 0x%lx\n", hr);
    if (pColl2)
    {
        hr = pColl2->AddFromArray(pArray);
        ok(hr == S_OK, "AddFromArray returned 0x%lx\n", hr);
        hr = pColl2->GetCount(&cObjects);
        ok(hr == S_OK && cObjects == 2, "GetCount returned 0x%lx, %u\n", hr, cObjects);
        hr = pColl2->RemoveObjectAt(0);
        ok(hr == S_OK, "RemoveObjectAt returned 0x%lx\n", hr);
        hr = pColl2->GetCount(&cObjects);
        ok(hr == S_OK && cObjects == 1, "GetCount returned 0x%lx, %u\n", hr, cObjects);
        hr = pColl2->Clear();
        ok(hr == S_OK, "Clear returned 0x%lx\n", hr);
        hr = pColl2->GetCount(&cObjects);
        ok(hr == S_OK && cObjects == 0, "GetCount returned 0x%lx, %u\n", hr, cObjects);
    }
    /* the source is untouched */
    hr = pColl->GetCount(&cObjects);
    ok(hr == S_OK && cObjects == 2, "GetCount returned 0x%lx, %u\n", hr, cObjects);
}

static void TestDestinationList(void)
{
    CComPtr<ICustomDestinationList> pDest;
    CComPtr<IObjectArray> pRemoved, pRemoved2;
    CComPtr<IObjectCollection> pTasks;
    CComPtr<IShellLinkW> pLink;
    UINT cMinSlots, cObjects;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(ICustomDestinationList, &pDest));
    ok(hr == S_OK, "CoCreateInstance(DestinationList) returned 0x%lx\n", hr);
    if (!pDest)
        return;

    hr = pDest->SetAppID(L"RosJumpListTest.App");
    ok(hr == S_OK, "SetAppID returned 0x%lx\n", hr);

    cMinSlots = 0;
    hr = pDest->BeginList(&cMinSlots, IID_PPV_ARG(IObjectArray, &pRemoved));
    ok(hr == S_OK, "BeginList returned 0x%lx\n", hr);
    ok(cMinSlots > 0, "BeginList gave %u slots\n", cMinSlots);
    ok(pRemoved != NULL, "BeginList gave no removed destinations array\n");
    if (pRemoved)
    {
        cObjects = 77;
        hr = pRemoved->GetCount(&cObjects);
        ok(hr == S_OK, "GetCount returned 0x%lx\n", hr);
        ok(cObjects == 0, "removed destinations count is %u\n", cObjects);
    }

    hr = CoCreateInstance(CLSID_EnumerableObjectCollection, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARG(IObjectCollection, &pTasks));
    ok(hr == S_OK, "CoCreateInstance(EnumerableObjectCollection) returned 0x%lx\n", hr);
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &pLink));
    ok(hr == S_OK, "CoCreateInstance(ShellLink) returned 0x%lx\n", hr);
    if (!pTasks || !pLink)
        return;
    pLink->SetPath(L"C:\\Windows\\notepad.exe");
    pLink->SetArguments(L"--new-window");
    pLink->SetDescription(L"New window");
    hr = pTasks->AddObject(pLink);
    ok(hr == S_OK, "AddObject returned 0x%lx\n", hr);

    hr = pDest->AddUserTasks(pTasks);
    ok(hr == S_OK, "AddUserTasks returned 0x%lx\n", hr);
    hr = pDest->AppendCategory(L"Recent things", pTasks);
    ok(hr == S_OK, "AppendCategory returned 0x%lx\n", hr);
    hr = pDest->AppendKnownCategory(KDC_RECENT);
    ok(hr == S_OK, "AppendKnownCategory returned 0x%lx\n", hr);
    hr = pDest->CommitList();
    ok(hr == S_OK, "CommitList returned 0x%lx\n", hr);

    /* a second transaction on the same object, aborted */
    cMinSlots = 0;
    hr = pDest->BeginList(&cMinSlots, IID_PPV_ARG(IObjectArray, &pRemoved2));
    ok(hr == S_OK, "second BeginList returned 0x%lx\n", hr);
    ok(cMinSlots > 0, "second BeginList gave %u slots\n", cMinSlots);
    hr = pDest->AbortList();
    ok(hr == S_OK, "AbortList returned 0x%lx\n", hr);

    pRemoved2.Release();
    hr = pDest->GetRemovedDestinations(IID_PPV_ARG(IObjectArray, &pRemoved2));
    ok(hr == S_OK, "GetRemovedDestinations returned 0x%lx\n", hr);
    if (pRemoved2)
    {
        cObjects = 77;
        hr = pRemoved2->GetCount(&cObjects);
        ok(hr == S_OK && cObjects == 0, "GetCount returned 0x%lx, %u\n", hr, cObjects);
    }

    hr = pDest->DeleteList(L"RosJumpListTest.App");
    ok(hr == S_OK, "DeleteList returned 0x%lx\n", hr);
}

START_TEST(IDestinationList)
{
    HRESULT hrInit = CoInitialize(NULL);

    CComPtr<IUnknown> pUnk;
    HRESULT hr = CoCreateInstance(CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARG(IUnknown, &pUnk));
    if (FAILED(hr))
    {
        skip("ICustomDestinationList is not available (0x%lx)\n", hr);
    }
    else
    {
        pUnk.Release();
        TestCollection();
        TestDestinationList();
    }

    if (SUCCEEDED(hrInit))
        CoUninitialize();
}
