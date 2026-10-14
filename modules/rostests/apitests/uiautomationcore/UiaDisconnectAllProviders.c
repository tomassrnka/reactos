/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for uiautomationcore!UiaDisconnectAllProviders (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64): the call returns S_OK, also when it is repeated and when no provider
 * exists. A node that a client got for a window of another thread is disconnected by it: the provider is asked no
 * more, the client gets UIA_E_ELEMENTNOTAVAILABLE, and the references that UI Automation held on the provider are
 * released. Windows 10 also survives a provider that calls it, or UiaDisconnectProvider, from Release inside the call. It hangs when a
 * provider calls it from Release inside UiaDisconnectProvider, and when it runs in a loop on one thread while
 * another thread creates nodes; ReactOS has to survive both, so those two tests run on ReactOS only.
 */

#define COBJMACROS
#include <apitest.h>
#include <ole2.h>
#include <initguid.h>
#include <uiautomation.h>

typedef HRESULT (WINAPI *PFN_UiaDisconnectAllProviders)(void);
typedef HRESULT (WINAPI *PFN_UiaDisconnectProvider)(IRawElementProviderSimple *);
typedef HRESULT (WINAPI *PFN_UiaNodeFromHandle)(HWND, HUIANODE *);
typedef BOOL (WINAPI *PFN_UiaNodeRelease)(HUIANODE);
typedef HRESULT (WINAPI *PFN_UiaGetPropertyValue)(HUIANODE, PROPERTYID, VARIANT *);
typedef LRESULT (WINAPI *PFN_UiaReturnRawElementProvider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple *);

static PFN_UiaDisconnectAllProviders pUiaDisconnectAllProviders;
static PFN_UiaDisconnectProvider pUiaDisconnectProvider;
static PFN_UiaNodeFromHandle pUiaNodeFromHandle;
static PFN_UiaNodeRelease pUiaNodeRelease;
static PFN_UiaGetPropertyValue pUiaGetPropertyValue;
static PFN_UiaReturnRawElementProvider pUiaReturnRawElementProvider;

#define STRESS_ROUNDS 150

struct provider
{
    IRawElementProviderSimple simple;
    IRawElementProviderFragment frag;
    LONG ref;
    LONG prop_calls;
    /* Second element of the runtime ID (the first is 5). */
    int rt_id;
    HWND hwnd;
    /* Runs once from Release, after hook_after calls of Release, when armed. */
    LONG releases;
    LONG hook_after;
    LONG hook_armed;
    void (*hook)(void);
};

static struct provider *impl_from_simple(IRawElementProviderSimple *iface)
{
    return CONTAINING_RECORD(iface, struct provider, simple);
}

static struct provider *impl_from_frag(IRawElementProviderFragment *iface)
{
    return CONTAINING_RECORD(iface, struct provider, frag);
}

static HRESULT WINAPI Simple_QueryInterface(IRawElementProviderSimple *iface, REFIID riid, void **ppv)
{
    struct provider *prov = impl_from_simple(iface);

    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRawElementProviderSimple))
        *ppv = &prov->simple;
    else if (IsEqualIID(riid, &IID_IRawElementProviderFragment))
        *ppv = &prov->frag;
    else
        return E_NOINTERFACE;

    IRawElementProviderSimple_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI Simple_AddRef(IRawElementProviderSimple *iface)
{
    return InterlockedIncrement(&impl_from_simple(iface)->ref);
}

static ULONG WINAPI Simple_Release(IRawElementProviderSimple *iface)
{
    struct provider *prov = impl_from_simple(iface);
    LONG count = InterlockedIncrement(&prov->releases) - 1, ref;

    if (prov->hook && count >= prov->hook_after && InterlockedExchange(&prov->hook_armed, 0))
        prov->hook();
    ref = InterlockedDecrement(&prov->ref);
    if (!ref)
        free(prov);
    return ref;
}

static HRESULT WINAPI Simple_get_ProviderOptions(IRawElementProviderSimple *iface, enum ProviderOptions *ret)
{
    *ret = ProviderOptions_ServerSideProvider;
    return S_OK;
}

