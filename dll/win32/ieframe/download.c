/*
 * File downloads of the Internet Browser: the File Download dialog, the progress dialog and the urlmon binding
 * behind them.
 *
 * Copyright 2026 ReactOS contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <wchar.h>

#include "ieframe.h"

#include "commctrl.h"
#include "commdlg.h"
#include "shellapi.h"
#include "shlobj.h"
#include "shlwapi.h"
#include "winreg.h"
#include "wininet.h"
#include "resource.h"

#include "download_name.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ieframe);

/*
 * Lifetime. Everything runs on the apartment thread that started the download (the browser's UI thread, or the
 * thread of DoFileDownload); urlmon delivers its notifications as messages to that thread.
 *
 * A Download is reference counted. Three kinds of references keep it alive:
 *  - the creating reference (self_ref) until the download has ended and its windows are gone,
 *  - urlmon's reference as the callback of the binding, until urlmon is done with it,
 *  - a temporary reference in every function that is entered from outside (window procedure, timer, callback) or that
 *    calls out, because almost every call out can run messages and end the download: a status text sink, ShellExecute,
 *    a message box, a dialog, DestroyWindow. Code that calls out looks at the state again afterwards.
 */

#define WM_DL_ASK    (WM_USER + 0x500)   /* the response started: ask the user */
#define WM_DL_PUMP   (WM_USER + 0x501)   /* copy data from the stream to the file */
#define WM_DL_STOP   (WM_USER + 0x502)   /* the binding has stopped; wparam is its result */
#define WM_DL_FAIL   (WM_USER + 0x503)   /* the server answered with an error; wparam is the status */

#define DL_TIMER_ID      1
#define DL_PUMP_SLICE_MS 40
#define DL_BUFFER_SIZE   0x10000
#define DL_NAME_SOURCE   1024
#define DL_URLMON_IDLE_MS 30000

#ifndef CSIDL_DOWNLOADS
#define CSIDL_DOWNLOADS  0x0047
#endif

typedef enum {
    DL_BINDING,     /* waiting for the response */
    DL_ASKING,      /* the File Download dialog or Save As is up */
    DL_SAVING,      /* writing the file */
    DL_ENDED        /* finished, failed or cancelled */
} dl_state_t;

typedef struct {
    IBindStatusCallback IBindStatusCallback_iface;
    IHttpNegotiate IHttpNegotiate_iface;

    LONG ref;
    BOOL self_ref;

    DocHost *doc_host;          /* NULL for DoFileDownload */
    BOOL process_lock;
    BOOL standalone;            /* DoFileDownload runs the message loop */

    HWND msg_hwnd;
    HWND ask_hwnd;
    HWND dlg;
    HWND owner;

    dl_state_t state;
    BOOL ok;                    /* the whole file was written */
    BOOL stopped;
    HRESULT stop_hr;
    BOOL start_reported;        /* urlmon called OnStartBinding, so it will call OnStopBinding */
    BOOL stop_reported;
    BOOL pump_posted;
    BOOL pumping;               /* download_pump is on the stack */
    BOOL pump_again;            /* a pump message came in while it was */
    BOOL open_when_done;
    BOOL is_exe;
    BOOL known_type;
    BOOL done_shown;
    BOOL file_owned;            /* the file at path was created or truncated by this download */
    BOOL name_from_header;

    WCHAR *url;
    WCHAR *headers;
    BYTE *post_data;
    DWORD post_data_len;

    WCHAR host[INTERNET_MAX_HOST_NAME_LENGTH + 1];
    WCHAR name[DL_LEAF_MAX + 1];
    WCHAR type_desc[128];
    WCHAR path[MAX_PATH];

    IBinding *binding;
    IStream *stream;
    HANDLE file;

    ULONGLONG total;
    ULONGLONG received;
    DWORD start_tick;
    DWORD end_tick;
    DWORD last_status_tick;
    BYTE *buf;
} Download;

static ATOM download_class;

static const WCHAR download_wndclassW[] = L"IEDownloadWnd";
static const WCHAR download_dlg_propW[] = L"IEDownloadDialog";
static const WCHAR ie_main_keyW[] = L"Software\\Microsoft\\Internet Explorer\\Main";

static inline Download *impl_from_IBindStatusCallback(IBindStatusCallback *iface)
{
    return CONTAINING_RECORD(iface, Download, IBindStatusCallback_iface);
}

static inline Download *impl_from_IHttpNegotiate(IHttpNegotiate *iface)
{
    return CONTAINING_RECORD(iface, Download, IHttpNegotiate_iface);
}

static void download_addref(Download *This)
{
    IBindStatusCallback_AddRef(&This->IBindStatusCallback_iface);
}

static void download_release(Download *This)
{
    IBindStatusCallback_Release(&This->IBindStatusCallback_iface);
}

/* nothing is left to show: the download ended and no dialog is open */
static BOOL download_is_over(Download *This)
{
    return This->state == DL_ENDED && !This->dlg && !This->ask_hwnd;
}

/* drops the creating reference once nothing is left to show; may free the download */
static void download_check_over(Download *This)
{
    if(!download_is_over(This))
        return;

    /* the message loop of DoFileDownload may be waiting inside GetMessage, which does not return for a sent message */
    if(This->standalone)
        PostMessageW(This->msg_hwnd, WM_NULL, 0, 0);

    if(This->self_ref) {
        This->self_ref = FALSE;
        download_release(This);
    }
}

/* strings and numbers */

static WCHAR *load_str(UINT id, WCHAR *buf, UINT len)
{
    buf[0] = 0;
    LoadStringW(ieframe_instance, id, buf, len);
    return buf;
}

static void fmt_str(WCHAR *buf, UINT len, UINT id, ...)
{
    WCHAR fmt[512];
    va_list args;

    load_str(id, fmt, ARRAY_SIZE(fmt));
    va_start(args, id);
    _vsnwprintf(buf, len, fmt, args);
    va_end(args);
    buf[len - 1] = 0;
}

/* three significant digits, truncated: 370KB, 2.00MB, 40.0MB, 200MB */
static void format_size(ULONGLONG n, WCHAR *buf, UINT len)
{
    static const WCHAR *units[] = { L"KB", L"MB", L"GB", L"TB" };
    double v;
    unsigned unit = 0;

    if(n < 1000) {
        WCHAR bytes[32];

        load_str(IDS_DL_UNIT_BYTES, bytes, ARRAY_SIZE(bytes));
        dl_snprintf(buf, len, L"%u %s", (unsigned)n, bytes);
        return;
    }

    v = (double)n / 1024.0;
    while(v >= 1000.0 && unit + 1 < ARRAY_SIZE(units)) {
        v /= 1024.0;
        unit++;
    }

    if(v >= 100.0)
        dl_snprintf(buf, len, L"%u%s", (unsigned)v, units[unit]);
    else if(v >= 10.0)
        dl_snprintf(buf, len, L"%u.%u%s", (unsigned)v, (unsigned)(v * 10.0) % 10, units[unit]);
    else
        dl_snprintf(buf, len, L"%u.%02u%s", (unsigned)v, (unsigned)(v * 100.0) % 100, units[unit]);
}

