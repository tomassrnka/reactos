/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ICustomDestinationList and IObjectCollection (jump list) implementation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * ReactOS has no taskbar jump list UI, so the destination list validates its arguments and
 * the transaction order, hands out empty "removed destinations" arrays and discards what the
 * application appends. That is enough for Chromium/Electron, which treat a failing BeginList
 * as an error ("Failed to begin Jump List transaction").
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#define JUMPLIST_MIN_SLOTS  10

/*************************************************************************
 * CObjectCollection
 */

CObjectCollection::CObjectCollection()
    : m_ppObjects(NULL)
    , m_cObjects(0)
    , m_cAllocated(0)
{
}

CObjectCollection::~CObjectCollection()
{
    Clear();
    free(m_ppObjects);
}

STDMETHODIMP CObjectCollection::GetCount(UINT *pcObjects)
{
    if (!pcObjects)
        return E_POINTER;
    *pcObjects = m_cObjects;
    return S_OK;
}

STDMETHODIMP CObjectCollection::GetAt(UINT uiIndex, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (uiIndex >= m_cObjects)
        return E_INVALIDARG;
    return m_ppObjects[uiIndex]->QueryInterface(riid, ppv);
}

STDMETHODIMP CObjectCollection::AddObject(IUnknown *punk)
{
    if (!punk)
        return E_INVALIDARG;

    if (m_cObjects == m_cAllocated)
    {
        UINT cNew = m_cAllocated ? m_cAllocated * 2 : 8;
        IUnknown **ppNew = (IUnknown **)realloc(m_ppObjects, cNew * sizeof(*ppNew));
        if (!ppNew)
            return E_OUTOFMEMORY;
        m_ppObjects = ppNew;
        m_cAllocated = cNew;
    }

    punk->AddRef();
    m_ppObjects[m_cObjects++] = punk;
    return S_OK;
}

STDMETHODIMP CObjectCollection::AddFromArray(IObjectArray *poaSource)
{
    UINT cSource;
    HRESULT hr;

    if (!poaSource)
        return E_INVALIDARG;

    hr = poaSource->GetCount(&cSource);
    if (FAILED(hr))
        return hr;

    /* Collect first so that a failure in the middle leaves this collection unchanged */
    UINT cOld = m_cObjects;
    for (UINT i = 0; i < cSource; ++i)
    {
        CComPtr<IUnknown> punk;
        hr = poaSource->GetAt(i, IID_PPV_ARG(IUnknown, &punk));
        if (SUCCEEDED(hr))
            hr = AddObject(punk);
        if (FAILED(hr))
        {
            while (m_cObjects > cOld)
                RemoveObjectAt(m_cObjects - 1);
            return hr;
        }
    }
    return S_OK;
}

STDMETHODIMP CObjectCollection::RemoveObjectAt(UINT uiIndex)
{
    if (uiIndex >= m_cObjects)
        return E_INVALIDARG;

    IUnknown *punk = m_ppObjects[uiIndex];
    MoveMemory(&m_ppObjects[uiIndex], &m_ppObjects[uiIndex + 1],
               (m_cObjects - uiIndex - 1) * sizeof(*m_ppObjects));
    --m_cObjects;
    punk->Release();
    return S_OK;
}

STDMETHODIMP CObjectCollection::Clear()
{
    while (m_cObjects)
        RemoveObjectAt(m_cObjects - 1);
    return S_OK;
}

/*************************************************************************
 * CDestinationList
 */

CDestinationList::CDestinationList()
    : m_bInTransaction(FALSE)
{
}

CDestinationList::~CDestinationList()
{
}

static HRESULT CreateEmptyCollection(REFIID riid, void **ppv)
{
    CComObject<CObjectCollection> *pCollection;
    HRESULT hr = CComObject<CObjectCollection>::CreateInstance(&pCollection);
    if (FAILED(hr))
        return hr;
    pCollection->AddRef();
    hr = pCollection->QueryInterface(riid, ppv);
    pCollection->Release();
    return hr;
}

STDMETHODIMP CDestinationList::SetAppID(LPCWSTR pszAppID)
{
    if (!pszAppID)
        return E_INVALIDARG;
    m_strAppID = pszAppID;
    return S_OK;
}

STDMETHODIMP CDestinationList::BeginList(UINT *pcMinSlots, REFIID riid, void **ppv)
{
    if (!pcMinSlots || !ppv)
        return E_POINTER;
    *pcMinSlots = 0;
    *ppv = NULL;

    HRESULT hr = CreateEmptyCollection(riid, ppv);
    if (FAILED(hr))
        return hr;

    /* A second BeginList restarts the transaction */
    m_bInTransaction = TRUE;
    *pcMinSlots = JUMPLIST_MIN_SLOTS;
    return S_OK;
}

STDMETHODIMP CDestinationList::AppendCategory(LPCWSTR pszCategory, IObjectArray *poa)
{
    if (!pszCategory || !poa)
        return E_INVALIDARG;
    if (!m_bInTransaction)
        return E_UNEXPECTED;
    return S_OK;
}

STDMETHODIMP CDestinationList::AppendKnownCategory(KNOWNDESTCATEGORY category)
{
    if (category != KDC_FREQUENT && category != KDC_RECENT)
        return E_INVALIDARG;
    if (!m_bInTransaction)
        return E_UNEXPECTED;
    return S_OK;
}

STDMETHODIMP CDestinationList::AddUserTasks(IObjectArray *poa)
{
    if (!poa)
        return E_INVALIDARG;
    if (!m_bInTransaction)
        return E_UNEXPECTED;
    return S_OK;
}

STDMETHODIMP CDestinationList::CommitList()
{
    if (!m_bInTransaction)
        return E_UNEXPECTED;
    m_bInTransaction = FALSE;
    return S_OK;
}

STDMETHODIMP CDestinationList::GetRemovedDestinations(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    return CreateEmptyCollection(riid, ppv);
}

STDMETHODIMP CDestinationList::DeleteList(LPCWSTR pszAppID)
{
    /* Nothing is persisted, so there is nothing to delete */
    return S_OK;
}

STDMETHODIMP CDestinationList::AbortList()
{
    m_bInTransaction = FALSE;
    return S_OK;
}
