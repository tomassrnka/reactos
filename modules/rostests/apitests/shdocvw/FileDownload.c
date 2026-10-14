/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for shdocvw!DoFileDownload: the File Download dialog, the progress dialog,
 *              the saved file, cancelling and errors, against a local HTTP server
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include <winsock2.h>
#include <apitest.h>
#include <shlwapi.h>

typedef BOOL (WINAPI *FN_DoFileDownload)(LPWSTR);

#define BODY_SIZE   300000
#define LONG_SIZE   (64 * 1024 * 1024)
#define SHORT_SENT  40000
#define SHORT_SIZE  100000

#define LONG_BASE \
    "b123456789b123456789b123456789b123456789b123456789b123456789b123456789b123456789b123456789b123456789"
#define LONG_EXT \
    "e123456789e123456789e123456789e123456789e123456789e123456789e123456789e123456789e123456789e123456789e123456789"

typedef enum {
    SCEN_SAVE,          /* Save, name the file, wait for completion, Close */
    SCEN_CANCEL_ASK,    /* Cancel in the File Download dialog */
    SCEN_CANCEL_SAVEAS, /* Save, then Cancel in Save As */
    SCEN_CANCEL_RUN,    /* Save, then Cancel in the progress dialog */
    SCEN_ERROR_BOX,     /* an error box has to appear; click OK */
    SCEN_OPEN           /* Open: the file goes to the temporary folder and is started */
} scenario_t;

static SOCKET g_listener = INVALID_SOCKET;
static USHORT g_port;
static volatile LONG g_stop_server;
static HANDLE g_server_thread;

static WCHAR g_target[MAX_PATH];
static WCHAR g_expected_name[MAX_PATH];
static WCHAR g_saveas_name[MAX_PATH];
static volatile LONG g_scenario_done;
static DWORD g_main_thread;
static BOOL g_verbose;
static volatile LONG g_saw_progress;
static volatile LONG g_saw_complete;
static volatile LONG g_saw_error;
static volatile LONG g_saw_name;
static volatile LONG g_saw_saveas;
static volatile LONG g_saw_ask;

static void fill_body(BYTE *buf, int start, int len)
{
    int i;

    for (i = 0; i < len; i++)
        buf[i] = (BYTE)(((start + i) * 31 + 7) & 0xff);
}

static int send_all(SOCKET s, const char *data, int len)
{
    while (len > 0)
    {
        int n = send(s, data, len, 0);

        if (n <= 0)
            return -1;
        data += n;
        len -= n;
    }
    return 0;
}

