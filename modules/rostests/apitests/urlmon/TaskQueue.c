/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a protocol which reports its last data and its result from inside a client Read
 *              reaches the client, and that nothing is delivered while that Read runs
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#define COBJMACROS
#define CONST_VTABLE
#include <apitest.h>

#include <ole2.h>
#include <urlmon.h>

#define WM_READ_NOW (WM_APP + 1)

static const char g_data[] = "01234567";
#define DATA_SIZE 8

typedef enum {
    MODE_READ_LATER,    /* the client reads from a posted message, outside every callback */
    MODE_READ_IN_CALLBACK,  /* the client reads inside OnDataAvailable of the first notification */
    MODE_PUMP_IN_READ,  /* like MODE_READ_LATER, and the protocol runs a message loop inside Read */
    MODE_WORKER,        /* a worker thread of the protocol reports the end while the client has not read yet; the client
                           then reads outside every callback and the Read runs a message loop */
    MODE_WORKER_LAST,   /* like MODE_WORKER, and the client keeps nothing: the worker's reference may be the last one */
    MODE_FILTER         /* like MODE_READ_LATER, behind a MIME filter that runs a message loop after it has read */
} test_mode_t;

typedef struct {
    IBindStatusCallback IBindStatusCallback_iface;
    LONG ref;
    IStream *stream;
} Callback;

typedef struct {
    IInternetProtocol IInternetProtocol_iface;
    LONG ref;
    IInternetProtocolSink *sink;
    DWORD sent;
    BOOL ended;
} Protocol;

static test_mode_t g_mode;
static Callback g_callback;
static Protocol *g_protocol;

static BOOL g_in_read;     /* the client is inside IStream_Read */
static int g_first, g_last, g_stop, g_calls_in_read, g_read_bytes, g_terminated;
static volatile LONG g_alive;   /* test protocols and filters that exist */
static HRESULT g_stop_hr;
static char g_read_buf[16];
static HANDLE g_go, g_posted, g_worker;
static DWORD g_destroy_thread;
static int g_reference_growth;

static void client_read(void)
{
    ULONG read = 0;
    HRESULT hr;

    ok(g_callback.stream != NULL, "no stream to read\n");
    if (!g_callback.stream)
        return;
    g_in_read = TRUE;
    hr = IStream_Read(g_callback.stream, g_read_buf, sizeof(g_read_buf), &read);
    g_in_read = FALSE;
    ok(hr == S_OK || hr == S_FALSE, "Read returned %08lx\n", hr);
    g_read_bytes += read;
}

static void pump_messages(DWORD ms)
{
    DWORD start = GetTickCount();
    MSG msg;

    while ((DWORD)(GetTickCount() - start) < ms)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (!msg.hwnd && msg.message == WM_READ_NOW)
            {
                client_read();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_stop)
            return;
        MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
    }
}

static Protocol *impl_from_IInternetProtocol(IInternetProtocol *iface)
{
    return CONTAINING_RECORD(iface, Protocol, IInternetProtocol_iface);
}

