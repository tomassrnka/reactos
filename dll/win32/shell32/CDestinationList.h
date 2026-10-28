/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ICustomDestinationList and IObjectCollection (jump list) headers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _CDESTINATIONLIST_H_
#define _CDESTINATIONLIST_H_

/* CLSID_EnumerableObjectCollection: an in-memory IObjectCollection (IObjectArray) */
class CObjectCollection :
    public CComCoClass<CObjectCollection, &CLSID_EnumerableObjectCollection>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IObjectCollection
{
private:
    IUnknown **m_ppObjects;
    UINT m_cObjects;
    UINT m_cAllocated;

public:
    CObjectCollection();
    ~CObjectCollection();

    // *** IObjectArray methods ***
    STDMETHOD(GetCount)(UINT *pcObjects) override;
    STDMETHOD(GetAt)(UINT uiIndex, REFIID riid, void **ppv) override;

    // *** IObjectCollection methods ***
    STDMETHOD(AddObject)(IUnknown *punk) override;
    STDMETHOD(AddFromArray)(IObjectArray *poaSource) override;
    STDMETHOD(RemoveObjectAt)(UINT uiIndex) override;
    STDMETHOD(Clear)() override;

DECLARE_REGISTRY_RESOURCEID(IDR_OBJECTCOLLECTION)
DECLARE_NOT_AGGREGATABLE(CObjectCollection)
DECLARE_PROTECT_FINAL_CONSTRUCT()

BEGIN_COM_MAP(CObjectCollection)
    COM_INTERFACE_ENTRY_IID(IID_IObjectCollection, IObjectCollection)
    COM_INTERFACE_ENTRY_IID(IID_IObjectArray, IObjectArray)
END_COM_MAP()
};

/* CLSID_DestinationList: accepts and discards jump list transactions (there is no taskbar jump list UI) */
class CDestinationList :
    public CComCoClass<CDestinationList, &CLSID_DestinationList>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public ICustomDestinationList
{
private:
    CStringW m_strAppID;
    BOOL m_bInTransaction;

public:
    CDestinationList();
    ~CDestinationList();

    // *** ICustomDestinationList methods ***
    STDMETHOD(SetAppID)(LPCWSTR pszAppID) override;
    STDMETHOD(BeginList)(UINT *pcMinSlots, REFIID riid, void **ppv) override;
    STDMETHOD(AppendCategory)(LPCWSTR pszCategory, IObjectArray *poa) override;
    STDMETHOD(AppendKnownCategory)(KNOWNDESTCATEGORY category) override;
    STDMETHOD(AddUserTasks)(IObjectArray *poa) override;
    STDMETHOD(CommitList)() override;
    STDMETHOD(GetRemovedDestinations)(REFIID riid, void **ppv) override;
    STDMETHOD(DeleteList)(LPCWSTR pszAppID) override;
    STDMETHOD(AbortList)() override;

DECLARE_REGISTRY_RESOURCEID(IDR_DESTINATIONLIST)
DECLARE_NOT_AGGREGATABLE(CDestinationList)
DECLARE_PROTECT_FINAL_CONSTRUCT()

BEGIN_COM_MAP(CDestinationList)
    COM_INTERFACE_ENTRY_IID(IID_ICustomDestinationList, ICustomDestinationList)
END_COM_MAP()
};

#endif // _CDESTINATIONLIST_H_
