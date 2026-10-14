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

typedef enum {
    SCEN_SAVE,          /* Save, name the file, wait for completion, Close */
    SCEN_CANCEL_ASK,    /* Cancel in the File Download dialog */
    SCEN_CANCEL_SAVEAS, /* Save, then Cancel in Save As */
    SCEN_CANCEL_RUN,    /* Save, then Cancel in the progress dialog */
    SCEN_ERROR_BOX      /* an error box has to appear; click OK */
} scenario_t;

static SOCKET g_listener = INVALID_SOCKET;
static USHORT g_port;
static volatile LONG g_stop_server;
static HANDLE g_server_thread;

static WCHAR g_target[MAX_PATH];
static WCHAR g_expected_name[MAX_PATH];
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
    else if (!strncmp(req, "GET /ok.bin", 11) || !strncmp(req, "GET /cd.bin", 11))
    {
        int sent = 0;
        BOOL cd = !strncmp(req, "GET /cd.bin", 11);

        n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n%s"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n",
                     cd ? "Content-Disposition: attachment; filename=\"from-header.dat\"\r\n" : "", BODY_SIZE);
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
                SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)g_target);
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

    DeleteFileW(g_target);
    stop_server();
}