static HRESULT WINAPI Protocol_QueryInterface(IInternetProtocol *iface, REFIID riid, void **ppv)
{
    Protocol *This = impl_from_IInternetProtocol(iface);

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IInternetProtocolRoot) ||
        IsEqualGUID(riid, &IID_IInternetProtocol))
    {
        *ppv = &This->IInternetProtocol_iface;
        InterlockedIncrement(&This->ref);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Protocol_AddRef(IInternetProtocol *iface)
{
    return InterlockedIncrement(&impl_from_IInternetProtocol(iface)->ref);
}

static ULONG WINAPI Protocol_Release(IInternetProtocol *iface)
{
    Protocol *This = impl_from_IInternetProtocol(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    if (!ref)
    {
        if (This->sink)
            IInternetProtocolSink_Release(This->sink);
        if (g_protocol == This)
            g_protocol = NULL;
        g_destroy_thread = GetCurrentThreadId();
        HeapFree(GetProcessHeap(), 0, This);
        InterlockedDecrement(&g_alive);
    }
    return ref;
}

static ULONG sink_references(IInternetProtocolSink *sink)
{
    ULONG ref = IInternetProtocolSink_AddRef(sink);

    IInternetProtocolSink_Release(sink);
    return ref - 1;
}

static DWORD WINAPI worker_thread(void *arg)
{
    Protocol *This = arg;

    WaitForSingleObject(g_go, 10000);
    if (g_mode == MODE_WORKER_LAST)
    {
        /* the references of the protocol that urlmon holds across this call, seen from outside */
        ULONG before = sink_references(This->sink), after;

        IInternetProtocolSink_ReportData(This->sink, BSCF_LASTDATANOTIFICATION, DATA_SIZE, DATA_SIZE);
        after = sink_references(This->sink);
        g_reference_growth = g_last ? -1 : (int)(after - before);
    }
    else
    {
        IInternetProtocolSink_ReportData(This->sink, BSCF_LASTDATANOTIFICATION, DATA_SIZE, DATA_SIZE);
    }
    IInternetProtocolSink_ReportResult(This->sink, S_OK, 0, NULL);
    SetEvent(g_posted);
    return 0;
}

static HRESULT WINAPI Protocol_Start(IInternetProtocol *iface, LPCWSTR url, IInternetProtocolSink *sink,
                                     IInternetBindInfo *bindinfo, DWORD flags, HANDLE_PTR reserved)
{
    Protocol *This = impl_from_IInternetProtocol(iface);

    This->sink = sink;
    IInternetProtocolSink_AddRef(sink);

    IInternetProtocolSink_ReportProgress(sink, BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE, L"application/octet-stream");
    IInternetProtocolSink_ReportData(sink, BSCF_FIRSTDATANOTIFICATION | BSCF_AVAILABLEDATASIZEUNKNOWN, 0, 0);
    if (g_mode == MODE_WORKER || g_mode == MODE_WORKER_LAST)
    {
        g_worker = CreateThread(NULL, 0, worker_thread, This, 0, NULL);
        if (g_mode == MODE_WORKER_LAST)
            SetThreadPriority(g_worker, THREAD_PRIORITY_LOWEST);
    }
    return S_OK;
}

static HRESULT WINAPI Protocol_Continue(IInternetProtocol *iface, PROTOCOLDATA *data)
{
    return S_OK;
}

static HRESULT WINAPI Protocol_Abort(IInternetProtocol *iface, HRESULT reason, DWORD options)
{
    return S_OK;
}

static HRESULT WINAPI Protocol_Terminate(IInternetProtocol *iface, DWORD options)
{
    Protocol *This = impl_from_IInternetProtocol(iface);

    g_terminated++;

    if (This->sink)
    {
        IInternetProtocolSink_Release(This->sink);
        This->sink = NULL;
    }
    return S_OK;
}

static HRESULT WINAPI Protocol_Suspend(IInternetProtocol *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Protocol_Resume(IInternetProtocol *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Protocol_Read(IInternetProtocol *iface, void *pv, ULONG cb, ULONG *read)
{
    Protocol *This = impl_from_IInternetProtocol(iface);
    DWORD n = min(cb, DATA_SIZE - This->sent);

    memcpy(pv, g_data + This->sent, n);
    This->sent += n;
    *read = n;

    if (g_mode == MODE_WORKER)
    {
        pump_messages(300);
    }
    else if (This->sent == DATA_SIZE && !This->ended)
    {
        This->ended = TRUE;
        /* the end of the data and the result are reported from inside the client's Read */
        IInternetProtocolSink_ReportData(This->sink, BSCF_LASTDATANOTIFICATION, DATA_SIZE, DATA_SIZE);
        IInternetProtocolSink_ReportResult(This->sink, S_OK, 0, NULL);
        if (g_mode == MODE_PUMP_IN_READ)
            pump_messages(300);
    }

    return This->sent == DATA_SIZE ? S_FALSE : S_OK;
}

static HRESULT WINAPI Protocol_Seek(IInternetProtocol *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Protocol_LockRequest(IInternetProtocol *iface, DWORD options)
{
    return S_OK;
}

static HRESULT WINAPI Protocol_UnlockRequest(IInternetProtocol *iface)
{
    return S_OK;
}

static const IInternetProtocolVtbl ProtocolVtbl =
{
    Protocol_QueryInterface,
    Protocol_AddRef,
    Protocol_Release,
    Protocol_Start,
    Protocol_Continue,
    Protocol_Abort,
    Protocol_Terminate,
    Protocol_Suspend,
    Protocol_Resume,
    Protocol_Read,
    Protocol_Seek,
    Protocol_LockRequest,
    Protocol_UnlockRequest
};

static HRESULT WINAPI Factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI Factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI Factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    Protocol *protocol;
    HRESULT hr;

    if (outer)
        return CLASS_E_NOAGGREGATION;

    protocol = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*protocol));
    if (!protocol)
        return E_OUTOFMEMORY;
    protocol->IInternetProtocol_iface.lpVtbl = &ProtocolVtbl;
    protocol->ref = 1;
    g_protocol = protocol;
    InterlockedIncrement(&g_alive);

    hr = IInternetProtocol_QueryInterface(&protocol->IInternetProtocol_iface, riid, ppv);
    IInternetProtocol_Release(&protocol->IInternetProtocol_iface);
    return hr;
}

static HRESULT WINAPI Factory_LockServer(IClassFactory *iface, BOOL lock)
{
    return S_OK;
}

static const IClassFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    Factory_CreateInstance,
    Factory_LockServer
};

static IClassFactory g_factory = { &FactoryVtbl };

/* a MIME filter that forwards its Read to the protocol behind it and then runs messages */
typedef struct {
    IInternetProtocol IInternetProtocol_iface;
    IInternetProtocolSink IInternetProtocolSink_iface;
    LONG ref;
    IInternetProtocolSink *sink;
    IInternetProtocol *protocol;
} Filter;

static Filter *filter_from_protocol(IInternetProtocol *iface)
{
    return CONTAINING_RECORD(iface, Filter, IInternetProtocol_iface);
}

static Filter *filter_from_sink(IInternetProtocolSink *iface)
{
    return CONTAINING_RECORD(iface, Filter, IInternetProtocolSink_iface);
}

static HRESULT WINAPI Filter_QueryInterface(IInternetProtocol *iface, REFIID riid, void **ppv)
{
    Filter *This = filter_from_protocol(iface);

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IInternetProtocolRoot) ||
        IsEqualGUID(riid, &IID_IInternetProtocol))
        *ppv = &This->IInternetProtocol_iface;
    else if (IsEqualGUID(riid, &IID_IInternetProtocolSink))
        *ppv = &This->IInternetProtocolSink_iface;
    else
    {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&This->ref);
    return S_OK;
}

