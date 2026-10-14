/*
 * File downloads for the Internet Browser: the File Download dialog, the progress
 * dialog and the urlmon binding behind them.
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
#include <math.h>

#include "ieframe.h"

#include "commctrl.h"
#include "commdlg.h"
#include "shellapi.h"
#include "shlobj.h"
#include "shlwapi.h"
#include "winreg.h"
#include "wininet.h"
#include "resource.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ieframe);

#define WM_DL_ASK    (WM_USER + 0x500)
#define WM_DL_PUMP   (WM_USER + 0x501)
#define WM_DL_STOP   (WM_USER + 0x502)
#define WM_DL_FAIL   (WM_USER + 0x503)

#define DL_TIMER_ID      1
#define DL_PUMP_SLICE_MS 40
#define DL_BUFFER_SIZE   0x10000

#ifndef CSIDL_DOWNLOADS
#define CSIDL_DOWNLOADS  0x0047
#endif

/* swprintf that always terminates the buffer and returns the stored length */
static int dl_swprintf(WCHAR *buf, size_t len, const WCHAR *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    _vsnwprintf(buf, len, fmt, args);
    va_end(args);
    buf[len - 1] = 0;
    return wcslen(buf);
}

typedef enum {
    DL_BINDING,     /* waiting for the response headers */
    DL_ASKING,      /* File Download dialog is up */
    DL_SAVING,      /* writing the file */
    DL_ENDED        /* finished, failed or cancelled */
} dl_state_t;

typedef struct {
    IBindStatusCallback IBindStatusCallback_iface;
    IHttpNegotiate IHttpNegotiate_iface;

    LONG ref;

    DocHost *doc_host;
    BOOL process_lock;
    BOOL standalone;            /* DoFileDownload: wait in a message loop until finished */

    HWND msg_hwnd;
    HWND ask_hwnd;
    HWND dlg;
    HWND owner;

    dl_state_t state;
    BOOL ended_ok;              /* the whole file was written */
    BOOL asked;
    BOOL stopped;
    BOOL start_reported;        /* urlmon called OnStartBinding: it calls OnStopBinding when it is done with us */
    BOOL stop_reported;
    HRESULT stop_hr;
    BOOL pump_posted;
    BOOL dialog_closed;
    BOOL open_when_done;
    BOOL is_temp;
    BOOL is_exe;
    BOOL known_type;
    BOOL complete_shown;
    BOOL engine_ref;            /* the creating reference lives until the download and its dialogs are over */
    BOOL file_owned;            /* the file at path was created by this download */

    WCHAR *url;
    WCHAR *headers;
    HGLOBAL post_data;
    DWORD post_data_len;

    WCHAR host[INTERNET_MAX_HOST_NAME_LENGTH + 1];
    WCHAR name[MAX_PATH];
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

static BOOL download_class_registered;

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

/* wakes the message loop of DoFileDownload, which waits for the last dialog to be closed */
static void download_signal_done(Download *This)
{
    This->dialog_closed = TRUE;
    if(This->msg_hwnd)
        PostMessageW(This->msg_hwnd, WM_NULL, 0, 0);
}

static WCHAR *load_dl_string(UINT id, WCHAR *buf, UINT len)
{
    buf[0] = 0;
    LoadStringW(ieframe_instance, id, buf, len);
    return buf;
}

static void format_dl_string(WCHAR *buf, UINT len, UINT id, ...)
{
    WCHAR fmt[512];
    va_list args;

    load_dl_string(id, fmt, ARRAY_SIZE(fmt));
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
        load_dl_string(IDS_DL_UNIT_BYTES, bytes, ARRAY_SIZE(bytes));
        dl_swprintf(buf, len, L"%u %s", (unsigned)n, bytes);
        return;
    }

    v = (double)n / 1024.0;
    while(v >= 1000.0 && unit + 1 < ARRAY_SIZE(units)) {
        v /= 1024.0;
        unit++;
    }

    if(v >= 100.0)
        dl_swprintf(buf, len, L"%u%s", (unsigned)v, units[unit]);
    else if(v >= 10.0)
        dl_swprintf(buf, len, L"%u.%u%s", (unsigned)v, (unsigned)(v * 10.0) % 10, units[unit]);
    else
        dl_swprintf(buf, len, L"%u.%02u%s", (unsigned)v, (unsigned)(v * 100.0) % 100, units[unit]);
}

static void format_duration(DWORD seconds, WCHAR *buf, UINT len)
{
    if(seconds >= 3600)
        format_dl_string(buf, len, IDS_DL_TIME_HMS, seconds / 3600, (seconds / 60) % 60, seconds % 60);
    else if(seconds >= 60)
        format_dl_string(buf, len, IDS_DL_TIME_MS, seconds / 60, seconds % 60);
    else
        format_dl_string(buf, len, IDS_DL_TIME_S, seconds ? seconds : 1);
}

static void format_rate(Download *This, WCHAR *buf, UINT len)
{
    DWORD end = This->end_tick ? This->end_tick : GetTickCount();
    DWORD ms = end - This->start_tick;
    WCHAR size[32];

    if(!ms)
        ms = 1;
    format_size(This->received * 1000 / ms, size, ARRAY_SIZE(size));
    format_dl_string(buf, len, IDS_DL_RATE_VAL, size);
}

static HWND get_owner_window(DocHost *doc_host)
{
    HWND hwnd = NULL;

    if(doc_host) {
        if(doc_host->frame_hwnd)
            hwnd = doc_host->frame_hwnd;
        else if(doc_host->hwnd)
            hwnd = doc_host->hwnd;
    }

    if(hwnd && IsWindow(hwnd))
        return GetAncestor(hwnd, GA_ROOT);
    return NULL;
}

static void set_status(Download *This, const WCHAR *text)
{
    if(This->doc_host)
        dochost_set_status_text(This->doc_host, text);
}

static BOOL is_device_name(const WCHAR *name);
static void prefix_device_name(WCHAR *name, size_t max);

/* cuts a name to max characters: the base name goes and the extension stays, as long as it leaves room for a character
   of the base; the result does not end in a dot or a space */
