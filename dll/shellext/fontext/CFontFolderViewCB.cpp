/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fonts folder view callback implementation
 * COPYRIGHT:   Copyright 2026 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(fontext);

void CFontFolderViewCB::Initialize(CFontExt* pFontExt, IShellView *psv, LPCITEMIDLIST pidlParent)
{
    ATLASSERT(pFontExt);
    ATLASSERT(psv);
    ATLASSERT(pidlParent);
    m_pFontExt = pFontExt;
    m_pShellView = psv;
    m_pidlParent.Attach(ILClone(pidlParent));
    if (!m_pidlParent)
        ERR("m_pidlParent was null\n");
}

BOOL CFontFolderViewCB::FilterEvent(PIDLIST_ABSOLUTE* apidls, LONG lEvent) const
{
    lEvent &= ~SHCNE_INTERRUPT;

    switch (lEvent)
    {
        case SHCNE_CREATE:
        case SHCNE_DELETE:
        case SHCNE_RENAMEITEM:
        case SHCNE_UPDATEITEM:
        case SHCNE_UPDATEDIR:
            break;
        default:
            return TRUE; // We don't want this event
    }

    // Our own delete notification carries a font item; the view removes it
    if (lEvent == SHCNE_DELETE && apidls && apidls[0] && _FontFromIL(ILFindLastID(apidls[0])))
        return FALSE;

    // File system items (directory watcher, font installation) are not items
    // of this folder: re-read the fonts and enumerate again
    if (g_FontCache)
        g_FontCache->Read();
    if (m_hwndView)
        ::PostMessageW(m_hwndView, WM_COMMAND, MAKEWPARAM(FCIDM_SHVIEW_REFRESH, 0), 0);
    return TRUE;
}

STDMETHODIMP
CFontFolderViewCB::MessageSFVCB(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case SFVM_QUERYFSNOTIFY: // Registering change notification
        {
            if (!m_pShellView || !m_pFontExt || !m_pidlParent)
                return E_FAIL;
            // Now, we can get the view window
            m_pShellView->GetWindow(&m_hwndView);
            m_pFontExt->SetViewWindow(m_hwndView);
            // Watch the fonts directory only
            SHChangeNotifyEntry* pEntry = (SHChangeNotifyEntry*)lParam;
            pEntry->pidl = m_pidlParent;
            pEntry->fRecursive = FALSE;
            return S_OK;
        }
        case SFVM_FSNOTIFY: // Change notification
        {
            if (FilterEvent((PIDLIST_ABSOLUTE*)wParam, (LONG)lParam))
                return S_FALSE; // Don't process
            return S_OK;
        }
    }
    return E_NOTIMPL;
}
