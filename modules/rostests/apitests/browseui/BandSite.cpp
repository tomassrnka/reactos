/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the persistence of the rebar band site
 */

#define _UNICODE
#define UNICODE
#include <apitest.h>
#include <shlobj.h>
#include <atlbase.h>
#include <tchar.h>
#include <atlcom.h>
#include <atlwin.h>

ULONG DbgPrint(PCH Format,...);

#include <stdio.h>
#include <shellutils.h>
#include <shlwapi.h>
#include <reactos/shlguid_undoc.h>

/* The window the band site puts its rebar into */
class CTestSite : public IOleWindow
{
    LONG m_cRef = 1;
    HWND m_hwnd;

public:
    CTestSite(HWND hwnd) : m_hwnd(hwnd) { }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IOleWindow)
        {
            *ppv = static_cast<IOleWindow *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG ref = InterlockedDecrement(&m_cRef);
        if (!ref)
            delete this;
        return ref;
    }
    STDMETHODIMP GetWindow(HWND *phwnd) override { *phwnd = m_hwnd; return S_OK; }
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
};

struct BandSite
{
    CComPtr<IUnknown> unk;
    CComPtr<IBandSite> site;
    CComPtr<IPersistStream> persist;
    CComPtr<IDeskBarClient> client;
    CComPtr<IOleWindow> owner;
};

static bool CreateBandSite(HWND hwnd, BandSite &bs)
{
    HRESULT hr = CoCreateInstance(CLSID_RebarBandSite, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IUnknown, &bs.unk));
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return false;

    hr = bs.unk->QueryInterface(IID_PPV_ARG(IBandSite, &bs.site));
    ok_hex(hr, S_OK);
    hr = bs.unk->QueryInterface(IID_PPV_ARG(IPersistStream, &bs.persist));
    ok_hex(hr, S_OK);
    hr = bs.unk->QueryInterface(IID_PPV_ARG(IDeskBarClient, &bs.client));
    ok_hex(hr, S_OK);
    if (!bs.site || !bs.persist || !bs.client)
        return false;

    bs.owner.Attach(new CTestSite(hwnd));
    hr = bs.client->SetDeskBarSite(bs.owner);
    ok_hex(hr, S_OK);
    return SUCCEEDED(hr);
}

static HRESULT AddFolderBand(BandSite &bs, PCIDLIST_ABSOLUTE pidl, BOOL bQuickLaunch)
{
    CComPtr<IShellFolderBand> band;
    HRESULT hr = CoCreateInstance(CLSID_ISFBand, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellFolderBand, &band));
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return hr;

    hr = band->InitializeSFB(NULL, pidl);
    ok_hex(hr, S_OK);
    if (bQuickLaunch)
    {
        BANDINFOSFB info = { ISFB_MASK_STATE, ISFB_STATE_QLINKSMODE, ISFB_STATE_QLINKSMODE };
        band->SetBandInfoSFB(&info);
    }

    hr = bs.site->AddBand(band);
    ok(SUCCEEDED(hr), "AddBand failed with 0x%08lx\n", hr);
    return hr;
}

static UINT CountBands(IBandSite *site)
{
    UINT count = 0;
    DWORD id;
    while (SUCCEEDED(site->EnumBands(count, &id)))
        count++;
    return count;
}

static void ExpectFolder(IBandSite *site, UINT index, PCIDLIST_ABSOLUTE pidl, const char *what)
{
    DWORD id;
    HRESULT hr = site->EnumBands(index, &id);
    ok(hr == S_OK, "%s: EnumBands(%u) returned 0x%08lx\n", what, index, hr);
    if (hr != S_OK)
        return;

    CComPtr<IShellFolderBand> band;
    hr = site->GetBandObject(id, IID_PPV_ARG(IShellFolderBand, &band));
    ok(hr == S_OK, "%s: GetBandObject returned 0x%08lx\n", what, hr);
    if (hr != S_OK)
        return;

    BANDINFOSFB info = { ISFB_MASK_IDLIST };
    hr = band->GetBandInfoSFB(&info);
    ok(hr == S_OK, "%s: GetBandInfoSFB returned 0x%08lx\n", what, hr);
    if (hr == S_OK)
    {
        ok(info.pidl && ILIsEqual(info.pidl, pidl), "%s: the band shows another folder\n", what);
        ILFree(info.pidl);
    }
}

static HRESULT SaveSite(BandSite &bs, CComPtr<IStream> &stream)
{
    HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    ok_hex(hr, S_OK);
    if (FAILED(hr))
        return hr;
    return bs.persist->Save(stream, TRUE);
}

static void Rewind(IStream *stream)
{
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, NULL);
}