static void format_duration(DWORD seconds, WCHAR *buf, UINT len)
{
    if(seconds >= 3600)
        fmt_str(buf, len, IDS_DL_TIME_HMS, seconds / 3600, (seconds / 60) % 60, seconds % 60);
    else if(seconds >= 60)
        fmt_str(buf, len, IDS_DL_TIME_MS, seconds / 60, seconds % 60);
    else
        fmt_str(buf, len, IDS_DL_TIME_S, seconds ? seconds : 1);
}

static void format_rate(Download *This, WCHAR *buf, UINT len)
{
    DWORD ms = (This->end_tick ? This->end_tick : GetTickCount()) - This->start_tick;
    WCHAR size[32];

    format_size(This->received * 1000 / (ms ? ms : 1), size, ARRAY_SIZE(size));
    fmt_str(buf, len, IDS_DL_RATE_VAL, size);
}

static void system_error_text(DWORD err, WCHAR *buf, UINT len)
{
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, buf, len, NULL);

    if(!n)
        dl_snprintf(buf, len, L"error %u", err);
    else
        while(n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
            buf[--n] = 0;
}

/* the window of the browser that started the download, if it is still there */
static HWND get_owner_window(DocHost *doc_host)
{
    HWND hwnd = doc_host->frame_hwnd ? doc_host->frame_hwnd : doc_host->hwnd;

    if(hwnd && IsWindow(hwnd))
        return GetAncestor(hwnd, GA_ROOT);
    return NULL;
}

static HWND dialog_owner(Download *This)
{
    return This->owner && IsWindow(This->owner) ? This->owner : NULL;
}

/* the text goes to the sinks of the browser, which may run messages */
static void set_status(Download *This, const WCHAR *text)
{
    if(!This->doc_host)
        return;
    download_addref(This);
    dochost_set_status_text(This->doc_host, text);
    download_release(This);
}

/* names and types */

static void set_name(Download *This, const WCHAR *source)
{
    WCHAR tmp[DL_NAME_SOURCE];

    lstrcpynW(tmp, source, ARRAY_SIZE(tmp));
    dl_make_leaf(tmp);
    lstrcpynW(This->name, tmp, ARRAY_SIZE(This->name));
}

/* the address of the file (after redirects): host and, unless the server named the file, the name */
static BOOL set_url(Download *This, const WCHAR *url)
{
    WCHAR source[DL_NAME_SOURCE];
    WCHAR *copy = wcsdup(url);

    if(!copy)
        return FALSE;
    free(This->url);
    This->url = copy;

    dl_split_url(url, This->host, ARRAY_SIZE(This->host), source, ARRAY_SIZE(source));
    if(!This->name_from_header)
        set_name(This, source);
    return TRUE;
}

/* registered friendly name of a file type; FALSE if the extension has no association */
static BOOL get_type_description(const WCHAR *ext, WCHAR *desc, UINT len)
{
    WCHAR progid[256], key[300];
    DWORD size;

    desc[0] = 0;
    if(!ext || !*ext)
        return FALSE;

    size = sizeof(progid);
    if(RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, NULL, progid, &size) || !progid[0])
        return FALSE;

    size = len * sizeof(WCHAR);
    if(RegGetValueW(HKEY_CLASSES_ROOT, progid, NULL, RRF_RT_REG_SZ, NULL, desc, &size) || !desc[0]) {
        SHFILEINFOW info;
        WCHAR fake[64];

        dl_snprintf(key, ARRAY_SIZE(key), L"%s\\shell\\open\\command", progid);
        size = 0;
        if(RegGetValueW(HKEY_CLASSES_ROOT, key, NULL, RRF_RT_REG_SZ, NULL, NULL, &size))
            return FALSE;

        /* an association without a description: describe it as the shell does */
        dl_snprintf(fake, ARRAY_SIZE(fake), L"x%s", ext);
        if(SHGetFileInfoW(fake, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
            lstrcpynW(desc, info.szTypeName, len);
    }
    return desc[0] != 0;
}

/* the folder Save As starts in, with its closing backslash */
static void get_default_save_dir(WCHAR *dir)
{
    WCHAR value[MAX_PATH];
    DWORD size = sizeof(value), type;
    HKEY key;

    dir[0] = 0;

    if(!RegOpenKeyW(HKEY_CURRENT_USER, ie_main_keyW, &key)) {
        if(!RegQueryValueExW(key, L"Default Download Directory", NULL, &type, (BYTE*)value, &size)
           && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            DWORD len;

            value[ARRAY_SIZE(value) - 1] = 0;
            len = ExpandEnvironmentStringsW(value, dir, MAX_PATH);
            if(!len || len > MAX_PATH || !PathIsDirectoryW(dir))
                dir[0] = 0;
        }
        RegCloseKey(key);
    }

    if(!dir[0]) {
        if(FAILED(SHGetFolderPathW(NULL, CSIDL_DOWNLOADS | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, dir))
           || !PathIsDirectoryW(dir))
            dir[0] = 0;
    }

    if(!dir[0]) {
        if(FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, dir)))
            GetCurrentDirectoryW(MAX_PATH, dir);
    }

    if(dir[0] && dir[wcslen(dir) - 1] != '\\') {
        if(wcslen(dir) + 2 > MAX_PATH)
            dir[0] = 0;
        else
            PathAddBackslashW(dir);
    }
}

/* the file */

static void close_file(Download *This)
{
    if(This->file != INVALID_HANDLE_VALUE) {
        CloseHandle(This->file);
        This->file = INVALID_HANDLE_VALUE;
    }
}

static void free_download(Download *This)
{
    if(This->msg_hwnd)
        DestroyWindow(This->msg_hwnd);
    if(This->doc_host)
        IOleClientSite_Release(&This->doc_host->IOleClientSite_iface);
    if(This->process_lock)
        released_obj();
    if(This->binding)
        IBinding_Release(This->binding);
    if(This->stream)
        IStream_Release(This->stream);
    close_file(This);
    free(This->url);
    free(This->headers);
    free(This->post_data);
    free(This->buf);
    free(This);
}

static void download_abort_binding(Download *This)
{
    IBinding *binding = This->binding;

    if(binding) {
        This->binding = NULL;
        IBinding_Abort(binding);
        IBinding_Release(binding);
    }
}