static ULONG WINAPI Filter_AddRef(IInternetProtocol *iface)
{
    return InterlockedIncrement(&filter_from_protocol(iface)->ref);
}

static ULONG WINAPI Filter_Release(IInternetProtocol *iface)
{
    Filter *This = filter_from_protocol(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    if (!ref)
    {
        if (This->sink)
            IInternetProtocolSink_Release(This->sink);
        if (This->protocol)
            IInternetProtocol_Release(This->protocol);
        HeapFree(GetProcessHeap(), 0, This);
        InterlockedDecrement(&g_alive);
    }
    return ref;
}

static HRESULT WINAPI Filter_Start(IInternetProtocol *iface, LPCWSTR url, IInternetProtocolSink *sink,
                                   IInternetBindInfo *bindinfo, DWORD flags, HANDLE_PTR reserved)
{
    Filter *This = filter_from_protocol(iface);
    PROTOCOLFILTERDATA *data = (PROTOCOLFILTERDATA *)reserved;

    This->sink = sink;
    IInternetProtocolSink_AddRef(sink);
    This->protocol = data->pProtocol;
    IInternetProtocol_AddRef(This->protocol);
    return S_OK;
}

static HRESULT WINAPI Filter_Continue(IInternetProtocol *iface, PROTOCOLDATA *data)
{
    Filter *This = filter_from_protocol(iface);

    return This->protocol ? IInternetProtocol_Continue(This->protocol, data) : S_OK;
}

static HRESULT WINAPI Filter_Abort(IInternetProtocol *iface, HRESULT reason, DWORD options)
{
    Filter *This = filter_from_protocol(iface);

    return This->protocol ? IInternetProtocol_Abort(This->protocol, reason, options) : S_OK;
}

static HRESULT WINAPI Filter_Terminate(IInternetProtocol *iface, DWORD options)
{
    Filter *This = filter_from_protocol(iface);
    IInternetProtocol *protocol = This->protocol;
    IInternetProtocolSink *sink = This->sink;
    HRESULT hr;

    /* urlmon and the filter hold each other: the filter lets go of urlmon here */
    IInternetProtocol_AddRef(iface);
    hr = protocol ? IInternetProtocol_Terminate(protocol, options) : S_OK;
    This->protocol = NULL;
    This->sink = NULL;
    if (protocol)
        IInternetProtocol_Release(protocol);
    if (sink)
        IInternetProtocolSink_Release(sink);
    IInternetProtocol_Release(iface);
    return hr;
}

static HRESULT WINAPI Filter_Suspend(IInternetProtocol *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Filter_Resume(IInternetProtocol *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Filter_Read(IInternetProtocol *iface, void *pv, ULONG cb, ULONG *read)
{
    Filter *This = filter_from_protocol(iface);
    HRESULT hr;

    if (!This->protocol)
        return E_FAIL;
    hr = IInternetProtocol_Read(This->protocol, pv, cb, read);
    pump_messages(300);
    return hr;
}

static HRESULT WINAPI Filter_Seek(IInternetProtocol *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Filter_LockRequest(IInternetProtocol *iface, DWORD options)
{
    Filter *This = filter_from_protocol(iface);

    return This->protocol ? IInternetProtocol_LockRequest(This->protocol, options) : S_OK;
}

/* Windows calls this after Terminate */
static HRESULT WINAPI Filter_UnlockRequest(IInternetProtocol *iface)
{
    Filter *This = filter_from_protocol(iface);

    return This->protocol ? IInternetProtocol_UnlockRequest(This->protocol) : S_OK;
}

static const IInternetProtocolVtbl FilterVtbl =
{
    Filter_QueryInterface,
    Filter_AddRef,
    Filter_Release,
    Filter_Start,
    Filter_Continue,
    Filter_Abort,
    Filter_Terminate,
    Filter_Suspend,
    Filter_Resume,
    Filter_Read,
    Filter_Seek,
    Filter_LockRequest,
    Filter_UnlockRequest
};

static HRESULT WINAPI FilterSink_QueryInterface(IInternetProtocolSink *iface, REFIID riid, void **ppv)
{
    return Filter_QueryInterface(&filter_from_sink(iface)->IInternetProtocol_iface, riid, ppv);
}

static ULONG WINAPI FilterSink_AddRef(IInternetProtocolSink *iface)
{
    return Filter_AddRef(&filter_from_sink(iface)->IInternetProtocol_iface);
}

static ULONG WINAPI FilterSink_Release(IInternetProtocolSink *iface)
{
    return Filter_Release(&filter_from_sink(iface)->IInternetProtocol_iface);
}

static HRESULT WINAPI FilterSink_Switch(IInternetProtocolSink *iface, PROTOCOLDATA *data)
{
    Filter *This = filter_from_sink(iface);

    return This->sink ? IInternetProtocolSink_Switch(This->sink, data) : S_OK;
}

static HRESULT WINAPI FilterSink_ReportProgress(IInternetProtocolSink *iface, ULONG status, LPCWSTR text)
{
    Filter *This = filter_from_sink(iface);

    return This->sink ? IInternetProtocolSink_ReportProgress(This->sink, status, text) : S_OK;
}

static HRESULT WINAPI FilterSink_ReportData(IInternetProtocolSink *iface, DWORD bscf, ULONG progress, ULONG max)
{
    Filter *This = filter_from_sink(iface);

    return This->sink ? IInternetProtocolSink_ReportData(This->sink, bscf, progress, max) : S_OK;
}

static HRESULT WINAPI FilterSink_ReportResult(IInternetProtocolSink *iface, HRESULT result, DWORD error, LPCWSTR text)
{
    Filter *This = filter_from_sink(iface);

    return This->sink ? IInternetProtocolSink_ReportResult(This->sink, result, error, text) : S_OK;
}

static const IInternetProtocolSinkVtbl FilterSinkVtbl =
{
    FilterSink_QueryInterface,
    FilterSink_AddRef,
    FilterSink_Release,
    FilterSink_Switch,
    FilterSink_ReportProgress,
    FilterSink_ReportData,
    FilterSink_ReportResult
};

static HRESULT WINAPI FilterFactory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    Filter *filter;
    HRESULT hr;

    if (outer)
        return CLASS_E_NOAGGREGATION;
    filter = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*filter));
    if (!filter)
        return E_OUTOFMEMORY;
    filter->IInternetProtocol_iface.lpVtbl = &FilterVtbl;
    filter->IInternetProtocolSink_iface.lpVtbl = &FilterSinkVtbl;
    filter->ref = 1;
    InterlockedIncrement(&g_alive);

    hr = IInternetProtocol_QueryInterface(&filter->IInternetProtocol_iface, riid, ppv);
    IInternetProtocol_Release(&filter->IInternetProtocol_iface);
    return hr;
}

static const IClassFactoryVtbl FilterFactoryVtbl =
{
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    FilterFactory_CreateInstance,
    Factory_LockServer
};

static IClassFactory g_filter_factory = { &FilterFactoryVtbl };

static Callback *impl_from_IBindStatusCallback(IBindStatusCallback *iface)
{
    return CONTAINING_RECORD(iface, Callback, IBindStatusCallback_iface);
}

static HRESULT WINAPI Callback_QueryInterface(IBindStatusCallback *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IBindStatusCallback))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Callback_AddRef(IBindStatusCallback *iface)
{
    return InterlockedIncrement(&impl_from_IBindStatusCallback(iface)->ref);
}

