/*
 * File names and files of the browser's downloads
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <shlwapi.h>

#include "download_name.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

NTSYSAPI ULONG NTAPI RtlIsDosDeviceName_U(PCWSTR Name);

int dl_snprintf(WCHAR *buf, size_t cap, const WCHAR *fmt, ...)
{
    va_list args;

    if(!cap)
        return 0;
    va_start(args, fmt);
    _vsnwprintf(buf, cap, fmt, args);
    va_end(args);
    buf[cap - 1] = 0;
    return wcslen(buf);
}

static int hex_value(WCHAR c)
{
    if(c >= '0' && c <= '9')
        return c - '0';
    if(c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if(c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* %XX at s; a zero byte becomes '_' */
static BOOL percent_byte(const WCHAR *s, char *out)
{
    int hi, lo;

    if(s[0] != '%' || !s[1] || !s[2])
        return FALSE;
    hi = hex_value(s[1]);
    lo = hex_value(s[2]);
    if(hi < 0 || lo < 0)
        return FALSE;
    *out = (hi << 4) | lo;
    if(!*out)
        *out = '_';
    return TRUE;
}

/* the bytes are UTF-8 when they are valid UTF-8 and one character per byte otherwise */
static void decode_bytes(const char *bytes, size_t n, WCHAR *out, size_t cap)
{
    WCHAR *tmp;
    int need;
    size_t i, len;

    if(!cap)
        return;
    out[0] = 0;
    if(!n)
        return;

    need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, n, NULL, 0);
    if(need > 0) {
        tmp = malloc(need * sizeof(WCHAR));
        if(!tmp)
            return;
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, n, tmp, need);
        len = min((size_t)need, cap - 1);
        /* do not cut a surrogate pair in two */
        if(len < (size_t)need && len && tmp[len - 1] >= 0xd800 && tmp[len - 1] < 0xdc00)
            len--;
        memcpy(out, tmp, len * sizeof(WCHAR));
        out[len] = 0;
        free(tmp);
        return;
    }

    len = min(n, cap - 1);
    for(i = 0; i < len; i++)
        out[i] = (unsigned char)bytes[i];
    out[len] = 0;
}

static void percent_decode(const WCHAR *src, WCHAR *out, size_t cap)
{
    size_t i, n = 0, len = wcslen(src);
    char *bytes = malloc(len * 4 + 1);

    if(!cap)
        return;
    out[0] = 0;
    if(!bytes)
        return;

    for(i = 0; i < len; i++) {
        if(percent_byte(src + i, bytes + n)) {
            n++;
            i += 2;
        }else if(src[i] < 0x80) {
            bytes[n++] = (char)src[i];
        }else {
            int pair = src[i] >= 0xd800 && src[i] < 0xdc00 && src[i + 1] >= 0xdc00 && src[i + 1] < 0xe000;
            int k = WideCharToMultiByte(CP_UTF8, 0, src + i, pair ? 2 : 1, bytes + n, 4, NULL, NULL);

            n += k;
            if(pair)
                i++;
        }
    }

    decode_bytes(bytes, n, out, cap);
    free(bytes);
}

void dl_split_url(const WCHAR *url, WCHAR *host, size_t host_cap, WCHAR *name, size_t name_cap)
{
    WCHAR *tmp, *p, *path, *last;

    if(host_cap)
        host[0] = 0;
    if(name_cap)
        name[0] = 0;

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
        WCHAR *authority = p + 3, *end = authority, *at;

        while(*end && *end != '/' && *end != '\\')
            end++;
        path = end;
        at = authority;
        for(p = authority; p < end; p++)
            if(*p == '@')
                at = p + 1;
        authority = at;
        if(host_cap) {
            size_t len = min((size_t)(end - authority), host_cap - 1);

            memcpy(host, authority, len * sizeof(WCHAR));
            host[len] = 0;
        }
    }else {
        path = tmp;
    }

    last = path;
    for(p = path; *p; p++)
        if(*p == '/' || *p == '\\')
            last = p + 1;
    percent_decode(last, name, name_cap);
    free(tmp);
}

