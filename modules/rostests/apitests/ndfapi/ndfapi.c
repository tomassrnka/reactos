/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for the argument checks and failure codes of ndfapi.dll
 *
 * The expected values were measured on Windows Server 2008 R2 and Windows 10, x64 and WOW64.
 */

#include <apitest.h>

#include <winsock2.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <winsvc.h>

typedef PVOID NDFHANDLE;

typedef HRESULT (WINAPI *PNdfCloseIncident)(NDFHANDLE);
typedef HRESULT (WINAPI *PNdfCancelIncident)(NDFHANDLE);
typedef HRESULT (WINAPI *PNdfCreateConnectivityIncident)(NDFHANDLE *);
typedef HRESULT (WINAPI *PNdfCreateWebIncident)(LPCWSTR, NDFHANDLE *);
typedef HRESULT (WINAPI *PNdfGetTraceFile)(NDFHANDLE, LPCWSTR *);
typedef HRESULT (WINAPI *PNdfCreateWebIncidentEx)(LPCWSTR, BOOL, LPWSTR, NDFHANDLE *);
typedef HRESULT (WINAPI *PNdfCreateSharingIncident)(LPCWSTR, NDFHANDLE *);
typedef HRESULT (WINAPI *PNdfCreateDNSIncident)(LPCWSTR, WORD, NDFHANDLE *);
typedef HRESULT (WINAPI *PNdfCreateWinSockIncident)(SOCKET, LPCWSTR, USHORT, LPCWSTR, PVOID, NDFHANDLE *);
typedef HRESULT (WINAPI *PNdfDiagnoseIncident)(NDFHANDLE, ULONG *, PVOID *, DWORD, DWORD);
typedef HRESULT (WINAPI *PNdfExecuteDiagnosis)(NDFHANDLE, HWND);

static HMODULE hNdfapi;
static PNdfCloseIncident pNdfCloseIncident;
static PNdfCancelIncident pNdfCancelIncident;
static PNdfGetTraceFile pNdfGetTraceFile;
static PNdfCreateConnectivityIncident pNdfCreateConnectivityIncident;
static PNdfCreateWebIncident pNdfCreateWebIncident;
static PNdfCreateWebIncidentEx pNdfCreateWebIncidentEx;
static PNdfCreateSharingIncident pNdfCreateSharingIncident;
static PNdfCreateDNSIncident pNdfCreateDNSIncident;
static PNdfCreateWinSockIncident pNdfCreateWinSockIncident;
static PNdfDiagnoseIncident pNdfDiagnoseIncident;
static PNdfExecuteDiagnosis pNdfExecuteDiagnosis;

static const char *ExportNames[] =
{
    "DllCanUnloadNow",
    "DllGetClassObject",
    "DllRegisterServer",
    "DllUnregisterServer",
    "NdfCancelIncident",
    "NdfCloseIncident",
    "NdfCreateConnectivityIncident",
    "NdfCreateDNSIncident",
    "NdfCreateGroupingIncident",
    "NdfCreateInboundIncident",
    "NdfCreateIncident",
    "NdfCreatePnrpIncident",
    "NdfCreateSharingIncident",
    "NdfCreateWebIncident",
    "NdfCreateWebIncidentEx",
    "NdfCreateWinSockIncident",
    "NdfDiagnoseIncident",
    "NdfExecuteDiagnosis",
    "NdfGetTraceFile",
    "NdfRepairIncident",
    "NdfRunDllDiagnoseIncident",
    "NdfRunDllDiagnoseNetConnectionIncident",
    "NdfRunDllDiagnoseWithAnswerFile",
    "NdfRunDllDuplicateIPDefendingSystem",
    "NdfRunDllDuplicateIPOffendingSystem",
    "NdfRunDllHelpTopic",
};

/* the error for a missing pointer or string, which has no name in the headers */
#define NDF_E_BAD_PARAM ((HRESULT)0x8008f905)

#define MARKER ((NDFHANDLE)(ULONG_PTR)0x5555)

static void test_exports(void)
{
    UINT i;

    for (i = 0; i < ARRAYSIZE(ExportNames); i++)
        ok(GetProcAddress(hNdfapi, ExportNames[i]) != NULL, "%s is not exported\n", ExportNames[i]);
}

