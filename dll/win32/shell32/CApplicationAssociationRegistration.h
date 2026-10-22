/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IApplicationAssociationRegistration implementation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _CAPPLICATIONASSOCIATIONREGISTRATION_H_
#define _CAPPLICATIONASSOCIATIONREGISTRATION_H_

class CApplicationAssociationRegistration :
    public CComCoClass<CApplicationAssociationRegistration, &CLSID_ApplicationAssociationRegistration>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IApplicationAssociationRegistration
{
public:
    CApplicationAssociationRegistration();
    ~CApplicationAssociationRegistration();

    // *** IApplicationAssociationRegistration methods ***
    STDMETHOD(QueryCurrentDefault)(LPCWSTR pszQuery, ASSOCIATIONTYPE atQueryType,
                                   ASSOCIATIONLEVEL alQueryLevel, LPWSTR *ppszAssociation) override;
    STDMETHOD(QueryAppIsDefault)(LPCWSTR pszQuery, ASSOCIATIONTYPE atQueryType,
                                 ASSOCIATIONLEVEL alQueryLevel, LPCWSTR pszAppRegistryName,
                                 BOOL *pfDefault) override;
    STDMETHOD(QueryAppIsDefaultAll)(ASSOCIATIONLEVEL alQueryLevel, LPCWSTR pszAppRegistryName,
                                    BOOL *pfDefault) override;
    STDMETHOD(SetAppAsDefault)(LPCWSTR pszAppRegistryName, LPCWSTR pszSet,
                               ASSOCIATIONTYPE atSetType) override;
    STDMETHOD(SetAppAsDefaultAll)(LPCWSTR pszAppRegistryName) override;
    STDMETHOD(ClearUserAssociations)() override;

DECLARE_REGISTRY_RESOURCEID(IDR_APPASSOCREGISTRATION)
DECLARE_NOT_AGGREGATABLE(CApplicationAssociationRegistration)
DECLARE_PROTECT_FINAL_CONSTRUCT()

BEGIN_COM_MAP(CApplicationAssociationRegistration)
    COM_INTERFACE_ENTRY_IID(IID_IApplicationAssociationRegistration, IApplicationAssociationRegistration)
END_COM_MAP()
};

#endif // _CAPPLICATIONASSOCIATIONREGISTRATION_H_
