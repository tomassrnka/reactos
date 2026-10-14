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
#include <strsafe.h>
#include <versionhelpers.h>
#include <reactos/shlguid_undoc.h>

/* The window the band site puts its rebar into */
class CTestSite : public IOleWindow
{
    LONG m_cRef = 1;
    HWND m_hwnd;

public:
    CTestSite(HWND hwnd) : m_hwnd(hwnd) { }
    virtual ~CTestSite() { }

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
    HWND hwndRebar = NULL;

    /* The bands and the site hold each other, so the bands go before the site does */
    ~BandSite()
    {
        DWORD id;
        while (site && SUCCEEDED(site->EnumBands(0, &id)))
        {
            if (FAILED(site->RemoveBand(id)))
                break;
        }
    }
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
    if (FAILED(hr))
        return false;

    hr = bs.client->GetWindow(&bs.hwndRebar);
    ok_hex(hr, S_OK);
    return bs.hwndRebar != NULL;
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

/* The folder of the band at an index of the rebar, from the left or the top */
static bool GetFolderAt(BandSite &bs, UINT index, CComHeapPtr<ITEMIDLIST_ABSOLUTE> &pidl, BOOL *pBreak = NULL)
{
    REBARBANDINFOW rbi = { sizeof(rbi), RBBIM_ID | RBBIM_STYLE };
    if (!SendMessageW(bs.hwndRebar, RB_GETBANDINFOW, index, (LPARAM)&rbi))
        return false;
    if (pBreak)
        *pBreak = !!(rbi.fStyle & RBBS_BREAK);

    CComPtr<IShellFolderBand> band;
    if (FAILED(bs.site->GetBandObject(rbi.wID, IID_PPV_ARG(IShellFolderBand, &band))))
        return false;

    BANDINFOSFB info = { ISFB_MASK_IDLIST };
    if (FAILED(band->GetBandInfoSFB(&info)))
        return false;
    pidl.Attach(info.pidl);
    return info.pidl != NULL;
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

static bool SameBytes(IStream *a, IStream *b)
{
    HGLOBAL ha, hb;
    STATSTG sa = {}, sb = {};
    a->Stat(&sa, STATFLAG_NONAME);
    b->Stat(&sb, STATFLAG_NONAME);
    if (sa.cbSize.QuadPart != sb.cbSize.QuadPart ||
        FAILED(GetHGlobalFromStream(a, &ha)) || FAILED(GetHGlobalFromStream(b, &hb)))
    {
        return false;
    }
    bool same = !memcmp(GlobalLock(ha), GlobalLock(hb), (size_t)sa.cbSize.QuadPart);
    GlobalUnlock(ha);
    GlobalUnlock(hb);
    return same;
}

static CComPtr<IStream> MakeStream(const void *data, ULONG cb)
{
    CComPtr<IStream> stream;
    CreateStreamOnHGlobal(NULL, TRUE, &stream);
    ULONG written;
    stream->Write(data, cb, &written);
    Rewind(stream);
    return stream;
}

/* The stream of this implementation: header, then entries of length, flags, width and the band */
#pragma pack(push, 1)
struct TEST_HEADER { DWORD magic, version, count; };
struct TEST_ENTRY { DWORD length, flags, cx; CLSID clsid; };
#pragma pack(pop)

static void TestRefusedStream(HWND hwnd, const char *what, IStream *stream)
{
    BandSite bs;
    if (!CreateBandSite(hwnd, bs))
        return;
    HRESULT hr = bs.persist->Load(stream);
    ok(FAILED(hr), "%s: Load returned 0x%08lx\n", what, hr);
    ok(CountBands(bs.site) == 0, "%s: %u bands were added\n", what, CountBands(bs.site));
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

    /* Folders that are empty, so that the toolbars have nothing to enumerate */
    WCHAR szBase[MAX_PATH], szDirs[3][MAX_PATH];
    GetTempPathW(_countof(szBase), szBase);
    for (UINT i = 0; i < 3; i++)
    {
        StringCchPrintfW(szDirs[i], _countof(szDirs[i]), L"%sbandsite_apitest_%lu_%u", szBase, GetCurrentProcessId(), i);
        CreateDirectoryW(szDirs[i], NULL);
    }

    CComHeapPtr<ITEMIDLIST_ABSOLUTE> pidlWindows, pidlTemp, pidlSystem;
    pidlWindows.Attach(ILCreateFromPathW(szDirs[0]));
    pidlTemp.Attach(ILCreateFromPathW(szDirs[1]));
    pidlSystem.Attach(ILCreateFromPathW(szDirs[2]));
    ok(pidlWindows && pidlTemp && pidlSystem, "ILCreateFromPath failed\n");

    BandSite one;
    if (pidlWindows && pidlTemp && pidlSystem && CreateBandSite(hwnd, one))
    {
        ok_int(CountBands(one.site), 0);
        AddFolderBand(one, pidlWindows, FALSE);
        AddFolderBand(one, pidlTemp, TRUE);
        AddFolderBand(one, pidlSystem, FALSE);
        ok_int(CountBands(one.site), 3);

        /* Quick Launch asks to be added to the front: move it to the end, and start a row at the middle band */
        ok_int((int)SendMessageW(one.hwndRebar, RB_GETBANDCOUNT, 0, 0), 3);
        SendMessageW(one.hwndRebar, RB_MOVEBAND, 0, 2);
        REBARBANDINFOW rbi = { sizeof(rbi), RBBIM_STYLE };
        SendMessageW(one.hwndRebar, RB_GETBANDINFOW, 1, (LPARAM)&rbi);
        rbi.fStyle |= RBBS_BREAK;
        SendMessageW(one.hwndRebar, RB_SETBANDINFOW, 1, (LPARAM)&rbi);

        CComPtr<IStream> stream;
        HRESULT hr = SaveSite(one, stream);
        ok_hex(hr, S_OK);

        if (hr == S_OK)
        {
            /* The band site reads back what it saved: the bands, their order and the break */
            BandSite two;
            if (CreateBandSite(hwnd, two))
            {
                Rewind(stream);
                hr = two.persist->Load(stream);
                ok_hex(hr, S_OK);
                ok_int(CountBands(two.site), 3);

                for (UINT i = 0; i < 3; i++)
                {
                    CComHeapPtr<ITEMIDLIST_ABSOLUTE> pidl1, pidl2;
                    BOOL break1 = FALSE, break2 = FALSE;
                    ok(GetFolderAt(one, i, pidl1, &break1), "no band at index %u of the first site\n", i);
                    ok(GetFolderAt(two, i, pidl2, &break2), "no band at index %u of the loaded site\n", i);
                    ok(pidl1 && pidl2 && ILIsEqual(pidl1, pidl2), "the band at index %u shows another folder\n", i);
                    ok(break1 == break2, "the band at index %u has break %d, expected %d\n", i, break2, break1);
                }

                /* A second save of the loaded site gives the same bytes */
                CComPtr<IStream> stream2;
                hr = SaveSite(two, stream2);
                ok_hex(hr, S_OK);
                if (hr == S_OK)
                    ok(SameBytes(stream, stream2), "the second save differs from the first\n");
            }

            /* Data that is not a band site stream and an empty stream are refused */
            static const BYTE junk[] = "this is not a band site stream at all, not even close";
            TestRefusedStream(hwnd, "junk", MakeStream(junk, sizeof(junk)));
            TestRefusedStream(hwnd, "empty", MakeStream(NULL, 0));

            /* A stream cut inside the first band: the form of the stream decides what is left of it */
            HGLOBAL hglobal;
            if (IsReactOS() && SUCCEEDED(GetHGlobalFromStream(stream, &hglobal)))
            {
                PVOID data = GlobalLock(hglobal);
                TestRefusedStream(hwnd, "cut", MakeStream(data, sizeof(TEST_HEADER) + 20));
                GlobalUnlock(hglobal);
            }
        }
    }

    /* Malformed streams, made by changing a saved one in the form of this implementation */
    if (IsReactOS() && pidlWindows)
    {
        BandSite src;
        CComPtr<IStream> good;
        if (CreateBandSite(hwnd, src) && SUCCEEDED(AddFolderBand(src, pidlWindows, FALSE)) && SUCCEEDED(SaveSite(src, good)))
        {
            HGLOBAL hglobal;
            STATSTG stat = {};
            good->Stat(&stat, STATFLAG_NONAME);
            ULONG cb = stat.cbSize.LowPart;
            CHeapPtr<BYTE> bytes;
            if (!bytes.Allocate(cb))
                return;
            if (SUCCEEDED(GetHGlobalFromStream(good, &hglobal)))
            {
                memcpy(bytes.m_pData, GlobalLock(hglobal), cb);
                GlobalUnlock(hglobal);
            }
            ok(cb > sizeof(TEST_HEADER) + sizeof(TEST_ENTRY), "the saved stream is only %lu bytes\n", cb);

            /* The stream itself loads */
            {
                BandSite bs;
                if (CreateBandSite(hwnd, bs))
                {
                    HRESULT hr = bs.persist->Load(MakeStream(bytes.m_pData, cb));
                    ok_hex(hr, S_OK);
                    ok_int(CountBands(bs.site), 1);
                }
            }

            TEST_ENTRY *entry = (TEST_ENTRY *)(bytes.m_pData + sizeof(TEST_HEADER));
            const DWORD goodLength = entry->length;
            ok(goodLength == cb - sizeof(TEST_HEADER) - sizeof(DWORD), "entry length %lu in a stream of %lu bytes\n", goodLength, cb);

            /* A length that wraps the position of the next entry back into the stream */
            entry->length = 0xFFFFFFFC;
            TestRefusedStream(hwnd, "wrapping length", MakeStream(bytes.m_pData, cb));
            /* A length beyond the end of the stream */
            entry->length = goodLength + 4;
            TestRefusedStream(hwnd, "length beyond the end", MakeStream(bytes.m_pData, cb));
            /* A length too short for the class */
            entry->length = 8;
            TestRefusedStream(hwnd, "length too short", MakeStream(bytes.m_pData, cb));
            /* A length one short: the band must not read the last byte, which is outside its entry */
            entry->length = goodLength - 1;
            TestRefusedStream(hwnd, "length one byte short", MakeStream(bytes.m_pData, cb));
            entry->length = goodLength;

            /* A class that cannot load itself from a stream */
            entry->clsid = CLSID_ACLHistory;
            TestRefusedStream(hwnd, "class without IPersistStream", MakeStream(bytes.m_pData, cb));
            entry->clsid = CLSID_ISFBand;

            /* A folder band with a version it does not know, and with a path that is not one */
            BYTE *payload = bytes.m_pData + sizeof(TEST_HEADER) + sizeof(TEST_ENTRY);
            DWORD version = *(DWORD *)payload;
            *(DWORD *)payload = 0x7FFFFFFF;
            TestRefusedStream(hwnd, "unknown band version", MakeStream(bytes.m_pData, cb));
            *(DWORD *)payload = version;
            DWORD cch = ((DWORD *)payload)[2];
            ((DWORD *)payload)[2] = 0x7FFFFFFF;
            TestRefusedStream(hwnd, "path of a size that does not fit", MakeStream(bytes.m_pData, cb));
            ((DWORD *)payload)[2] = cch;
            ((WCHAR *)(payload + 3 * sizeof(DWORD)))[0] = L'\0';
            TestRefusedStream(hwnd, "path that ends early", MakeStream(bytes.m_pData, cb));
            memset(payload + 3 * sizeof(DWORD), 'Z', cch * sizeof(WCHAR));
            TestRefusedStream(hwnd, "path of a folder that is not there", MakeStream(bytes.m_pData, cb));
        }
    }
    else
    {
        skip("The form of the stream is that of ReactOS\n");
    }

    for (UINT i = 0; i < 3; i++)
        RemoveDirectoryW(szDirs[i]);
    DestroyWindow(hwnd);
}