static void test_create(void)
{
    NDFHANDLE Incident;
    HRESULT hr;

    /* an incident is made from a string or from nothing, and the handle is not written on failure */
    Incident = MARKER;
    hr = pNdfCreateWebIncident(L"http://www.example.com/", &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    ok(Incident != NULL && Incident != MARKER, "got %p\n", Incident);
    if (hr == S_OK)
    {
        hr = pNdfCloseIncident(Incident);
        ok(hr == S_OK, "got 0x%lx\n", hr);
    }
    Incident = MARKER;
    hr = pNdfCreateWebIncident(NULL, &Incident);
    ok(hr == NDF_E_BAD_PARAM, "got 0x%lx\n", hr);
    ok(Incident == MARKER, "got %p\n", Incident);
    hr = pNdfCreateWebIncident(L"http://www.example.com/", NULL);
    ok(hr == NDF_E_BAD_PARAM, "got 0x%lx\n", hr);

    /* the string is not looked at */
    Incident = MARKER;
    hr = pNdfCreateWebIncident(L"", &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (hr == S_OK)
        pNdfCloseIncident(Incident);
    Incident = MARKER;
    hr = pNdfCreateWebIncident(L"not a url", &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (hr == S_OK)
        pNdfCloseIncident(Incident);

    Incident = MARKER;
    hr = pNdfCreateConnectivityIncident(&Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    ok(Incident != NULL && Incident != MARKER, "got %p\n", Incident);
    if (hr == S_OK)
        pNdfCloseIncident(Incident);
    hr = pNdfCreateConnectivityIncident(NULL);
    ok(hr == NDF_E_BAD_PARAM, "got 0x%lx\n", hr);

    Incident = MARKER;
    hr = pNdfCreateWebIncidentEx(L"http://www.example.com/", TRUE, NULL, &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (hr == S_OK)
        pNdfCloseIncident(Incident);
    Incident = MARKER;
    hr = pNdfCreateWebIncidentEx(NULL, TRUE, NULL, &Incident);
    ok(hr == NDF_E_BAD_PARAM, "got 0x%lx\n", hr);
    ok(Incident == MARKER, "got %p\n", Incident);

    /* the path of a share may be missing */
    Incident = MARKER;
    hr = pNdfCreateSharingIncident(L"\\\\ndfapi-apitest\\share", &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (hr == S_OK)
        pNdfCloseIncident(Incident);
    Incident = MARKER;
    hr = pNdfCreateSharingIncident(NULL, &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (hr == S_OK)
        pNdfCloseIncident(Incident);

    Incident = MARKER;
    hr = pNdfCreateDNSIncident(L"example.com", 1, &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (hr == S_OK)
        pNdfCloseIncident(Incident);
    Incident = MARKER;
    hr = pNdfCreateDNSIncident(NULL, 1, &Incident);
    ok(hr == NDF_E_BAD_PARAM, "got 0x%lx\n", hr);
    ok(Incident == MARKER, "got %p\n", Incident);
}

static void test_handle_checks(void)
{
    NDFHANDLE Incident;
    LPCWSTR TraceFile = (LPCWSTR)(ULONG_PTR)0x1234;
    ULONG Count;
    PVOID Causes;
    HRESULT hr;

    /* nothing to close is closed, and nothing else can be used */
    hr = pNdfCloseIncident(NULL);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pNdfCancelIncident(NULL);
    ok(hr == E_HANDLE, "got 0x%lx\n", hr);
    hr = pNdfGetTraceFile(NULL, &TraceFile);
    ok(hr == E_HANDLE, "got 0x%lx\n", hr);
    ok(TraceFile == (LPCWSTR)(ULONG_PTR)0x1234, "got %p\n", TraceFile);

    Count = 0xdeadbeef;
    Causes = (PVOID)(ULONG_PTR)0x1234;
    hr = pNdfDiagnoseIncident(NULL, &Count, &Causes, 1000, 0);
    ok(hr == E_HANDLE, "got 0x%lx\n", hr);
    ok(Count == 0xdeadbeef, "got %lx\n", Count);
    ok(Causes == (PVOID)(ULONG_PTR)0x1234, "got %p\n", Causes);
    hr = pNdfExecuteDiagnosis(NULL, NULL);
    ok(hr == E_HANDLE, "got 0x%lx\n", hr);

    Incident = NULL;
    hr = pNdfCreateWebIncident(L"http://www.example.com/", &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (!Incident)
    {
        skip("No incident to work with\n");
        return;
    }
    hr = pNdfDiagnoseIncident(Incident, NULL, &Causes, 1000, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pNdfDiagnoseIncident(Incident, &Count, NULL, 1000, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);

    /* nothing is running yet, so there is nothing to cancel and no trace of it */
    hr = pNdfCancelIncident(Incident);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pNdfGetTraceFile(Incident, &TraceFile);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pNdfCloseIncident(Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
}

static BOOL service_exists(PCWSTR Name)
{
    SC_HANDLE Manager, Service;
    BOOL Exists = TRUE;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!Manager)
        return TRUE;
    Service = OpenServiceW(Manager, Name, SERVICE_QUERY_STATUS);
    if (Service)
        CloseServiceHandle(Service);
    else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
        Exists = FALSE;
    CloseServiceHandle(Manager);
    return Exists;
}

static void test_diagnosis_without_service(void)
{
    NDFHANDLE Incident = NULL;
    ULONG Count;
    PVOID Causes;
    HRESULT hr;

    /* Windows runs the diagnosis in its Diagnostic Policy Service; the test needs a system that has none */
    if (service_exists(L"DPS"))
    {
        skip("The Diagnostic Policy Service is installed\n");
        return;
    }

    hr = pNdfCreateWebIncident(L"http://www.example.com/", &Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (!Incident)
    {
        skip("No incident to work with\n");
        return;
    }

    /* the diagnosis finds nothing to work with, and the outputs say so */
    Count = 0xdeadbeef;
    Causes = (PVOID)(ULONG_PTR)0x1234;
    hr = pNdfDiagnoseIncident(Incident, &Count, &Causes, 1000, 0);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got 0x%lx\n", hr);
    ok(Count == 0, "got %lu\n", Count);
    ok(Causes == NULL, "got %p\n", Causes);

    hr = pNdfCloseIncident(Incident);
    ok(hr == S_OK, "got 0x%lx\n", hr);
}

static void test_winsock(void)
{
    struct sockaddr_in Address;
    NDFHANDLE Incident;
    SOCKET Listener, Client, Server;
    WSADATA Data;
    HRESULT hr;
    int Length;

    if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    /* the socket is asked for its address: not a socket, and a socket that has none */
    Incident = MARKER;
    hr = pNdfCreateWinSockIncident(INVALID_SOCKET, L"www.example.com", 80, NULL, NULL, &Incident);
    ok(hr == HRESULT_FROM_WIN32(WSAENOTSOCK), "got 0x%lx\n", hr);
    ok(Incident == MARKER, "got %p\n", Incident);

    Client = socket(AF_INET, SOCK_STREAM, 0);
    ok(Client != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (Client == INVALID_SOCKET)
        goto done;
    Incident = MARKER;
    hr = pNdfCreateWinSockIncident(Client, L"www.example.com", 80, NULL, NULL, &Incident);
    ok(hr == HRESULT_FROM_WIN32(WSAEINVAL), "got 0x%lx\n", hr);
    ok(Incident == MARKER, "got %p\n", Incident);
    hr = pNdfCreateWinSockIncident(Client, NULL, 80, NULL, NULL, &Incident);
    ok(hr == HRESULT_FROM_WIN32(WSAEINVAL), "got 0x%lx\n", hr);

    /* a connected socket makes an incident */
    Listener = socket(AF_INET, SOCK_STREAM, 0);
    memset(&Address, 0, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Length = sizeof(Address);
    if (Listener != INVALID_SOCKET &&
        bind(Listener, (struct sockaddr *)&Address, sizeof(Address)) == 0 &&
        getsockname(Listener, (struct sockaddr *)&Address, &Length) == 0 &&
        listen(Listener, 1) == 0)
    {
        /* a bound socket that is not connected has no remote address */
        Incident = MARKER;
        hr = pNdfCreateWinSockIncident(Listener, L"127.0.0.1", ntohs(Address.sin_port), NULL, NULL, &Incident);
        ok(hr == HRESULT_FROM_WIN32(WSAENOTCONN), "got 0x%lx\n", hr);
        ok(Incident == MARKER, "got %p\n", Incident);

        if (connect(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0)
        {
            Server = accept(Listener, NULL, NULL);
            Incident = MARKER;
            hr = pNdfCreateWinSockIncident(Client, L"127.0.0.1", ntohs(Address.sin_port), NULL, NULL, &Incident);
            ok(hr == S_OK, "got 0x%lx\n", hr);
            ok(Incident != NULL && Incident != MARKER, "got %p\n", Incident);
            if (hr == S_OK)
                pNdfCloseIncident(Incident);

            /* the host and the port are not needed, the handle is */
            Incident = MARKER;
            hr = pNdfCreateWinSockIncident(Client, NULL, 0, NULL, NULL, &Incident);
            ok(hr == S_OK, "got 0x%lx\n", hr);
            if (hr == S_OK)
                pNdfCloseIncident(Incident);
            hr = pNdfCreateWinSockIncident(Client, L"127.0.0.1", 80, NULL, NULL, NULL);
            ok(hr == NDF_E_BAD_PARAM, "got 0x%lx\n", hr);
            if (Server != INVALID_SOCKET)
                closesocket(Server);
        }
        else
        {
            skip("No connected socket to work with\n");
        }
    }
    else
    {
        skip("No connected socket to work with\n");
    }
    if (Listener != INVALID_SOCKET)
        closesocket(Listener);
    closesocket(Client);

done:
    WSACleanup();
}

static void test_com(void)
{
    HRESULT (WINAPI *pDllCanUnloadNow)(void) = (PVOID)GetProcAddress(hNdfapi, "DllCanUnloadNow");
    HRESULT (WINAPI *pDllGetClassObject)(const GUID *, const GUID *, PVOID *) = (PVOID)GetProcAddress(hNdfapi, "DllGetClassObject");
    static const GUID Unknown = { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    static const GUID IID_ClassFactory = { 0x00000001, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    PVOID Object = (PVOID)(ULONG_PTR)0x1234;
    HRESULT hr;

    if (!pDllCanUnloadNow || !pDllGetClassObject)
    {
        skip("The COM entry points are not available\n");
        return;
    }

    hr = pDllCanUnloadNow();
    ok(hr == S_OK, "got 0x%lx\n", hr);

    /* there is no class to hand out, and the output is cleared */
    hr = pDllGetClassObject(&Unknown, &IID_ClassFactory, &Object);
    ok(hr == CLASS_E_CLASSNOTAVAILABLE, "got 0x%lx\n", hr);
    ok(Object == NULL, "got %p\n", Object);
}

START_TEST(ndfapi)
{
    hNdfapi = LoadLibraryW(L"ndfapi.dll");
    ok(hNdfapi != NULL, "LoadLibrary failed with %lu\n", GetLastError());
    if (!hNdfapi)
    {
        skip("ndfapi.dll is not available\n");
        return;
    }

    test_exports();

    pNdfCloseIncident = (PVOID)GetProcAddress(hNdfapi, "NdfCloseIncident");
    pNdfCancelIncident = (PVOID)GetProcAddress(hNdfapi, "NdfCancelIncident");
    pNdfGetTraceFile = (PVOID)GetProcAddress(hNdfapi, "NdfGetTraceFile");
    pNdfCreateConnectivityIncident = (PVOID)GetProcAddress(hNdfapi, "NdfCreateConnectivityIncident");
    pNdfCreateWebIncident = (PVOID)GetProcAddress(hNdfapi, "NdfCreateWebIncident");
    pNdfCreateWebIncidentEx = (PVOID)GetProcAddress(hNdfapi, "NdfCreateWebIncidentEx");
    pNdfCreateSharingIncident = (PVOID)GetProcAddress(hNdfapi, "NdfCreateSharingIncident");
    pNdfCreateDNSIncident = (PVOID)GetProcAddress(hNdfapi, "NdfCreateDNSIncident");
    pNdfCreateWinSockIncident = (PVOID)GetProcAddress(hNdfapi, "NdfCreateWinSockIncident");
    pNdfDiagnoseIncident = (PVOID)GetProcAddress(hNdfapi, "NdfDiagnoseIncident");
    pNdfExecuteDiagnosis = (PVOID)GetProcAddress(hNdfapi, "NdfExecuteDiagnosis");
    if (!pNdfCloseIncident || !pNdfCancelIncident || !pNdfGetTraceFile || !pNdfCreateConnectivityIncident ||
        !pNdfCreateWebIncident || !pNdfCreateWebIncidentEx || !pNdfCreateSharingIncident ||
        !pNdfCreateDNSIncident || !pNdfCreateWinSockIncident || !pNdfDiagnoseIncident ||
        !pNdfExecuteDiagnosis)
    {
        skip("Not all of the exports are available\n");
        FreeLibrary(hNdfapi);
        return;
    }

    test_create();
    test_handle_checks();
    test_winsock();
    test_diagnosis_without_service();
    test_com();

    FreeLibrary(hNdfapi);
}
