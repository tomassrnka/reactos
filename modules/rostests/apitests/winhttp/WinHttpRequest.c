/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A synchronous WinHttpRequest.Send against a local server returns
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windows.h>
#include <winsock2.h>
#include <ole2.h>
#include <oleauto.h>

static const GUID CLSID_WinHttpRequestTest = {0x2087c2f4, 0x2cef, 0x4953, {0xa8, 0xab, 0x66, 0x77, 0x9b, 0x67, 0x04, 0x95}};

static SOCKET g_Listen;
static WCHAR g_Url[64];
static LONG g_Status;
static HRESULT g_SendResult;

static DWORD WINAPI ServerThread(void *param)
{
    static const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    char buf[1024];
    int got, total = 0;
    SOCKET client = accept(g_Listen, NULL, NULL);

    if (client == INVALID_SOCKET) return 1;
    while (total < (int)sizeof(buf) - 1 && (got = recv(client, buf + total, sizeof(buf) - 1 - total, 0)) > 0)
    {
        total += got;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    send(client, reply, sizeof(reply) - 1, 0);
    shutdown(client, SD_SEND);
    closesocket(client);
    return 0;
}

static HRESULT Invoke(IDispatch *disp, const WCHAR *name, WORD flags, VARIANT *args, UINT count, VARIANT *result)
{
    DISPPARAMS params = { args, NULL, count, 0 };
    DISPID id, named = DISPID_PROPERTYPUT;
    OLECHAR *names[1];
    HRESULT hr;

    names[0] = (OLECHAR *)name;
    hr = disp->lpVtbl->GetIDsOfNames(disp, &IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    if (flags & DISPATCH_PROPERTYPUT) { params.rgdispidNamedArgs = &named; params.cNamedArgs = 1; }
    return disp->lpVtbl->Invoke(disp, id, &IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, NULL, NULL);
}

static DWORD WINAPI ClientThread(void *param)
{
    IDispatch *req = NULL;
    VARIANT args[3], result;
    HRESULT hr;

    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_WinHttpRequestTest, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch, (void **)&req);
    if (FAILED(hr)) { g_SendResult = hr; CoUninitialize(); return 1; }

    /* arguments are in reverse order: Method, Url, Async */
    VariantInit(&args[2]); V_VT(&args[2]) = VT_BSTR; V_BSTR(&args[2]) = SysAllocString(L"GET");
    VariantInit(&args[1]); V_VT(&args[1]) = VT_BSTR; V_BSTR(&args[1]) = SysAllocString(g_Url);
    VariantInit(&args[0]); V_VT(&args[0]) = VT_BOOL; V_BOOL(&args[0]) = VARIANT_FALSE;
    {
        VARIANT open[3];
        open[0] = args[0]; open[1] = args[1]; open[2] = args[2];
        VariantInit(&result);
        hr = Invoke(req, L"Open", DISPATCH_METHOD, open, 3, &result);
    }
    SysFreeString(V_BSTR(&args[2]));
    SysFreeString(V_BSTR(&args[1]));
    if (FAILED(hr)) { g_SendResult = hr; req->lpVtbl->Release(req); CoUninitialize(); return 1; }

    VariantInit(&result);
    g_SendResult = Invoke(req, L"Send", DISPATCH_METHOD, NULL, 0, &result);

    VariantInit(&result);
    if (SUCCEEDED(Invoke(req, L"Status", DISPATCH_PROPERTYGET, NULL, 0, &result)) && V_VT(&result) == VT_I4)
        g_Status = V_I4(&result);

    req->lpVtbl->Release(req);
    CoUninitialize();
    return 0;
}

START_TEST(WinHttpRequest)
{
    WSADATA wsa;
    struct sockaddr_in addr;
    int len = sizeof(addr);
    HANDLE server, client;
    DWORD wait;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_Listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(g_Listen, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind failed %d\n", WSAGetLastError());
    ok(listen(g_Listen, 1) == 0, "listen failed %d\n", WSAGetLastError());
    getsockname(g_Listen, (struct sockaddr *)&addr, &len);
    wsprintfW(g_Url, L"http://127.0.0.1:%u/", (unsigned)ntohs(addr.sin_port));

    server = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    client = CreateThread(NULL, 0, ClientThread, NULL, 0, NULL);

    /* a synchronous Send that never returns is the failure this test is for */
    wait = WaitForSingleObject(client, 20000);
    ok(wait == WAIT_OBJECT_0, "the synchronous Send did not return\n");
    if (wait == WAIT_OBJECT_0)
    {
        ok(g_SendResult == S_OK, "Send returned %08lx\n", (unsigned long)g_SendResult);
        ok(g_Status == 200, "Status is %ld\n", g_Status);
    }
    else
    {
        skip("the Send never returned; leaving without cleanup\n");
        ExitProcess(1);
    }
    WaitForSingleObject(server, 5000);
    closesocket(g_Listen);
    CloseHandle(client);
    CloseHandle(server);
    WSACleanup();
}