static ULONG WINAPI Callback_Release(IBindStatusCallback *iface)
{
    return InterlockedDecrement(&impl_from_IBindStatusCallback(iface)->ref);
}

static HRESULT WINAPI Callback_OnStartBinding(IBindStatusCallback *iface, DWORD reserved, IBinding *binding)
{
    return S_OK;
}

static HRESULT WINAPI Callback_GetPriority(IBindStatusCallback *iface, LONG *priority)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Callback_OnLowResource(IBindStatusCallback *iface, DWORD reserved)
{
    return S_OK;
}

static HRESULT WINAPI Callback_OnProgress(IBindStatusCallback *iface, ULONG progress, ULONG progress_max,
                                          ULONG status, LPCWSTR text)
{
    return S_OK;
}

static HRESULT WINAPI Callback_OnStopBinding(IBindStatusCallback *iface, HRESULT hr, LPCWSTR error)
{
    g_stop++;
    g_stop_hr = hr;
    if (g_in_read)
        g_calls_in_read++;
    return S_OK;
}

static HRESULT WINAPI Callback_GetBindInfo(IBindStatusCallback *iface, DWORD *bindf, BINDINFO *bindinfo)
{
    DWORD size = bindinfo->cbSize;

    memset(bindinfo, 0, size);
    bindinfo->cbSize = size;
    *bindf = BINDF_ASYNCHRONOUS | BINDF_ASYNCSTORAGE | BINDF_PULLDATA | BINDF_NOWRITECACHE;
    return S_OK;
}