/* Every way of ending a download goes through here. A partial file is removed, only if this download made it. */
static void download_end(Download *This)
{
    HWND dlg;

    if(This->state == DL_ENDED)
        return;

    download_addref(This);
    This->state = DL_ENDED;

    close_file(This);
    if(!This->ok && This->file_owned) {
        This->file_owned = FALSE;
        DeleteFileW(This->path);
    }

    download_abort_binding(This);

    if(This->ask_hwnd)
        EndDialog(This->ask_hwnd, IDCANCEL);
    dlg = This->dlg;
    if(dlg && !This->done_shown)
        DestroyWindow(dlg);

    download_check_over(This);
    download_release(This);
}

static void download_cancel(Download *This)
{
    download_addref(This);
    download_end(This);
    set_status(This, L"");
    download_release(This);
}

static void show_download_error(Download *This, const WCHAR *reason)
{
    WCHAR title[64], text[1024];

    fmt_str(text, ARRAY_SIZE(text), IDS_DL_ERR_FMT, This->name, This->host, reason);
    load_str(IDS_DL_ERR_TITLE, title, ARRAY_SIZE(title));
    download_addref(This);
    MessageBoxW(dialog_owner(This), text, title, MB_OK | MB_ICONERROR);
    download_release(This);
}

static void download_fail(Download *This, const WCHAR *reason)
{
    download_addref(This);
    download_cancel(This);
    show_download_error(This, reason);
    download_release(This);
}

static void download_fail_hresult(Download *This, HRESULT hres)
{
    WCHAR reason[512];

    fmt_str(reason, ARRAY_SIZE(reason), IDS_DL_ERR_NET, hres);
    download_fail(This, reason);
}

/* the progress dialog */

static void update_progress_ui(Download *This)
{
    WCHAR caption[512], line[256], received[64], total[64], left[64];
    HWND dlg = This->dlg;
    DWORD now, elapsed_ms;

    if(!dlg || This->done_shown)
        return;

    now = GetTickCount();
    elapsed_ms = now - This->start_tick;

    format_size(This->received, received, ARRAY_SIZE(received));
    total[0] = 0;
    if(This->total) {
        UINT pct = (UINT)min(100, This->received * 100 / This->total);

        fmt_str(caption, ARRAY_SIZE(caption), IDS_DL_CAPTION_PCT, pct, This->name, This->host);
        SendDlgItemMessageW(dlg, IDC_DLP_BAR, PBM_SETPOS, pct, 0);

        format_size(This->total, total, ARRAY_SIZE(total));
        if(This->received && elapsed_ms) {
            ULONGLONG rate = This->received * 1000 / elapsed_ms;

            if(rate) {
                format_duration((DWORD)((This->total - min(This->received, This->total)) / rate), left, ARRAY_SIZE(left));
                fmt_str(line, ARRAY_SIZE(line), IDS_DL_TIMELEFT_VAL, left, received, total);
                SetDlgItemTextW(dlg, IDC_DLP_VALUE1, line);
            }
        }
    }else {
        fmt_str(caption, ARRAY_SIZE(caption), IDS_DL_CAPTION_COPIED, received, This->name, This->host);
        fmt_str(line, ARRAY_SIZE(line), IDS_DL_TIMELEFT_UNKNOWN, received);
        SetDlgItemTextW(dlg, IDC_DLP_VALUE1, line);
    }
    SetWindowTextW(dlg, caption);

    format_rate(This, line, ARRAY_SIZE(line));
    SetDlgItemTextW(dlg, IDC_DLP_VALUE3, line);

    if(now - This->last_status_tick >= 1000) {
        WCHAR status[INTERNET_MAX_URL_LENGTH + 64], progress[128];

        This->last_status_tick = now;
        if(This->total)
            dl_snprintf(progress, ARRAY_SIZE(progress), L"%u%% of %s",
                        (UINT)min(100, This->received * 100 / This->total), total);
        else
            lstrcpynW(progress, received, ARRAY_SIZE(progress));
        fmt_str(status, ARRAY_SIZE(status), IDS_DL_STATUS_PROGRESS, This->url, progress);
        set_status(This, status);
    }
}

static void show_complete_ui(Download *This)
{
    WCHAR text[512], size[64], duration[64];
    HWND dlg = This->dlg;
    HWND bar;

    This->end_tick = GetTickCount();
    This->done_shown = TRUE;
    if(!dlg)
        return;

    SetWindowTextW(dlg, load_str(IDS_DL_CAPTION_DONE, text, ARRAY_SIZE(text)));
    SetDlgItemTextW(dlg, IDC_DLP_TITLE, load_str(IDS_DL_COMPLETE, text, ARRAY_SIZE(text)));

    bar = GetDlgItem(dlg, IDC_DLP_BAR);
    SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
    SetWindowLongW(bar, GWL_STYLE, GetWindowLongW(bar, GWL_STYLE) & ~PBS_MARQUEE);
    SendMessageW(bar, PBM_SETRANGE32, 0, 100);
    SendMessageW(bar, PBM_SETPOS, 100, 0);

    SetDlgItemTextW(dlg, IDC_DLP_LABEL1, load_str(IDS_DL_DOWNLOADED, text, ARRAY_SIZE(text)));
    format_size(This->received, size, ARRAY_SIZE(size));
    format_duration((This->end_tick - This->start_tick + 500) / 1000, duration, ARRAY_SIZE(duration));
    fmt_str(text, ARRAY_SIZE(text), IDS_DL_DOWNLOADED_VAL, size, duration);
    SetDlgItemTextW(dlg, IDC_DLP_VALUE1, text);

    format_rate(This, text, ARRAY_SIZE(text));
    SetDlgItemTextW(dlg, IDC_DLP_VALUE3, text);

    SetDlgItemTextW(dlg, IDCANCEL, load_str(IDS_DL_BTN_CLOSE, text, ARRAY_SIZE(text)));
    EnableWindow(GetDlgItem(dlg, IDC_DLP_OPEN), TRUE);
    EnableWindow(GetDlgItem(dlg, IDC_DLP_FOLDER), TRUE);
}

static void open_downloaded_file(Download *This)
{
    SHELLEXECUTEINFOW info = { sizeof(info) };

    download_addref(This);

    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = This->dlg ? This->dlg : dialog_owner(This);
    info.lpFile = This->path;
    info.nShow = SW_SHOWNORMAL;

    if(!ShellExecuteExW(&info)) {
        WCHAR reason[512], err[400];

        system_error_text(GetLastError(), err, ARRAY_SIZE(err));
        fmt_str(reason, ARRAY_SIZE(reason), IDS_DL_ERR_OPEN, err);
        show_download_error(This, reason);
    }
    download_release(This);
}