static void cut_file_name(WCHAR *name, size_t max)
{
    WCHAR *ext = PathFindExtensionW(name);
    size_t len = wcslen(name), ext_len = wcslen(ext);

    if(len <= max)
        return;
    if(ext_len < max)
        memmove(name + max - ext_len, ext, (ext_len + 1) * sizeof(WCHAR));
    else
        name[max] = 0;

    for(len = wcslen(name); len && (name[len - 1] == '.' || name[len - 1] == ' '); len--)
        name[len - 1] = 0;
}

/* keeps a name within max characters: the name is kept whole when it fits; a name that is cut short and is a device
   name by then is cut one character shorter and gets the underscore, so the extension stays */
static void limit_file_name(WCHAR *name, size_t max)
{
    WCHAR copy[MAX_PATH];

    if(wcslen(name) <= max)
        return;

    lstrcpynW(copy, name, ARRAY_SIZE(copy));
    cut_file_name(name, max);
    if(max > 1 && is_device_name(name)) {
        lstrcpynW(name, copy, ARRAY_SIZE(copy));
        cut_file_name(name, max - 1);
    }
    prefix_device_name(name, max + 1);
}

static void split_url(Download *This, const WCHAR *url)
{
    WCHAR *tmp, *p;
    DWORD len;

    This->host[0] = 0;
    This->name[0] = 0;

    tmp = wcsdup(url);
    if(!tmp)
        return;

    p = wcschr(tmp, '#');
    if(p)
        *p = 0;
    p = wcschr(tmp, '?');
    if(p)
        *p = 0;

    p = wcsstr(tmp, L"://");
    if(p) {
        WCHAR *host = p + 3, *end = host;

        while(*end && *end != '/' && *end != '\\')
            end++;
        len = min(end - host, ARRAY_SIZE(This->host) - 1);
        memcpy(This->host, host, len * sizeof(WCHAR));
        This->host[len] = 0;
        p = wcschr(This->host, '@');
        if(p)
            memmove(This->host, p + 1, (wcslen(p + 1) + 1) * sizeof(WCHAR));
        p = end;
    }else {
        p = tmp;
    }

    {
        WCHAR *last = p;

        for(; *p; p++)
            if(*p == '/' || *p == '\\')
                last = p + 1;
        len = ARRAY_SIZE(This->name);
        if(*last) {
            DWORD size = len;
            if(FAILED(UrlUnescapeW(last, This->name, &size, 0)))
                lstrcpynW(This->name, last, len);
        }
    }

    free(tmp);
}

static void sanitize_file_name(WCHAR *name)
{
    WCHAR *src = name, *dst = name;

    for(; *src; src++) {
        if(*src < ' ' || wcschr(L"<>:\"/\\|?*", *src))
            *dst++ = '_';
        else
            *dst++ = *src;
    }
    *dst = 0;

    /* no trailing dots or spaces */
    while(dst > name && (dst[-1] == '.' || dst[-1] == ' '))
        *--dst = 0;

    prefix_device_name(name, MAX_PATH);
}

/* DOS device names open the device whatever the extension or the directory */
static BOOL is_device_name(const WCHAR *name)
{
    static const WCHAR *devices[] = { L"CON", L"PRN", L"AUX", L"NUL" };
    size_t base = 0, i;

    while(name[base] && name[base] != '.')
        base++;
    while(base && name[base - 1] == ' ')
        base--;

    for(i = 0; i < ARRAY_SIZE(devices); i++)
        if(base == 3 && !wcsnicmp(name, devices[i], 3))
            return TRUE;
    return base == 4 && (!wcsnicmp(name, L"COM", 3) || !wcsnicmp(name, L"LPT", 3)) && name[3] >= '1' && name[3] <= '9';
}

/* a device name gets an underscore in front; max is the room of the buffer in characters, the end included */
static void prefix_device_name(WCHAR *name, size_t max)
{
    size_t len;

    if(!is_device_name(name))
        return;

    len = wcslen(name);
    if(len + 2 > max)
        name[max - 2] = 0, len = max - 2;
    memmove(name + 1, name, (len + 1) * sizeof(WCHAR));
    name[0] = '_';
}