static HRESULT WINAPI Callback_OnDataAvailable(IBindStatusCallback *iface, DWORD bscf, DWORD size,
                                               FORMATETC *format, STGMEDIUM *stgmed)
{
    Callback *This = impl_from_IBindStatusCallback(iface);

    if (g_in_read)
        g_calls_in_read++;

    if (bscf & BSCF_FIRSTDATANOTIFICATION)
    {
        g_first++;
        if (stgmed && stgmed->tymed == TYMED_ISTREAM && stgmed->pstm && !This->stream && g_mode != MODE_WORKER_LAST)
        {
            This->stream = stgmed->pstm;
            IStream_AddRef(This->stream);
        }

        if (g_mode == MODE_READ_IN_CALLBACK)
            client_read();
        else if (g_mode != MODE_WORKER && g_mode != MODE_WORKER_LAST)
            PostThreadMessageW(GetCurrentThreadId(), WM_READ_NOW, 0, 0);
    }
    if (bscf & BSCF_LASTDATANOTIFICATION)
        g_last++;
    return S_OK;
}

static HRESULT WINAPI Callback_OnObjectAvailable(IBindStatusCallback *iface, REFIID riid, IUnknown *unk)
{
    return S_OK;
}

static const IBindStatusCallbackVtbl CallbackVtbl =
{
    Callback_QueryInterface,
    Callback_AddRef,
    Callback_Release,
    Callback_OnStartBinding,
    Callback_GetPriority,
    Callback_OnLowResource,
    Callback_OnProgress,
    Callback_OnStopBinding,
    Callback_GetBindInfo,
    Callback_OnDataAvailable,
    Callback_OnObjectAvailable
};