BOOL dl_find_header(const WCHAR *headers, const WCHAR *name, WCHAR *value, size_t cap)
{
    size_t name_len = wcslen(name);
    const WCHAR *line = headers;

    while(*line) {
        const WCHAR *eol = line, *v;

        while(*eol && *eol != '\r' && *eol != '\n')
            eol++;
        if(!wcsnicmp(line, name, name_len) && line[name_len] == ':') {
            v = line + name_len + 1;
            while(v < eol && (*v == ' ' || *v == '\t'))
                v++;
            if((size_t)(eol - v) >= cap)
                return FALSE;
            memcpy(value, v, (eol - v) * sizeof(WCHAR));
            value[eol - v] = 0;
            return TRUE;
        }
        line = eol;
        while(*line == '\r' || *line == '\n')
            line++;
    }
    return FALSE;
}

/* The next parameter of "type; key=value; key=\"quoted value\"": key and val are copied out (both buffers hold as many
   characters as the whole string); *pp moves past it. FALSE at the end. */
static BOOL next_parameter(const WCHAR **pp, WCHAR *key, WCHAR *val)
{
    const WCHAR *p = *pp;
    WCHAR *out;

    while(*p == ' ' || *p == '\t' || *p == ';')
        p++;
    if(!*p)
        return FALSE;

    out = key;
    while(*p && *p != '=' && *p != ';')
        *out++ = *p++;
    while(out > key && (out[-1] == ' ' || out[-1] == '\t'))
        out--;
    *out = 0;

    val[0] = 0;
    if(*p == '=') {
        p++;
        while(*p == ' ' || *p == '\t')
            p++;
        out = val;
        if(*p == '"') {
            /* only \" and \\ are escapes: a backslash before anything else is a path separator, which is cut off later */
            for(p++; *p && *p != '"'; p++) {
                if(*p == '\\' && (p[1] == '"' || p[1] == '\\'))
                    p++;
                *out++ = *p;
            }
            if(*p == '"')
                p++;
        }else {
            while(*p && *p != ';')
                *out++ = *p++;
            while(out > val && (out[-1] == ' ' || out[-1] == '\t'))
                out--;
        }
        *out = 0;
    }
    while(*p && *p != ';')
        p++;

    *pp = p;
    return TRUE;
}

/* RFC 5987: charset'language'percent-encoded; only UTF-8 and ISO-8859-1 are understood */
static BOOL decode_extended_value(const WCHAR *val, WCHAR *out, size_t cap)
{
    const WCHAR *quote = wcschr(val, '\''), *text;
    BOOL latin1;
    size_t i, n = 0, len;
    char *bytes;

    if(!quote)
        return FALSE;
    latin1 = quote - val == 10 && !wcsnicmp(val, L"ISO-8859-1", 10);
    if(!latin1 && !(quote - val == 5 && !wcsnicmp(val, L"UTF-8", 5)))
        return FALSE;
    text = wcschr(quote + 1, '\'');
    if(!text)
        return FALSE;
    text++;

    len = wcslen(text);
    bytes = malloc(len + 1);
    if(!bytes)
        return FALSE;
    for(i = 0; i < len; i++) {
        if(percent_byte(text + i, bytes + n)) {
            n++;
            i += 2;
        }else if(text[i] < 0x80) {
            bytes[n++] = (char)text[i];
        }else {
            free(bytes);
            return FALSE;
        }
    }

    if(latin1) {
        len = min(n, cap - 1);
        for(i = 0; i < len; i++)
            out[i] = (unsigned char)bytes[i];
        out[len] = 0;
    }else {
        decode_bytes(bytes, n, out, cap);
    }
    free(bytes);
    return out[0] != 0;
}