static void open_containing_folder(Download *This)
{
    WCHAR params[MAX_PATH + 16];

    download_addref(This);
    dl_snprintf(params, ARRAY_SIZE(params), L"/select,\"%s\"", This->path);
    ShellExecuteW(This->dlg, L"open", L"explorer.exe", params, NULL, SW_SHOWNORMAL);
    download_release(This);
}

static void close_progress_dialog(Download *This)
{
    HWND dlg = This->dlg;

    download_addref(This);
    if(dlg)
        DestroyWindow(dlg);
    download_check_over(This);
    download_release(This);
}

static INT_PTR CALLBACK progress_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    Download *This = (Download*)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch(msg) {
    case WM_INITDIALOG: {
        WCHAR text[512];
        SHFILEINFOW info;
        HWND bar;

        This = (Download*)lparam;
        SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        SetPropW(hwnd, download_dlg_propW, (HANDLE)1);
        This->dlg = hwnd;

        fmt_str(text, ARRAY_SIZE(text), IDS_DL_NAME_FROM, This->name, This->host);
        SetDlgItemTextW(hwnd, IDC_DLP_NAME, text);
        SetDlgItemTextW(hwnd, IDC_DLP_VALUE2, This->path);
        SetDlgItemTextW(hwnd, IDC_DLP_LABEL1, load_str(IDS_DL_TIMELEFT, text, ARRAY_SIZE(text)));
        SetDlgItemTextW(hwnd, IDC_DLP_VALUE3, L"");
        if(This->is_exe)
            SetDlgItemTextW(hwnd, IDC_DLP_OPEN, load_str(IDS_DL_BTN_RUN, text, ARRAY_SIZE(text)));

        bar = GetDlgItem(hwnd, IDC_DLP_BAR);
        SendMessageW(bar, PBM_SETRANGE32, 0, 100);
        if(!This->total) {
            SetWindowLongW(bar, GWL_STYLE, GetWindowLongW(bar, GWL_STYLE) | PBS_MARQUEE);
            SendMessageW(bar, PBM_SETMARQUEE, TRUE, 60);
        }

        if(SHGetFileInfoW(This->name, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                          SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES))
            SendDlgItemMessageW(hwnd, IDC_DLP_ICON, STM_SETICON, (WPARAM)info.hIcon, 0);

        update_progress_ui(This);
        SetTimer(hwnd, DL_TIMER_ID, 500, NULL);
        return TRUE;
    }
    case WM_TIMER:
        if(This && This->state == DL_SAVING) {
            download_addref(This);
            update_progress_ui(This);
            download_release(This);
        }
        return TRUE;
    case WM_COMMAND:
        if(!This)
            return TRUE;
        switch(LOWORD(wparam)) {
        case IDCANCEL:
            download_addref(This);
            if(This->state == DL_SAVING)
                download_cancel(This);
            else
                close_progress_dialog(This);
            download_release(This);
            return TRUE;
        case IDC_DLP_OPEN:
            open_downloaded_file(This);
            return TRUE;
        case IDC_DLP_FOLDER:
            open_containing_folder(This);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if(This) {
            download_addref(This);
            if(This->state == DL_SAVING)
                download_cancel(This);
            else
                close_progress_dialog(This);
            download_release(This);
        }
        return TRUE;
    case WM_DESTROY: {
        HICON icon = (HICON)SendDlgItemMessageW(hwnd, IDC_DLP_ICON, STM_GETICON, 0, 0);

        if(icon)
            DestroyIcon(icon);
        KillTimer(hwnd, DL_TIMER_ID);
        RemovePropW(hwnd, download_dlg_propW);
        SetWindowLongPtrW(hwnd, DWLP_USER, 0);
        if(This && This->dlg == hwnd) {
            This->dlg = NULL;
            /* the last thing: this may free the download */
            download_check_over(This);
        }
        return TRUE;
    }
    }
    return FALSE;
}

static void create_progress_dialog(Download *This)
{
    CreateDialogParamW(ieframe_instance, MAKEINTRESOURCEW(IDD_DOWNLOAD_PROGRESS), NULL, progress_dlg_proc, (LPARAM)This);
    if(This->dlg) {
        ShowWindow(This->dlg, SW_SHOWNORMAL);
        SetForegroundWindow(This->dlg);
    }
}

/* the File Download dialog */

typedef struct {
    Download *dl;
    UINT title_id;
    UINT question_id;
    UINT warn_id;
} ask_params_t;

static INT_PTR CALLBACK ask_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch(msg) {
    case WM_INITDIALOG: {
        ask_params_t *params = (ask_params_t*)lparam;
        Download *This = params->dl;
        WCHAR text[1024], size[64], type[256];
        SHFILEINFOW info;

        SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        This->ask_hwnd = hwnd;

        SetWindowTextW(hwnd, load_str(params->title_id, text, ARRAY_SIZE(text)));
        SetDlgItemTextW(hwnd, IDC_DLA_QUESTION, load_str(params->question_id, text, ARRAY_SIZE(text)));
        SetDlgItemTextW(hwnd, IDC_DLA_WARNING, load_str(params->warn_id, text, ARRAY_SIZE(text)));
        SetDlgItemTextW(hwnd, IDC_DLA_NAME, This->name);
        SetDlgItemTextW(hwnd, IDC_DLA_FROM, This->host);

        if(This->known_type)
            lstrcpynW(type, This->type_desc, ARRAY_SIZE(type));
        else
            load_str(IDS_DL_TYPE_UNKNOWN, type, ARRAY_SIZE(type));
        if(This->total) {
            format_size(This->total, size, ARRAY_SIZE(size));
            fmt_str(text, ARRAY_SIZE(text), IDS_DL_TYPE_SIZE, type, size);
            SetDlgItemTextW(hwnd, IDC_DLA_TYPE, text);
        }else {
            SetDlgItemTextW(hwnd, IDC_DLA_TYPE, type);
        }

        if(SHGetFileInfoW(This->name, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                          SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES))
            SendDlgItemMessageW(hwnd, IDC_DLA_ICON, STM_SETICON, (WPARAM)info.hIcon, 0);
        SendDlgItemMessageW(hwnd, IDC_DLA_WARNICON, STM_SETICON, (WPARAM)LoadIconW(NULL, (LPCWSTR)IDI_WARNING), 0);

        if(This->is_exe)
            SetDlgItemTextW(hwnd, IDC_DLA_OPEN, load_str(IDS_DL_BTN_RUN, text, ARRAY_SIZE(text)));
        if(!This->known_type)
            ShowWindow(GetDlgItem(hwnd, IDC_DLA_OPEN), SW_HIDE);
        SetFocus(GetDlgItem(hwnd, IDCANCEL));
        return FALSE;
    }
    case WM_COMMAND:
        switch(LOWORD(wparam)) {
        case IDC_DLA_OPEN:
        case IDC_DLA_SAVE:
        case IDCANCEL:
            EndDialog(hwnd, LOWORD(wparam));
            return TRUE;
        }
        break;
    case WM_DESTROY: {
        ask_params_t *params = (ask_params_t*)GetWindowLongPtrW(hwnd, DWLP_USER);
        HICON icon = (HICON)SendDlgItemMessageW(hwnd, IDC_DLA_ICON, STM_GETICON, 0, 0);

        if(icon)
            DestroyIcon(icon);
        if(params && params->dl->ask_hwnd == hwnd)
            params->dl->ask_hwnd = NULL;
        return TRUE;
    }
    }
    return FALSE;
}