static int hex_digit(WCHAR c)
{
    if(c >= '0' && c <= '9')
        return c - '0';
    if(c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if(c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Content-Disposition value -> file name; returns TRUE if it names an attachment's file */
static BOOL parse_content_disposition(const WCHAR *value, WCHAR *name, UINT len)
{
    const WCHAR *p;
    WCHAR *tmp = NULL;
    char *bytes = NULL;
    unsigned n = 0;
    BOOL ret = FALSE;

    name[0] = 0;

    p = StrStrIW(value, L"filename*=");
    if(p) {
        /* RFC 5987: charset'language'percent-encoded */
        p += 10;
        const WCHAR *q = wcschr(p, '\'');
        q = q ? wcschr(q + 1, '\'') : NULL;
        if(q) {
            bytes = malloc(wcslen(q) + 1);
            if(bytes) {
                for(q++; *q && *q != ';' && *q != ' '; q++) {
                    if(*q == '%' && hex_digit(q[1]) >= 0 && hex_digit(q[2]) >= 0) {
                        bytes[n++] = (hex_digit(q[1]) << 4) | hex_digit(q[2]);
                        q += 2;
                    }else {
                        bytes[n++] = (char)*q;
                    }
                }
                bytes[n] = 0;
                if(MultiByteToWideChar(CP_UTF8, 0, bytes, -1, name, len))
                    ret = TRUE;
                else
                    name[0] = 0;
                free(bytes);
            }
        }
    }

    if(!ret) {
        p = StrStrIW(value, L"filename");
        while(p) {
            const WCHAR *q = p + 8;
            while(*q == ' ')
                q++;
            if(*q == '=')
                break;
            p = StrStrIW(p + 8, L"filename");
        }
        if(p) {
            p = wcschr(p, '=') + 1;
            while(*p == ' ')
                p++;
            tmp = calloc(wcslen(p) + 1, sizeof(WCHAR));
            if(tmp) {
                WCHAR *dst = tmp;
                BOOL high = FALSE;

                if(*p == '"') {
                    for(p++; *p && *p != '"'; p++) {
                        if(*p == '\\' && p[1])
                            p++;
                        high |= *p >= 0x80;
                        *dst++ = *p;
                    }
                }else {
                    for(; *p && *p != ';' && *p != ' '; p++) {
                        high |= *p >= 0x80;
                        *dst++ = *p;
                    }
                }
                *dst = 0;

                /* the header may carry UTF-8 bytes, one per character */
                if(high) {
                    bytes = malloc(wcslen(tmp) + 1);
                    if(bytes) {
                        BOOL bytes_only = TRUE;
                        unsigned i;

                        for(i = 0; tmp[i]; i++) {
                            if(tmp[i] > 0xff) {
                                bytes_only = FALSE;
                                break;
                            }
                            bytes[i] = (char)tmp[i];
                        }
                        bytes[i] = 0;
                        if(bytes_only && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, -1, name, len))
                            ret = TRUE;
                        free(bytes);
                    }
                }
                if(!ret) {
                    lstrcpynW(name, tmp, len);
                    ret = name[0] != 0;
                }
                free(tmp);
            }
        }
    }

    if(ret) {
        WCHAR *base = name, *c;

        for(c = name; *c; c++)
            if(*c == '/' || *c == '\\')
                base = c + 1;
        if(base != name)
            memmove(name, base, (wcslen(base) + 1) * sizeof(WCHAR));
        sanitize_file_name(name);
        ret = name[0] != 0;
    }
    return ret;
}

static BOOL is_executable_ext(const WCHAR *ext)
{
    static const WCHAR *exts[] = { L".exe", L".com", L".bat", L".cmd", L".scr", L".pif", L".msi", L".cpl", L".vbs", L".js", L".jse", L".wsf", L".reg", L".hta", L".lnk" };
    unsigned i;

    for(i = 0; i < ARRAY_SIZE(exts); i++)
        if(!wcsicmp(ext, exts[i]))
            return TRUE;
    return FALSE;
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
        dl_swprintf(key, ARRAY_SIZE(key), L"%s\\shell\\open\\command", progid);
        size = 0;
        if(RegGetValueW(HKEY_CLASSES_ROOT, key, NULL, RRF_RT_REG_SZ, NULL, NULL, &size))
            return FALSE;
        /* an association without a description: describe it as the system does */
        {
            SHFILEINFOW info;
            WCHAR fake[64];

            dl_swprintf(fake, ARRAY_SIZE(fake), L"x%s", ext);
            if(SHGetFileInfoW(fake, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
                lstrcpynW(desc, info.szTypeName, len);
        }
    }
    return desc[0] != 0;
}

static void get_default_save_dir(WCHAR *dir)
{
    WCHAR value[MAX_PATH];
    DWORD size = sizeof(value);
    HKEY key;

    dir[0] = 0;

    if(!RegOpenKeyW(HKEY_CURRENT_USER, ie_main_keyW, &key)) {
        DWORD type;

        if(!RegQueryValueExW(key, L"Default Download Directory", NULL, &type, (BYTE*)value, &size)
           && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            value[ARRAY_SIZE(value) - 1] = 0;
            ExpandEnvironmentStringsW(value, dir, MAX_PATH);
            if(!PathIsDirectoryW(dir))
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
}

/* creates a new file in the temp directory, with the downloaded name or the next free "name (n).ext" */
static BOOL create_temp_file(Download *This)
{
    WCHAR dir[MAX_PATH], base[MAX_PATH], ext[MAX_PATH], leaf[MAX_PATH], candidate[MAX_PATH];
    WCHAR *dot;
    unsigned i;
    DWORD len;

    len = GetTempPathW(ARRAY_SIZE(dir), dir);
    if(!len || len >= ARRAY_SIZE(dir))
        return FALSE;
    lstrcpynW(base, This->name, ARRAY_SIZE(base));
    dot = PathFindExtensionW(base);
    lstrcpynW(ext, dot, ARRAY_SIZE(ext));
    *dot = 0;

    /* the whole path has to fit MAX_PATH: shorten the base name, the extension stays */
    {
        size_t room = MAX_PATH - 1 - wcslen(dir) - wcslen(ext) - 9 /* " (999)", a separator and an underscore */;

        if((int)room < 1)
            return FALSE;
        if(wcslen(base) > room) {
            size_t len;

            base[room] = 0;
            /* a name cut short does not end in a dot or a space */
            for(len = wcslen(base); len > 1 && (base[len - 1] == '.' || base[len - 1] == ' '); len--)
                base[len - 1] = 0;
        }
    }

    for(i = 0; i < 1000; i++) {
        if(i)
            dl_swprintf(leaf, ARRAY_SIZE(leaf), L"%s (%u)%s", base, i, ext);
        else
            dl_swprintf(leaf, ARRAY_SIZE(leaf), L"%s%s", base, ext);
        /* shortening and the number can make a device name of a harmless one (NULpayload.txt, NUL (1).txt) */
        prefix_device_name(leaf, ARRAY_SIZE(leaf));
        if(!PathCombineW(candidate, dir, leaf))
            return FALSE;

        This->file = CreateFileW(candidate, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if(This->file != INVALID_HANDLE_VALUE) {
            lstrcpynW(This->path, candidate, ARRAY_SIZE(This->path));
            This->file_owned = TRUE;
            This->is_temp = TRUE;
            return TRUE;
        }
        if(GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
    }
    return FALSE;
}

static void free_download(Download *This)
{
    if(This->msg_hwnd)
        DestroyWindow(This->msg_hwnd);
    if(This->doc_host) {
        DocHost *doc_host = This->doc_host;

        This->doc_host = NULL;
        IOleClientSite_Release(&doc_host->IOleClientSite_iface);
    }
    if(This->process_lock)
        released_obj();
    if(This->binding)
        IBinding_Release(This->binding);
    if(This->stream)
        IStream_Release(This->stream);
    if(This->file != INVALID_HANDLE_VALUE)
        CloseHandle(This->file);
    if(This->post_data)
        GlobalFree(This->post_data);
    free(This->url);
    free(This->headers);
    free(This->buf);
    free(This);
}

/* drops the creating reference once nothing is left to show; the object may be freed by this */
static void download_release_engine(Download *This)
{
    if(!This->engine_ref || This->state != DL_ENDED || This->dlg || This->ask_hwnd)
        return;
    This->engine_ref = FALSE;
    download_release(This);
}

/* every way of ending a download goes through here; the object may be freed by this */
static void download_end(Download *This)
{
    HWND dlg;

    if(This->state == DL_ENDED)
        return;
    This->state = DL_ENDED;

    if(This->file != INVALID_HANDLE_VALUE) {
        CloseHandle(This->file);
        This->file = INVALID_HANDLE_VALUE;
    }

    if(!This->ended_ok && This->file_owned && This->path[0])
        DeleteFileW(This->path);

    dlg = This->dlg;
    if(dlg && !This->complete_shown) {
        This->dlg = NULL;
        DestroyWindow(dlg);
    }

    download_release_engine(This);
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

static void download_cancel(Download *This)
{
    download_addref(This);
    This->ended_ok = FALSE;
    download_abort_binding(This);
    download_end(This);
    if(This->doc_host)
        set_status(This, L"");
    if(This->standalone)
        download_signal_done(This);
    download_release(This);
}

static void show_download_error(Download *This, const WCHAR *reason)
{
    WCHAR title[64], text[1024];

    format_dl_string(text, ARRAY_SIZE(text), IDS_DL_ERR_FMT, This->name, This->host, reason);
    load_dl_string(IDS_DL_ERR_TITLE, title, ARRAY_SIZE(title));
    MessageBoxW(This->owner && IsWindow(This->owner) ? This->owner : NULL, text, title, MB_OK | MB_ICONERROR);
}

static void download_fail(Download *This, const WCHAR *reason)
{
    download_addref(This);
    download_cancel(This);
    show_download_error(This, reason);
    download_release(This);
}

static void system_error_text(DWORD err, WCHAR *buf, UINT len)
{
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, buf, len, NULL);

    if(!n)
        dl_swprintf(buf, len, L"error %u", err);
    else
        while(n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
            buf[--n] = 0;
}

static void update_progress_ui(Download *This)
{
    WCHAR caption[512], line[256], a[64], b[64], c[64];
    HWND dlg = This->dlg;
    DWORD now, elapsed_ms;

    if(!dlg || This->complete_shown)
        return;

    now = GetTickCount();
    elapsed_ms = now - This->start_tick;

    format_size(This->received, a, ARRAY_SIZE(a));
    if(This->total) {
        UINT pct = (UINT)min(100, This->received * 100 / This->total);

        format_dl_string(caption, ARRAY_SIZE(caption), IDS_DL_CAPTION_PCT, pct, This->name, This->host);
        SendDlgItemMessageW(dlg, IDC_DLP_BAR, PBM_SETPOS, pct, 0);

        format_size(This->total, b, ARRAY_SIZE(b));
        if(This->received && elapsed_ms) {
            ULONGLONG rate = This->received * 1000 / elapsed_ms;

            if(rate) {
                DWORD left = (DWORD)((This->total - min(This->received, This->total)) / rate);

                format_duration(left, c, ARRAY_SIZE(c));
                format_dl_string(line, ARRAY_SIZE(line), IDS_DL_TIMELEFT_VAL, c, a, b);
                SetDlgItemTextW(dlg, IDC_DLP_VALUE1, line);
            }
        }
    }else {
        format_dl_string(caption, ARRAY_SIZE(caption), IDS_DL_CAPTION_COPIED, a, This->name, This->host);
        format_dl_string(line, ARRAY_SIZE(line), IDS_DL_TIMELEFT_UNKNOWN, a);
        SetDlgItemTextW(dlg, IDC_DLP_VALUE1, line);
    }
    SetWindowTextW(dlg, caption);

    format_rate(This, line, ARRAY_SIZE(line));
    SetDlgItemTextW(dlg, IDC_DLP_VALUE3, line);

    if(now - This->last_status_tick >= 1000) {
        WCHAR status[INTERNET_MAX_URL_LENGTH + 64], progress[128];

        This->last_status_tick = now;
        if(This->total)
            dl_swprintf(progress, ARRAY_SIZE(progress), L"%u%% of %s", (UINT)min(100, This->received * 100 / This->total), b);
        else
            lstrcpynW(progress, a, ARRAY_SIZE(progress));
        format_dl_string(status, ARRAY_SIZE(status), IDS_DL_STATUS_PROGRESS, This->url, progress);
        set_status(This, status);
    }
}

static void show_complete_ui(Download *This)
{
    WCHAR text[512], a[64], c[64];
    HWND dlg = This->dlg;
    DWORD secs;

    This->end_tick = GetTickCount();
    This->complete_shown = TRUE;

    if(!dlg)
        return;

    secs = (This->end_tick - This->start_tick + 500) / 1000;

    load_dl_string(IDS_DL_CAPTION_DONE, text, ARRAY_SIZE(text));
    SetWindowTextW(dlg, text);
    load_dl_string(IDS_DL_COMPLETE, text, ARRAY_SIZE(text));
    SetDlgItemTextW(dlg, IDC_DLP_TITLE, text);

    SendDlgItemMessageW(dlg, IDC_DLP_BAR, PBM_SETMARQUEE, FALSE, 0);
    SetWindowLongW(GetDlgItem(dlg, IDC_DLP_BAR), GWL_STYLE,
                   GetWindowLongW(GetDlgItem(dlg, IDC_DLP_BAR), GWL_STYLE) & ~PBS_MARQUEE);
    SendDlgItemMessageW(dlg, IDC_DLP_BAR, PBM_SETRANGE32, 0, 100);
    SendDlgItemMessageW(dlg, IDC_DLP_BAR, PBM_SETPOS, 100, 0);

    load_dl_string(IDS_DL_DOWNLOADED, text, ARRAY_SIZE(text));
    SetDlgItemTextW(dlg, IDC_DLP_LABEL1, text);
    format_size(This->received, a, ARRAY_SIZE(a));
    format_duration(secs, c, ARRAY_SIZE(c));
    format_dl_string(text, ARRAY_SIZE(text), IDS_DL_DOWNLOADED_VAL, a, c);
    SetDlgItemTextW(dlg, IDC_DLP_VALUE1, text);

    format_rate(This, text, ARRAY_SIZE(text));
    SetDlgItemTextW(dlg, IDC_DLP_VALUE3, text);

    load_dl_string(IDS_DL_BTN_CLOSE, text, ARRAY_SIZE(text));
    SetDlgItemTextW(dlg, IDCANCEL, text);
    EnableWindow(GetDlgItem(dlg, IDC_DLP_OPEN), TRUE);
    EnableWindow(GetDlgItem(dlg, IDC_DLP_FOLDER), TRUE);
}

static void open_downloaded_file(Download *This)
{
    SHELLEXECUTEINFOW info = { sizeof(info) };

    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = This->dlg ? This->dlg : This->owner;
    info.lpFile = This->path;
    info.nShow = SW_SHOWNORMAL;

    if(!ShellExecuteExW(&info)) {
        WCHAR reason[512], err[400];

        system_error_text(GetLastError(), err, ARRAY_SIZE(err));
        format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_OPEN, err);
        show_download_error(This, reason);
    }
}

static void open_containing_folder(Download *This)
{
    WCHAR params[MAX_PATH + 16];

    dl_swprintf(params, ARRAY_SIZE(params), L"/select,\"%s\"", This->path);
    ShellExecuteW(This->dlg, L"open", L"explorer.exe", params, NULL, SW_SHOWNORMAL);
}

static void close_progress_dialog(Download *This)
{
    HWND dlg = This->dlg;

    /* destroying the dialog drops its reference */
    download_addref(This);
    if(dlg) {
        This->dlg = NULL;
        DestroyWindow(dlg);
    }
    download_signal_done(This);
    download_release_engine(This);
    download_release(This);
}

static void download_finished_ok(Download *This)
{
    WCHAR status[INTERNET_MAX_URL_LENGTH + 64];

    This->ended_ok = TRUE;
    if(This->file != INVALID_HANDLE_VALUE) {
        CloseHandle(This->file);
        This->file = INVALID_HANDLE_VALUE;
    }

    show_complete_ui(This);
    This->state = DL_ENDED;

    format_dl_string(status, ARRAY_SIZE(status), IDS_DL_STATUS_DONE, This->name);
    set_status(This, status);

    if(This->open_when_done) {
        close_progress_dialog(This);
        open_downloaded_file(This);
    }else if(This->dlg && IsDlgButtonChecked(This->dlg, IDC_DLP_CLOSE) == BST_CHECKED) {
        close_progress_dialog(This);
    }else if(!This->dlg) {
        download_signal_done(This);
    }

    download_release_engine(This);
}

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

static void download_check_finish(Download *This);

static void download_pump(Download *This)
{
    DWORD start;
    ULONG read;
    HRESULT hres;

    This->pump_posted = FALSE;
    if(This->state != DL_SAVING)
        return;

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
        if(read) {
            DWORD err;

            if(!write_all(This, This->buf, read, &err)) {
                WCHAR reason[512], text[400];

                system_error_text(err, text, ARRAY_SIZE(text));
                format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_WRITE, text);
                download_fail(This, reason);
                return;
            }
            This->received += read;
        }
        if(FAILED(hres) && hres != E_PENDING) {
            WARN("read failed %08lx\n", hres);
            break;
        }
    }while(read && GetTickCount() - start < DL_PUMP_SLICE_MS);

    update_progress_ui(This);

    if(read) {
        if(!This->pump_posted) {
            This->pump_posted = TRUE;
            PostMessageW(This->msg_hwnd, WM_DL_PUMP, 0, 0);
        }
        return;
    }

    download_check_finish(This);
}

/* all data is in the file once the binding has stopped and a read returns nothing */
static void download_check_finish(Download *This)
{
    if(This->state != DL_SAVING || !This->stopped)
        return;

    if(FAILED(This->stop_hr) && This->stop_hr != E_ABORT) {
        WCHAR reason[512];

        format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_NET, This->stop_hr);
        download_fail(This, reason);
        return;
    }

    if(This->total && This->received < This->total) {
        WCHAR reason[512], a[64], b[64];

        format_size(This->received, a, ARRAY_SIZE(a));
        format_size(This->total, b, ARRAY_SIZE(b));
        format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_INTERRUPTED, a, b);
        download_fail(This, reason);
        return;
    }

    download_finished_ok(This);
}

static INT_PTR CALLBACK progress_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    Download *This;

    switch(msg) {
    case WM_INITDIALOG: {
        WCHAR text[512], size[64];
        SHFILEINFOW info;
        HICON icon;
        HWND bar;

        This = (Download*)lparam;
        SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        SetPropW(hwnd, download_dlg_propW, (HANDLE)1);
        This->dlg = hwnd;
        download_addref(This);

        format_dl_string(text, ARRAY_SIZE(text), IDS_DL_NAME_FROM, This->name, This->host);
        SetDlgItemTextW(hwnd, IDC_DLP_NAME, text);
        SetDlgItemTextW(hwnd, IDC_DLP_VALUE2, This->path);
        load_dl_string(IDS_DL_TIMELEFT, text, ARRAY_SIZE(text));
        SetDlgItemTextW(hwnd, IDC_DLP_LABEL1, text);
        SetDlgItemTextW(hwnd, IDC_DLP_VALUE3, L"");
        format_size(0, size, ARRAY_SIZE(size));

        if(This->is_exe) {
            load_dl_string(IDS_DL_BTN_RUN, text, ARRAY_SIZE(text));
            SetDlgItemTextW(hwnd, IDC_DLP_OPEN, text);
        }

        bar = GetDlgItem(hwnd, IDC_DLP_BAR);
        SendMessageW(bar, PBM_SETRANGE32, 0, 100);
        if(!This->total) {
            SetWindowLongW(bar, GWL_STYLE, GetWindowLongW(bar, GWL_STYLE) | PBS_MARQUEE);
            SendMessageW(bar, PBM_SETMARQUEE, TRUE, 60);
        }

        if(SHGetFileInfoW(This->name, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                          SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
            icon = info.hIcon;
            SendDlgItemMessageW(hwnd, IDC_DLP_ICON, STM_SETICON, (WPARAM)icon, 0);
        }

        update_progress_ui(This);
        SetTimer(hwnd, DL_TIMER_ID, 500, NULL);
        return TRUE;
    }
    case WM_TIMER:
        This = (Download*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if(This && This->state == DL_SAVING) {
            /* the status text goes to a sink, which may run messages */
            download_addref(This);
            update_progress_ui(This);
            download_release(This);
        }
        return TRUE;
    case WM_COMMAND:
        This = (Download*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if(!This)
            return TRUE;
        switch(LOWORD(wparam)) {
        case IDCANCEL:
            if(This->state == DL_SAVING)
                download_cancel(This);
            else
                close_progress_dialog(This);
            return TRUE;
        case IDC_DLP_OPEN:
            download_addref(This);
            open_downloaded_file(This);
            download_release(This);
            return TRUE;
        case IDC_DLP_FOLDER:
            download_addref(This);
            open_containing_folder(This);
            download_release(This);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        This = (Download*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if(This) {
            if(This->state == DL_SAVING)
                download_cancel(This);
            else
                close_progress_dialog(This);
        }
        return TRUE;
    case WM_DESTROY: {
        HICON icon = (HICON)SendDlgItemMessageW(hwnd, IDC_DLP_ICON, STM_GETICON, 0, 0);

        if(icon)
            DestroyIcon(icon);
        KillTimer(hwnd, DL_TIMER_ID);
        RemovePropW(hwnd, download_dlg_propW);
        This = (Download*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if(This) {
            SetWindowLongPtrW(hwnd, DWLP_USER, 0);
            if(This->dlg == hwnd)
                This->dlg = NULL;
            download_signal_done(This);
            download_release_engine(This);
            download_release(This);
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

typedef struct {
    Download *dl;
    BOOL has_open;
    UINT warn_id;
    UINT question_id;
    UINT title_id;
} ask_params_t;

static INT_PTR CALLBACK ask_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch(msg) {
    case WM_INITDIALOG: {
        ask_params_t *params = (ask_params_t*)lparam;
        Download *This = params->dl;
        WCHAR text[1024], size[64], type[256];
        SHFILEINFOW info;
        HICON warn;

        SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        This->ask_hwnd = hwnd;

        SetWindowTextW(hwnd, load_dl_string(params->title_id, text, ARRAY_SIZE(text)));
        SetDlgItemTextW(hwnd, IDC_DLA_QUESTION, load_dl_string(params->question_id, text, ARRAY_SIZE(text)));
        SetDlgItemTextW(hwnd, IDC_DLA_WARNING, load_dl_string(params->warn_id, text, ARRAY_SIZE(text)));
        SetDlgItemTextW(hwnd, IDC_DLA_NAME, This->name);
        SetDlgItemTextW(hwnd, IDC_DLA_FROM, This->host);

        lstrcpynW(type, This->known_type ? This->type_desc : load_dl_string(IDS_DL_TYPE_UNKNOWN, text, ARRAY_SIZE(text)), ARRAY_SIZE(type));
        if(This->total) {
            format_size(This->total, size, ARRAY_SIZE(size));
            format_dl_string(text, ARRAY_SIZE(text), IDS_DL_TYPE_SIZE, type, size);
            SetDlgItemTextW(hwnd, IDC_DLA_TYPE, text);
        }else {
            SetDlgItemTextW(hwnd, IDC_DLA_TYPE, type);
        }

        if(SHGetFileInfoW(This->name, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                          SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES))
            SendDlgItemMessageW(hwnd, IDC_DLA_ICON, STM_SETICON, (WPARAM)info.hIcon, 0);

        warn = LoadIconW(NULL, (LPCWSTR)IDI_WARNING);
        SendDlgItemMessageW(hwnd, IDC_DLA_WARNICON, STM_SETICON, (WPARAM)warn, 0);

        if(This->is_exe)
            SetDlgItemTextW(hwnd, IDC_DLA_OPEN, load_dl_string(IDS_DL_BTN_RUN, text, ARRAY_SIZE(text)));
        if(!params->has_open)
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
        HICON icon = (HICON)SendDlgItemMessageW(hwnd, IDC_DLA_ICON, STM_GETICON, 0, 0);

        if(icon)
            DestroyIcon(icon);
        {
            ask_params_t *params = (ask_params_t*)GetWindowLongPtrW(hwnd, DWLP_USER);
            if(params && params->dl->ask_hwnd == hwnd)
                params->dl->ask_hwnd = NULL;
        }
        return TRUE;
    }
    }
    return FALSE;
}

static BOOL choose_save_path(Download *This, WCHAR *file)
{
    WCHAR dir[MAX_PATH], filter[512], desc[160], pattern[48], all[64], ext[32];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR *dot = PathFindExtensionW(This->name);
    WCHAR *p = filter;

    get_default_save_dir(dir);

    ext[0] = 0;
    if(dot && *dot && wcslen(dot) < ARRAY_SIZE(ext))
        lstrcpynW(ext, dot, ARRAY_SIZE(ext));

    if(ext[0]) {
        lstrcpynW(desc, This->known_type ? This->type_desc : L"", ARRAY_SIZE(desc));
        if(!desc[0]) {
            /* "BIN File", as the shell names a type without a registered description */
            WCHAR *c;

            dl_swprintf(desc, ARRAY_SIZE(desc), L"%s File", ext + 1);
            for(c = desc; *c && *c != ' '; c++)
                *c = towupper(*c);
        }
        dl_swprintf(pattern, ARRAY_SIZE(pattern), L"*%s", ext);
        p += dl_swprintf(p, ARRAY_SIZE(filter) - (p - filter), L"%s", desc) + 1;
        p += dl_swprintf(p, ARRAY_SIZE(filter) - (p - filter), L"%s", pattern) + 1;
    }
    load_dl_string(IDS_DL_SAVEAS_ALL, all, ARRAY_SIZE(all));
    p += dl_swprintf(p, ARRAY_SIZE(filter) - (p - filter), L"%s", all) + 1;
    p += dl_swprintf(p, ARRAY_SIZE(filter) - (p - filter), L"%s", L"*.*") + 1;
    *p = 0;

    lstrcpynW(file, This->name, MAX_PATH);
    {
        /* the name offered has to fit behind the folder (a separator and the end take two characters) */
        size_t room = MAX_PATH - 2 - wcslen(dir);

        if(room >= 8 && wcslen(file) > room)
            limit_file_name(file, room);
    }
    ofn.hwndOwner = This->owner && IsWindow(This->owner) ? This->owner : NULL;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir;
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    return GetSaveFileNameW(&ofn);
}

static void download_ask(Download *This)
{
    ask_params_t params;
    INT_PTR choice;
    WCHAR status[INTERNET_MAX_URL_LENGTH + 64];
    WCHAR *ext;

    if(This->asked || This->state != DL_BINDING)
        return;
    This->asked = TRUE;

    if(!This->name[0])
        lstrcpynW(This->name, L"download", ARRAY_SIZE(This->name));

    ext = PathFindExtensionW(This->name);
    This->is_exe = ext && is_executable_ext(ext);
    This->known_type = get_type_description(ext, This->type_desc, ARRAY_SIZE(This->type_desc));
    if(This->is_exe && !This->known_type) {
        lstrcpynW(This->type_desc, L"Application", ARRAY_SIZE(This->type_desc));
        This->known_type = TRUE;
    }

    params.dl = This;
    params.has_open = This->known_type;
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
    download_addref(This);
    format_dl_string(status, ARRAY_SIZE(status), IDS_DL_STATUS_START, This->url);
    set_status(This, status);

    if(This->state != DL_ASKING) {
        /* ended while a status text sink ran messages */
        download_release_engine(This);
        download_release(This);
        return;
    }

    choice = DialogBoxParamW(ieframe_instance, MAKEINTRESOURCEW(IDD_DOWNLOAD_ASK),
                             This->owner && IsWindow(This->owner) ? This->owner : NULL,
                             ask_dlg_proc, (LPARAM)&params);

    if(This->state != DL_ASKING) {
        /* ended while the dialog was open */
        download_release_engine(This);
        download_release(This);
        return;
    }

    if(choice == IDC_DLA_SAVE) {
        WCHAR file[MAX_PATH];

        if(!choose_save_path(This, file))
            choice = IDCANCEL;
        else if(This->state == DL_ASKING)
            lstrcpynW(This->path, file, ARRAY_SIZE(This->path));
    }else if(choice == IDC_DLA_OPEN) {
        This->open_when_done = TRUE;
    }

    if(This->state != DL_ASKING) {
        /* ended while the Save As dialog was open */
        download_release_engine(This);
        download_release(This);
        return;
    }

    if(choice != IDC_DLA_SAVE && choice != IDC_DLA_OPEN) {
        download_cancel(This);
        download_release(This);
        return;
    }

    if(choice == IDC_DLA_SAVE) {
        This->file = CreateFileW(This->path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        This->file_owned = This->file != INVALID_HANDLE_VALUE;
    }else {
        create_temp_file(This);
    }

    if(This->file == INVALID_HANDLE_VALUE) {
        WCHAR reason[512], text[400];

        system_error_text(GetLastError(), text, ARRAY_SIZE(text));
        format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_CREATE, text);
        This->path[0] = 0;
        download_fail(This, reason);
        download_release(This);
        return;
    }

    This->state = DL_SAVING;
    This->start_tick = GetTickCount();
    This->last_status_tick = 0;
    create_progress_dialog(This);

    if(!This->pump_posted) {
        This->pump_posted = TRUE;
        PostMessageW(This->msg_hwnd, WM_DL_PUMP, 0, 0);
    }
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
            else if(This->state == DL_ASKING && FAILED(This->stop_hr) && This->stop_hr != E_ABORT) {
                WCHAR reason[512];

                /* the connection failed while the question was open; close it and report */
                format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_NET, This->stop_hr);
                if(This->ask_hwnd)
                    EndDialog(This->ask_hwnd, IDCANCEL);
                download_cancel(This);
                show_download_error(This, reason);
            }else if(This->state == DL_BINDING) {
                if(FAILED(This->stop_hr) && This->stop_hr != E_ABORT) {
                    WCHAR reason[512];

                    format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_NET, This->stop_hr);
                    download_cancel(This);
                    show_download_error(This, reason);
                }else if(!This->asked) {
                    download_ask(This);
                }
            }
            download_release(This);
        }
        return 0;
    case WM_DL_FAIL:
        if(This) {
            WCHAR reason[512];

            download_addref(This);
            if(wparam)
                format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_HTTP, (UINT)wparam);
            else
                format_dl_string(reason, ARRAY_SIZE(reason), IDS_DL_ERR_NET, (UINT)lparam);
            if(This->ask_hwnd)
                EndDialog(This->ask_hwnd, IDCANCEL);
            download_cancel(This);
            show_download_error(This, reason);
            download_release(This);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static BOOL register_download_class(void)
{
    WNDCLASSW wc = { 0 };

    if(download_class_registered)
        return TRUE;

    wc.lpfnWndProc = download_wndproc;
    wc.hInstance = ieframe_instance;
    wc.lpszClassName = download_wndclassW;
    if(!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;
    download_class_registered = TRUE;
    return TRUE;
}

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
        if(text && *text && !This->asked) {
            free(This->url);
            This->url = wcsdup(text);
            split_url(This, text);
            sanitize_file_name(This->name);
            limit_file_name(This->name, ARRAY_SIZE(This->name) - 2);
        }
        break;
    case BINDSTATUS_BEGINDOWNLOADDATA:
        if(progress_max)
            This->total = progress_max;
        if(!This->asked)
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
        void *src, *dst;

        if(!copy)
            return E_OUTOFMEMORY;

        src = GlobalLock(This->post_data);
        dst = GlobalLock(copy);
        memcpy(dst, src, This->post_data_len);
        ((BYTE*)dst)[This->post_data_len] = 0;
        GlobalUnlock(copy);
        GlobalUnlock(This->post_data);

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

    if(This->state == DL_SAVING && !This->pump_posted) {
        This->pump_posted = TRUE;
        PostMessageW(This->msg_hwnd, WM_DL_PUMP, 0, 0);
    }
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
    if(This->headers && *This->headers) {
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
    const WCHAR *cd;

    TRACE("(%p)->(%lu %s)\n", This, code, debugstr_w(response_headers));

    if(code >= 400) {
        PostMessageW(This->msg_hwnd, WM_DL_FAIL, code, 0);
        return S_OK;
    }

    if(response_headers && !This->asked) {
        cd = StrStrIW(response_headers, L"\nContent-Disposition:");
        if(cd) {
            WCHAR value[512], *end;

            cd += 21;
            lstrcpynW(value, cd, ARRAY_SIZE(value));
            end = wcschr(value, '\r');
            if(end)
                *end = 0;
            end = wcschr(value, '\n');
            if(end)
                *end = 0;
            WCHAR file[MAX_PATH];

            if(parse_content_disposition(value, file, ARRAY_SIZE(file))) {
                lstrcpynW(This->name, file, ARRAY_SIZE(This->name));
                limit_file_name(This->name, ARRAY_SIZE(This->name) - 2);
                TRACE("file name from Content-Disposition: %s\n", debugstr_w(This->name));
            }
        }
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

static Download *create_download(DocHost *doc_host, const WCHAR *url, const WCHAR *headers,
        const BYTE *post_data, DWORD post_data_len)
{
    Download *This;

    if(!register_download_class())
        return NULL;

    This = calloc(1, sizeof(*This));
    if(!This)
        return NULL;

    This->IBindStatusCallback_iface.lpVtbl = &DownloadVtbl;
    This->IHttpNegotiate_iface.lpVtbl = &DownloadHttpNegotiateVtbl;
    This->ref = 1;
    This->engine_ref = TRUE;
    This->file = INVALID_HANDLE_VALUE;
    This->state = DL_BINDING;
    This->url = wcsdup(url);
    This->headers = headers && *headers ? wcsdup(headers) : NULL;
    if(headers && *headers && !This->headers) {
        free_download(This);
        return NULL;
    }

    if(post_data && post_data_len) {
        This->post_data = GlobalAlloc(GMEM_MOVEABLE, post_data_len);
        if(This->post_data) {
            void *dst = GlobalLock(This->post_data);

            memcpy(dst, post_data, post_data_len);
            GlobalUnlock(This->post_data);
            This->post_data_len = post_data_len;
        }
    }

    if(!This->url || (post_data_len && !This->post_data)) {
        free_download(This);
        return NULL;
    }

    split_url(This, url);
    sanitize_file_name(This->name);
    limit_file_name(This->name, ARRAY_SIZE(This->name) - 2);

    if(doc_host) {
        This->doc_host = doc_host;
        IOleClientSite_AddRef(&doc_host->IOleClientSite_iface);
        This->owner = get_owner_window(doc_host);
        This->process_lock = lock_ie_process(doc_host);
    }

    This->msg_hwnd = CreateWindowExW(0, download_wndclassW, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE,
                                     NULL, ieframe_instance, This);
    if(!This->msg_hwnd) {
        free_download(This);
        return NULL;
    }

    return This;
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

/* Called for content the document cannot show and for attachments. */
HRESULT start_file_download(DocHost *doc_host, const WCHAR *url, const WCHAR *headers,
        const BYTE *post_data, DWORD post_data_len)
{
    Download *This;
    HRESULT hres;

    TRACE("(%p %s)\n", doc_host, debugstr_w(url));

    This = create_download(doc_host, url, headers, post_data, post_data_len);
    if(!This)
        return E_OUTOFMEMORY;

    hres = start_download_binding(This);
    if(FAILED(hres) && hres != MK_S_ASYNCHRONOUS) {
        download_end(This);
        return hres;
    }

    /* the object keeps itself alive until the download and its dialogs are over */
    return S_OK;
}

/* The browser's message loop gives the keys of the progress dialogs (Tab, Enter, Escape) to them. */
BOOL download_translate_message(MSG *msg)
{
    HWND root;

    if(!msg->hwnd || (msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST))
        return FALSE;

    root = GetAncestor(msg->hwnd, GA_ROOT);
    if(!root || !GetPropW(root, download_dlg_propW))
        return FALSE;

    return IsDialogMessageW(root, msg);
}

/* The notifications of urlmon that are still queued own references of the binding and of the download: they run
   before the apartment goes, and until urlmon has reported the end of the binding (the caller holds a reference of the
   download for that); a quit message that is queued ahead of them is kept for the caller. A binding that never ends
   does not hold the thread for more than DL_DRAIN_MAX_MS. */
#define DL_DRAIN_MAX_MS 30000

static void drain_download_messages(Download *This)
{
    DWORD start = GetTickCount();
    BOOL quit = FALSE;
    WPARAM code = 0;
    int idle = 0;
    MSG msg;

    while(GetTickCount() - start < DL_DRAIN_MAX_MS) {
        if(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if(msg.message == WM_QUIT) {
                quit = TRUE;
                code = msg.wParam;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            idle = 0;
        }else if(This->start_reported && !This->stop_reported) {
            MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
        }else if(++idle > 3) {
            break;
        }else {
            Sleep(5);
        }
    }

    if(quit)
        PostQuitMessage((int)code);
}

/***********************************************************************
 *          DoFileDownload (IEFRAME.@)
 *
 * Downloads the file behind an Internet address: asks what to do with it, saves it and
 * shows the progress. Returns when the download has ended and its dialog is closed.
 */
BOOL WINAPI DoFileDownload(LPWSTR url)
{
    Download *This;
    HRESULT coinit;
    BOOL ret;
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

    download_addref(This);
    This->standalone = TRUE;
    if(FAILED(start_download_binding(This))) {
        download_end(This);
        drain_download_messages(This);
        download_release(This);
        if(SUCCEEDED(coinit))
            CoUninitialize();
        return FALSE;
    }

    while(!This->dialog_closed && !(This->state == DL_ENDED && !This->dlg)) {
        if(!GetMessageW(&msg, NULL, 0, 0)) {
            PostQuitMessage(msg.wParam);
            break;
        }
        if(This->dlg && IsDialogMessageW(This->dlg, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ret = This->ended_ok;
    download_abort_binding(This);
    download_end(This);
    /* left the loop with a dialog still open (WM_QUIT) */
    if(This->dlg)
        close_progress_dialog(This);
    if(This->ask_hwnd)
        EndDialog(This->ask_hwnd, IDCANCEL);
    download_release_engine(This);

    /* the reference of this function is kept until urlmon is done with the download */
    drain_download_messages(This);
    download_release(This);
    if(SUCCEEDED(coinit))
        CoUninitialize();
    return ret;
}
