/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Saving an Internet shortcut, with its icon properties released and still open
 */

#define COBJMACROS
#define WIN32_NO_STATUS
#include <apitest.h>
#include <windows.h>
#include <shlobj.h>
#include <shlguid.h>
#include <intshcut.h>
#include <initguid.h>

DEFINE_GUID(CLSID_InternetShortcutTest, 0xFBF23B40, 0xE3F0, 0x101B, 0x84, 0x88, 0x00, 0xAA, 0x00, 0x3E, 0x56, 0xF8);
DEFINE_GUID(FMTID_IntshcutTest, 0x000214A0, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

static void check_file(const WCHAR *path, const char *what)
{
    char data[512];
    DWORD size = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    ok(file != INVALID_HANDLE_VALUE, "%s: the shortcut file was not written, error %lu\n", what, GetLastError());
    if (file == INVALID_HANDLE_VALUE) return;

    memset(data, 0, sizeof(data));
    ReadFile(file, data, sizeof(data) - 1, &size, NULL);
    CloseHandle(file);
    ok(strstr(data, "[InternetShortcut]") != NULL, "%s: missing the section header: %s\n", what, data);
    ok(strstr(data, "URL=http://some/bogus/url.html") != NULL, "%s: missing the URL: %s\n", what, data);
    /* Windows writes IconFile and IconIndex, ReactOS ICONFILE and ICONINDEX; the names are not case sensitive */
    CharLowerA(data);
    ok(strstr(data, "iconfile=c:\\icons\\test.ico") != NULL, "%s: missing the icon file: %s\n", what, data);
    ok(strstr(data, "iconindex=3") != NULL, "%s: missing the icon index: %s\n", what, data);
}

static void save_with_properties(BOOL keep_open)
{
    IUniformResourceLocatorW *url;
    IPropertySetStorage *propsets;
    IPropertyStorage *props;
    IPersistFile *pf;
    WCHAR path[MAX_PATH];
    PROPSPEC ps[2];
    PROPVARIANT pv[2];
    HANDLE file;
    DWORD written;
    HRESULT hr;
    const char *what = keep_open ? "storage held open" : "storage released";

    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"testopenprops.url");

    /* an older file that the save must replace completely */
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(file, "old content that is longer than the new URL line, ICONFILE=old\r\n", 62, &written, NULL);
    CloseHandle(file);

    hr = CoCreateInstance(&CLSID_InternetShortcutTest, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUniformResourceLocatorW, (void **)&url);
    ok(hr == S_OK, "%s: CoCreateInstance returned %08lx\n", what, hr);
    if (hr != S_OK) return;

    hr = url->lpVtbl->SetURL(url, L"http://some/bogus/url.html", 0);
    ok(hr == S_OK, "%s: SetURL returned %08lx\n", what, hr);

    hr = url->lpVtbl->QueryInterface(url, &IID_IPropertySetStorage, (void **)&propsets);
    ok(hr == S_OK, "%s: QueryInterface(IPropertySetStorage) returned %08lx\n", what, hr);
    hr = IPropertySetStorage_Open(propsets, &FMTID_IntshcutTest, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, &props);
    ok(hr == S_OK, "%s: Open returned %08lx\n", what, hr);

    ps[0].ulKind = PRSPEC_PROPID;
    ps[0].propid = PID_IS_ICONFILE;
    ps[1].ulKind = PRSPEC_PROPID;
    ps[1].propid = PID_IS_ICONINDEX;
    pv[0].vt = VT_LPWSTR;
    pv[0].pwszVal = (WCHAR *)L"C:\\icons\\test.ico";
    pv[1].vt = VT_I4;
    pv[1].lVal = 3;
    hr = IPropertyStorage_WriteMultiple(props, 2, ps, pv, 0);
    ok(hr == S_OK, "%s: WriteMultiple returned %08lx\n", what, hr);
    hr = IPropertyStorage_Commit(props, STGC_DEFAULT);
    ok(hr == S_OK, "%s: Commit returned %08lx\n", what, hr);
    if (!keep_open)
    {
        IPropertyStorage_Release(props);
        props = NULL;
    }

    hr = url->lpVtbl->QueryInterface(url, &IID_IPersistFile, (void **)&pf);
    ok(hr == S_OK, "%s: QueryInterface(IPersistFile) returned %08lx\n", what, hr);
    hr = IPersistFile_Save(pf, path, FALSE);
    ok(hr == S_OK, "%s: Save returned %08lx\n", what, hr);
    IPersistFile_Release(pf);

    check_file(path, what);
    DeleteFileW(path);

    if (props) IPropertyStorage_Release(props);
    IPropertySetStorage_Release(propsets);
    url->lpVtbl->Release(url);
}