static BOOL add_filter_pair(WCHAR **p, WCHAR *end, const WCHAR *desc, const WCHAR *pattern)
{
    size_t desc_len = wcslen(desc) + 1, pattern_len = wcslen(pattern) + 1;

    /* the list ends with one more zero */
    if((size_t)(end - *p) < desc_len + pattern_len + 1)
        return FALSE;
    memcpy(*p, desc, desc_len * sizeof(WCHAR));
    *p += desc_len;
    memcpy(*p, pattern, pattern_len * sizeof(WCHAR));
    *p += pattern_len;
    return TRUE;
}

/* The Save As dialog; the chosen path goes to file (MAX_PATH characters), nothing is created. */
static BOOL choose_save_path(Download *This, WCHAR *file)
{
    WCHAR dir[MAX_PATH], filter[512], desc[160], pattern[48], all[64];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR *ext = PathFindExtensionW(This->name);
    WCHAR *p = filter;
    BOOL ret;

    get_default_save_dir(dir);

    filter[0] = 0;
    if(*ext && wcslen(ext) < 32) {
        if(This->known_type) {
            lstrcpynW(desc, This->type_desc, ARRAY_SIZE(desc));
        }else {
            /* "BIN File", as the shell names a type without a registered description */
            WCHAR *c;

            dl_snprintf(desc, ARRAY_SIZE(desc), L"%s File", ext + 1);
            for(c = desc; *c && *c != ' '; c++)
                *c = towupper(*c);
        }
        dl_snprintf(pattern, ARRAY_SIZE(pattern), L"*%s", ext);
        add_filter_pair(&p, filter + ARRAY_SIZE(filter), desc, pattern);
    }
    add_filter_pair(&p, filter + ARRAY_SIZE(filter), load_str(IDS_DL_SAVEAS_ALL, all, ARRAY_SIZE(all)), L"*.*");
    *p = 0;

    /* the name offered has to fit behind the folder */
    lstrcpynW(file, This->name, MAX_PATH);
    if(!dir[0] || !dl_fit_leaf(dir, file, 0))
        file[0] = 0;
    else
        dl_unmask_device(dir, file);

    ofn.hwndOwner = dialog_owner(This);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir;
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    download_addref(This);
    ret = GetSaveFileNameW(&ofn);
    download_release(This);
    return ret;
}

/* copying */

static BOOL write_all(Download *This, const BYTE *data, DWORD size, DWORD *err)
{
    while(size) {
        DWORD written = 0;

        if(!WriteFile(This->file, data, size, &written, NULL) || !written) {
            *err = GetLastError();
            if(!*err)
                *err = ERROR_DISK_FULL;
            return FALSE;
        }
        data += written;
        size -= written;
    }
    return TRUE;
}

static void download_finished_ok(Download *This)
{
    WCHAR status[INTERNET_MAX_URL_LENGTH + 64];

    download_addref(This);

    This->ok = TRUE;
    close_file(This);
    show_complete_ui(This);
    This->state = DL_ENDED;

    fmt_str(status, ARRAY_SIZE(status), IDS_DL_STATUS_DONE, This->name);
    set_status(This, status);

    if(This->open_when_done) {
        close_progress_dialog(This);
        open_downloaded_file(This);
    }else if(This->dlg && IsDlgButtonChecked(This->dlg, IDC_DLP_CLOSE) == BST_CHECKED) {
        close_progress_dialog(This);
    }

    download_check_over(This);
    download_release(This);
}

/* all data is in the file once the binding has stopped and a read returns nothing */
static void download_check_finish(Download *This)
{
    if(This->state != DL_SAVING || !This->stopped)
        return;

    if(FAILED(This->stop_hr) && This->stop_hr != E_ABORT) {
        download_fail_hresult(This, This->stop_hr);
        return;
    }

    if(This->total && This->received < This->total) {
        WCHAR reason[512], received[64], total[64];

        format_size(This->received, received, ARRAY_SIZE(received));
        format_size(This->total, total, ARRAY_SIZE(total));
        fmt_str(reason, ARRAY_SIZE(reason), IDS_DL_ERR_INTERRUPTED, received, total);
        download_fail(This, reason);
        return;
    }

    download_finished_ok(This);
}

static void post_pump(Download *This)
{
    if(This->state == DL_SAVING && !This->pump_posted) {
        This->pump_posted = TRUE;
        PostMessageW(This->msg_hwnd, WM_DL_PUMP, 0, 0);
    }
}

static void pump_slice(Download *This)
{
    DWORD start;
    ULONG read;
    HRESULT hres;

    if(!This->stream) {
        download_check_finish(This);
        return;
    }

    if(!This->buf) {
        This->buf = malloc(DL_BUFFER_SIZE);
        if(!This->buf) {
            download_fail(This, L"out of memory");
            return;
        }
    }

    start = GetTickCount();
    do {
        read = 0;
        hres = IStream_Read(This->stream, This->buf, DL_BUFFER_SIZE, &read);
        /* the Read can run messages: the download may have been cancelled or have failed meanwhile */
        if(This->state != DL_SAVING)
            return;
        if(read) {
            DWORD err;

            if(!write_all(This, This->buf, read, &err)) {
                WCHAR reason[512], text[400];

                system_error_text(err, text, ARRAY_SIZE(text));
                fmt_str(reason, ARRAY_SIZE(reason), IDS_DL_ERR_WRITE, text);
                download_fail(This, reason);
                return;
            }
            This->received += read;
        }
        if(FAILED(hres) && hres != E_PENDING)
            WARN("read failed %08lx\n", hres);
    }while(read && SUCCEEDED(hres) && GetTickCount() - start < DL_PUMP_SLICE_MS);

    update_progress_ui(This);
    if(This->state != DL_SAVING || This->pump_again)
        return;

    if(read && SUCCEEDED(hres)) {
        post_pump(This);
        return;
    }
    download_check_finish(This);
}

/* Reading can run messages (a protocol that does so, a status text sink): a pump message that arrives meanwhile only
   makes the slice that runs read once more before it decides that the data is at its end. */