static void run_bind(test_mode_t mode, const char *name)
{
    IMoniker *mon;
    IBindCtx *bindctx;
    IUnknown *unk = NULL;
    HRESULT hr;

    g_mode = mode;
    g_in_read = FALSE;
    g_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_posted = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_worker = NULL;
    g_first = g_last = g_stop = g_calls_in_read = g_read_bytes = g_terminated = 0;
    g_stop_hr = E_FAIL;
    memset(g_read_buf, 0, sizeof(g_read_buf));
    memset(&g_callback, 0, sizeof(g_callback));
    g_callback.IBindStatusCallback_iface.lpVtbl = &CallbackVtbl;
    g_callback.ref = 1;

    hr = CreateURLMoniker(NULL, L"rostest:task-queue", &mon);
    ok(hr == S_OK, "%s: CreateURLMoniker returned %08lx\n", name, hr);
    if (FAILED(hr))
        return;
    hr = CreateAsyncBindCtx(0, &g_callback.IBindStatusCallback_iface, NULL, &bindctx);
    ok(hr == S_OK, "%s: CreateAsyncBindCtx returned %08lx\n", name, hr);

    hr = IMoniker_BindToStorage(mon, bindctx, NULL, &IID_IStream, (void **)&unk);
    ok(hr == S_OK || hr == MK_S_ASYNCHRONOUS, "%s: BindToStorage returned %08lx\n", name, hr);
    if (unk)
        IUnknown_Release(unk);

    if (mode == MODE_WORKER)
    {
        /* the worker queues the end of the data and the result, which posts the message that runs them;
           only then does the client read, while that message waits */
        SetEvent(g_go);
        ok(WaitForSingleObject(g_posted, 5000) == WAIT_OBJECT_0, "%s: the worker did not report\n", name);
        client_read();
    }

    /* five seconds are plenty: the notifications are posted messages */
    pump_messages(5000);

    ok(g_first == 1, "%s: %d first-data notifications\n", name, g_first);
    ok(g_read_bytes == DATA_SIZE, "%s: read %d bytes\n", name, g_read_bytes);
    ok(!memcmp(g_read_buf, g_data, DATA_SIZE), "%s: wrong data\n", name);
    ok(g_last == 1, "%s: %d last-data notifications\n", name, g_last);
    ok(g_stop == 1, "%s: %d OnStopBinding calls\n", name, g_stop);
    ok(g_stop_hr == S_OK, "%s: OnStopBinding result %08lx\n", name, g_stop_hr);
    ok(g_calls_in_read == 0, "%s: %d notifications arrived while the client was inside Read\n", name, g_calls_in_read);

    /* the result of the protocol is a notification too: urlmon ends the protocol when it has run it */
    {
        DWORD start = GetTickCount();

        while (!g_terminated && (DWORD)(GetTickCount() - start) < 3000)
        {
            pump_messages(20);
            Sleep(10);
        }
    }
    ok(g_terminated == 1, "%s: the protocol was terminated %d times\n", name, g_terminated);

    if (g_callback.stream)
        IStream_Release(g_callback.stream);
    g_callback.stream = NULL;
    IBindCtx_Release(bindctx);
    IMoniker_Release(mon);

    if (g_worker)
    {
        WaitForSingleObject(g_worker, 5000);
        CloseHandle(g_worker);
    }
    CloseHandle(g_go);
    CloseHandle(g_posted);

    /* the binding lets go of the protocol and of the filter once it is done */
    {
        DWORD start = GetTickCount();

        pump_messages(200);
        while (g_alive && (DWORD)(GetTickCount() - start) < 3000)
        {
            pump_messages(20);
            Sleep(10);
        }
    }
    /* Windows keeps one object of a filter for the life of the process */
    if (is_reactos())
        ok(!g_alive, "%s: %ld test objects are still alive\n", name, g_alive);
}

