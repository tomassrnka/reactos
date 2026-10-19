/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for shdocvw!DoFileDownload: the File Download dialog, the progress dialog,
 *              the saved file, cancelling and errors, against a local HTTP server
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#define COBJMACROS
#define CONST_VTABLE
#include <winsock2.h>
#include <apitest.h>
#include <shlwapi.h>
#include <ole2.h>
#include <urlmon.h>

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
    SCEN_OPEN,          /* Open: the file goes to the temporary folder and is started */
    SCEN_QUIT_SAVING,   /* Save, then a quit message arrives while the data comes */
    SCEN_QUIT_ASKING,   /* a quit message arrives while the File Download dialog is open */
    SCEN_FAIL_ASKING,   /* the download fails while the File Download dialog is open */
    SCEN_FAIL_SAVEAS,   /* Save; the download fails while Save As is open; then OK in Save As */
    SCEN_OVERWRITE_NO,  /* Save onto an existing file, answer No to the question, then Cancel */
    SCEN_OVERWRITE_YES, /* Save onto an existing file, answer Yes */
    SCEN_SAVE_DEVICE,   /* Save, and type the name of a device in Save As */
    SCEN_QUIT_DONE,     /* Save; a quit message arrives while the Download complete dialog is open */
    SCEN_AUTOCLOSE      /* Save, check "Close this dialog box when download completes" while the data comes */
} scenario_t;

static SOCKET g_listener = INVALID_SOCKET;
static USHORT g_port;
static volatile LONG g_stop_server;
static HANDLE g_server_thread;

static WCHAR g_target[MAX_PATH];
static WCHAR g_device[MAX_PATH];
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
static volatile LONG g_saw_prompt;
static volatile LONG g_quit_sent;

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
    else if (!strncmp(req, "GET /slow.bin", 13))
    {
        int sent = 0;

        n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", BODY_SIZE);
        send_all(c, head, n);
        while (sent < BODY_SIZE)
        {
            int len = min(16384, BODY_SIZE - sent);

            fill_body(chunk, sent, len);
            if (send_all(c, (char *)chunk, len) < 0)
                break;
            sent += len;
            Sleep(100);
        }
    }
    else if (!strncmp(req, "GET /ok.bin", 11) || !strncmp(req, "GET /cd.bin", 11) || !strncmp(req, "GET /longext.bin", 16) ||
             !strncmp(req, "GET /cd-evil.bin", 16) || !strncmp(req, "GET /cd-nul.bin", 15) || !strncmp(req, "GET /pl/", 8))
    {
        int sent = 0;
        const char *cd = "";
        char longname[320];

        if (!strncmp(req, "GET /cd.bin", 11))
            cd = "Content-Disposition: attachment; filename=\"from-header.dat\"\r\n";
        else if (!strncmp(req, "GET /cd-evil.bin", 16))
            cd = "Content-Disposition: attachment; filename=\"..\\..\\evil name.txt\"\r\n";
        else if (!strncmp(req, "GET /cd-nul.bin", 15))
            cd = "Content-Disposition: attachment; filename=\"NUL.txt\"\r\n";
        else if (!strncmp(req, "GET /longext.bin", 16))
        {
            /* a name of 100 characters, a dot and an extension of 110 characters: 211 in all */
            snprintf(longname, sizeof(longname), "Content-Disposition: attachment; filename=\"%s.%s\"\r\n", LONG_BASE, LONG_EXT);
            cd = longname;
        }
        n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n%s"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", cd, BODY_SIZE);
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

typedef struct {
    const WCHAR *text;
    HWND found;
} find_prompt_t;