static void download_pump(Download *This)
{
    DWORD start;

    This->pump_posted = FALSE;
    if(This->state != DL_SAVING)
        return;
    if(This->pumping) {
        /* the slice that runs reads again before it decides that the data is at its end */
        This->pump_again = TRUE;
        return;
    }

    This->pumping = TRUE;
    start = GetTickCount();
    do {
        This->pump_again = FALSE;
        pump_slice(This);
    }while(This->pump_again && This->state == DL_SAVING && GetTickCount() - start < DL_PUMP_SLICE_MS);
    This->pumping = FALSE;

    /* a read that keeps running messages does not hold the thread: the next slice comes from the message queue */
    if(This->pump_again && This->state == DL_SAVING)
        post_pump(This);
}

/* the question, then the file */

static void download_ask(Download *This)
{
    ask_params_t params;
    INT_PTR choice;
    WCHAR status[INTERNET_MAX_URL_LENGTH + 64], file[MAX_PATH];
    WCHAR *ext;

    if(This->state != DL_BINDING)
        return;
    download_addref(This);

    ext = PathFindExtensionW(This->name);
    This->is_exe = dl_is_executable_ext(ext);
    This->known_type = get_type_description(ext, This->type_desc, ARRAY_SIZE(This->type_desc));
    if(This->is_exe && !This->known_type) {
        load_str(IDS_DL_TYPE_APP, This->type_desc, ARRAY_SIZE(This->type_desc));
        This->known_type = TRUE;
    }

    params.dl = This;
    if(This->is_exe) {
        params.title_id = IDS_DL_ASK_TITLE_WARN;
        params.question_id = IDS_DL_Q_RUN;
        params.warn_id = IDS_DL_WARN_APP;
    }else if(This->known_type) {
        params.title_id = IDS_DL_ASK_TITLE;
        params.question_id = IDS_DL_Q_OPEN;
        params.warn_id = IDS_DL_WARN_FILE;
    }else {
        params.title_id = IDS_DL_ASK_TITLE;
        params.question_id = IDS_DL_Q_SAVE;
        params.warn_id = IDS_DL_WARN_UNKNOWN;
    }

    This->state = DL_ASKING;
    fmt_str(status, ARRAY_SIZE(status), IDS_DL_STATUS_START, This->url);
    set_status(This, status);
    if(This->state != DL_ASKING)
        goto done;

    choice = DialogBoxParamW(ieframe_instance, MAKEINTRESOURCEW(IDD_DOWNLOAD_ASK), dialog_owner(This),
                             ask_dlg_proc, (LPARAM)&params);
    if(This->state != DL_ASKING)
        goto done;

    file[0] = 0;
    if(choice == IDC_DLA_SAVE) {
        if(!choose_save_path(This, file))
            choice = IDCANCEL;
        if(This->state != DL_ASKING)
            goto done;
    }

    if(choice != IDC_DLA_SAVE && choice != IDC_DLA_OPEN) {
        download_cancel(This);
        goto done;
    }

    if(choice == IDC_DLA_SAVE) {
        /* The name offered was made safe for local folders, but the user can type any name. A device is no file. */
        if(dl_path_opens_device(file)) {
            This->file = INVALID_HANDLE_VALUE;
            SetLastError(ERROR_INVALID_NAME);
        }else {
            This->file = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if(This->file != INVALID_HANDLE_VALUE)
                lstrcpynW(This->path, file, ARRAY_SIZE(This->path));
        }
    }else {
        WCHAR dir[MAX_PATH], leaf[DL_LEAF_MAX + 1];

        This->open_when_done = TRUE;
        if(dl_get_temp_dir(dir)) {
            lstrcpynW(leaf, This->name, ARRAY_SIZE(leaf));
            This->file = dl_create_unique_file(dir, leaf, This->path);
        }else {
            This->file = INVALID_HANDLE_VALUE;
            SetLastError(ERROR_PATH_NOT_FOUND);
        }
    }

    if(This->file == INVALID_HANDLE_VALUE) {
        WCHAR reason[512], text[400];

        system_error_text(GetLastError(), text, ARRAY_SIZE(text));
        fmt_str(reason, ARRAY_SIZE(reason), IDS_DL_ERR_CREATE, text);
        download_fail(This, reason);
        goto done;
    }
    This->file_owned = TRUE;

    This->state = DL_SAVING;
    This->start_tick = GetTickCount();
    This->last_status_tick = 0;
    create_progress_dialog(This);
    post_pump(This);

done:
    /* a dialog that was closed by the end of the download is gone now */
    download_check_over(This);
    download_release(This);
}