/*
 * A worker thread of the protocol reports the end of the data and the result. The client holds nothing, so the reference
 * that urlmon takes for the worker's call can be the last one once the apartment thread has run the tasks; the protocol
 * has to be ended on the apartment thread anyway, which owns the notification window. The scheduler of the test machine
 * does not let that race happen on demand, so the test also looks at the references: after the call two are held for the
 * apartment thread, the message that runs the tasks and the one that returns the call's own.
 */
static void test_last_reference(void)
{
    DWORD_PTR old_mask, system_mask;
    int old_priority, wrong = 0, i;
    BOOL set;

    GetProcessAffinityMask(GetCurrentProcess(), &old_mask, &system_mask);
    set = SetProcessAffinityMask(GetCurrentProcess(), 1);
    old_priority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    (void)set;
    (void)system_mask;

    /* the apartment thread runs everything as soon as the worker lets it; the check of the references does not depend
       on it, the thread that ends the protocol and the notification window do */
    for (i = 0; i < 20; i++)
    {
        IMoniker *mon;
        IBindCtx *bindctx;
        IUnknown *unk = NULL;
        DWORD start;
        HRESULT hr;

        g_mode = MODE_WORKER_LAST;
        g_in_read = FALSE;
        g_go = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_posted = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_worker = NULL;
        g_destroy_thread = 0;
        g_first = g_last = g_stop = g_calls_in_read = g_read_bytes = g_terminated = 0;
        g_stop_hr = E_FAIL;
        memset(&g_callback, 0, sizeof(g_callback));
        g_callback.IBindStatusCallback_iface.lpVtbl = &CallbackVtbl;
        g_callback.ref = 1;

        hr = CreateURLMoniker(NULL, L"rostest:last-reference", &mon);
        ok(hr == S_OK, "CreateURLMoniker returned %08lx\n", hr);
        hr = CreateAsyncBindCtx(0, &g_callback.IBindStatusCallback_iface, NULL, &bindctx);
        ok(hr == S_OK, "CreateAsyncBindCtx returned %08lx\n", hr);
        hr = IMoniker_BindToStorage(mon, bindctx, NULL, &IID_IStream, (void **)&unk);
        ok(hr == S_OK || hr == MK_S_ASYNCHRONOUS, "BindToStorage returned %08lx\n", hr);
        if (unk)
            IUnknown_Release(unk);
        IBindCtx_Release(bindctx);
        IMoniker_Release(mon);

        SetEvent(g_go);
        start = GetTickCount();
        while ((g_alive || !g_terminated) && (DWORD)(GetTickCount() - start) < 5000)
        {
            pump_messages(20);
            Sleep(1);
        }
        ok(g_terminated == 1, "the protocol was terminated %d times\n", g_terminated);
        ok(!g_alive, "%ld test objects are still alive\n", g_alive);
        if (g_destroy_thread != GetCurrentThreadId())
            wrong++;
        /* urlmon's own references across the worker's call: the message that runs the tasks and the one that gives the
           reference of the call back to the apartment thread; -1: the apartment thread was quicker (not measurable) */
        if (g_reference_growth != -1)
            ok(g_reference_growth == 2, "the worker's call left %d references, expected 2\n", g_reference_growth);

        if (g_worker)
        {
            WaitForSingleObject(g_worker, 5000);
            CloseHandle(g_worker);
        }
        CloseHandle(g_go);
        CloseHandle(g_posted);
    }

    ok(wrong == 0, "%d protocols were ended on another thread than the apartment thread\n", wrong);
    pump_messages(100);
    ok(!FindWindowExW(HWND_MESSAGE, NULL, L"URL Moniker Notification Window", NULL),
       "the notification window of urlmon is still there\n");

    SetThreadPriority(GetCurrentThread(), old_priority);
    SetProcessAffinityMask(GetCurrentProcess(), old_mask);
}