static HRESULT WINAPI Simple_GetPatternProvider(IRawElementProviderSimple *iface, PATTERNID id, IUnknown **ret)
{
    *ret = NULL;
    return S_OK;
}

static HRESULT WINAPI Simple_GetPropertyValue(IRawElementProviderSimple *iface, PROPERTYID id, VARIANT *ret)
{
    struct provider *prov = impl_from_simple(iface);

    InterlockedIncrement(&prov->prop_calls);
    VariantInit(ret);
    if (id == UIA_ControlTypePropertyId)
    {
        V_VT(ret) = VT_I4;
        V_I4(ret) = UIA_ButtonControlTypeId;
    }
    return S_OK;
}

static HRESULT WINAPI Simple_get_HostRawElementProvider(IRawElementProviderSimple *iface,
                                                        IRawElementProviderSimple **ret)
{
    *ret = NULL;
    return S_OK;
}

static IRawElementProviderSimpleVtbl SimpleVtbl =
{
    Simple_QueryInterface,
    Simple_AddRef,
    Simple_Release,
    Simple_get_ProviderOptions,
    Simple_GetPatternProvider,
    Simple_GetPropertyValue,
    Simple_get_HostRawElementProvider,
};

static HRESULT WINAPI Frag_QueryInterface(IRawElementProviderFragment *iface, REFIID riid, void **ppv)
{
    return Simple_QueryInterface(&impl_from_frag(iface)->simple, riid, ppv);
}

static ULONG WINAPI Frag_AddRef(IRawElementProviderFragment *iface)
{
    return Simple_AddRef(&impl_from_frag(iface)->simple);
}

static ULONG WINAPI Frag_Release(IRawElementProviderFragment *iface)
{
    return Simple_Release(&impl_from_frag(iface)->simple);
}

static HRESULT WINAPI Frag_Navigate(IRawElementProviderFragment *iface, enum NavigateDirection dir,
                                    IRawElementProviderFragment **ret)
{
    *ret = NULL;
    return S_OK;
}

static HRESULT WINAPI Frag_GetRuntimeId(IRawElementProviderFragment *iface, SAFEARRAY **ret)
{
    struct provider *prov = impl_from_frag(iface);
    LONG idx, val[2] = { 5, prov->rt_id };

    *ret = NULL;
    if (!prov->rt_id)
        return S_OK;

    if (!(*ret = SafeArrayCreateVector(VT_I4, 0, 2)))
        return E_OUTOFMEMORY;
    for (idx = 0; idx < 2; idx++)
        SafeArrayPutElement(*ret, &idx, &val[idx]);
    return S_OK;
}

static HRESULT WINAPI Frag_get_BoundingRectangle(IRawElementProviderFragment *iface, struct UiaRect *ret)
{
    memset(ret, 0, sizeof(*ret));
    return S_OK;
}

static HRESULT WINAPI Frag_GetEmbeddedFragmentRoots(IRawElementProviderFragment *iface, SAFEARRAY **ret)
{
    *ret = NULL;
    return S_OK;
}

static HRESULT WINAPI Frag_SetFocus(IRawElementProviderFragment *iface)
{
    return S_OK;
}

static HRESULT WINAPI Frag_get_FragmentRoot(IRawElementProviderFragment *iface, IRawElementProviderFragmentRoot **ret)
{
    *ret = NULL;
    return S_OK;
}