/* a header value that carries UTF-8 bytes one per character is turned into the characters */
static void decode_plain_value(const WCHAR *val, WCHAR *out, size_t cap)
{
    size_t i, len = wcslen(val);
    BOOL bytes_only = TRUE, high = FALSE;
    char *bytes;

    for(i = 0; i < len; i++) {
        if(val[i] >= 0x100)
            bytes_only = FALSE;
        else if(val[i] >= 0x80)
            high = TRUE;
    }

    bytes = (bytes_only && high) ? malloc(len + 1) : NULL;
    if(bytes) {
        int need;

        for(i = 0; i < len; i++)
            bytes[i] = (char)val[i];
        need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, len, NULL, 0);
        if(need > 0) {
            decode_bytes(bytes, len, out, cap);
            free(bytes);
            return;
        }
        free(bytes);
    }
    lstrcpynW(out, val, cap);
}

BOOL dl_name_from_disposition(const WCHAR *value, WCHAR *name, size_t cap)
{
    const WCHAR *p = value;
    WCHAR *key, *val, *plain, *ext;
    BOOL have_plain = FALSE, have_ext = FALSE, ret = FALSE;
    size_t size = wcslen(value) + 1;

    if(!cap)
        return FALSE;
    name[0] = 0;

    key = malloc(size * sizeof(WCHAR));
    val = malloc(size * sizeof(WCHAR));
    plain = malloc(size * sizeof(WCHAR));
    ext = malloc(size * sizeof(WCHAR));
    if(!key || !val || !plain || !ext)
        goto done;

    /* the first item is the type (attachment, inline) */
    while(*p && *p != ';')
        p++;

    while(next_parameter(&p, key, val)) {
        if(!wcsicmp(key, L"filename*")) {
            if(!have_ext && decode_extended_value(val, ext, size))
                have_ext = TRUE;
        }else if(!wcsicmp(key, L"filename")) {
            if(!have_plain && val[0]) {
                decode_plain_value(val, plain, size);
                have_plain = plain[0] != 0;
            }
        }
    }

    if(have_ext) {
        lstrcpynW(name, ext, cap);
        ret = TRUE;
    }else if(have_plain) {
        lstrcpynW(name, plain, cap);
        ret = TRUE;
    }

done:
    free(key);
    free(val);
    free(plain);
    free(ext);
    return ret;
}

static void trim_tail(WCHAR *name)
{
    size_t len = wcslen(name);

    while(len && (name[len - 1] == '.' || name[len - 1] == ' '))
        name[--len] = 0;
}

/* cuts name to max characters: the base goes and the extension stays; an extension that leaves no room for a
   character of the base is cut instead */
static void cut_keep_extension(WCHAR *name, size_t max)
{
    WCHAR *ext = PathFindExtensionW(name);
    size_t len = wcslen(name), ext_len = wcslen(ext);

    if(len <= max)
        return;
    if(ext_len < max)
        memmove(name + max - ext_len, ext, (ext_len + 1) * sizeof(WCHAR));
    else
        name[max] = 0;
    trim_tail(name);
}

void dl_make_leaf(WCHAR *name)
{
    WCHAR *c, *base = name;

    for(c = name; *c; c++)
        if(*c == '/' || *c == '\\')
            base = c + 1;
    if(base != name)
        memmove(name, base, (wcslen(base) + 1) * sizeof(WCHAR));

    /* the characters that turn the text around (U+200E, U+200F, U+202A-U+202E, U+2066-U+2069) could make a name read as
       another one, so they go too */
    for(c = name; *c; c++)
        if(*c < ' ' || wcschr(L"<>:\"|?*", *c) || *c == 0x200e || *c == 0x200f || (*c >= 0x202a && *c <= 0x202e)
           || (*c >= 0x2066 && *c <= 0x2069))
            *c = '_';

    trim_tail(name);
    cut_keep_extension(name, DL_LEAF_MAX);
    if(!name[0])
        lstrcpyW(name, L"download");
}

/* the longest name that fits behind dir: the limit of one name and of MAX_PATH, less the room kept free for reserve */
static size_t name_room(const WCHAR *dir, size_t reserve)
{
    size_t dir_len = wcslen(dir), room = DL_LEAF_MAX;

    if(dir_len + reserve + 1 >= MAX_PATH || reserve >= DL_LEAF_MAX)
        return 0;
    if(room > MAX_PATH - 1 - dir_len)
        room = MAX_PATH - 1 - dir_len;
    return room > reserve ? room - reserve : 0;
}