static LRESULT CALLBACK download_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    Download *This = (Download*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch(msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lparam)->lpCreateParams);
        return 0;
    case WM_DL_ASK:
        if(This) {
            download_addref(This);
            download_ask(This);
            download_release(This);
        }
        return 0;
    case WM_DL_PUMP:
        if(This) {
            download_addref(This);
            download_pump(This);
            download_release(This);
        }
        return 0;
    case WM_DL_STOP:
        if(This) {
            download_addref(This);
            This->stopped = TRUE;
            This->stop_hr = (HRESULT)wparam;
            if(This->state == DL_SAVING)
                download_pump(This);
            else if(This->state != DL_ENDED && FAILED(This->stop_hr) && This->stop_hr != E_ABORT)
                download_fail_hresult(This, This->stop_hr);
            else if(This->state == DL_BINDING)
                download_ask(This);
            download_release(This);
        }
        return 0;
    case WM_DL_FAIL:
        if(This && This->state != DL_ENDED) {
            WCHAR reason[512];

            download_addref(This);
            fmt_str(reason, ARRAY_SIZE(reason), IDS_DL_ERR_HTTP, (UINT)wparam);
            download_fail(This, reason);
            download_release(This);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void register_download_class(void)
{
    WNDCLASSW wc = { 0 };

    wc.lpfnWndProc = download_wndproc;
    wc.hInstance = ieframe_instance;
    wc.lpszClassName = download_wndclassW;
    download_class = RegisterClassW(&wc);
}

void unregister_download_class(void)
{
    if(download_class)
        UnregisterClassW(MAKEINTRESOURCEW(download_class), ieframe_instance);
}

/* the urlmon callbacks */

static HRESULT WINAPI Download_QueryInterface(IBindStatusCallback *iface, REFIID riid, void **ppv)
{
    Download *This = impl_from_IBindStatusCallback(iface);

    if(IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IBindStatusCallback, riid))
        *ppv = &This->IBindStatusCallback_iface;
    else if(IsEqualGUID(&IID_IHttpNegotiate, riid))
        *ppv = &This->IHttpNegotiate_iface;
    else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI Download_AddRef(IBindStatusCallback *iface)
{
    Download *This = impl_from_IBindStatusCallback(iface);

    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI Download_Release(IBindStatusCallback *iface)
{
    Download *This = impl_from_IBindStatusCallback(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    if(!ref)
        free_download(This);
    return ref;
}

static HRESULT WINAPI Download_OnStartBinding(IBindStatusCallback *iface, DWORD reserved, IBinding *pbind)
{
    Download *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%lx %p)\n", This, reserved, pbind);

    This->start_reported = TRUE;
    if(This->state == DL_ENDED) {
        IBinding_Abort(pbind);
        return S_OK;
    }

    if(This->binding)
        IBinding_Release(This->binding);
    This->binding = pbind;
    IBinding_AddRef(pbind);
    return S_OK;
}

static HRESULT WINAPI Download_GetPriority(IBindStatusCallback *iface, LONG *priority)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Download_OnLowResource(IBindStatusCallback *iface, DWORD reserved)
{
    return S_OK;
}

static HRESULT WINAPI Download_OnProgress(IBindStatusCallback *iface, ULONG progress, ULONG progress_max,
        ULONG status, LPCWSTR text)
{
    Download *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%lu %lu %lu %s)\n", This, progress, progress_max, status, debugstr_w(text));

    if(This->state == DL_ENDED)
        return S_OK;

    switch(status) {
    case BINDSTATUS_REDIRECTING:
        if(text && *text && This->state == DL_BINDING)
            set_url(This, text);
        break;
    case BINDSTATUS_BEGINDOWNLOADDATA:
        if(progress_max)
            This->total = progress_max;
        if(This->state == DL_BINDING)
            PostMessageW(This->msg_hwnd, WM_DL_ASK, 0, 0);
        break;
    case BINDSTATUS_DOWNLOADINGDATA:
    case BINDSTATUS_ENDDOWNLOADDATA:
        if(progress_max && !This->total)
            This->total = progress_max;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI Download_OnStopBinding(IBindStatusCallback *iface, HRESULT hres, LPCWSTR error)
{
    Download *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%08lx %s)\n", This, hres, debugstr_w(error));

    This->stop_reported = TRUE;
    if(This->binding) {
        IBinding_Release(This->binding);
        This->binding = NULL;
    }

    if(This->state != DL_ENDED)
        PostMessageW(This->msg_hwnd, WM_DL_STOP, (WPARAM)hres, 0);
    return S_OK;
}

static HRESULT WINAPI Download_GetBindInfo(IBindStatusCallback *iface, DWORD *bindf, BINDINFO *bindinfo)
{
    Download *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%p %p)\n", This, bindf, bindinfo);

    *bindf = BINDF_ASYNCHRONOUS | BINDF_ASYNCSTORAGE | BINDF_PULLDATA | BINDF_GETNEWESTVERSION | BINDF_NOWRITECACHE;

    if(This->post_data) {
        HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, This->post_data_len + 1);
        BYTE *dst;

        if(!copy)
            return E_OUTOFMEMORY;
        dst = GlobalLock(copy);
        memcpy(dst, This->post_data, This->post_data_len);
        dst[This->post_data_len] = 0;
        GlobalUnlock(copy);

        bindinfo->dwBindVerb = BINDVERB_POST;
        bindinfo->stgmedData.tymed = TYMED_HGLOBAL;
        bindinfo->stgmedData.hGlobal = copy;
        bindinfo->stgmedData.pUnkForRelease = NULL;
        bindinfo->cbstgmedData = This->post_data_len;
    }
    return S_OK;
}

static HRESULT WINAPI Download_OnDataAvailable(IBindStatusCallback *iface, DWORD bscf, DWORD size,
        FORMATETC *formatetc, STGMEDIUM *stgmed)
{
    Download *This = impl_from_IBindStatusCallback(iface);

    TRACE("(%p)->(%lx %lu %p %p)\n", This, bscf, size, formatetc, stgmed);

    if(This->state == DL_ENDED)
        return S_OK;

    if(!This->stream && stgmed && stgmed->tymed == TYMED_ISTREAM && stgmed->pstm) {
        This->stream = stgmed->pstm;
        IStream_AddRef(This->stream);
    }

    post_pump(This);
    return S_OK;
}

static HRESULT WINAPI Download_OnObjectAvailable(IBindStatusCallback *iface, REFIID riid, IUnknown *unk)
{
    return S_OK;
}

static const IBindStatusCallbackVtbl DownloadVtbl = {
    Download_QueryInterface,
    Download_AddRef,
    Download_Release,
    Download_OnStartBinding,
    Download_GetPriority,
    Download_OnLowResource,
    Download_OnProgress,
    Download_OnStopBinding,
    Download_GetBindInfo,
    Download_OnDataAvailable,
    Download_OnObjectAvailable
};

static HRESULT WINAPI DownloadHttpNegotiate_QueryInterface(IHttpNegotiate *iface, REFIID riid, void **ppv)
{
    return Download_QueryInterface(&impl_from_IHttpNegotiate(iface)->IBindStatusCallback_iface, riid, ppv);
}

static ULONG WINAPI DownloadHttpNegotiate_AddRef(IHttpNegotiate *iface)
{
    return Download_AddRef(&impl_from_IHttpNegotiate(iface)->IBindStatusCallback_iface);
}

static ULONG WINAPI DownloadHttpNegotiate_Release(IHttpNegotiate *iface)
{
    return Download_Release(&impl_from_IHttpNegotiate(iface)->IBindStatusCallback_iface);
}

static HRESULT WINAPI DownloadHttpNegotiate_BeginningTransaction(IHttpNegotiate *iface, LPCWSTR url,
        LPCWSTR headers, DWORD reserved, LPWSTR *additional)
{
    Download *This = impl_from_IHttpNegotiate(iface);

    *additional = NULL;
    if(This->headers) {
        SIZE_T size = (wcslen(This->headers) + 1) * sizeof(WCHAR);

        *additional = CoTaskMemAlloc(size);
        if(!*additional)
            return E_OUTOFMEMORY;
        memcpy(*additional, This->headers, size);
    }
    return S_OK;
}

static HRESULT WINAPI DownloadHttpNegotiate_OnResponse(IHttpNegotiate *iface, DWORD code,
        LPCWSTR response_headers, LPCWSTR request_headers, LPWSTR *additional)
{
    Download *This = impl_from_IHttpNegotiate(iface);
    WCHAR value[1024], file[DL_NAME_SOURCE];

    TRACE("(%p)->(%lu %s)\n", This, code, debugstr_w(response_headers));

    if(code >= 400) {
        PostMessageW(This->msg_hwnd, WM_DL_FAIL, code, 0);
        return S_OK;
    }

    if(This->state == DL_BINDING && response_headers && dl_find_header(response_headers, L"Content-Disposition", value, ARRAY_SIZE(value))
       && dl_name_from_disposition(value, file, ARRAY_SIZE(file))) {
        This->name_from_header = TRUE;
        set_name(This, file);
        TRACE("file name from Content-Disposition: %s\n", debugstr_w(This->name));
    }
    return S_OK;
}