static void serve_connection(SOCKET c)
{
    char req[2048], head[512];
    int got = 0, n;
    BYTE *chunk;

    while (got < (int)sizeof(req) - 1)
    {
        n = recv(c, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    req[got] = 0;
    chunk = HeapAlloc(GetProcessHeap(), 0, 65536);
    if (!chunk)
        return;

    if (!strncmp(req, "GET /missing.bin", 16))
    {
        static const char body[] = "\1binary 404";

        n = snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Type: application/octet-stream\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", (int)sizeof(body));
        send_all(c, head, n);
        send_all(c, body, sizeof(body));
    }
    else if (!strncmp(req, "GET /short.bin", 14))
    {
        n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", SHORT_SIZE);
        send_all(c, head, n);
        fill_body(chunk, 0, SHORT_SENT);
        send_all(c, (char *)chunk, SHORT_SENT);
    }
    else if (!strncmp(req, "GET /long.bin", 13))
    {
        int sent = 0;

        n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", LONG_SIZE);
        send_all(c, head, n);
        while (sent < LONG_SIZE && !g_stop_server)
        {
            fill_body(chunk, sent, 16384);
            if (send_all(c, (char *)chunk, 16384) < 0)
                break;
            sent += 16384;
            Sleep(20);
        }
    }
    else if (!strncmp(req, "GET /ok.bin", 11) || !strncmp(req, "GET /cd.bin", 11) || !strncmp(req, "GET /longext.bin", 16) ||
             !strncmp(req, "GET /pl/", 8))
    {
        int sent = 0;
        BOOL cd = !strncmp(req, "GET /cd.bin", 11);
        BOOL longext = !strncmp(req, "GET /longext.bin", 16);
        char longname[320];

        /* a name of 100 characters, a dot and an extension of 110 characters: 211 in all */
        snprintf(longname, sizeof(longname), "Content-Disposition: attachment; filename=\"%s.%s\"\r\n", LONG_BASE, LONG_EXT);
        n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n%s"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n",
                     cd ? "Content-Disposition: attachment; filename=\"from-header.dat\"\r\n" : (longext ? longname : ""), BODY_SIZE);
        send_all(c, head, n);
        while (sent < BODY_SIZE)
        {
            int len = min(65536, BODY_SIZE - sent);

            fill_body(chunk, sent, len);
            if (send_all(c, (char *)chunk, len) < 0)
                break;
            sent += len;
        }
    }
    else
    {
        static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(c, nf, sizeof(nf) - 1);
    }

    HeapFree(GetProcessHeap(), 0, chunk);
}

static DWORD WINAPI server_thread(void *arg)
{
    while (!g_stop_server)
    {
        fd_set fds;
        struct timeval tv = { 0, 100000 };
        SOCKET c;

        FD_ZERO(&fds);
        FD_SET(g_listener, &fds);
        if (select(0, &fds, NULL, NULL, &tv) <= 0)
            continue;

        c = accept(g_listener, NULL, NULL);
        if (c == INVALID_SOCKET)
            continue;
        serve_connection(c);
        shutdown(c, SD_BOTH);
        closesocket(c);
    }
    return 0;
}

static BOOL start_server(void)
{
    WSADATA wsa;
    struct sockaddr_in addr;
    int len = sizeof(addr);

    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return FALSE;

    g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listener == INVALID_SOCKET)
        return FALSE;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(g_listener, 4) ||
        getsockname(g_listener, (struct sockaddr *)&addr, &len))
        return FALSE;
    g_port = ntohs(addr.sin_port);

    g_stop_server = 0;
    g_server_thread = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    return g_server_thread != NULL;
}

static void stop_server(void)
{
    g_stop_server = 1;
    if (g_server_thread)
    {
        WaitForSingleObject(g_server_thread, 10000);
        CloseHandle(g_server_thread);
        g_server_thread = NULL;
    }
    if (g_listener != INVALID_SOCKET)
        closesocket(g_listener);
    g_listener = INVALID_SOCKET;
    WSACleanup();
}

typedef struct {
    const WCHAR *text;
    BOOL contains;
    HWND found;
} find_text_t;

static void strip_accel(WCHAR *s)
{
    WCHAR *d = s;

    for (; *s; s++)
        if (*s != '&')
            *d++ = *s;
    *d = 0;
}