static void read_file(const WCHAR *path, char *data, DWORD max)
{
    DWORD size = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    data[0] = 0;
    if (file == INVALID_HANDLE_VALUE) return;
    ReadFile(file, data, max - 1, &size, NULL);
    data[size] = 0;
    CloseHandle(file);
    CharLowerA(data);
}

static IUniformResourceLocatorW *create_shortcut(void)
{
    IUniformResourceLocatorW *url;
    HRESULT hr = CoCreateInstance(&CLSID_InternetShortcutTest, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IUniformResourceLocatorW, (void **)&url);

    ok(hr == S_OK, "CoCreateInstance returned %08lx\n", hr);
    if (hr != S_OK) return NULL;
    hr = url->lpVtbl->SetURL(url, L"http://some/bogus/url.html", 0);
    ok(hr == S_OK, "SetURL returned %08lx\n", hr);
    return url;
}

static IPropertyStorage *write_icon(IPropertySetStorage *propsets, BOOL create, const WCHAR *icon, LONG index)
{
    IPropertyStorage *props = NULL;
    PROPSPEC ps[2];
    PROPVARIANT pv[2];
    HRESULT hr;

    if (create)
        hr = IPropertySetStorage_Create(propsets, &FMTID_IntshcutTest, NULL, PROPSETFLAG_DEFAULT,
                                        STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, &props);
    else
        hr = IPropertySetStorage_Open(propsets, &FMTID_IntshcutTest, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, &props);
    ok(hr == S_OK, "%s returned %08lx\n", create ? "Create" : "Open", hr);
    if (hr != S_OK) return NULL;

    if (icon)
    {
        ps[0].ulKind = PRSPEC_PROPID;
        ps[0].propid = PID_IS_ICONFILE;
        ps[1].ulKind = PRSPEC_PROPID;
        ps[1].propid = PID_IS_ICONINDEX;
        pv[0].vt = VT_LPWSTR;
        pv[0].pwszVal = (WCHAR *)icon;
        pv[1].vt = VT_I4;
        pv[1].lVal = index;
        hr = IPropertyStorage_WriteMultiple(props, 2, ps, pv, 0);
        ok(hr == S_OK, "WriteMultiple returned %08lx\n", hr);
        hr = IPropertyStorage_Commit(props, STGC_DEFAULT);
        ok(hr == S_OK, "Commit returned %08lx\n", hr);
    }
    return props;
}

/* A failed save leaves the old file as it was and no temporary file behind */
static void test_failed_save(void)
{
    static const char old_content[] = "[InternetShortcut]\r\nURL=http://old.example/\r\nIconFile=C:\\old.ico\r\nIconIndex=7\r\n";
    IUniformResourceLocatorW *url;
    IPropertySetStorage *propsets;
    IPropertyStorage *props;
    IPersistFile *pf;
    WCHAR dir[MAX_PATH], path[MAX_PATH], find[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE file, search;
    char data[512];
    DWORD written;
    int files = 0;
    HRESULT hr;

    GetTempPathW(MAX_PATH, dir);
    lstrcatW(dir, L"testurldir");
    CreateDirectoryW(dir, NULL);
    lstrcpyW(path, dir);
    lstrcatW(path, L"\\kept.url");

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(file, old_content, sizeof(old_content) - 1, &written, NULL);
    CloseHandle(file);

    url = create_shortcut();
    if (!url) return;
    url->lpVtbl->QueryInterface(url, &IID_IPropertySetStorage, (void **)&propsets);
    props = write_icon(propsets, FALSE, L"C:\\icons\\test.ico", 3);
    url->lpVtbl->QueryInterface(url, &IID_IPersistFile, (void **)&pf);

    /* a destination that cannot be replaced: another handle keeps it open without sharing */
    file = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(file != INVALID_HANDLE_VALUE, "could not lock the file\n");
    hr = IPersistFile_Save(pf, path, FALSE);
    ok(hr != S_OK, "Save over a locked file returned %08lx\n", hr);
    CloseHandle(file);

    read_file(path, data, sizeof(data));
    ok(strstr(data, "http://old.example/") != NULL && strstr(data, "iconindex=7") != NULL,
       "the old file was changed: %s\n", data);

    lstrcpyW(find, dir);
    lstrcatW(find, L"\\*");
    search = FindFirstFileW(find, &fd);
    if (search != INVALID_HANDLE_VALUE)
    {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files++; } while (FindNextFileW(search, &fd));
        FindClose(search);
    }
    ok(files == 1, "the directory holds %d files\n", files);

    /* the same save works once the file is free, and replaces the content */
    hr = IPersistFile_Save(pf, path, FALSE);
    ok(hr == S_OK, "Save returned %08lx\n", hr);
    read_file(path, data, sizeof(data));
    ok(strstr(data, "http://some/bogus/url.html") != NULL && strstr(data, "old.example") == NULL, "not replaced: %s\n", data);

    IPersistFile_Release(pf);
    if (props) IPropertyStorage_Release(props);
    IPropertySetStorage_Release(propsets);
    url->lpVtbl->Release(url);
    DeleteFileW(path);
    RemoveDirectoryW(dir);
}

