/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     COM surrogate (dllhost.exe) activation and lifetime
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#define WIN32_NO_STATUS
#define COBJMACROS
#include <apitest.h>
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>

/* The class belongs to an AppID with an empty DllSurrogate value: COM runs it in a
 * dllhost.exe of the bitness of the registry view that has the class. The test
 * registers it in the view of its own bitness (for a 32-bit test on amd64 that is
 * the 32-bit surrogate in SysWOW64). The last byte tells the 64-bit and the 32-bit
 * run of the test apart. */
static GUID CLSID_Server = { 0x5d0e1f10, 0xd11c, 0x4057, { 0x9a, 0x5e, 0x53, 0x75, 0x72, 0x72, 0x6f, 0x00 } };
static GUID AppId = { 0x5d0e1f11, 0xd11c, 0x4057, { 0x9a, 0x5e, 0x53, 0x75, 0x72, 0x72, 0x6f, 0x00 } };

#define EXIT_TIMEOUT 20000

static void GuidString(const GUID *guid, WCHAR *out)
{
    StringFromGUID2(guid, out, 40);
}

static BOOL SetString(const WCHAR *path, const WCHAR *name, const WCHAR *value)
{
    HKEY key;
    LONG ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);

    if (ret)
        return FALSE;
    ret = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return !ret;
}