static BOOL CALLBACK find_child_text_proc(HWND hwnd, LPARAM lparam)
{
    find_text_t *f = (find_text_t *)lparam;
    WCHAR text[512];

    if (!GetWindowTextW(hwnd, text, ARRAY_SIZE(text)))
        return TRUE;
    strip_accel(text);
    if (f->contains ? wcsstr(text, f->text) != NULL : !wcscmp(text, f->text))
    {
        f->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND find_child_text(HWND dlg, const WCHAR *text, BOOL contains)
{
    find_text_t f = { text, contains, NULL };

    EnumChildWindows(dlg, find_child_text_proc, (LPARAM)&f);
    return f.found;
}

typedef struct {
    const WCHAR *caption;
    BOOL contains;
    HWND found;
} find_dialog_t;

static BOOL CALLBACK find_dialog_proc(HWND hwnd, LPARAM lparam)
{
    find_dialog_t *f = (find_dialog_t *)lparam;
    WCHAR cls[32], caption[512];
    DWORD pid;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
        return TRUE;
    if (!GetClassNameW(hwnd, cls, ARRAY_SIZE(cls)) || wcscmp(cls, L"#32770"))
        return TRUE;
    if (!GetWindowTextW(hwnd, caption, ARRAY_SIZE(caption)))
        return TRUE;
    if (g_verbose)
        trace("dialog %p \"%S\"\n", hwnd, caption);
    if (f->contains ? wcsstr(caption, f->caption) != NULL : !wcscmp(caption, f->caption))
    {
        f->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND find_dialog(const WCHAR *caption, BOOL contains)
{
    find_dialog_t f = { caption, contains, NULL };

    EnumWindows(find_dialog_proc, (LPARAM)&f);
    return f.found;
}

static BOOL click_button(HWND dlg, const WCHAR *text)
{
    HWND btn = find_child_text(dlg, text, FALSE);

    if (!btn)
        return FALSE;
    SendMessageW(btn, BM_CLICK, 0, 0);
    return TRUE;
}

static BOOL CALLBACK find_edit_proc(HWND hwnd, LPARAM lparam)
{
    WCHAR cls[32];
    HWND *edit = (HWND *)lparam;

    if (!GetClassNameW(hwnd, cls, ARRAY_SIZE(cls)))
        return TRUE;
    if (g_verbose)
        trace("Save As child %p class %S id %x visible %d\n", hwnd, cls, GetDlgCtrlID(hwnd), IsWindowVisible(hwnd));
    if (!wcsicmp(cls, L"Edit") && IsWindowVisible(hwnd) && (!*edit || GetDlgCtrlID(hwnd) == 0x480))
        *edit = hwnd;
    return TRUE;
}

static BOOL name_in_dialog(HWND dlg, const WCHAR *name)
{
    return find_child_text(dlg, name, FALSE) != NULL;
}

static DWORD WINAPI driver_thread(void *arg)
{
    scenario_t scen = (scenario_t)(INT_PTR)arg;
    DWORD start = GetTickCount();
    BOOL saved = FALSE, cancelled_progress = FALSE;
    int i;


    while (GetTickCount() - start < 40000 && !g_scenario_done)
    {
        HWND dlg;

        Sleep(100);

        dlg = find_dialog(L"Internet Browser", FALSE);
        if (dlg && find_child_text(dlg, L"Cannot download", TRUE))
        {
            g_saw_error = 1;
            click_button(dlg, L"OK");
            continue;
        }

        dlg = find_dialog(L"File Download", TRUE);
        if (dlg && !find_child_text(dlg, L"Save", FALSE))
            dlg = NULL;
        if (dlg && !saved)
        {
            /* the ask dialog has a Save button, the progress dialog never does */
            g_saw_ask = 1;
            if (g_expected_name[0] && name_in_dialog(dlg, g_expected_name))
                g_saw_name = 1;
            if (scen == SCEN_CANCEL_ASK)
            {
                click_button(dlg, L"Cancel");
                saved = TRUE;
            }
            else if (scen == SCEN_OPEN)
            {
                click_button(dlg, L"Open");
                saved = TRUE;
            }
            else
            {
                click_button(dlg, L"Save");
                saved = TRUE;
            }
            continue;
        }

        dlg = find_dialog(L"Save As", FALSE);
        if (dlg)
        {
            HWND edit = NULL;

            g_saw_saveas = 1;
            if (scen == SCEN_CANCEL_SAVEAS)
            {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                continue;
            }
            EnumChildWindows(dlg, find_edit_proc, (LPARAM)&edit);
            if (!edit)
            {
                g_verbose = TRUE;
                EnumChildWindows(dlg, find_edit_proc, (LPARAM)&edit);
                trace("no file name edit found in Save As\n");
            }
            else
            {
                /* what the dialog offers, before the test names the file */
                if (!g_saveas_name[0])
                    GetWindowTextW(edit, g_saveas_name, ARRAY_SIZE(g_saveas_name));
                SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)g_target);
            }
            SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            continue;
        }

        dlg = find_dialog(L"Download complete", FALSE);
        if (dlg)
        {
            g_saw_complete = 1;
            click_button(dlg, L"Close");
            continue;
        }

        dlg = find_dialog(L"127.0.0.1", TRUE);
        if (dlg && find_child_text(dlg, L"Cancel", FALSE))
        {
            g_saw_progress = 1;
            if (scen == SCEN_CANCEL_RUN && !cancelled_progress)
            {
                Sleep(500);
                cancelled_progress = TRUE;
                click_button(dlg, L"Cancel");
            }
        }
    }

    /* timed out: end whatever is still open, so that DoFileDownload returns and the test can report */
    if (!g_scenario_done)
    {
        trace("driver timed out\n");
        g_verbose = TRUE;
    }
    for (i = 0; i < 100 && !g_scenario_done; i++)
    {
        HWND dlg = find_dialog(L"127.0.0.1", TRUE);

        if (!dlg)
            dlg = find_dialog(L"File Download", TRUE);
        if (!dlg)
            dlg = find_dialog(L"Download complete", FALSE);
        if (!dlg)
            dlg = find_dialog(L"Internet Browser", FALSE);
        if (!dlg)
            dlg = find_dialog(L"Save As", FALSE);
        if (dlg)
            SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        Sleep(200);
    }
    if (!g_scenario_done)
        PostThreadMessageW(g_main_thread, WM_QUIT, 0, 0);
    return 0;
}

static BOOL run_download(scenario_t scen, const char *path, const WCHAR *expected_name, BOOL *ret_value)
{
    FN_DoFileDownload pDoFileDownload;
    HMODULE shdocvw = LoadLibraryW(L"shdocvw.dll");
    WCHAR url[256];
    HANDLE driver;
    DWORD wait;

    pDoFileDownload = (FN_DoFileDownload)GetProcAddress(shdocvw, "DoFileDownload");
    if (!pDoFileDownload)
    {
        skip("shdocvw!DoFileDownload is not exported\n");
        return FALSE;
    }

    swprintf(url, ARRAY_SIZE(url), L"http://127.0.0.1:%u/%S", g_port, path);
    lstrcpynW(g_expected_name, expected_name ? expected_name : L"", ARRAY_SIZE(g_expected_name));
    g_scenario_done = 0;
    g_saveas_name[0] = 0;
    g_saw_progress = g_saw_complete = g_saw_error = g_saw_name = g_saw_saveas = g_saw_ask = 0;

    g_main_thread = GetCurrentThreadId();
    driver = CreateThread(NULL, 0, driver_thread, (void *)(INT_PTR)scen, 0, NULL);
    *ret_value = pDoFileDownload(url);
    g_scenario_done = 1;
    wait = WaitForSingleObject(driver, 5000);
    ok(wait == WAIT_OBJECT_0, "driver thread did not end\n");
    CloseHandle(driver);
    return TRUE;
}

static BOOL file_matches(const WCHAR *path, int expected_size)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE *got, *want;
    DWORD read = 0, size;
    BOOL ret = FALSE;

    if (file == INVALID_HANDLE_VALUE)
        return FALSE;

    size = GetFileSize(file, NULL);
    if (size == (DWORD)expected_size)
    {
        got = HeapAlloc(GetProcessHeap(), 0, size);
        want = HeapAlloc(GetProcessHeap(), 0, size);
        if (got && want && ReadFile(file, got, size, &read, NULL) && read == size)
        {
            fill_body(want, 0, size);
            ret = !memcmp(got, want, size);
        }
        HeapFree(GetProcessHeap(), 0, got);
        HeapFree(GetProcessHeap(), 0, want);
    }
    CloseHandle(file);
    return ret;
}

static void test_invalid_arguments(void)
{
    FN_DoFileDownload pDoFileDownload;
    HMODULE shdocvw = LoadLibraryW(L"shdocvw.dll");
    WCHAR path[MAX_PATH];
    BOOL ret;

    pDoFileDownload = (FN_DoFileDownload)GetProcAddress(shdocvw, "DoFileDownload");
    if (!pDoFileDownload)
    {
        skip("shdocvw!DoFileDownload is not exported\n");
        return;
    }

    /* Windows (IE8) answers FALSE too, but shows a Security Alert box; the ReactOS call shows nothing */
    GetModuleFileNameW(NULL, path, ARRAY_SIZE(path));
    ret = pDoFileDownload(path);
    ok_int(ret, FALSE);

    {
        WCHAR url[MAX_PATH + 16], *c;

        swprintf(url, ARRAY_SIZE(url), L"file:///%s", path);
        for (c = url; *c; c++)
            if (*c == '\\')
                *c = '/';
        ret = pDoFileDownload(url);
        ok_int(ret, FALSE);
    }
}

/* A folder for temporary files with a path of exactly Length characters, made of directories one below the other */
static BOOL make_deep_temp(WCHAR *path, size_t Length)
{
    unsigned n = 0;

    GetTempPathW(MAX_PATH, path);
    PathAddBackslashW(path);
    while (wcslen(path) < Length)
    {
        size_t left = Length - wcslen(path), part, i;
        WCHAR *end = path + wcslen(path);

        if (left < 3)
            return FALSE;
        part = left - 1;                /* the backslash is one of them */
        if (part > 40)
        {
            part = 40;
            if (left - 41 < 3)
                part = left - 4;        /* what is left must be able to take another directory */
        }
        end[0] = L's';
        end[1] = L'0' + (n++ % 10);
        for (i = 2; i < part; i++)
            end[i] = L'x';
        end[part] = L'\\';
        end[part + 1] = 0;
        if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
    }
    return wcslen(path) == Length;
}

static void remove_deep_temp(const WCHAR *base, const WCHAR *deep)
{
    WCHAR path[MAX_PATH];
    size_t length;

    wcscpy(path, deep);
    while ((length = wcslen(path)) > wcslen(base) && !_wcsnicmp(path, base, wcslen(base)))
    {
        WCHAR *slash;

        if (path[length - 1] == L'\\')
            path[length - 1] = 0;
        if (!RemoveDirectoryW(path))
            break;
        slash = wcsrchr(path, L'\\');
        if (!slash)
            break;
        slash[1] = 0;
    }
}

static void set_temp_folder(const WCHAR *deep, WCHAR *old_temp, BOOL *have_old_temp, WCHAR *old_tmp, BOOL *have_old_tmp)
{
    *have_old_temp = GetEnvironmentVariableW(L"TEMP", old_temp, MAX_PATH) != 0;
    *have_old_tmp = GetEnvironmentVariableW(L"TMP", old_tmp, MAX_PATH) != 0;
    SetEnvironmentVariableW(L"TEMP", deep);
    SetEnvironmentVariableW(L"TMP", deep);
}

static void restore_temp_folder(const WCHAR *old_temp, BOOL have_old_temp, const WCHAR *old_tmp, BOOL have_old_tmp)
{
    SetEnvironmentVariableW(L"TEMP", have_old_temp ? old_temp : NULL);
    SetEnvironmentVariableW(L"TMP", have_old_tmp ? old_tmp : NULL);
}

static BOOL register_sdlt(void)
{
    static const WCHAR ext_key[] = L"Software\\Classes\\.sdlt";
    static const WCHAR type_key[] = L"Software\\Classes\\sdltfile";
    HKEY key;

    /* an extension of our own that has a description, and a program that does nothing */
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ext_key, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
        return FALSE;
    RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)L"sdltfile", sizeof(L"sdltfile"));
    RegCloseKey(key);
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, type_key, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
    {
        RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)L"SDLT Test File", sizeof(L"SDLT Test File"));
        RegCloseKey(key);
    }
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\sdltfile\\shell\\open\\command", 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
    {
        static const WCHAR command[] = L"cmd.exe /c exit";

        RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)command, sizeof(command));
        RegCloseKey(key);
    }
    return TRUE;
}