/* Creating the property set again replaces it: the old icon must not be saved */
static void test_create_replaces_icon(void)
{
    IUniformResourceLocatorW *url;
    IPropertySetStorage *propsets;
    IPropertyStorage *props;
    IPersistFile *pf;
    WCHAR path[MAX_PATH];
    char data[512];
    HRESULT hr;

    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"testcreateicon.url");

    url = create_shortcut();
    if (!url) return;
    url->lpVtbl->QueryInterface(url, &IID_IPropertySetStorage, (void **)&propsets);
    url->lpVtbl->QueryInterface(url, &IID_IPersistFile, (void **)&pf);

    /* Windows does not implement Create for the shortcut's property set */
    hr = IPropertySetStorage_Create(propsets, &FMTID_IntshcutTest, NULL, PROPSETFLAG_DEFAULT,
                                    STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, &props);
    if (hr == E_NOTIMPL)
    {
        skip("IPropertySetStorage::Create is not implemented\n");
        IPersistFile_Release(pf);
        IPropertySetStorage_Release(propsets);
        url->lpVtbl->Release(url);
        return;
    }
    ok(hr == S_OK, "Create returned %08lx\n", hr);
    if (props) IPropertyStorage_Release(props);

    props = write_icon(propsets, FALSE, L"C:\\icons\\old.ico", 1);
    if (props) IPropertyStorage_Release(props);

    /* a new set without icon, still held open */
    props = write_icon(propsets, TRUE, NULL, 0);
    hr = IPersistFile_Save(pf, path, FALSE);
    ok(hr == S_OK, "Save returned %08lx\n", hr);
    read_file(path, data, sizeof(data));
    ok(strstr(data, "url=http://some/bogus/url.html") != NULL, "missing the URL: %s\n", data);
    ok(strstr(data, "old.ico") == NULL, "the replaced icon was saved: %s\n", data);
    if (props) IPropertyStorage_Release(props);

    /* and a new set with another icon */
    props = write_icon(propsets, TRUE, L"C:\\icons\\new.ico", 5);
    hr = IPersistFile_Save(pf, path, FALSE);
    ok(hr == S_OK, "Save returned %08lx\n", hr);
    read_file(path, data, sizeof(data));
    ok(strstr(data, "new.ico") != NULL && strstr(data, "iconindex=5") != NULL, "the new icon is missing: %s\n", data);
    ok(strstr(data, "old.ico") == NULL, "the replaced icon was saved: %s\n", data);
    if (props) IPropertyStorage_Release(props);

    IPersistFile_Release(pf);
    IPropertySetStorage_Release(propsets);
    url->lpVtbl->Release(url);
    DeleteFileW(path);
}

START_TEST(InternetShortcut)
{
    CoInitialize(NULL);
    save_with_properties(FALSE);
    save_with_properties(TRUE);
    test_failed_save();
    test_create_replaces_icon();
    CoUninitialize();
}