static IRawElementProviderFragmentVtbl FragVtbl =
{
    Frag_QueryInterface,
    Frag_AddRef,
    Frag_Release,
    Frag_Navigate,
    Frag_GetRuntimeId,
    Frag_get_BoundingRectangle,
    Frag_GetEmbeddedFragmentRoots,
    Frag_SetFocus,
    Frag_get_FragmentRoot,
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_GETOBJECT && lparam == UiaRootObjectId)
    {
        struct provider *prov = (struct provider *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

        if (prov)
            return pUiaReturnRawElementProvider(hwnd, wparam, lparam, &prov->simple);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/*
 * The windows belong to a thread of their own, as the nodes are only connected through the provider thread when
 * the client is not the thread of the window: a client on the thread of the window gets the provider unwrapped.
 */
#define WM_CREATE_TEST_WINDOW (WM_USER + 1)
#define WM_DESTROY_TEST_WINDOW (WM_USER + 2)

static HWND controller;
static HANDLE window_thread;

static LRESULT CALLBACK ControllerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_CREATE_TEST_WINDOW:
        {
            struct provider *prov = (struct provider *)lparam;
            HWND window = CreateWindowExW(0, L"UiaDisconnectAllProviders", L"test", WS_OVERLAPPEDWINDOW,
                                          0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);

            if (window)
                SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)prov);
            return (LRESULT)window;
        }
        case WM_DESTROY_TEST_WINDOW:
            return DestroyWindow((HWND)wparam);
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI WindowThread(void *arg)
{
    HANDLE ready = arg;
    WNDCLASSW cls;
    MSG msg;

    memset(&cls, 0, sizeof(cls));
    cls.lpfnWndProc = WndProc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"UiaDisconnectAllProviders";
    RegisterClassW(&cls);
    cls.lpfnWndProc = ControllerProc;
    cls.lpszClassName = L"UiaDisconnectAllProvidersController";
    RegisterClassW(&cls);

    controller = CreateWindowExW(0, cls.lpszClassName, L"controller", 0, 0, 0, 0, 0, NULL, NULL, cls.hInstance, NULL);
    SetEvent(ready);
    if (!controller)
        return 1;

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(controller);
    UnregisterClassW(L"UiaDisconnectAllProviders", GetModuleHandleW(NULL));
    UnregisterClassW(L"UiaDisconnectAllProvidersController", GetModuleHandleW(NULL));
    return 0;
}