static void unregister_sdlt(void)
{
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\sdltfile");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\.sdlt");
}

/*
 * The temporary file of Open and Run is shortened to fit the path; a harmless name can become a device name when
 * it is cut (NULpayload.sdlt to NUL.sdlt), and the download would go to the device and report success. The room for
 * the base name is 245 minus the length of the folder path (the extension is five characters): at 243 the old
 * calculation cut NULpayload.sdlt to the device name, at 242 the new one has to put an underscore in front.
 * When the first name is taken, the next one is "name (1)", which is no device name.
 */
static void test_temp_name_after_shortening(size_t dir_length, const WCHAR *expected_leaf, const WCHAR *device_leaf,
                                            const WCHAR *taken_leaf)
{
    WCHAR old_temp[MAX_PATH], old_tmp[MAX_PATH], base[MAX_PATH], deep[MAX_PATH], expected[MAX_PATH], other[MAX_PATH], taken[MAX_PATH];
    BOOL have_old_temp, have_old_tmp, ret = FALSE;

    GetTempPathW(ARRAY_SIZE(base), base);

    if (!make_deep_temp(deep, dir_length))
    {
        skip("cannot make a folder with a path of %u characters\n", (unsigned)dir_length);
        remove_deep_temp(base, deep);
        return;
    }

    if (!register_sdlt())
    {
        skip("cannot register an extension\n");
        remove_deep_temp(base, deep);
        return;
    }

    set_temp_folder(deep, old_temp, &have_old_temp, old_tmp, &have_old_tmp);

    swprintf(expected, ARRAY_SIZE(expected), L"%s%s", deep, expected_leaf);
    /* the extended form names the file, not the device: the folder has no file of that name */
    swprintf(other, ARRAY_SIZE(other), L"\\\\?\\%s%s", deep, device_leaf);
    DeleteFileW(expected);
    taken[0] = 0;
    if (taken_leaf)
    {
        HANDLE file;

        swprintf(taken, ARRAY_SIZE(taken), L"%s%s", deep, taken_leaf);
        file = CreateFileW(taken, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        ok(file != INVALID_HANDLE_VALUE, "cannot create %S: %lu\n", taken, GetLastError());
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }

    if (run_download(SCEN_OPEN, "pl/NULpayload.sdlt", L"NULpayload.sdlt", &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_ask, "no File Download dialog\n");
        ok(file_matches(expected, BODY_SIZE), "the file was not saved as %S in the temporary folder\n", expected);
        ok(GetFileAttributesW(other) == INVALID_FILE_ATTRIBUTES, "a file %S exists (it would be the device)\n", other);
    }

    DeleteFileW(expected);
    if (taken[0])
        DeleteFileW(taken);
    restore_temp_folder(old_temp, have_old_temp, old_tmp, have_old_tmp);
    unregister_sdlt();
    remove_deep_temp(base, deep);
}

static const WCHAR main_keyW[] = L"Software\\Microsoft\\Internet Explorer\\Main";

/* the folder Save As starts in; the old value (if any) is kept for pop_download_dir */
static BOOL push_download_dir(const WCHAR *dir, WCHAR *old_dir, BOOL *had_old)
{
    DWORD size = MAX_PATH * sizeof(WCHAR), type = 0;
    HKEY key;

    *had_old = FALSE;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, main_keyW, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL))
        return FALSE;
    *had_old = !RegQueryValueExW(key, L"Default Download Directory", NULL, &type, (BYTE *)old_dir, &size) && type == REG_SZ;
    RegSetValueExW(key, L"Default Download Directory", 0, REG_SZ, (const BYTE *)dir, (wcslen(dir) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return TRUE;
}

static void pop_download_dir(const WCHAR *old_dir, BOOL had_old)
{
    HKEY key;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, main_keyW, 0, KEY_WRITE, &key))
        return;
    if (had_old)
        RegSetValueExW(key, L"Default Download Directory", 0, REG_SZ, (const BYTE *)old_dir, (wcslen(old_dir) + 1) * sizeof(WCHAR));
    else
        RegDeleteValueW(key, L"Default Download Directory");
    RegCloseKey(key);
}