static BOOL CALLBACK find_prompt_proc(HWND hwnd, LPARAM lparam)
{
    find_prompt_t *f = (find_prompt_t *)lparam;
    WCHAR cls[32];
    DWORD pid;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
        return TRUE;
    if (!GetClassNameW(hwnd, cls, ARRAY_SIZE(cls)) || wcscmp(cls, L"#32770"))
        return TRUE;
    if (find_child_text(hwnd, f->text, TRUE))
    {
        f->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* a dialog of this process with a text that contains the given text */
static HWND find_prompt(const WCHAR *text)
{
    find_prompt_t f = { text, NULL };

    EnumWindows(find_prompt_proc, (LPARAM)&f);
    return f.found;
}

/* The engine's hidden window takes the message that a server error sends. The test uses it to end a download from the
   outside while a dialog is open: no server of ours makes this ReactOS' network stack fail at that moment. */
#define WM_DL_FAIL_MESSAGE (WM_USER + 0x503)

static void inject_failure(void)
{
    HWND hwnd = FindWindowExW(HWND_MESSAGE, NULL, L"IEDownloadWnd", NULL);

    ok(hwnd != NULL, "no window of the download engine\n");
    if (hwnd)
        PostMessageW(hwnd, WM_DL_FAIL_MESSAGE, 500, 0);
}

static DWORD WINAPI driver_thread(void *arg)
{
    scenario_t scen = (scenario_t)(INT_PTR)arg;
    DWORD start = GetTickCount(), saveas_at = 0, ok_at = 0;
    BOOL saved = FALSE, cancelled_progress = FALSE, prompt_answered = FALSE, checked = FALSE;
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

        dlg = find_prompt(L"already exists");
        if (dlg)
        {
            g_saw_prompt = 1;
            if (scen == SCEN_OVERWRITE_NO && !prompt_answered)
            {
                prompt_answered = TRUE;
                click_button(dlg, L"No");
            }
            else
            {
                click_button(dlg, L"Yes");
            }
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
            if (scen == SCEN_QUIT_ASKING)
            {
                if (!g_quit_sent)
                {
                    g_quit_sent = 1;
                    PostThreadMessageW(g_main_thread, WM_QUIT, 7, 0);
                }
                continue;
            }
            if (scen == SCEN_FAIL_ASKING)
            {
                /* the engine closes the question itself and shows the error */
                if (!g_quit_sent)
                {
                    g_quit_sent = 1;
                    inject_failure();
                }
                continue;
            }
            if (scen == SCEN_CANCEL_ASK)
                click_button(dlg, L"Cancel");
            else if (scen == SCEN_OPEN)
                click_button(dlg, L"Open");
            else
                click_button(dlg, L"Save");
            saved = TRUE;
            continue;
        }

        dlg = find_dialog(L"Save As", FALSE);
        if (dlg)
        {
            HWND edit = NULL;

            g_saw_saveas = 1;
            if (!saveas_at)
                saveas_at = GetTickCount();

            if (scen == SCEN_CANCEL_SAVEAS || (scen == SCEN_OVERWRITE_NO && prompt_answered))
            {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                continue;
            }
            /* the download fails while the dialog is open; the box about it is answered by the loop above */
            if (scen == SCEN_FAIL_SAVEAS)
            {
                if (!g_quit_sent)
                {
                    g_quit_sent = 1;
                    inject_failure();
                }
                if (GetTickCount() - saveas_at < 2000)
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
                SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)(scen == SCEN_SAVE_DEVICE ? g_device : g_target));
            }
            /* posted, once: the overwrite question opens inside the dialog's handler, and a sent message would keep
               the driver from answering it */
            if (!ok_at || GetTickCount() - ok_at > 3000)
            {
                ok_at = GetTickCount();
                PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            }
            continue;
        }

        dlg = find_dialog(L"Download complete", FALSE);
        if (dlg)
        {
            g_saw_complete = 1;
            if (scen == SCEN_QUIT_DONE)
            {
                if (!g_quit_sent)
                {
                    g_quit_sent = 1;
                    PostThreadMessageW(g_main_thread, WM_QUIT, 9, 0);
                }
                continue;
            }
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
            if (scen == SCEN_AUTOCLOSE && !checked)
            {
                HWND box = find_child_text(dlg, L"Close this dialog box when download completes", FALSE);

                if (box)
                {
                    checked = TRUE;
                    SendMessageW(box, BM_CLICK, 0, 0);
                }
            }
            if (scen == SCEN_QUIT_SAVING && !g_quit_sent)
            {
                Sleep(500);
                g_quit_sent = 1;
                PostThreadMessageW(g_main_thread, WM_QUIT, 7, 0);
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

static BOOL run_url(scenario_t scen, const WCHAR *url, const WCHAR *expected_name, BOOL *ret_value)
{
    FN_DoFileDownload pDoFileDownload;
    HMODULE shdocvw = LoadLibraryW(L"shdocvw.dll");
    HANDLE driver;
    DWORD wait;

    pDoFileDownload = (FN_DoFileDownload)GetProcAddress(shdocvw, "DoFileDownload");
    if (!pDoFileDownload)
    {
        skip("shdocvw!DoFileDownload is not exported\n");
        return FALSE;
    }

    trace("scenario %d: %S\n", (int)scen, url);
    lstrcpynW(g_expected_name, expected_name ? expected_name : L"", ARRAY_SIZE(g_expected_name));
    g_scenario_done = 0;
    g_saveas_name[0] = 0;
    g_saw_progress = g_saw_complete = g_saw_error = g_saw_name = g_saw_saveas = g_saw_ask = g_saw_prompt = g_quit_sent = 0;

    g_main_thread = GetCurrentThreadId();
    driver = CreateThread(NULL, 0, driver_thread, (void *)(INT_PTR)scen, 0, NULL);
    *ret_value = pDoFileDownload((LPWSTR)url);
    g_scenario_done = 1;
    wait = WaitForSingleObject(driver, 5000);
    ok(wait == WAIT_OBJECT_0, "driver thread did not end\n");
    CloseHandle(driver);
    return TRUE;
}

static BOOL run_download(scenario_t scen, const char *path, const WCHAR *expected_name, BOOL *ret_value)
{
    WCHAR url[256];

    swprintf(url, ARRAY_SIZE(url), L"http://127.0.0.1:%u/%S", g_port, path);
    return run_url(scen, url, expected_name, ret_value);
}

/* the quit message of a scenario has to be given back to the caller */
static BOOL take_quit(WPARAM *code)
{
    MSG msg;

    if (!PeekMessageW(&msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE))
        return FALSE;
    *code = msg.wParam;
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

static BOOL file_is_text(const WCHAR *path, const char *text)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    char buf[16] = "";
    DWORD read = 0;
    BOOL ret;

    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(file, buf, sizeof(buf) - 1, &read, NULL);
    CloseHandle(file);
    ret = read == strlen(text) && !memcmp(buf, text, read);
    return ret;
}

static void write_text(const WCHAR *path, const char *text)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written;

    ok(file != INVALID_HANDLE_VALUE, "cannot create %S: %lu\n", path, GetLastError());
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteFile(file, text, strlen(text), &written, NULL);
        CloseHandle(file);
    }
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

    ret = pDoFileDownload(NULL);
    ok_int(ret, FALSE);
}

/* A folder with a path of exactly Length characters (with the closing backslash), made of directories one below the other */
static BOOL make_deep_folder(const WCHAR *base, WCHAR *path, size_t Length)
{
    unsigned n = 0;

    wcscpy(path, base);
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

static void remove_deep_folder(const WCHAR *base, const WCHAR *deep)
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

static const WCHAR main_keyW[] = L"Software\\Microsoft\\Internet Explorer\\Main";

typedef struct {
    BOOL had;
    DWORD type;
    DWORD size;
    BYTE *data;
} saved_value_t;

/* the folder Save As starts in; the old value (if any) is kept, with its type and bytes, for pop_download_dir */
static BOOL push_download_dir(const WCHAR *dir, saved_value_t *saved)
{
    HKEY key;
    LONG ret;

    memset(saved, 0, sizeof(*saved));
    if (RegCreateKeyExW(HKEY_CURRENT_USER, main_keyW, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL))
        return FALSE;

    ret = RegQueryValueExW(key, L"Default Download Directory", NULL, &saved->type, NULL, &saved->size);
    if (ret == ERROR_SUCCESS)
    {
        saved->data = HeapAlloc(GetProcessHeap(), 0, saved->size + 1);
        if (!saved->data || RegQueryValueExW(key, L"Default Download Directory", NULL, &saved->type, saved->data, &saved->size))
        {
            /* a value that cannot be kept is not touched */
            HeapFree(GetProcessHeap(), 0, saved->data);
            saved->data = NULL;
            RegCloseKey(key);
            return FALSE;
        }
        saved->had = TRUE;
    }
    else if (ret != ERROR_FILE_NOT_FOUND)
    {
        RegCloseKey(key);
        return FALSE;
    }

    RegSetValueExW(key, L"Default Download Directory", 0, REG_SZ, (const BYTE *)dir, (wcslen(dir) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return TRUE;
}

static void pop_download_dir(saved_value_t *saved)
{
    HKEY key;

    if (!RegOpenKeyExW(HKEY_CURRENT_USER, main_keyW, 0, KEY_WRITE, &key))
    {
        if (saved->had)
            RegSetValueExW(key, L"Default Download Directory", 0, saved->type, saved->data, saved->size);
        else
            RegDeleteValueW(key, L"Default Download Directory");
        RegCloseKey(key);
    }
    HeapFree(GetProcessHeap(), 0, saved->data);
    saved->data = NULL;
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

/* Open: a device name from the server becomes a file name in the temporary folder, not the device */
static void test_open_device_name(void)
{
    WCHAR temp[MAX_PATH], expected[MAX_PATH], device[MAX_PATH];
    BOOL ret = FALSE;

    if (!register_sdlt())
    {
        skip("cannot register an extension\n");
        return;
    }

    GetTempPathW(ARRAY_SIZE(temp), temp);
    swprintf(expected, ARRAY_SIZE(expected), L"%sNU_.sdlt", temp);
    swprintf(device, ARRAY_SIZE(device), L"\\\\?\\%sNUL.sdlt", temp);
    DeleteFileW(expected);

    if (run_download(SCEN_OPEN, "pl/NUL.sdlt", L"NUL.sdlt", &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_ask, "no File Download dialog\n");
        ok(file_matches(expected, BODY_SIZE), "the file was not saved as %S\n", expected);
        ok(GetFileAttributesW(device) == INVALID_FILE_ATTRIBUTES, "a file %S exists (it would be the device)\n", device);
    }

    DeleteFileW(expected);
    unregister_sdlt();
}

/*
 * The name Save As offers is shortened to fit the default folder and a device name that is left by the cut is made a
 * file name that keeps the extension. The folder is as deep as one can be made; the room behind it is only a few
 * characters, and the extension takes all of it but the length of the device name.
 */
static void test_saveas_name_after_shortening(const char *device, const char *stem)
{
    WCHAR base[MAX_PATH], deep[MAX_PATH], expected[MAX_PATH];
    saved_value_t saved;
    char url[MAX_PATH], ext[MAX_PATH];
    BOOL ret = FALSE;
    size_t length, room, device_len = strlen(device), i;

    GetTempPathW(ARRAY_SIZE(base), base);
    for (length = 248; length >= 240; length--)
    {
        if (make_deep_folder(base, deep, length))
            break;
        remove_deep_folder(base, deep);
    }
    if (length < 240)
    {
        skip("cannot make a folder with a path of about 248 characters\n");
        return;
    }

    room = MAX_PATH - 1 - length;
    ext[0] = '.';
    for (i = 1; i < room - device_len; i++)
        ext[i] = 'e';
    ext[i] = 0;
    swprintf(expected, ARRAY_SIZE(expected), L"%.*S_%S", (int)device_len - 1, device, ext);
    snprintf(url, sizeof(url), "pl/%s%s", stem, ext);

    if (!push_download_dir(deep, &saved))
    {
        skip("cannot set the default download folder\n");
        remove_deep_folder(base, deep);
        return;
    }

    DeleteFileW(g_target);
    if (run_download(SCEN_SAVE, url, NULL, &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_saveas, "no Save As dialog\n");
        ok(!wcscmp(g_saveas_name, expected), "Save As offers \"%S\", expected \"%S\"\n", g_saveas_name, expected);
    }

    pop_download_dir(&saved);
    DeleteFileW(g_target);
    remove_deep_folder(base, deep);
}

/* The questions of Save As: an existing file is replaced only after Yes; No and Cancel leave it as it is */
static void test_overwrite(void)
{
    BOOL ret = FALSE;

    write_text(g_target, "OLD");
    if (run_download(SCEN_OVERWRITE_NO, "ok.bin", NULL, &ret))
    {
        ok(ret == FALSE, "DoFileDownload returned %d after No and Cancel\n", ret);
        ok(g_saw_prompt, "no question about the existing file\n");
        ok(file_is_text(g_target, "OLD"), "the existing file was changed\n");
    }

    write_text(g_target, "OLD");
    if (run_download(SCEN_OVERWRITE_YES, "ok.bin", NULL, &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d after Yes\n", ret);
        ok(g_saw_prompt, "no question about the existing file\n");
        ok(file_matches(g_target, BODY_SIZE), "the existing file was not replaced\n");
    }
    DeleteFileW(g_target);
}

/*
 * The download fails while a dialog is open. The engine ends the download: the error is shown, the question closes,
 * a file that is named like the suggested name stays untouched, and what the user chooses after the failure creates
 * nothing.
 */
static void test_failure_while_dialogs_are_open(const WCHAR *dir)
{
    WCHAR same_name[MAX_PATH];
    saved_value_t saved;
    BOOL ret = FALSE, set;

    swprintf(same_name, ARRAY_SIZE(same_name), L"%sok.bin", dir);
    set = push_download_dir(dir, &saved);

    write_text(same_name, "OLD");
    DeleteFileW(g_target);
    if (run_download(SCEN_FAIL_ASKING, "ok.bin", NULL, &ret))
    {
        ok(ret == FALSE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_error, "no error message\n");
        ok(!g_saw_saveas, "Save As was opened for a failed download\n");
        ok(file_is_text(same_name, "OLD"), "the file with the suggested name changed\n");
    }

    write_text(same_name, "OLD");
    DeleteFileW(g_target);
    if (run_download(SCEN_FAIL_SAVEAS, "ok.bin", NULL, &ret))
    {
        ok(ret == FALSE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_error, "no error message\n");
        ok(file_is_text(same_name, "OLD"), "the file with the suggested name changed\n");
        ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "a file was created after the failure\n");
    }

    DeleteFileW(same_name);
    if (set)
        pop_download_dir(&saved);
}

/* the name of a device typed in Save As is no file: nothing is created and the user is told */
static void test_save_device(void)
{
    WCHAR dir[MAX_PATH], extended[MAX_PATH + 8];
    BOOL ret = FALSE;

    GetTempPathW(ARRAY_SIZE(dir), dir);
    swprintf(g_device, ARRAY_SIZE(g_device), L"%sNUL.txt", dir);
    swprintf(extended, ARRAY_SIZE(extended), L"\\\\?\\%s", g_device);

    if (run_download(SCEN_SAVE_DEVICE, "ok.bin", NULL, &ret))
    {
        ok(ret == FALSE, "DoFileDownload returned %d for a device\n", ret);
        ok(g_saw_saveas, "no Save As dialog\n");
        ok(g_saw_error, "no error message\n");
        ok(GetFileAttributesW(extended) == INVALID_FILE_ATTRIBUTES, "a file %S was created\n", extended);
    }
}

/*
 * Urlmon calls OnStopBinding when the last data is there, and a protocol may deliver its result later. A protocol that
 * does so (it replaces http for this test) is still working when the user cancels; DoFileDownload may not return before
 * urlmon has let go of the binding.
 */
typedef struct {
    IInternetProtocol IInternetProtocol_iface;
    LONG ref;
    IInternetProtocolSink *sink;
    DWORD sent;
} DelayedProtocol;

static volatile LONG g_result_done;

static DelayedProtocol *impl_from_IInternetProtocol(IInternetProtocol *iface)
{
    return CONTAINING_RECORD(iface, DelayedProtocol, IInternetProtocol_iface);
}

static HRESULT WINAPI DelayedProtocol_QueryInterface(IInternetProtocol *iface, REFIID riid, void **ppv)
{
    DelayedProtocol *This = impl_from_IInternetProtocol(iface);

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

static ULONG WINAPI DelayedProtocol_AddRef(IInternetProtocol *iface)
{
    return InterlockedIncrement(&impl_from_IInternetProtocol(iface)->ref);
}

static ULONG WINAPI DelayedProtocol_Release(IInternetProtocol *iface)
{
    DelayedProtocol *This = impl_from_IInternetProtocol(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    if (!ref)
    {
        if (This->sink)
            IInternetProtocolSink_Release(This->sink);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

typedef struct {
    IInternetProtocolSink *sink;
} delayed_result_t;

static DWORD WINAPI delayed_result_thread(void *arg)
{
    delayed_result_t *result = arg;

    Sleep(1500);
    IInternetProtocolSink_ReportResult(result->sink, S_OK, 0, NULL);
    InterlockedExchange(&g_result_done, 1);
    IInternetProtocolSink_Release(result->sink);
    HeapFree(GetProcessHeap(), 0, result);
    return 0;
}

static HRESULT WINAPI DelayedProtocol_Start(IInternetProtocol *iface, LPCWSTR url, IInternetProtocolSink *sink,
                                            IInternetBindInfo *bindinfo, DWORD flags, HANDLE_PTR reserved)
{
    DelayedProtocol *This = impl_from_IInternetProtocol(iface);
    delayed_result_t *result;
    HANDLE thread;

    This->sink = sink;
    IInternetProtocolSink_AddRef(sink);

    IInternetProtocolSink_ReportProgress(sink, BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE, L"application/octet-stream");
    IInternetProtocolSink_ReportData(sink, BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION | BSCF_DATAFULLYAVAILABLE,
                                     8, 8);

    /* the result comes later, from another thread that holds the sink itself */
    result = HeapAlloc(GetProcessHeap(), 0, sizeof(*result));
    if (!result)
        return S_OK;
    result->sink = sink;
    IInternetProtocolSink_AddRef(sink);
    thread = CreateThread(NULL, 0, delayed_result_thread, result, 0, NULL);
    if (thread)
    {
        CloseHandle(thread);
    }
    else
    {
        IInternetProtocolSink_Release(sink);
        HeapFree(GetProcessHeap(), 0, result);
    }
    return S_OK;
}

static HRESULT WINAPI DelayedProtocol_Continue(IInternetProtocol *iface, PROTOCOLDATA *data)
{
    return S_OK;
}

static HRESULT WINAPI DelayedProtocol_Abort(IInternetProtocol *iface, HRESULT reason, DWORD options)
{
    return S_OK;
}

static HRESULT WINAPI DelayedProtocol_Terminate(IInternetProtocol *iface, DWORD options)
{
    DelayedProtocol *This = impl_from_IInternetProtocol(iface);

    if (This->sink)
    {
        IInternetProtocolSink_Release(This->sink);
        This->sink = NULL;
    }
    return S_OK;
}

static HRESULT WINAPI DelayedProtocol_Suspend(IInternetProtocol *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI DelayedProtocol_Resume(IInternetProtocol *iface)
{
    return E_NOTIMPL;
}

/* a Read that runs messages and, on the way, gives the engine the pump message that its own queue would bring later, or
   presses Cancel in the progress dialog */
static volatile LONG g_read_pumps;
static volatile LONG g_read_cancels;

static void run_messages(DWORD ms)
{
    DWORD start = GetTickCount();
    MSG msg;

    while ((DWORD)(GetTickCount() - start) < ms)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

/* a protocol that gives data for g_flood_ms and runs the engine's pump message inside every Read; at the first Read it
   posts a message to the sentinel window, which the engine's thread may dispatch only when it is back in its loop */
static volatile LONG g_flood_ms;
static DWORD g_flood_start, g_flood_until, g_sentinel_tick;
static HWND g_sentinel;

static LRESULT CALLBACK sentinel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_APP)
    {
        g_sentinel_tick = GetTickCount();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HRESULT flood_read(void *pv, ULONG cb, ULONG *read)
{
    HWND hwnd = FindWindowExW(HWND_MESSAGE, NULL, L"IEDownloadWnd", NULL);
    ULONG n = min(cb, 8);
    MSG msg;

    if (!g_flood_start)
    {
        g_flood_start = GetTickCount();
        g_flood_until = g_flood_start + g_flood_ms;
        PostMessageW(g_sentinel, WM_APP, 0, 0);
    }
    if ((int)(g_flood_until - GetTickCount()) <= 0)
    {
        *read = 0;
        return S_FALSE;
    }
    if (hwnd)
    {
        PostMessageW(hwnd, WM_USER + 0x501, 0, 0);
        while (PeekMessageW(&msg, hwnd, WM_USER + 0x501, WM_USER + 0x501, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    memset(pv, 'x', n);
    *read = n;
    return S_OK;
}

static HRESULT WINAPI DelayedProtocol_Read(IInternetProtocol *iface, void *pv, ULONG cb, ULONG *read)
{
    DelayedProtocol *This = impl_from_IInternetProtocol(iface);
    DWORD n;

    if (g_flood_ms)
        return flood_read(pv, cb, read);
    n = min(cb, 8 - This->sent);

    memset(pv, 'x', n);
    This->sent += n;
    *read = n;

    /* once, at the first Read */
    if (InterlockedExchange(&g_read_pumps, 0))
    {
        HWND hwnd = FindWindowExW(HWND_MESSAGE, NULL, L"IEDownloadWnd", NULL);

        if (hwnd)
            PostMessageW(hwnd, WM_USER + 0x501, 0, 0);
        run_messages(200);
    }
    if (InterlockedExchange(&g_read_cancels, 0))
    {
        HWND dlg = find_dialog(L"delayed.test", TRUE);

        if (dlg)
            PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        run_messages(200);
    }
    return This->sent == 8 ? S_FALSE : S_OK;
}

static HRESULT WINAPI DelayedProtocol_Seek(IInternetProtocol *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *pos)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI DelayedProtocol_LockRequest(IInternetProtocol *iface, DWORD options)
{
    return S_OK;
}

static HRESULT WINAPI DelayedProtocol_UnlockRequest(IInternetProtocol *iface)
{
    return S_OK;
}

static const IInternetProtocolVtbl DelayedProtocolVtbl =
{
    DelayedProtocol_QueryInterface,
    DelayedProtocol_AddRef,
    DelayedProtocol_Release,
    DelayedProtocol_Start,
    DelayedProtocol_Continue,
    DelayedProtocol_Abort,
    DelayedProtocol_Terminate,
    DelayedProtocol_Suspend,
    DelayedProtocol_Resume,
    DelayedProtocol_Read,
    DelayedProtocol_Seek,
    DelayedProtocol_LockRequest,
    DelayedProtocol_UnlockRequest
};

static HRESULT WINAPI DelayedFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI DelayedFactory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI DelayedFactory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI DelayedFactory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    DelayedProtocol *protocol;
    HRESULT hr;

    if (outer)
        return CLASS_E_NOAGGREGATION;
    protocol = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*protocol));
    if (!protocol)
        return E_OUTOFMEMORY;
    protocol->IInternetProtocol_iface.lpVtbl = &DelayedProtocolVtbl;
    protocol->ref = 1;

    hr = IInternetProtocol_QueryInterface(&protocol->IInternetProtocol_iface, riid, ppv);
    IInternetProtocol_Release(&protocol->IInternetProtocol_iface);
    return hr;
}

static HRESULT WINAPI DelayedFactory_LockServer(IClassFactory *iface, BOOL lock)
{
    return S_OK;
}

static const IClassFactoryVtbl DelayedFactoryVtbl =
{
    DelayedFactory_QueryInterface,
    DelayedFactory_AddRef,
    DelayedFactory_Release,
    DelayedFactory_CreateInstance,
    DelayedFactory_LockServer
};

static IClassFactory g_delayed_factory = { &DelayedFactoryVtbl };

static IInternetSession *register_delayed_protocol(void)
{
    IInternetSession *session;
    HRESULT hr;

    hr = CoInternetGetSession(0, &session, 0);
    ok(hr == S_OK, "CoInternetGetSession returned %08lx\n", hr);
    if (FAILED(hr))
        return NULL;
    hr = IInternetSession_RegisterNameSpace(session, &g_delayed_factory, &CLSID_NULL, L"http", 0, NULL, 0);
    ok(hr == S_OK, "RegisterNameSpace returned %08lx\n", hr);
    if (FAILED(hr))
    {
        IInternetSession_Release(session);
        return NULL;
    }
    return session;
}

static void unregister_delayed_protocol(IInternetSession *session)
{
    IInternetSession_UnregisterNameSpace(session, &g_delayed_factory, L"http");
    IInternetSession_Release(session);
}

static void test_teardown_waits_for_urlmon(void)
{
    IInternetSession *session;
    BOOL ret = FALSE;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    session = register_delayed_protocol();
    if (session)
    {
        g_result_done = 0;
        if (run_url(SCEN_CANCEL_ASK, L"http://delayed.test/x.bin", NULL, &ret))
        {
            ok(ret == FALSE, "DoFileDownload returned %d after Cancel\n", ret);
            ok(g_saw_ask, "no File Download dialog\n");
            ok(g_result_done, "DoFileDownload returned before urlmon had the result of the protocol\n");
        }
        unregister_delayed_protocol(session);
    }

    CoUninitialize();
}

/*
 * A Read that runs messages lets a second pump message of the engine in. The end of the binding has been reported
 * already, so the nested pump would find the end of the data before the outer Read has written its bytes, and report
 * a truncated download.
 */
static void test_pump_inside_read(void)
{
    IInternetSession *session;
    BOOL ret = FALSE;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    session = register_delayed_protocol();
    if (session)
    {
        g_read_pumps = 1;
        g_result_done = 0;
        DeleteFileW(g_target);
        if (run_url(SCEN_SAVE, L"http://delayed.test/x.bin", NULL, &ret))
        {
            ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
            ok(!g_saw_error, "an error was shown\n");
            ok(g_saw_complete, "no Download complete dialog\n");
            ok(g_result_done, "DoFileDownload returned before urlmon had the result of the protocol\n");
            ok(file_is_text(g_target, "xxxxxxxx"), "the saved file is not the served data\n");
        }
        g_read_pumps = 0;
        unregister_delayed_protocol(session);
        DeleteFileW(g_target);
    }

    CoUninitialize();
}

/*
 * Cancel arrives while a Read runs messages. The bytes the Read returned are not written to the closed file, and no
 * error about it is shown.
 */
static void test_cancel_inside_read(void)
{
    IInternetSession *session;
    BOOL ret = FALSE;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    session = register_delayed_protocol();
    if (session)
    {
        g_read_cancels = 1;
        g_result_done = 0;
        DeleteFileW(g_target);
        if (run_url(SCEN_SAVE, L"http://delayed.test/x.bin", NULL, &ret))
        {
            ok(ret == FALSE, "DoFileDownload returned %d after Cancel\n", ret);
            ok(g_saw_saveas, "no Save As dialog\n");
            ok(!g_saw_error, "an error was shown after Cancel\n");
            ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "the partial file was left\n");
        }
        g_read_cancels = 0;
        unregister_delayed_protocol(session);
    }

    CoUninitialize();
}

/*
 * A protocol that runs a pump message of the engine inside every Read must not keep the engine's thread: the call comes
 * back to the message loop after one slice and the next slice is posted. A message that was posted at the first Read
 * is dispatched while the data still flows.
 */
static void test_endless_pump(void)
{
    static const WCHAR classW[] = L"SdlSentinel";
    IInternetSession *session;
    WNDCLASSW wc = { 0 };
    BOOL ret = FALSE;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    wc.lpfnWndProc = sentinel_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = classW;
    RegisterClassW(&wc);
    g_sentinel = CreateWindowExW(0, classW, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    ok(g_sentinel != NULL, "no sentinel window\n");

    session = register_delayed_protocol();
    if (session && g_sentinel)
    {
        g_flood_start = g_sentinel_tick = 0;
        g_flood_ms = 2000;
        DeleteFileW(g_target);
        if (run_url(SCEN_SAVE, L"http://delayed.test/x.bin", NULL, &ret))
        {
            ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
            ok(g_flood_start != 0, "the protocol was not read\n");
            ok(g_sentinel_tick != 0, "the message was not dispatched\n");
            ok(g_sentinel_tick - g_flood_start < 800, "the message queue was kept for %lu ms\n", g_sentinel_tick - g_flood_start);
        }
        g_flood_ms = 0;
        unregister_delayed_protocol(session);
        DeleteFileW(g_target);
    }
    else if (session)
    {
        unregister_delayed_protocol(session);
    }

    if (g_sentinel)
        DestroyWindow(g_sentinel);
    UnregisterClassW(classW, wc.hInstance);
    CoUninitialize();
}

/* the box "Close this dialog box when download completes" closes the dialog without a click, and the call returns */
static void test_autoclose(void)
{
    BOOL ret = FALSE;

    DeleteFileW(g_target);
    if (run_download(SCEN_AUTOCLOSE, "slow.bin", NULL, &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_progress, "no progress dialog\n");
        ok(!g_saw_complete, "the Download complete dialog stayed open\n");
        ok(file_matches(g_target, BODY_SIZE), "the saved file differs from the served data\n");
    }
    DeleteFileW(g_target);
}

/* an extension of 200 characters is longer than every buffer of the dialog texts */
static void test_long_extension(void)
{
    char url[300];
    BOOL ret = FALSE;
    int i;

    strcpy(url, "pl/a.");
    for (i = 0; i < 200; i++)
        url[5 + i] = 'e';
    url[205] = 0;

    DeleteFileW(g_target);
    if (run_download(SCEN_SAVE, url, NULL, &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
        ok(g_saw_saveas, "no Save As dialog\n");
        ok(file_matches(g_target, BODY_SIZE), "the saved file differs from the served data\n");
    }
    DeleteFileW(g_target);
}

/* a quit message ends the download, removes what was written and goes back to the caller */
static void test_quit(void)
{
    BOOL ret = FALSE;
    WPARAM code = 0;

    DeleteFileW(g_target);
    if (run_download(SCEN_QUIT_SAVING, "long.bin", NULL, &ret))
    {
        ok(ret == FALSE, "DoFileDownload returned %d after the quit message\n", ret);
        ok(g_saw_progress, "no progress dialog\n");
        ok(GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES, "the partial file was left\n");
        ok(take_quit(&code) && code == 7, "the quit message was not given back (%Iu)\n", code);
        ok(!find_dialog(L"127.0.0.1", TRUE), "the progress dialog is still there\n");
    }

    if (run_download(SCEN_QUIT_ASKING, "ok.bin", NULL, &ret))
    {
        ok(ret == FALSE, "DoFileDownload returned %d after the quit message\n", ret);
        ok(g_saw_ask, "no File Download dialog\n");
        ok(!find_dialog(L"File Download", TRUE), "the question is still there\n");
        /* the dialog may or may not hand the message on; either way nothing is left behind */
        take_quit(&code);
    }

    /* a finished download whose dialog is still open: the file stays, the dialog goes, the quit message comes back */
    DeleteFileW(g_target);
    if (run_download(SCEN_QUIT_DONE, "ok.bin", NULL, &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d after a quit with the finished dialog open\n", ret);
        ok(file_matches(g_target, BODY_SIZE), "the saved file differs from the served data\n");
        ok(take_quit(&code) && code == 9, "the quit message was not given back (%Iu)\n", code);
        ok(!find_dialog(L"Download complete", FALSE), "the finished dialog is still there\n");
    }

    /* the next download works */
    DeleteFileW(g_target);
    if (run_download(SCEN_SAVE, "ok.bin", NULL, &ret))
    {
        ok(ret == TRUE, "DoFileDownload returned %d after a quit\n", ret);
        ok(file_matches(g_target, BODY_SIZE), "the saved file differs from the served data\n");
    }
    DeleteFileW(g_target);
}

START_TEST(FileDownload)
{
    WCHAR dir[MAX_PATH], scratch[MAX_PATH];

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
    PathCombineW(scratch, dir, L"sdl-test-dir");
    CreateDirectoryW(scratch, NULL);
    PathAddBackslashW(scratch);

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

        /* a name with a directory part is cut to the file name, in the question and in Save As */
        {
            saved_value_t saved;
            BOOL set = push_download_dir(scratch, &saved);

            DeleteFileW(g_target);
            if (run_download(SCEN_SAVE, "cd-evil.bin", L"evil name.txt", &ret))
            {
                ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
                ok(g_saw_name, "the dialog does not show the file name without its directory\n");
                ok(!set || !wcscmp(g_saveas_name, L"evil name.txt"), "Save As offers \"%S\"\n", g_saveas_name);
            }

            /* a device name is not offered as it is */
            DeleteFileW(g_target);
            if (run_download(SCEN_SAVE, "cd-nul.bin", L"NUL.txt", &ret))
            {
                ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
                ok(!set || !wcscmp(g_saveas_name, L"NU_.txt"), "Save As offers \"%S\"\n", g_saveas_name);
                ok(file_matches(g_target, BODY_SIZE), "the saved file differs from the served data\n");
            }
            if (set)
                pop_download_dir(&saved);
        }

        /* a name that fits the folder is kept whole, extension included */
        {
            WCHAR longname[260];
            saved_value_t saved;
            BOOL set;

            swprintf(longname, ARRAY_SIZE(longname), L"%S.%S", LONG_BASE, LONG_EXT);
            /* the folder Save As starts in is short, so that the whole name of 211 characters fits behind it */
            set = push_download_dir(dir, &saved);
            DeleteFileW(g_target);
            if (run_download(SCEN_SAVE, "longext.bin", longname, &ret))
            {
                ok(ret == TRUE, "DoFileDownload returned %d\n", ret);
                ok(g_saw_name, "the dialog does not show the whole name of 211 characters\n");
                ok(!set || !wcscmp(g_saveas_name, longname), "Save As offers \"%S\", not the whole name\n", g_saveas_name);
            }
            if (set)
                pop_download_dir(&saved);
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

        /* nobody listens */
        {
            WCHAR url[128];
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            struct sockaddr_in addr;
            int len = sizeof(addr);

            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            bind(s, (struct sockaddr *)&addr, sizeof(addr));
            getsockname(s, (struct sockaddr *)&addr, &len);
            closesocket(s);
            swprintf(url, ARRAY_SIZE(url), L"http://127.0.0.1:%u/x.bin", ntohs(addr.sin_port));

            DeleteFileW(g_target);
            if (run_url(SCEN_ERROR_BOX, url, NULL, &ret))
            {
                ok(ret == FALSE, "DoFileDownload returned %d for a refused connection\n", ret);
                ok(g_saw_error, "no error message\n");
            }
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

    test_overwrite();
    test_save_device();
    test_autoclose();
    test_long_extension();
    test_failure_while_dialogs_are_open(scratch);
    test_quit();
    test_teardown_waits_for_urlmon();
    test_pump_inside_read();
    test_cancel_inside_read();
    test_endless_pump();
    test_open_device_name();
    test_saveas_name_after_shortening("COM1", "COM1payload");
    test_saveas_name_after_shortening("NUL", "NULpayload");

    DeleteFileW(g_target);
    RemoveDirectoryW(scratch);
    stop_server();
}