START_TEST(BandSite)
{
    CCoInit init;
    ok_hex(init.hr, S_OK);
    if (FAILED(init.hr))
    {
        skip("CoInitialize failed with 0x%08lX\n", init.hr);
        return;
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"bandsite", WS_POPUP, 0, 0, 400, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");
    if (!hwnd)
        return;

    WCHAR szWindows[MAX_PATH], szTemp[MAX_PATH];
    GetWindowsDirectoryW(szWindows, _countof(szWindows));
    GetTempPathW(_countof(szTemp), szTemp);
    PathRemoveBackslashW(szTemp);

    CComHeapPtr<ITEMIDLIST_ABSOLUTE> pidlWindows, pidlTemp;
    pidlWindows.Attach(ILCreateFromPathW(szWindows));
    pidlTemp.Attach(ILCreateFromPathW(szTemp));
    ok(pidlWindows && pidlTemp, "ILCreateFromPath failed\n");

    BandSite one;
    if (pidlWindows && pidlTemp && CreateBandSite(hwnd, one))
    {
        ok_int(CountBands(one.site), 0);
        AddFolderBand(one, pidlWindows, FALSE);
        AddFolderBand(one, pidlTemp, TRUE);
        ok_int(CountBands(one.site), 2);

        CComPtr<IStream> stream;
        HRESULT hr = SaveSite(one, stream);
        ok_hex(hr, S_OK);

        if (hr == S_OK)
        {
            /* The band site reads back what it saved */
            BandSite two;
            if (CreateBandSite(hwnd, two))
            {
                Rewind(stream);
                hr = two.persist->Load(stream);
                ok_hex(hr, S_OK);
                ok_int(CountBands(two.site), 2);

                /* The band that asks to be added to the front comes first, as in the first band site */
                UINT first = 0, second = 1;
                DWORD id;
                CComPtr<IShellFolderBand> band;
                if (one.site->EnumBands(0, &id) == S_OK &&
                    SUCCEEDED(one.site->GetBandObject(id, IID_PPV_ARG(IShellFolderBand, &band))))
                {
                    BANDINFOSFB info = { ISFB_MASK_IDLIST };
                    if (SUCCEEDED(band->GetBandInfoSFB(&info)) && info.pidl)
                    {
                        if (!ILIsEqual(info.pidl, pidlTemp))
                        {
                            first = 1;
                            second = 0;
                        }
                        ILFree(info.pidl);
                    }
                }
                PCIDLIST_ABSOLUTE pidls[2] = { pidlWindows, pidlTemp };
                ExpectFolder(two.site, 0, pidls[first], "first band");
                ExpectFolder(two.site, 1, pidls[second], "second band");

                /* A second save of the loaded site gives the same bytes */
                CComPtr<IStream> stream2;
                hr = SaveSite(two, stream2);
                ok_hex(hr, S_OK);
                if (hr == S_OK)
                {
                    STATSTG stat1 = {}, stat2 = {};
                    stream->Stat(&stat1, STATFLAG_NONAME);
                    stream2->Stat(&stat2, STATFLAG_NONAME);
                    ok_int(stat1.cbSize.LowPart == stat2.cbSize.LowPart, TRUE);
                }
            }

            /* Data that is not a band site stream is refused and adds no band */
            BandSite three;
            if (CreateBandSite(hwnd, three))
            {
                static const BYTE junk[] = "this is not a band site stream at all, not even close";
                CComPtr<IStream> bad;
                CreateStreamOnHGlobal(NULL, TRUE, &bad);
                ULONG written;
                bad->Write(junk, sizeof(junk), &written);
                Rewind(bad);
                hr = three.persist->Load(bad);
                ok(FAILED(hr), "Load of junk returned 0x%08lx\n", hr);
                ok_int(CountBands(three.site), 0);

                /* An empty stream as well */
                CComPtr<IStream> empty;
                CreateStreamOnHGlobal(NULL, TRUE, &empty);
                hr = three.persist->Load(empty);
                ok(FAILED(hr), "Load of an empty stream returned 0x%08lx\n", hr);
                ok_int(CountBands(three.site), 0);
            }

            /* A stream cut inside the first band adds no band */
            BandSite four;
            if (CreateBandSite(hwnd, four))
            {
                HGLOBAL hglobal;
                if (SUCCEEDED(GetHGlobalFromStream(stream, &hglobal)))
                {
                    CComPtr<IStream> cut;
                    CreateStreamOnHGlobal(NULL, TRUE, &cut);
                    PVOID data = GlobalLock(hglobal);
                    ULONG written;
                    cut->Write(data, 12 + 20, &written);
                    GlobalUnlock(hglobal);
                    Rewind(cut);
                    hr = four.persist->Load(cut);
                    ok(FAILED(hr), "Load of a cut stream returned 0x%08lx\n", hr);
                    ok_int(CountBands(four.site), 0);
                }
            }
        }
    }

    one = BandSite();
    DestroyWindow(hwnd);
}