/*
 * The name Save As offers is shortened to fit the default folder, and a device name that is left by the cut keeps the
 * extension. The longest folder that can be made has a path of about 248 characters with its closing backslash, so
 * only a few characters remain behind it (room). A name whose extension is room minus the length of the device name
 * long is cut to that device name and the extension: the repair has to cut the base name one character more and keep
 * the extension (COM1payload.eeee is COM1.eeee, a device name, and becomes COM.eeee). The old repair cut the end of
 * the extension to make room for the underscore.
 */
static void test_saveas_name_after_shortening(const char *device, const char *stem)
{
    WCHAR base[MAX_PATH], deep[MAX_PATH], old_dir[MAX_PATH] = L"", expected[MAX_PATH];
    char url[MAX_PATH], ext[MAX_PATH];
    BOOL had_old, ret = FALSE;
    size_t length, room, device_len = strlen(device), i;

    GetTempPathW(ARRAY_SIZE(base), base);
    for (length = 248; length >= 240; length--)
    {
        if (make_deep_temp(deep, length))
            break;
        remove_deep_temp(base, deep);
    }
    if (length < 240)
    {
        skip("cannot make a folder with a path of about 248 characters\n");
        return;
    }

    /* the room the dialog leaves for the name: the closing zero and a separator are two characters */
    room = MAX_PATH - 2 - length;
    ext[0] = '.';
    for (i = 1; i < room - device_len; i++)
        ext[i] = 'e';
    ext[i] = 0;
    /* the name after the repair: the device name without its last character, then the extension */
    swprintf(expected, ARRAY_SIZE(expected), L"%.*S%S", (int)device_len - 1, device, ext);
    snprintf(url, sizeof(url), "pl/%s%s", stem, ext);

    if (!push_download_dir(deep, old_dir, &had_old))
    {
        skip("cannot set the default download folder\n");
        remove_deep_temp(base, deep);
        return;
    }

    DeleteFileW(g_target);
    if (run_download(SCEN_SAVE, url, NULL, &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_saveas, "no Save As dialog\n");
        ok(!wcscmp(g_saveas_name, expected), "Save As offers \"%S\", expected \"%S\"\n", g_saveas_name, expected);
    }

    pop_download_dir(old_dir, had_old);
    DeleteFileW(g_target);
    remove_deep_temp(base, deep);
}