BOOL dl_fit_leaf(const WCHAR *dir, WCHAR *name, size_t reserve)
{
    size_t room = name_room(dir, reserve);

    if(!room)
        return FALSE;
    cut_keep_extension(name, room);
    return name[0] != 0;
}

BOOL dl_path_opens_device(const WCHAR *path)
{
    return RtlIsDosDeviceName_U(path) != 0;
}

/* The check is made for dir and for a folder of a local drive: the system sees no device in a path on a network share,
   and the name may end up on a local drive. */
static BOOL opens_device(const WCHAR *dir, const WCHAR *name)
{
    static const WCHAR local_dirW[] = L"C:\\";
    WCHAR path[MAX_PATH];

    if(wcslen(dir) + wcslen(name) < ARRAY_SIZE(path)) {
        lstrcpyW(path, dir);
        lstrcatW(path, name);
        if(dl_path_opens_device(path))
            return TRUE;
    }
    if(wcslen(local_dirW) + wcslen(name) < ARRAY_SIZE(path)) {
        lstrcpyW(path, local_dirW);
        lstrcatW(path, name);
        return dl_path_opens_device(path);
    }
    return FALSE;
}

void dl_unmask_device(const WCHAR *dir, WCHAR *name)
{
    WCHAR *end;

    if(!opens_device(dir, name))
        return;

    for(end = name; *end && *end != '.'; end++)
        ;
    if(end > name)
        end[-1] = '_';
    if(opens_device(dir, name))
        lstrcpyW(name, L"download");
}

BOOL dl_get_temp_dir(WCHAR *dir)
{
    DWORD len = GetTempPathW(MAX_PATH, dir);

    return len && len < MAX_PATH;
}

HANDLE dl_create_unique_file(const WCHAR *dir, const WCHAR *leaf, WCHAR *path)
{
    WCHAR base[DL_LEAF_MAX + 1], ext[DL_LEAF_MAX + 1], name[MAX_PATH], suffix[16];
    size_t ext_len, base_len, room;
    WCHAR *dot;
    unsigned i;

    lstrcpynW(base, leaf, ARRAY_SIZE(base));
    dot = PathFindExtensionW(base);
    lstrcpynW(ext, dot, ARRAY_SIZE(ext));
    *dot = 0;
    ext_len = wcslen(ext);
    base_len = wcslen(base);

    for(i = 0; i < 1000; i++) {
        HANDLE file;
        DWORD err;
        size_t suffix_len;

        suffix[0] = 0;
        if(i)
            dl_snprintf(suffix, ARRAY_SIZE(suffix), L" (%u)", i);
        suffix_len = wcslen(suffix);

        /* the room for the base: the limit of one name and of MAX_PATH, less the number and the extension */
        room = name_room(dir, suffix_len + ext_len);
        if(!room) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return INVALID_HANDLE_VALUE;
        }

        memcpy(name, base, min(base_len, room) * sizeof(WCHAR));
        name[min(base_len, room)] = 0;
        lstrcatW(name, suffix);
        lstrcatW(name, ext);
        trim_tail(name);
        if(!name[0])
            lstrcpyW(name, L"download");

        dl_unmask_device(dir, name);

        lstrcpyW(path, dir);
        lstrcatW(path, name);
        file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if(file != INVALID_HANDLE_VALUE)
            return file;

        err = GetLastError();
        if(err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
            return INVALID_HANDLE_VALUE;
    }

    SetLastError(ERROR_FILE_EXISTS);
    return INVALID_HANDLE_VALUE;
}

BOOL dl_is_executable_ext(const WCHAR *ext)
{
    static const WCHAR *exts[] = { L".exe", L".com", L".bat", L".cmd", L".scr", L".pif", L".msi", L".cpl", L".vbs",
                                   L".js", L".jse", L".wsf", L".reg", L".hta", L".lnk" };
    size_t i;

    for(i = 0; i < ARRAY_SIZE(exts); i++)
        if(!wcsicmp(ext, exts[i]))
            return TRUE;
    return FALSE;
}