static BOOL Register(void)
{
    WCHAR clsid[40], appid[40], path[MAX_PATH], key[200], *p;

    GuidString(&CLSID_Server, clsid);
    GuidString(&AppId, appid);

    GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    p = wcsrchr(path, L'\\');
    lstrcpyW(p ? p + 1 : path, L"dllhost_server.dll");

    wsprintfW(key, L"SOFTWARE\\Classes\\CLSID\\%s", clsid);
    if (!SetString(key, NULL, L"dllhost surrogate test") || !SetString(key, L"AppID", appid))
        return FALSE;
    wsprintfW(key, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", clsid);
    if (!SetString(key, NULL, path) || !SetString(key, L"ThreadingModel", L"Both"))
        return FALSE;
    wsprintfW(key, L"SOFTWARE\\Classes\\AppID\\%s", appid);
    return SetString(key, NULL, L"dllhost surrogate test") && SetString(key, L"DllSurrogate", L"");
}

static void Unregister(void)
{
    WCHAR clsid[40], appid[40], key[200];

    GuidString(&CLSID_Server, clsid);
    GuidString(&AppId, appid);
    wsprintfW(key, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    wsprintfW(key, L"SOFTWARE\\Classes\\CLSID\\%s", clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    wsprintfW(key, L"SOFTWARE\\Classes\\AppID\\%s", appid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
}

static BOOL IsWow64(HANDLE process)
{
    typedef BOOL (WINAPI *PFN_IsWow64Process)(HANDLE, PBOOL);
    PFN_IsWow64Process pIsWow64Process =
        (PFN_IsWow64Process)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    BOOL wow64 = FALSE;

    if (pIsWow64Process)
        pIsWow64Process(process, &wow64);
    return wow64;
}

static BOOL GetImageName(DWORD pid, WCHAR *name)
{
    PROCESSENTRY32W entry;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    BOOL found = FALSE;

    if (snap == INVALID_HANDLE_VALUE)
        return FALSE;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry))
    {
        do
        {
            if (entry.th32ProcessID == pid)
            {
                lstrcpyW(name, entry.szExeFile);
                found = TRUE;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

/* Activates the server; returns the object and the process the server lives in. */
static IPersist *Activate(DWORD *pid, HRESULT *hr_out)
{
    IPersist *persist = NULL;
    GUID id;
    HRESULT hr;

    *pid = 0;
    hr = CoCreateInstance(&CLSID_Server, NULL, CLSCTX_LOCAL_SERVER, &IID_IPersist, (void **)&persist);
    *hr_out = hr;
    if (hr != S_OK)
        return NULL;

    memset(&id, 0, sizeof(id));
    hr = IPersist_GetClassID(persist, &id);
    ok(hr == S_OK, "IPersist::GetClassID returned %08lx\n", hr);
    ok(id.Data2 == 0xd11c && id.Data3 == 0x4057, "method result is not the server's\n");
    *pid = id.Data1;
    return persist;
}

/* A hung COM call must not hang the test run */
static DWORD WINAPI Watchdog(void *param)
{
    Sleep(240000);
    trace("Surrogate test timed out\n");
    Unregister();
    ExitProcess(1);
    return 0;
}

START_TEST(Surrogate)
{
    BOOL clientWow64 = IsWow64(GetCurrentProcess());
    IPersist *first, *second, *third, *fourth;
    WCHAR name[MAX_PATH];
    HANDLE process, process2, process3 = NULL;
    DWORD pid, pid2, pid3, pid4, wait;
    GUID id;
    HRESULT hr;

    CloseHandle(CreateThread(NULL, 0, Watchdog, NULL, 0, NULL));

    /* The 64-bit and the 32-bit run use different classes: they may run in one session */
    CLSID_Server.Data4[7] = sizeof(void *) == 4 ? 0x32 : 0x64;
    AppId.Data4[7] = CLSID_Server.Data4[7];

    if (!Register())
    {
        skip("Cannot register the surrogate test class (administrator needed)\n");
        return;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(hr == S_OK, "CoInitializeEx returned %08lx\n", hr);

    /* Activation in a separate dllhost.exe, and a method call */
    first = Activate(&pid, &hr);
    ok(hr == S_OK, "CoCreateInstance returned %08lx\n", hr);
    if (!first)
        goto done;
    ok(pid != GetCurrentProcessId(), "server runs in the test process\n");
    ok(GetImageName(pid, name), "server process %lu does not exist\n", pid);
    ok(!lstrcmpiW(name, L"dllhost.exe"), "server process is %ls\n", name);
    process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    ok(process != NULL, "OpenProcess failed, error %lu\n", GetLastError());
    if (process)
        ok(IsWow64(process) == clientWow64, "surrogate %s the bitness of the test (wow64 %d)\n",
           IsWow64(process) == clientWow64 ? "matches" : "does not match", IsWow64(process));

    /* A second object of the class lives in the same surrogate */
    second = Activate(&pid2, &hr);
    ok(hr == S_OK, "second CoCreateInstance returned %08lx\n", hr);
    ok(pid2 == pid, "second object in process %lu, first in %lu\n", pid2, pid);
    if (second)
    {
        IPersist_Release(second);
        if (process)
        {
            wait = WaitForSingleObject(process, 1500);
            ok(wait == WAIT_TIMEOUT, "surrogate exited while an object is in use (%lu)\n", wait);
        }
    }

    /* The surrogate exits after the last release */
    IPersist_Release(first);
    if (process)
    {
        wait = WaitForSingleObject(process, EXIT_TIMEOUT);
        ok(wait == WAIT_OBJECT_0, "surrogate did not exit after the last release (%lu)\n", wait);
        if (wait != WAIT_OBJECT_0)
            TerminateProcess(process, 0);
    }

    /* A new activation starts a new surrogate. The handle of the old one stays open
     * so that its process id cannot be reused by the new one. */
    first = Activate(&pid2, &hr);
    ok(hr == S_OK, "CoCreateInstance after the exit returned %08lx\n", hr);
    ok(pid2 != pid, "no new surrogate (process %lu)\n", pid2);
    if (process)
        CloseHandle(process);
    if (!first)
        goto done;

    /* A killed surrogate leaves nothing behind: the next activation launches a new one */
    process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid2);
    ok(process != NULL, "OpenProcess failed, error %lu\n", GetLastError());
    if (process)
    {
        TerminateProcess(process, 0);
        wait = WaitForSingleObject(process, EXIT_TIMEOUT);
        ok(wait == WAIT_OBJECT_0, "killed surrogate still running (%lu)\n", wait);
        process3 = process;
    }
    IPersist_Release(first);

    third = Activate(&pid3, &hr);
    ok(hr == S_OK, "CoCreateInstance after the kill returned %08lx\n", hr);
    ok(pid3 != 0 && pid3 != pid2, "no new surrogate after the kill (process %lu, was %lu)\n", pid3, pid2);
    if (third)
    {
        ok(GetImageName(pid3, name) && !lstrcmpiW(name, L"dllhost.exe"), "new server process is %ls\n", name);
        process2 = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid3);
        IPersist_Release(third);
        if (process2)
        {
            wait = WaitForSingleObject(process2, EXIT_TIMEOUT);
            ok(wait == WAIT_OBJECT_0, "new surrogate did not exit after the last release (%lu)\n", wait);
            if (wait != WAIT_OBJECT_0)
                TerminateProcess(process2, 0);
            CloseHandle(process2);
        }
    }
    if (process3)
        CloseHandle(process3);

    /* CoDisconnectObject drops the references of the object's clients: the surrogate
     * exits although the client still holds a (now dead) proxy. */
    fourth = Activate(&pid4, &hr);
    ok(hr == S_OK, "CoCreateInstance for the disconnect case returned %08lx\n", hr);
    if (fourth)
    {
        process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid4);
        ok(process != NULL, "OpenProcess failed, error %lu\n", GetLastError());
        IPersist_GetClassID(fourth, &id);
        hr = IPersist_GetClassID(fourth, &id); /* the server disconnects the object in this call */
        trace("call that disconnects the object returned %08lx\n", hr);
        if (process)
        {
            wait = WaitForSingleObject(process, EXIT_TIMEOUT);
            ok(wait == WAIT_OBJECT_0, "surrogate did not exit after its object was disconnected (%lu)\n", wait);
            if (wait != WAIT_OBJECT_0)
                TerminateProcess(process, 0);
            CloseHandle(process);
        }
        hr = IPersist_GetClassID(fourth, &id);
        ok(FAILED(hr), "call on the disconnected object returned %08lx\n", hr);
        IPersist_Release(fourth);
    }

done:
    CoUninitialize();
    Unregister();
}