START_TEST(TaskQueue)
{
    IInternetSession *session;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    hr = CoInternetGetSession(0, &session, 0);
    ok(hr == S_OK, "CoInternetGetSession returned %08lx\n", hr);
    if (FAILED(hr))
        goto done;

    hr = IInternetSession_RegisterNameSpace(session, &g_factory, &CLSID_NULL, L"rostest", 0, NULL, 0);
    ok(hr == S_OK, "RegisterNameSpace returned %08lx\n", hr);
    if (FAILED(hr))
    {
        IInternetSession_Release(session);
        goto done;
    }

    /* it looks into the references of ReactOS' urlmon; Windows ends the protocol earlier and the worker would use it */
    if (is_reactos())
        test_last_reference();

    /* the data ends and the result comes while the client is inside Read, outside any notification */
    run_bind(MODE_READ_LATER, "read later");
    /* the same, and the Read runs a message loop: nothing may be delivered under it */
    run_bind(MODE_PUMP_IN_READ, "pump in read");
    /* the notifications are queued and posted before the client reads; the Read runs a message loop that dispatches the
       message: it must not run them under the Read */
    run_bind(MODE_WORKER, "worker");
    /* the client reads inside OnDataAvailable: the notifications come after that call, as always */
    run_bind(MODE_READ_IN_CALLBACK, "read in callback");

    /* a MIME filter that runs messages after its Read: the client is inside Read all the same */
    hr = IInternetSession_RegisterMimeFilter(session, &g_filter_factory, &CLSID_NULL, L"application/octet-stream");
    ok(hr == S_OK, "RegisterMimeFilter returned %08lx\n", hr);
    if (hr == S_OK)
    {
        run_bind(MODE_FILTER, "filter");
        IInternetSession_UnregisterMimeFilter(session, &g_filter_factory, L"application/octet-stream");
    }

    IInternetSession_UnregisterNameSpace(session, &g_factory, L"rostest");
    IInternetSession_Release(session);

done:
    CoUninitialize();
}