static BOOL start_window_thread(void)
{
    HANDLE ready = CreateEventW(NULL, FALSE, FALSE, NULL);

    window_thread = CreateThread(NULL, 0, WindowThread, ready, 0, NULL);
    ok(window_thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (window_thread)
        WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    ok(controller != NULL, "no controller window\n");
    return controller != NULL;
}

static void stop_window_thread(void)
{
    SendMessageW(controller, WM_CLOSE, 0, 0);
    WaitForSingleObject(window_thread, INFINITE);
    CloseHandle(window_thread);
}

/* The test holds one reference; the provider is freed by the last Release, so a late callback is harmless. */
static struct provider *new_provider(int rt_id)
{
    struct provider *prov = calloc(1, sizeof(*prov));

    prov->simple.lpVtbl = &SimpleVtbl;
    prov->frag.lpVtbl = &FragVtbl;
    prov->ref = 1;
    prov->rt_id = rt_id;
    prov->hwnd = (HWND)SendMessageW(controller, WM_CREATE_TEST_WINDOW, 0, (LPARAM)prov);
    ok(prov->hwnd != NULL, "no window\n");
    return prov;
}

static void free_provider(struct provider *prov)
{
    ok(SendMessageW(controller, WM_DESTROY_TEST_WINDOW, (WPARAM)prov->hwnd, 0), "DestroyWindow failed\n");
    IRawElementProviderSimple_Release(&prov->simple);
}

static LONG get_ref(struct provider *prov)
{
    return InterlockedCompareExchange(&prov->ref, 0, 0);
}

/* Windows may release its references a moment after the call. */
static LONG wait_ref(struct provider *prov, LONG expected)
{
    int i;

    for (i = 0; i < 100 && get_ref(prov) != expected; i++)
        Sleep(10);
    return get_ref(prov);
}

#define ok_ref(prov, expected) \
    do { LONG _ref = wait_ref(prov, expected); ok(_ref == (expected), "%s: ref %ld, expected %ld\n", #prov, _ref, (LONG)(expected)); } while (0)

static HUIANODE get_node(struct provider *prov)
{
    HUIANODE node = NULL;
    HRESULT hr = pUiaNodeFromHandle(prov->hwnd, &node);

    ok_hr(hr, S_OK);
    ok(node != NULL, "no node\n");
    return node;
}

static void check_node_works(HUIANODE node, struct provider *prov)
{
    LONG calls = prov->prop_calls;
    VARIANT v;
    HRESULT hr;

    VariantInit(&v);
    hr = pUiaGetPropertyValue(node, UIA_ControlTypePropertyId, &v);
    ok_hr(hr, S_OK);
    ok(V_VT(&v) == VT_I4, "vt %d\n", V_VT(&v));
    ok(prov->prop_calls > calls, "the provider was not asked\n");
    VariantClear(&v);
}

static void check_node_gone(HUIANODE node, struct provider *prov)
{
    LONG calls = prov->prop_calls;
    VARIANT v;
    HRESULT hr;

    VariantInit(&v);
    hr = pUiaGetPropertyValue(node, UIA_ControlTypePropertyId, &v);
    ok_hr(hr, UIA_E_ELEMENTNOTAVAILABLE);
    ok(prov->prop_calls == calls, "the provider was asked\n");
    VariantClear(&v);
}

static void release_node(HUIANODE node)
{
    ok(pUiaNodeRelease(node), "UiaNodeRelease failed\n");
}

static void call_all(void)
{
    ok_hr(pUiaDisconnectAllProviders(), S_OK);
}

static void test_empty(void)
{
    int i;

    for (i = 0; i < 2; i++)
        call_all();
}

static void test_one_node(void)
{
    struct provider *prov;
    HUIANODE node;

    prov = new_provider(11);
    node = get_node(prov);
    ok(get_ref(prov) >= 2, "ref %ld\n", get_ref(prov));
    check_node_works(node, prov);

    call_all();
    ok_ref(prov, 1);
    check_node_gone(node, prov);

    call_all();
    release_node(node);
    ok_ref(prov, 1);
    free_provider(prov);
}

static void test_two_providers(void)
{
    struct provider *prov1, *prov2;
    HUIANODE node1, node2;

    prov1 = new_provider(21);
    prov2 = new_provider(22);
    node1 = get_node(prov1);
    node2 = get_node(prov2);
    check_node_works(node1, prov1);
    check_node_works(node2, prov2);

    call_all();
    ok_ref(prov1, 1);
    ok_ref(prov2, 1);
    check_node_gone(node1, prov1);
    check_node_gone(node2, prov2);

    release_node(node1);
    release_node(node2);
    free_provider(prov1);
    free_provider(prov2);
}

static void test_nodes_without_runtime_id(void)
{
    struct provider *prov;
    HUIANODE node;

    prov = new_provider(0);
    node = get_node(prov);
    check_node_works(node, prov);

    call_all();
    ok_ref(prov, 1);
    check_node_gone(node, prov);

    release_node(node);
    free_provider(prov);
}

static void test_same_runtime_id(void)
{
    struct provider *prov1, *prov2;
    HUIANODE node1, node2, node3;

    prov1 = new_provider(31);
    prov2 = new_provider(31);
    /* Two nodes of one provider, one node of another provider with the same runtime ID. */
    node1 = get_node(prov1);
    node2 = get_node(prov1);
    node3 = get_node(prov2);
    check_node_works(node1, prov1);
    check_node_works(node2, prov1);
    check_node_works(node3, prov2);

    call_all();
    ok_ref(prov1, 1);
    ok_ref(prov2, 1);
    check_node_gone(node1, prov1);
    check_node_gone(node2, prov1);
    check_node_gone(node3, prov2);

    release_node(node1);
    release_node(node2);
    release_node(node3);
    free_provider(prov1);
    free_provider(prov2);
}

static void test_released_node(void)
{
    struct provider *prov1, *prov2;
    HUIANODE node1, node2;

    prov1 = new_provider(41);
    prov2 = new_provider(42);
    node1 = get_node(prov1);
    node2 = get_node(prov2);

    release_node(node1);
    ok_ref(prov1, 1);

    call_all();
    ok_ref(prov2, 1);
    check_node_gone(node2, prov2);

    release_node(node2);
    free_provider(prov1);
    free_provider(prov2);
}

/* New nodes of a provider are connected again after a bulk disconnection. */
static void test_reconnect(void)
{
    struct provider *prov;
    HUIANODE node1, node2;

    prov = new_provider(51);
    node1 = get_node(prov);
    call_all();
    ok_ref(prov, 1);

    node2 = get_node(prov);
    check_node_works(node2, prov);
    check_node_gone(node1, prov);

    call_all();
    ok_ref(prov, 1);
    check_node_gone(node2, prov);

    release_node(node1);
    release_node(node2);
    free_provider(prov);
}

static struct provider *hook_other;
static HUIANODE hook_node;
static HRESULT hook_hr;
static LONG hook_calls;

/* The result starts as a success, so that a hook that did not run fails the check. */
static void hook_reset(HUIANODE node)
{
    hook_node = node;
    hook_hr = S_OK;
    hook_calls = 0;
}

static void hook_query_node(void)
{
    VARIANT v;

    VariantInit(&v);
    hook_hr = pUiaGetPropertyValue(hook_node, UIA_ControlTypePropertyId, &v);
    VariantClear(&v);
    InterlockedIncrement(&hook_calls);
}

static void check_hook(void)
{
    int i;

    for (i = 0; i < 100 && hook_calls == 0; i++)
        Sleep(10);
    ok(hook_calls == 1, "the hook ran %ld times\n", hook_calls);
    ok(FAILED(hook_hr), "the node answered during its disconnection: %#lx\n", hook_hr);
}

static void hook_call_all(void)
{
    call_all();
}

/* The node of the other provider has to be disconnected when the nested call returns. */
static void hook_disconnect_other(void)
{
    ok_hr(pUiaDisconnectProvider(&hook_other->simple), S_OK);
    hook_query_node();
}

static void hook_call_all_and_other(void)
{
    call_all();
    ok_hr(pUiaDisconnectProvider(&hook_other->simple), S_OK);
}

/* Windows 10 22H2 hangs in the tests that ask this (measured), so they run on ReactOS only. */
static BOOL hangs_on_windows(const char *what)
{
    if (is_reactos())
        return FALSE;
    skip("%s hangs on Windows\n", what);
    return TRUE;
}

/*
 * The application asks for a property of the node that is being disconnected, from the Release of its provider. The
 * node must not answer. Windows 10 gives E_FAIL inside UiaDisconnectAllProviders and UIA_E_ELEMENTNOTAVAILABLE
 * inside UiaDisconnectProvider, so only the failure is checked.
 */
static void test_query_in_release(void)
{
    struct provider *prov;
    HUIANODE node;

    prov = new_provider(111);
    node = get_node(prov);
    hook_reset(node);
    prov->hook = hook_query_node;
    prov->hook_armed = 1;
    call_all();
    ok_ref(prov, 1);
    check_hook();
    check_node_gone(node, prov);
    release_node(node);
    free_provider(prov);

    if (!pUiaDisconnectProvider)
        return;

    /* The same inside UiaDisconnectProvider, counting its own calls of Release first. */
    prov = new_provider(112);
    ok_hr(pUiaDisconnectProvider(&prov->simple), S_OK);
    prov->hook_after = prov->releases;
    node = get_node(prov);
    prov->releases = 0;
    hook_reset(node);
    prov->hook = hook_query_node;
    prov->hook_armed = 1;
    ok_hr(pUiaDisconnectProvider(&prov->simple), S_OK);
    ok_ref(prov, 1);
    check_hook();
    check_node_gone(node, prov);
    release_node(node);
    free_provider(prov);
}

/* The bulk call again, from the Release of a provider inside the bulk call. */
static void test_reentrant_bulk(void)
{
    struct provider *prov1, *prov2;
    HUIANODE node1, node2;

    prov1 = new_provider(61);
    prov2 = new_provider(62);
    node1 = get_node(prov1);
    node2 = get_node(prov2);
    prov1->hook = hook_call_all;
    prov1->hook_armed = 1;
    prov2->hook = hook_call_all;
    prov2->hook_armed = 1;
    call_all();
    ok_ref(prov1, 1);
    ok_ref(prov2, 1);
    check_node_gone(node1, prov1);
    check_node_gone(node2, prov2);
    release_node(node1);
    release_node(node2);
    free_provider(prov1);
    free_provider(prov2);
}

/* UiaDisconnectProvider of another connected provider, from a Release inside the bulk call. */
static void test_reentrant_single_in_bulk(void)
{
    struct provider *prov1, *prov2;
    HUIANODE node1, node2;

    if (!pUiaDisconnectProvider)
        return;

    prov1 = new_provider(71);
    prov2 = new_provider(72);
    node1 = get_node(prov1);
    node2 = get_node(prov2);
    hook_other = prov2;
    hook_reset(node2);
    prov1->hook = hook_disconnect_other;
    prov1->hook_armed = 1;
    call_all();
    ok_ref(prov1, 1);
    ok_ref(prov2, 1);
    check_hook();
    check_node_gone(node1, prov1);
    check_node_gone(node2, prov2);
    release_node(node1);
    release_node(node2);
    free_provider(prov1);
    free_provider(prov2);
}

/*
 * The bulk call from a Release inside UiaDisconnectProvider, with three nodes that have one runtime ID: the single
 * disconnection is in the middle of its group. The calls of Release that UiaDisconnectProvider makes for its own use
 * are counted first, on a provider that has no node.
 */
static void test_reentrant_bulk_in_single(void)
{
    struct provider *prov1, *prov2, *prov3;
    HUIANODE node1, node2, node3;

    if (!pUiaDisconnectProvider || hangs_on_windows("UiaDisconnectAllProviders in UiaDisconnectProvider"))
        return;

    prov1 = new_provider(81);
    prov2 = new_provider(81);
    prov3 = new_provider(81);
    ok_hr(pUiaDisconnectProvider(&prov1->simple), S_OK);
    prov1->hook_after = prov1->releases;
    node1 = get_node(prov1);
    node2 = get_node(prov2);
    node3 = get_node(prov3);
    prov1->releases = 0;
    prov1->hook = hook_call_all;
    prov1->hook_armed = 1;
    prov2->hook = hook_call_all;
    prov2->hook_armed = 1;
    ok_hr(pUiaDisconnectProvider(&prov1->simple), S_OK);
    ok_ref(prov1, 1);
    ok_ref(prov2, 1);
    ok_ref(prov3, 1);
    check_node_gone(node1, prov1);
    check_node_gone(node2, prov2);
    check_node_gone(node3, prov3);
    release_node(node1);
    release_node(node2);
    release_node(node3);
    free_provider(prov1);
    free_provider(prov2);
    free_provider(prov3);
}

/* Both calls from a Release inside the bulk call, with a runtime ID that two providers share. */
static void test_reentrant_both_in_bulk(void)
{
    struct provider *prov1, *prov2;
    HUIANODE node1, node2;

    if (!pUiaDisconnectProvider)
        return;

    prov1 = new_provider(91);
    prov2 = new_provider(91);
    node1 = get_node(prov1);
    node2 = get_node(prov2);
    hook_other = prov2;
    prov1->hook = hook_call_all_and_other;
    prov1->hook_armed = 1;
    call_all();
    ok_ref(prov1, 1);
    ok_ref(prov2, 1);
    check_node_gone(node1, prov1);
    check_node_gone(node2, prov2);
    release_node(node1);
    release_node(node2);
    free_provider(prov1);
    free_provider(prov2);
}

static volatile LONG stop_thread;
static volatile LONG thread_calls;
static volatile LONG thread_failures;

static DWORD WINAPI disconnect_thread(void *arg)
{
    while (!stop_thread)
    {
        if (pUiaDisconnectAllProviders() != S_OK)
            InterlockedIncrement(&thread_failures);
        InterlockedIncrement(&thread_calls);
    }
    return 0;
}

/* Nodes are created and released while another thread disconnects all providers over and over. */
static void test_concurrent_release(void)
{
    struct provider *prov1, *prov2;
    HANDLE thread;
    int i;

    if (hangs_on_windows("UiaDisconnectAllProviders next to UiaNodeFromHandle"))
        return;

    prov1 = new_provider(101);
    prov2 = new_provider(101);
    stop_thread = 0;
    thread = CreateThread(NULL, 0, disconnect_thread, NULL, 0, NULL);
    ok(thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!thread)
        return;

    for (i = 0; i < STRESS_ROUNDS; i++)
    {
        HUIANODE node1 = NULL, node2 = NULL, node3 = NULL;

        pUiaNodeFromHandle(prov1->hwnd, &node1);
        pUiaNodeFromHandle(prov2->hwnd, &node2);
        pUiaNodeFromHandle(prov1->hwnd, &node3);
        if (i & 1)
        {
            if (node2) pUiaNodeRelease(node2);
            if (node1) pUiaNodeRelease(node1);
        }
        else
        {
            if (node1) pUiaNodeRelease(node1);
            if (node2) pUiaNodeRelease(node2);
        }
        if (node3) pUiaNodeRelease(node3);
    }

    stop_thread = 1;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    ok(thread_calls > 0, "the thread made no call\n");
    ok(thread_failures == 0, "%ld calls failed\n", thread_failures);

    call_all();
    ok_ref(prov1, 1);
    ok_ref(prov2, 1);
    free_provider(prov1);
    free_provider(prov2);
}

START_TEST(UiaDisconnectAllProviders)
{
    /* ReactOS has the function from export level 0x601, which a kernel32 with the Windows 7 functions shows. */
    BOOL Required = is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    HMODULE Module = LoadLibraryW(L"uiautomationcore.dll");
    HRESULT hr;

    if (!Module)
    {
        ok(!Required, "uiautomationcore.dll did not load: %lu\n", GetLastError());
        skip("uiautomationcore.dll is not available\n");
        return;
    }

    pUiaDisconnectAllProviders = (PFN_UiaDisconnectAllProviders)GetProcAddress(Module, "UiaDisconnectAllProviders");
    if (!pUiaDisconnectAllProviders)
    {
        ok(!Required, "UiaDisconnectAllProviders is not exported\n");
        skip("UiaDisconnectAllProviders is not exported\n");
        return;
    }

    pUiaDisconnectProvider = (PFN_UiaDisconnectProvider)GetProcAddress(Module, "UiaDisconnectProvider");
    pUiaNodeFromHandle = (PFN_UiaNodeFromHandle)GetProcAddress(Module, "UiaNodeFromHandle");
    pUiaNodeRelease = (PFN_UiaNodeRelease)GetProcAddress(Module, "UiaNodeRelease");
    pUiaGetPropertyValue = (PFN_UiaGetPropertyValue)GetProcAddress(Module, "UiaGetPropertyValue");
    pUiaReturnRawElementProvider =
        (PFN_UiaReturnRawElementProvider)GetProcAddress(Module, "UiaReturnRawElementProvider");
    if (!pUiaNodeFromHandle || !pUiaNodeRelease || !pUiaGetPropertyValue || !pUiaReturnRawElementProvider)
    {
        ok(!Required, "the UI Automation client functions are not exported\n");
        skip("the UI Automation client functions are not exported\n");
        return;
    }

    test_empty();

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(SUCCEEDED(hr), "CoInitializeEx failed: %#lx\n", hr);

    if (!start_window_thread())
    {
        CoUninitialize();
        return;
    }

    test_one_node();
    test_two_providers();
    test_nodes_without_runtime_id();
    test_same_runtime_id();
    test_released_node();
    test_reconnect();
    test_query_in_release();
    test_reentrant_bulk();
    test_reentrant_single_in_bulk();
    test_reentrant_bulk_in_single();
    test_reentrant_both_in_bulk();
    test_concurrent_release();

    test_empty();

    stop_window_thread();
    CoUninitialize();
}