static const IHttpNegotiateVtbl DownloadHttpNegotiateVtbl = {
    DownloadHttpNegotiate_QueryInterface,
    DownloadHttpNegotiate_AddRef,
    DownloadHttpNegotiate_Release,
    DownloadHttpNegotiate_BeginningTransaction,
    DownloadHttpNegotiate_OnResponse
};

/* starting */

static Download *create_download(DocHost *doc_host, const WCHAR *url, const WCHAR *headers,
        const BYTE *post_data, DWORD post_data_len)
{
    Download *This;

    if(!download_class)
        return NULL;

    This = calloc(1, sizeof(*This));
    if(!This)
        return NULL;

    This->IBindStatusCallback_iface.lpVtbl = &DownloadVtbl;
    This->IHttpNegotiate_iface.lpVtbl = &DownloadHttpNegotiateVtbl;
    This->ref = 1;
    This->self_ref = TRUE;
    This->file = INVALID_HANDLE_VALUE;
    This->state = DL_BINDING;

    if(headers && *headers) {
        This->headers = wcsdup(headers);
        if(!This->headers)
            goto fail;
    }
    if(post_data && post_data_len) {
        This->post_data = malloc(post_data_len);
        if(!This->post_data)
            goto fail;
        memcpy(This->post_data, post_data, post_data_len);
        This->post_data_len = post_data_len;
    }
    if(!set_url(This, url))
        goto fail;

    if(doc_host) {
        This->doc_host = doc_host;
        IOleClientSite_AddRef(&doc_host->IOleClientSite_iface);
        This->owner = get_owner_window(doc_host);
        This->process_lock = lock_ie_process(doc_host);
    }

    This->msg_hwnd = CreateWindowExW(0, download_wndclassW, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, ieframe_instance, This);
    if(!This->msg_hwnd)
        goto fail;
    return This;

fail:
    free_download(This);
    return NULL;
}

static HRESULT start_download_binding(Download *This)
{
    IMoniker *mon;
    IBindCtx *bindctx;
    IUnknown *unk = NULL;
    HRESULT hres;

    hres = CreateURLMoniker(NULL, This->url, &mon);
    if(FAILED(hres))
        return hres;

    hres = CreateAsyncBindCtx(0, &This->IBindStatusCallback_iface, NULL, &bindctx);
    if(FAILED(hres)) {
        IMoniker_Release(mon);
        return hres;
    }

    hres = IMoniker_BindToStorage(mon, bindctx, NULL, &IID_IStream, (void**)&unk);
    if(unk)
        IUnknown_Release(unk);
    IBindCtx_Release(bindctx);
    IMoniker_Release(mon);

    if(FAILED(hres) && hres != MK_S_ASYNCHRONOUS)
        WARN("BindToStorage failed: %08lx\n", hres);
    return hres;
}

/* Called by the browser for content the document cannot show and for attachments. */
HRESULT start_file_download(DocHost *doc_host, const WCHAR *url, const WCHAR *headers,
        const BYTE *post_data, DWORD post_data_len)
{
    Download *This;
    HRESULT hres;

    TRACE("(%p %s)\n", doc_host, debugstr_w(url));

    This = create_download(doc_host, url, headers, post_data, post_data_len);
    if(!This)
        return E_OUTOFMEMORY;

    download_addref(This);
    hres = start_download_binding(This);
    if(FAILED(hres) && hres != MK_S_ASYNCHRONOUS)
        download_end(This);
    download_release(This);

    /* the download keeps itself alive until it and its dialogs are over */
    return FAILED(hres) ? hres : S_OK;
}

/* The browser's message loop gives the keys of the progress dialogs (Tab, Enter, Escape) to them. */
BOOL download_translate_message(MSG *msg)
{
    HWND root;

    if(!msg->hwnd || msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST)
        return FALSE;

    root = GetAncestor(msg->hwnd, GA_ROOT);
    if(!root || !GetPropW(root, download_dlg_propW))
        return FALSE;

    return IsDialogMessageW(root, msg);
}

/***********************************************************************
 *          DoFileDownload (IEFRAME.@)
 *
 * Downloads the file behind an Internet address: asks what to do with it, saves it and shows the progress.
 * Returns when the download has ended and its dialog is closed.
 */
BOOL WINAPI DoFileDownload(LPWSTR url)
{
    Download *This;
    HRESULT coinit, hres;
    BOOL ret, quit = FALSE;
    WPARAM quit_code = 0;
    DWORD idle_start;
    MSG msg;

    TRACE("(%s)\n", debugstr_w(url));

    if(!url || (wcsnicmp(url, L"http://", 7) && wcsnicmp(url, L"https://", 8) && wcsnicmp(url, L"ftp://", 6)))
        return FALSE;

    coinit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    This = create_download(NULL, url, NULL, NULL, 0);
    if(!This) {
        if(SUCCEEDED(coinit))
            CoUninitialize();
        return FALSE;
    }

    /* the reference of this function lives until urlmon has finished with the binding */
    download_addref(This);
    This->standalone = TRUE;

    hres = start_download_binding(This);
    if(FAILED(hres) && hres != MK_S_ASYNCHRONOUS)
        download_end(This);

    while(!download_is_over(This)) {
        if(!GetMessageW(&msg, NULL, 0, 0)) {
            quit = TRUE;
            quit_code = msg.wParam;
            break;
        }
        if(This->dlg && IsDialogMessageW(This->dlg, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ret = This->ok;
    /* a download that is not over (the quit message) loses its dialogs here */
    download_end(This);
    close_progress_dialog(This);

    /* Urlmon holds queued notifications of the binding, which own references: they run before the apartment goes.
       OnStopBinding always follows OnStartBinding, so that wait has no time limit. After it urlmon may still own the
       callback for a while (a protocol can report its result later); the wait for it ends when urlmon lets go, or when
       nothing has happened for DL_URLMON_IDLE_MS (a reference that is not released by messages cannot be waited for). */
    idle_start = GetTickCount();
    for(;;) {
        BOOL stop_pending = This->start_reported && !This->stop_reported;

        if(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if(msg.message == WM_QUIT) {
                quit = TRUE;
                quit_code = msg.wParam;
            }else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            idle_start = GetTickCount();
            continue;
        }

        if(!stop_pending && (InterlockedCompareExchange(&This->ref, 0, 0) <= 1
                             || (DWORD)(GetTickCount() - idle_start) >= DL_URLMON_IDLE_MS))
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }

    /* the caller's loop must still see the quit message */
    if(quit)
        PostQuitMessage((int)quit_code);

    download_release(This);
    if(SUCCEEDED(coinit))
        CoUninitialize();
    return ret;
}