START_TEST(FileDownload)
{
    WCHAR dir[MAX_PATH];

    if (!is_reactos())
    {
        /* Windows' DoFileDownload answers FALSE for a path but then shows a Security Alert box on its own */
        skip("this test drives the dialogs of the ReactOS download flow\n");
        return;
    }

    test_invalid_arguments();

    if (!start_server())
    {
        skip("could not start the local HTTP server\n");
        return;
    }

    GetTempPathW(ARRAY_SIZE(dir), dir);
    PathCombineW(g_target, dir, L"sdl-test-saved.bin");

    {
        BOOL ret = FALSE;

        /* save: file name from the URL, content, completion dialog */
        DeleteFileW(g_target);
        if (run_download(SCEN_SAVE, "ok.bin", L"ok.bin", &ret))
        {
            ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
            ok(g_saw_ask, "no File Download dialog\n");
            ok(g_saw_name, "the dialog does not show the file name\n");
            ok(g_saw_saveas, "no Save As dialog\n");
            ok(g_saw_progress || g_saw_complete, "no progress dialog\n");
            ok(g_saw_complete, "no Download complete dialog\n");
            ok(file_matches(g_target, BODY_SIZE), "the saved file differs from the served data\n");
        }

        /* the name comes from Content-Disposition */
        DeleteFileW(g_target);
        if (run_download(SCEN_SAVE, "cd.bin", L"from-header.dat", &ret))
        {
            ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
            ok(g_saw_name, "the dialog does not show the Content-Disposition name\n");
            ok(file_matches(g_target, BODY_SIZE), "the saved file differs from the served data\n");
        }

        /* a name that fits the folder is kept whole, extension included */
        {
            WCHAR longname[260];

            WCHAR old_dir[MAX_PATH] = L"";
            BOOL had_old, set;

            swprintf(longname, ARRAY_SIZE(longname), L"%S.%S", LONG_BASE, LONG_EXT);
            /* the folder Save As starts in is short, so that the whole name of 211 characters fits behind it */
            set = push_download_dir(dir, old_dir, &had_old);
            DeleteFileW(g_target);
            if (run_download(SCEN_SAVE, "longext.bin", longname, &ret))
            {
                ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
                ok(g_saw_name, "the dialog does not show the whole name of 211 characters\n");
                ok(!set || !wcscmp(g_saveas_name, longname), "Save As offers \"%S\", not the whole name\n", g_saveas_name);
            }
            if (set)
                pop_download_dir(old_dir, had_old);
        }

        /* cancel in the first dialog: nothing is written */
        DeleteFileW(g_target);
        if (run_download(SCEN_CANCEL_ASK, "ok.bin", NULL, &ret))
        {
            ok(ret == FALSE, "DoFileDownload returned %d after Cancel\n", ret);
            ok(g_saw_ask, "no File Download dialog\n");
            ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "a file was written\n");
        }

        /* cancel in Save As cancels the download */
        DeleteFileW(g_target);
        if (run_download(SCEN_CANCEL_SAVEAS, "ok.bin", NULL, &ret))
        {
            ok(ret == FALSE, "DoFileDownload returned %d after Cancel in Save As\n", ret);
            ok(g_saw_saveas, "no Save As dialog\n");
            ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "a file was written\n");
        }

        /* cancel while the data is coming: the partial file is removed */
        DeleteFileW(g_target);
        if (run_download(SCEN_CANCEL_RUN, "long.bin", NULL, &ret))
        {
            ok(ret == FALSE, "DoFileDownload returned %d after Cancel in the progress dialog\n", ret);
            ok(g_saw_progress, "no progress dialog\n");
            ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "the partial file was left\n");
        }

        /* HTTP error */
        DeleteFileW(g_target);
        if (run_download(SCEN_ERROR_BOX, "missing.bin", NULL, &ret))
        {
            ok(ret == FALSE, "DoFileDownload returned %d for an HTTP error\n", ret);
            ok(g_saw_error, "no error message\n");
            ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "a file was written\n");
        }

        /* the connection closes before the announced length: an error, no file */
        DeleteFileW(g_target);
        if (run_download(SCEN_SAVE, "short.bin", NULL, &ret))
        {
            ok(ret == FALSE, "DoFileDownload returned %d for a truncated download\n", ret);
            ok(g_saw_error || !g_saw_complete, "a truncated download was reported as complete\n");
            ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "the truncated file was kept\n");
        }
    }

    test_temp_name_after_shortening(242, L"_NUL.sdlt", L"NUL.sdlt", NULL);
    test_temp_name_after_shortening(243, L"NU.sdlt", L"NUL.sdlt", NULL);
    test_temp_name_after_shortening(242, L"NUL (1).sdlt", L"NUL.sdlt", L"_NUL.sdlt");
    test_saveas_name_after_shortening("COM1", "COM1payload");
    test_saveas_name_after_shortening("NUL", "NULpayload");

    DeleteFileW(g_target);
    stop_server();
}
