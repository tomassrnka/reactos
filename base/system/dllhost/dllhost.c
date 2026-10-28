/*
 * PROJECT:     ReactOS DllHost
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     COM surrogate.
 * COPYRIGHT:   Copyright 2018 Eric Kohl
 *              Copyright 2026 Tomas Srnka
 */

/*
 * combase starts "dllhost.exe /Processid:{CLSID}" for a class whose AppID has
 * a DllSurrogate value, then waits for the CLSID to show up in rpcss. The
 * surrogate loads the in-process server, registers its class factory as a
 * local server and stays up until it is asked to exit: combase calls
 * ISurrogate::FreeSurrogate when the last external reference to an object of the
 * surrogate is released, as Windows does. A client that dies without releasing
 * its objects keeps the surrogate alive.
 */

/* INCLUDES *****************************************************************/

#define WIN32_NO_STATUS
#define COBJMACROS
#include <windows.h>
#include <objbase.h>

/* GLOBALS ******************************************************************/

static HANDLE g_hExitEvent;

/* ISurrogate ***************************************************************/

static HRESULT WINAPI
Surrogate_QueryInterface(ISurrogate *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISurrogate))
    {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Surrogate_AddRef(ISurrogate *This)  { return 2; }
static ULONG WINAPI Surrogate_Release(ISurrogate *This) { return 1; }

static HRESULT WINAPI
Surrogate_LoadDllServer(ISurrogate *This, REFCLSID rclsid)
{
    /* The server named on the command line is loaded at startup. */
    return S_OK;
}

static HRESULT WINAPI
Surrogate_FreeSurrogate(ISurrogate *This)
{
    SetEvent(g_hExitEvent);
    return S_OK;
}

static const ISurrogateVtbl g_SurrogateVtbl =
{
    Surrogate_QueryInterface,
    Surrogate_AddRef,
    Surrogate_Release,
    Surrogate_LoadDllServer,
    Surrogate_FreeSurrogate
};

static ISurrogate g_Surrogate = { (ISurrogateVtbl *)&g_SurrogateVtbl };

/* FUNCTIONS ****************************************************************/

/* Find "/Processid:{CLSID}" (switch case-insensitive, "-" accepted). */
static BOOL
ParseProcessId(LPCWSTR pszCmdLine, CLSID *pClsid)
{
    static const WCHAR szSwitch[] = L"processid:";
    WCHAR szGuid[40];
    LPCWSTR p, q;
    UINT i;

    for (p = pszCmdLine; *p; p++)
    {
        if (*p != L'/' && *p != L'-')
            continue;

        for (i = 0, q = p + 1; szSwitch[i]; i++, q++)
        {
            if (towlower(*q) != szSwitch[i])
                break;
        }
        if (szSwitch[i])
            continue;

        for (i = 0; *q && *q != L' ' && *q != L'\t' && i < ARRAYSIZE(szGuid) - 1; i++, q++)
            szGuid[i] = *q;
        szGuid[i] = UNICODE_NULL;

        return SUCCEEDED(CLSIDFromString(szGuid, pClsid));
    }
    return FALSE;
}

INT
WINAPI
wWinMain(
    HINSTANCE hInst,
    HINSTANCE hPrevInst,
    LPWSTR lpCmdLine,
    INT nCmdShow)
{
    IClassFactory *pFactory = NULL;
    CLSID clsid;
    DWORD dwCookie;
    HRESULT hr;
    INT nRet = 1;

    if (!ParseProcessId(GetCommandLineW(), &clsid))
        return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return 1;

    g_hExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_hExitEvent)
        goto done;

    /* Not implemented by every COM runtime; registration below does not depend on it. */
    CoRegisterSurrogate(&g_Surrogate);

    hr = CoGetClassObject(&clsid, CLSCTX_INPROC_SERVER, NULL, &IID_IClassFactory, (void **)&pFactory);
    if (FAILED(hr))
        goto done;

    hr = CoRegisterClassObject(&clsid,
                               (IUnknown *)pFactory,
                               CLSCTX_LOCAL_SERVER,
                               REGCLS_MULTIPLEUSE | REGCLS_SURROGATE,
                               &dwCookie);
    if (FAILED(hr))
        goto done;

    nRet = 0;
    WaitForSingleObject(g_hExitEvent, INFINITE);

    CoRevokeClassObject(dwCookie);

done:
    if (pFactory)
        IClassFactory_Release(pFactory);
    if (g_hExitEvent)
        CloseHandle(g_hExitEvent);
    CoUninitialize();
    return nRet;
}

/* EOF */
