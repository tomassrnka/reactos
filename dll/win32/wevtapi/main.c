/*
 * Copyright 2012 Austin English
 * Copyright 2012 André Hentschel
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
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winerror.h"
#include "winevt.h"
#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

/*
 * There is no event log service behind this library, which reads no event and writes none. What
 * it knows about are the classic logs that the registry lists under the EventLog key, which it
 * offers as channels. The handles, the arguments and the channel names are checked the way
 * Windows checks them. A call that would need the service fails with the error of an RPC server
 * that cannot be reached, after those checks.
 */

enum object_type
{
    OBJECT_SESSION = 1,
    OBJECT_CHANNEL_ENUM,
    OBJECT_CHANNEL_CONFIG,
    OBJECT_RENDER_CONTEXT,
    OBJECT_BOOKMARK,
    OBJECT_PUBLISHER_ENUM,
};

struct bookmark_entry
{
    WCHAR *channel;
    ULONGLONG record_id;
    BOOL has_record_id;
    BOOL is_current;
    BOOL has_is_current;
};

struct object
{
    struct list entry;
    enum object_type type;
    BOOL remote;                    /* session: the server is another machine */
    DWORD index;                    /* channel enumeration: the next key */
    WCHAR *name;                    /* channel configuration: the channel */
    struct bookmark_entry *marks;   /* bookmark */
    DWORD mark_count;
    WCHAR *direction;               /* bookmark: the Direction attribute of the list, if it has one */
};

static CRITICAL_SECTION object_cs;
static CRITICAL_SECTION_DEBUG object_cs_debug =
{
    0, 0, &object_cs,
    { &object_cs_debug.ProcessLocksList, &object_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": object_cs") }
};
static CRITICAL_SECTION object_cs = { &object_cs_debug, -1, 0, 0, 0, 0 };

static struct list object_list = LIST_INIT(object_list);

static const WCHAR eventlog_key[] = L"System\\CurrentControlSet\\Services\\EventLog";
static const WCHAR default_log_dir[] = L"%SystemRoot%\\System32\\Winevt\\Logs\\";

static BOOL fail(DWORD error)
{
    SetLastError(error);
    return FALSE;
}

static struct object *find_object(EVT_HANDLE handle, enum object_type type)
{
    struct object *object;

    LIST_FOR_EACH_ENTRY(object, &object_list, struct object, entry)
        if ((EVT_HANDLE)object == handle)
            return (type == 0 || object->type == type) ? object : NULL;
    return NULL;
}

static EVT_HANDLE new_object(enum object_type type, BOOL remote, const WCHAR *name)
{
    struct object *object = calloc(1, sizeof(*object));

    if (!object)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    object->type = type;
    object->remote = remote;
    if (name && !(object->name = _wcsdup(name)))
    {
        free(object);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    EnterCriticalSection(&object_cs);
    list_add_head(&object_list, &object->entry);
    LeaveCriticalSection(&object_cs);
    return (EVT_HANDLE)object;
}

static void free_marks(struct bookmark_entry *marks, DWORD count);

static void free_object(struct object *object)
{
    free_marks(object->marks, object->mark_count);
    free(object->direction);
    free(object->name);
    free(object);
}

/* A handle that is NULL or a session. Sets the last error and returns FALSE for anything else. */
static BOOL get_session(EVT_HANDLE session, BOOL *remote)
{
    struct object *object;
    BOOL ret = TRUE;

    *remote = FALSE;
    if (!session) return TRUE;

    EnterCriticalSection(&object_cs);
    object = find_object(session, OBJECT_SESSION);
    if (object) *remote = object->remote;
    else ret = FALSE;
    LeaveCriticalSection(&object_cs);

    if (!ret) SetLastError(ERROR_INVALID_HANDLE);
    return ret;
}

static BOOL is_local_server(const WCHAR *server)
{
    WCHAR name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = ARRAY_SIZE(name);

    if (!server || !server[0] || !wcscmp(server, L".") || !_wcsicmp(server, L"localhost")) return TRUE;
    return GetComputerNameW(name, &size) && !_wcsicmp(server, name);
}

static BOOL no_service(void)
{
    return fail(RPC_S_SERVER_UNAVAILABLE);
}

static BOOL is_bad_channel_char(WCHAR c)
{
    return c < ' ' || c == '<' || c == '>' || c == '"' || c == '&' || c == '|';
}

/* ERROR_SUCCESS when the classic log exists, else the error Windows gives for the name */
static DWORD check_channel(const WCHAR *name)
{
    HKEY root, key;
    const WCHAR *p;
    LSTATUS status;

    if (!name || !name[0]) return ERROR_EVT_INVALID_CHANNEL_PATH;
    for (p = name; *p; p++)
        if (is_bad_channel_char(*p)) return ERROR_EVT_INVALID_CHANNEL_PATH;
    if (wcschr(name, '\\')) return ERROR_EVT_CHANNEL_NOT_FOUND;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, eventlog_key, 0, KEY_READ, &root)) return ERROR_EVT_CHANNEL_NOT_FOUND;
    status = RegOpenKeyExW(root, name, 0, KEY_READ, &key);
    if (!status) RegCloseKey(key);
    RegCloseKey(root);
    return status ? ERROR_EVT_CHANNEL_NOT_FOUND : ERROR_SUCCESS;
}

/* A reader for the small subset of XML that structured queries and bookmarks use */

#define XML_MAX_NAME  64
#define XML_MAX_ATTRS 16
#define XML_MAX_DEPTH 32

struct xml_attr
{
    WCHAR name[XML_MAX_NAME];
    WCHAR *value;
};

struct xml_tag
{
    WCHAR name[XML_MAX_NAME];
    struct xml_attr attrs[XML_MAX_ATTRS];
    DWORD attr_count;
    BOOL closing;
    BOOL empty;
    BOOL junk;      /* there was text other than white space before the tag */
};

enum { XML_ERROR = -1, XML_END = 0, XML_TAG = 1 };

static void free_tag(struct xml_tag *tag)
{
    DWORD i;

    for (i = 0; i < tag->attr_count; i++)
        free(tag->attrs[i].value);
    tag->attr_count = 0;
}

static BOOL is_xml_space(WCHAR c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static BOOL is_name_char(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
}

static BOOL read_name(const WCHAR **p, WCHAR *name)
{
    DWORD len = 0;

    while (is_name_char(**p))
    {
        if (len + 1 >= XML_MAX_NAME) return FALSE;
        name[len++] = *(*p)++;
    }
    name[len] = 0;
    return len != 0;
}

/* Writes the character of a numeric reference as UTF-16. Returns the number of characters, or 0 for a number that is no character. */
static int encode_char(ULONG ch, WCHAR *out)
{
    if (!ch || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff) || ch == 0xfffe || ch == 0xffff || (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r'))
        return 0;
    if (ch < 0x10000)
    {
        out[0] = ch;
        return 1;
    }
    ch -= 0x10000;
    out[0] = 0xd800 | (ch >> 10);
    out[1] = 0xdc00 | (ch & 0x3ff);
    return 2;
}

/* The text of an attribute value with its references resolved, or NULL for text that is not valid */
static WCHAR *decode_value(const WCHAR *start, const WCHAR *end)
{
    WCHAR *value = calloc(end - start + 1, sizeof(WCHAR)), *out = value;

    if (!value) return NULL;
    while (start < end)
    {
        if (*start != '&')
        {
            *out++ = *start++;
            continue;
        }
        if (end - start >= 4 && !wcsncmp(start, L"&lt;", 4)) { *out++ = '<'; start += 4; }
        else if (end - start >= 4 && !wcsncmp(start, L"&gt;", 4)) { *out++ = '>'; start += 4; }
        else if (end - start >= 5 && !wcsncmp(start, L"&amp;", 5)) { *out++ = '&'; start += 5; }
        else if (end - start >= 6 && !wcsncmp(start, L"&quot;", 6)) { *out++ = '"'; start += 6; }
        else if (end - start >= 6 && !wcsncmp(start, L"&apos;", 6)) { *out++ = '\''; start += 6; }
        else if (end - start >= 4 && start[1] == '#')
        {
            const WCHAR *p = start + 2;
            ULONG ch = 0, base = 10;
            int len;

            if (*p == 'x') { base = 16; p++; }
            if (p >= end || *p == ';') goto error;
            for (; p < end && *p != ';'; p++)
            {
                ULONG digit = *p >= '0' && *p <= '9' ? *p - '0' : base == 16 && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
                              base == 16 && *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 16;
                if (digit >= base || ch > 0x10ffff) goto error;
                ch = ch * base + digit;
            }
            if (p >= end || !(len = encode_char(ch, out))) goto error;
            out += len;
            start = p + 1;
        }
        else goto error;
    }
    *out = 0;
    return value;

error:
    free(value);
    return NULL;
}

/* Reads the next tag, skipping text, comments and processing instructions. Text that is not white space is noted in the tag. */
static int next_tag(const WCHAR **text, struct xml_tag *tag)
{
    const WCHAR *p = *text, *end;
    BOOL junk = FALSE;

    memset(tag, 0, sizeof(*tag));
    for (;;)
    {
        while (*p && *p != '<')
        {
            if (*p == '&')
            {
                /* a reference in text must be one that is known */
                WCHAR *value;

                for (end = p; *end && *end != ';' && *end != '<' && end - p <= 12; end++) {}
                if (*end != ';' || !(value = decode_value(p, end + 1))) return XML_ERROR;
                free(value);
                junk = TRUE;
                p = end + 1;
                continue;
            }
            if (!is_xml_space(*p)) junk = TRUE;
            p++;
        }
        if (!*p)
        {
            *text = p;
            tag->junk = junk;
            return XML_END;
        }
        if (!wcsncmp(p, L"<!--", 4))
        {
            if (!(end = wcsstr(p + 4, L"-->"))) return XML_ERROR;
            p = end + 3;
        }
        else if (!wcsncmp(p, L"<?", 2))
        {
            if (!(end = wcsstr(p + 2, L"?>"))) return XML_ERROR;
            p = end + 2;
        }
        else if (!wcsncmp(p, L"<![CDATA[", 9))
        {
            if (!(end = wcsstr(p + 9, L"]]>"))) return XML_ERROR;
            junk = TRUE;
            p = end + 3;
        }
        else break;
    }
    tag->junk = junk;

    p++;
    if (*p == '/')
    {
        p++;
        tag->closing = TRUE;
    }
    if (!read_name(&p, tag->name)) return XML_ERROR;
    if (!is_xml_space(*p) && *p != '>' && *p != '/') return XML_ERROR;

    for (;;)
    {
        struct xml_attr *attr;
        WCHAR quote;

        while (is_xml_space(*p)) p++;
        if (*p == '>')
        {
            p++;
            break;
        }
        if (*p == '/' && !tag->closing)
        {
            if (p[1] != '>') goto error;
            p += 2;
            tag->empty = TRUE;
            break;
        }
        if (tag->closing || tag->attr_count == XML_MAX_ATTRS) goto error;

        attr = &tag->attrs[tag->attr_count];
        if (!read_name(&p, attr->name)) goto error;
        while (is_xml_space(*p)) p++;
        if (*p++ != '=') goto error;
        while (is_xml_space(*p)) p++;
        quote = *p++;
        if (quote != '"' && quote != '\'') goto error;
        for (end = p; *end && *end != quote && *end != '<'; end++) {}
        if (*end != quote) goto error;
        if (!(attr->value = decode_value(p, end))) goto error;
        tag->attr_count++;
        p = end + 1;
        /* the next attribute, or the end of the tag, follows after white space */
        if (!is_xml_space(*p) && *p != '>' && *p != '/') goto error;
    }

    *text = p;
    return XML_TAG;

error:
    free_tag(tag);
    return XML_ERROR;
}

static const WCHAR *find_attr(const struct xml_tag *tag, const WCHAR *name)
{
    DWORD i;

    for (i = 0; i < tag->attr_count; i++)
        if (!wcscmp(tag->attrs[i].name, name)) return tag->attrs[i].value;
    return NULL;
}

static BOOL has_duplicate_attr(const struct xml_tag *tag)
{
    DWORD i, j;

    for (i = 0; i < tag->attr_count; i++)
        for (j = i + 1; j < tag->attr_count; j++)
            if (!wcscmp(tag->attrs[i].name, tag->attrs[j].name)) return TRUE;
    return FALSE;
}

/* A structured query is XML whose first element is QueryList, after any declaration or comment */
static BOOL is_query_list(const WCHAR *query)
{
    struct xml_tag tag;
    const WCHAR *p = query;
    BOOL ret;
    int status;

    if (!query) return FALSE;
    status = next_tag(&p, &tag);
    ret = status == XML_TAG && !tag.closing && !wcscmp(tag.name, L"QueryList");
    if (status == XML_TAG) free_tag(&tag);
    return ret;
}

static DWORD check_path(const WCHAR *path, BOOL file)
{
    if (!file) return check_channel(path);
    if (!path[0]) return ERROR_EVT_INVALID_CHANNEL_PATH;
    return GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
}

/* The paths that a structured query selects. ERROR_SUCCESS when the query is well formed and all of them exist. */
static DWORD check_query_list(const WCHAR *query, BOOL file)
{
    WCHAR open[XML_MAX_DEPTH][XML_MAX_NAME];
    WCHAR *query_path = NULL;
    const WCHAR *p = query;
    struct xml_tag tag;
    DWORD depth = 0, error = ERROR_SUCCESS;
    BOOL found = FALSE, root = FALSE, select = FALSE;
    int ret;

    while (!error && (ret = next_tag(&p, &tag)) == XML_TAG)
    {
        const WCHAR *path;

        /* text belongs inside the element of a query, not around the document */
        if ((tag.junk && !depth) || has_duplicate_attr(&tag)) error = ERROR_EVT_MALFORMED_XML_TEXT;
        else if (tag.closing)
        {
            if (!depth || wcscmp(open[--depth], tag.name)) error = ERROR_EVT_MALFORMED_XML_TEXT;
            else if (!wcscmp(tag.name, L"Query"))
            {
                free(query_path);
                query_path = NULL;
            }
        }
        else
        {
            if (!depth && root) error = ERROR_EVT_MALFORMED_XML_TEXT;
            else if (!root && wcscmp(tag.name, L"QueryList")) error = ERROR_EVT_INVALID_QUERY;
            root = TRUE;

            path = find_attr(&tag, L"Path");
            if (!error && !wcscmp(tag.name, L"Query"))
            {
                /* the path of a query is the default of its selections, and is looked at when one uses it */
                free(query_path);
                query_path = path ? _wcsdup(path) : NULL;
            }
            else if (!error && (!wcscmp(tag.name, L"Select") || !wcscmp(tag.name, L"Suppress")))
            {
                if (!wcscmp(tag.name, L"Select")) select = TRUE;
                if (!path) path = query_path;
                /* a selection with no path of its own and no default of its query selects nothing */
                if (!path && !wcscmp(tag.name, L"Select")) error = ERROR_EVT_INVALID_QUERY;
                if (path)
                {
                    error = check_path(path, file);
                    if (error == ERROR_EVT_INVALID_CHANNEL_PATH) error = ERROR_EVT_INVALID_QUERY;
                    found = found || !error;
                }
            }
            if (!error && !tag.empty)
            {
                if (depth == XML_MAX_DEPTH) error = ERROR_EVT_MALFORMED_XML_TEXT;
                else lstrcpyW(open[depth++], tag.name);
            }
        }
        free_tag(&tag);
    }
    free(query_path);
    if (ret == XML_ERROR) error = ERROR_EVT_MALFORMED_XML_TEXT;
    if (!error && ret == XML_END && tag.junk && !depth) error = ERROR_EVT_MALFORMED_XML_TEXT;
    if (error) return error;
    if (depth || !root) return ERROR_EVT_MALFORMED_XML_TEXT;
    if (!select) return ERROR_INVALID_PARAMETER;
    return found ? ERROR_SUCCESS : ERROR_EVT_INVALID_QUERY;
}

/* The checks of a query, a subscription or an export up to the point where the service is needed */
static DWORD check_query(const WCHAR *path, const WCHAR *query, BOOL file)
{
    /* a structured query names its channels or files itself, and the path is not looked at */
    if (is_query_list(query)) return check_query_list(query, file);

    if (file)
    {
        if (!path || !path[0]) return ERROR_EVT_INVALID_CHANNEL_PATH;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return GetLastError();
        return ERROR_SUCCESS;
    }

    if (!path || !path[0]) return ERROR_EVT_INVALID_CHANNEL_PATH;

    if (check_channel(path)) return check_channel(path);
    if (query && !query[0]) return ERROR_EVT_INVALID_QUERY;
    return ERROR_SUCCESS;
}

EVT_HANDLE WINAPI EvtOpenSession(EVT_LOGIN_CLASS login_class, void *login, DWORD timeout, DWORD flags)
{
    EVT_RPC_LOGIN *rpc = login;

    FIXME("(%u %p %lu %lu) semi-stub\n", login_class, login, timeout, flags);

    if (login_class != EvtRpcLogin || !login || flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (rpc->Flags > EvtRpcLoginAuthNTLM)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    return new_object(OBJECT_SESSION, !is_local_server(rpc->Server), NULL);
}

BOOL WINAPI EvtClose(EVT_HANDLE handle)
{
    struct object *object;

    TRACE("(%p)\n", handle);

    EnterCriticalSection(&object_cs);
    object = find_object(handle, 0);
    if (object) list_remove(&object->entry);
    LeaveCriticalSection(&object_cs);

    if (!object) return fail(ERROR_INVALID_HANDLE);
    free_object(object);
    return TRUE;
}

BOOL WINAPI EvtCancel(EVT_HANDLE handle)
{
    BOOL found;

    TRACE("(%p)\n", handle);

    /* the handles that can be cancelled are queries and subscriptions, which are never created */
    if (!handle) return TRUE;
    EnterCriticalSection(&object_cs);
    found = find_object(handle, 0) != NULL;
    LeaveCriticalSection(&object_cs);
    return fail(found ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
}

DWORD WINAPI EvtGetExtendedStatus(DWORD size, WCHAR *buffer, DWORD *used)
{
    TRACE("(%lu %p %p)\n", size, buffer, used);

    if (!used) return ERROR_INVALID_PARAMETER;
    *used = 0;
    return ERROR_SUCCESS;
}

EVT_HANDLE WINAPI EvtOpenChannelEnum(EVT_HANDLE session, DWORD flags)
{
    BOOL remote;

    FIXME("(%p %lu) semi-stub\n", session, flags);

    if (flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!get_session(session, &remote)) return NULL;
    if (remote)
    {
        no_service();
        return NULL;
    }
    return new_object(OBJECT_CHANNEL_ENUM, FALSE, NULL);
}

BOOL WINAPI EvtNextChannelPath(EVT_HANDLE channel_enum, DWORD buffer_len, WCHAR *buffer, DWORD *used)
{
    struct object *object;
    WCHAR name[256];
    DWORD size, len;
    HKEY key;
    LSTATUS status;
    BOOL ret = FALSE;

    TRACE("(%p %lu %p %p)\n", channel_enum, buffer_len, buffer, used);

    EnterCriticalSection(&object_cs);
    object = find_object(channel_enum, OBJECT_CHANNEL_ENUM);
    if (!object) SetLastError(ERROR_INVALID_HANDLE);
    else if (!used || (buffer_len && !buffer)) SetLastError(ERROR_INVALID_PARAMETER);
    else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, eventlog_key, 0, KEY_READ, &key)) SetLastError(ERROR_NO_MORE_ITEMS);
    else
    {
        size = ARRAY_SIZE(name);
        status = RegEnumKeyExW(key, object->index, name, &size, NULL, NULL, NULL, NULL);
        RegCloseKey(key);
        if (status) SetLastError(ERROR_NO_MORE_ITEMS);
        else
        {
            len = size + 1;
            *used = len;
            if (buffer_len < len) SetLastError(ERROR_INSUFFICIENT_BUFFER);
            else
            {
                memcpy(buffer, name, len * sizeof(WCHAR));
                object->index++;
                ret = TRUE;
            }
        }
    }
    LeaveCriticalSection(&object_cs);
    return ret;
}

EVT_HANDLE WINAPI EvtOpenChannelConfig(EVT_HANDLE session, const WCHAR *path, DWORD flags)
{
    DWORD error;
    BOOL remote;

    FIXME("(%p %s %lu) semi-stub\n", session, debugstr_w(path), flags);

    if (flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!get_session(session, &remote)) return NULL;
    if (!path)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (remote)
    {
        no_service();
        return NULL;
    }
    if ((error = check_channel(path)))
    {
        SetLastError(error);
        return NULL;
    }
    return new_object(OBJECT_CHANNEL_CONFIG, FALSE, path);
}

static BOOL get_log_file_path(const WCHAR *channel, WCHAR **path)
{
    DWORD type, size = 0, len;
    WCHAR *key_name, *value = NULL;
    HKEY key;

    len = lstrlenW(eventlog_key) + 1 + lstrlenW(channel) + 1;
    if (!(key_name = calloc(len, sizeof(WCHAR)))) return FALSE;
    lstrcpyW(key_name, eventlog_key);
    lstrcatW(key_name, L"\\");
    lstrcatW(key_name, channel);

    *path = NULL;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, KEY_READ, &key))
    {
        if (!RegQueryValueExW(key, L"File", NULL, &type, NULL, &size) && (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(WCHAR))
        {
            /* the terminator is added, as a value is not guaranteed to have one */
            value = calloc((size + sizeof(WCHAR) - 1) / sizeof(WCHAR) + 1, sizeof(WCHAR));
            if (value && RegQueryValueExW(key, L"File", NULL, &type, (BYTE *)value, &size))
            {
                free(value);
                value = NULL;
            }
        }
        RegCloseKey(key);
    }
    free(key_name);

    if (!value || !value[0])
    {
        free(value);
        len = lstrlenW(default_log_dir) + lstrlenW(channel) + ARRAY_SIZE(L".evtx");
        if (!(value = calloc(len, sizeof(WCHAR)))) return FALSE;
        lstrcpyW(value, default_log_dir);
        lstrcatW(value, channel);
        lstrcatW(value, L".evtx");
    }
    *path = value;
    return TRUE;
}

BOOL WINAPI EvtGetChannelConfigProperty(EVT_HANDLE config, EVT_CHANNEL_CONFIG_PROPERTY_ID id, DWORD flags,
                                        DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    struct object *object;
    WCHAR *path = NULL;
    DWORD needed;
    BOOL ret = FALSE;

    FIXME("(%p %i %lu %lu %p %p) semi-stub\n", config, id, flags, size, buffer, used);

    EnterCriticalSection(&object_cs);
    object = find_object(config, OBJECT_CHANNEL_CONFIG);
    if (!object) SetLastError(ERROR_INVALID_HANDLE);
    else if ((int)id < 0 || id >= EvtChannelConfigPropertyIdEND || flags || !used) SetLastError(ERROR_INVALID_PARAMETER);
    else switch (id)
    {
    case EvtChannelConfigEnabled:
    case EvtChannelConfigClassicEventlog:
        *used = sizeof(EVT_VARIANT);
        if (size < sizeof(EVT_VARIANT) || !buffer) SetLastError(ERROR_INSUFFICIENT_BUFFER);
        else
        {
            buffer->BooleanVal = TRUE;
            buffer->Type = EvtVarTypeBoolean;
            ret = TRUE;
        }
        break;

    case EvtChannelLoggingConfigLogFilePath:
        if (!get_log_file_path(object->name, &path))
        {
            SetLastError(ERROR_OUTOFMEMORY);
            break;
        }
        needed = sizeof(EVT_VARIANT) + (lstrlenW(path) + 1) * sizeof(WCHAR);
        *used = needed;
        if (size < needed || !buffer) SetLastError(ERROR_INSUFFICIENT_BUFFER);
        else
        {
            buffer->StringVal = (WCHAR *)(buffer + 1);
            lstrcpyW((WCHAR *)buffer->StringVal, path);
            buffer->Type = EvtVarTypeString;
            ret = TRUE;
        }
        free(path);
        break;

    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        break;
    }
    LeaveCriticalSection(&object_cs);
    return ret;
}

BOOL WINAPI EvtSetChannelConfigProperty(EVT_HANDLE config, EVT_CHANNEL_CONFIG_PROPERTY_ID id, DWORD flags,
                                        PEVT_VARIANT value)
{
    BOOL found;

    FIXME("(%p %i %lu %p) semi-stub\n", config, id, flags, value);

    EnterCriticalSection(&object_cs);
    found = find_object(config, OBJECT_CHANNEL_CONFIG) != NULL;
    LeaveCriticalSection(&object_cs);

    if (!found) return fail(ERROR_INVALID_HANDLE);
    if ((int)id < 0 || id >= EvtChannelConfigPropertyIdEND || flags || !value) return fail(ERROR_INVALID_PARAMETER);

    /* the registry that backs the channels is not changed through this interface */
    return fail(ERROR_NOT_SUPPORTED);
}

BOOL WINAPI EvtSaveChannelConfig(EVT_HANDLE config, DWORD flags)
{
    BOOL found;

    FIXME("(%p %#lx) semi-stub\n", config, flags);

    EnterCriticalSection(&object_cs);
    found = find_object(config, OBJECT_CHANNEL_CONFIG) != NULL;
    LeaveCriticalSection(&object_cs);

    if (!found) return fail(ERROR_INVALID_HANDLE);
    if (flags) return fail(ERROR_INVALID_PARAMETER);

    /* nothing can have been changed, so there is nothing to save */
    return TRUE;
}

EVT_HANDLE WINAPI EvtQuery(EVT_HANDLE session, const WCHAR *path, const WCHAR *query, DWORD flags)
{
    DWORD error;
    BOOL remote;

    FIXME("(%p %s %s %#lx) semi-stub\n", session, debugstr_w(path), debugstr_w(query), flags);

    if ((flags & 3) != EvtQueryChannelPath && (flags & 3) != EvtQueryFilePath)
        goto invalid;
    if ((flags & (EvtQueryForwardDirection | EvtQueryReverseDirection)) == (EvtQueryForwardDirection | EvtQueryReverseDirection))
        goto invalid;
    if (flags & ~(0x3 | EvtQueryForwardDirection | EvtQueryReverseDirection | EvtQueryTolerateQueryErrors))
        goto invalid;

    if (!get_session(session, &remote)) return NULL;
    if (remote)
    {
        no_service();
        return NULL;
    }
    if ((error = check_query(path, query, (flags & 3) == EvtQueryFilePath)))
    {
        SetLastError(error);
        return NULL;
    }
    no_service();
    return NULL;

invalid:
    SetLastError(ERROR_INVALID_PARAMETER);
    return NULL;
}

BOOL WINAPI EvtNext(EVT_HANDLE result_set, DWORD size, EVT_HANDLE *array, DWORD timeout, DWORD flags, DWORD *ret_count)
{
    FIXME("(%p %lu %p %lu %#lx %p) semi-stub\n", result_set, size, array, timeout, flags, ret_count);

    /* no result set exists, as no query succeeds */
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtSeek(EVT_HANDLE result_set, LONGLONG position, EVT_HANDLE bookmark, DWORD timeout, DWORD flags)
{
    FIXME("(%p %s %p %lu %#lx) semi-stub\n", result_set, wine_dbgstr_longlong(position), bookmark, timeout, flags);

    /* the origin is checked before the handle */
    if ((flags & EvtSeekOriginMask) < EvtSeekRelativeToFirst || (flags & EvtSeekOriginMask) > EvtSeekRelativeToBookmark ||
        (flags & ~(EvtSeekOriginMask | EvtSeekStrict)))
        return fail(ERROR_INVALID_PARAMETER);
    return fail(ERROR_INVALID_HANDLE);
}

EVT_HANDLE WINAPI EvtSubscribe(EVT_HANDLE session, HANDLE signal_event, const WCHAR *path, const WCHAR *query,
                               EVT_HANDLE bookmark, void *context, EVT_SUBSCRIBE_CALLBACK callback, DWORD flags)
{
    DWORD error, origin = flags & EvtSubscribeOriginMask;
    BOOL remote, found;

    FIXME("(%p %p %s %s %p %p %p %#lx) semi-stub\n", session, signal_event, debugstr_w(path), debugstr_w(query),
          bookmark, context, callback, flags);

    if (!origin || (!signal_event && !callback) || (signal_event && callback) ||
        (flags & ~(EvtSubscribeOriginMask | EvtSubscribeTolerateQueryErrors | EvtSubscribeStrict)))
        goto invalid;
    if ((origin == EvtSubscribeStartAfterBookmark) != (bookmark != NULL))
        goto invalid;

    if (!get_session(session, &remote)) return NULL;
    if (bookmark)
    {
        EnterCriticalSection(&object_cs);
        found = find_object(bookmark, OBJECT_BOOKMARK) != NULL;
        LeaveCriticalSection(&object_cs);
        if (!found)
        {
            SetLastError(ERROR_INVALID_HANDLE);
            return NULL;
        }
    }
    if (remote)
    {
        no_service();
        return NULL;
    }
    if ((error = check_query(path, query, FALSE)))
    {
        SetLastError(error);
        return NULL;
    }
    no_service();
    return NULL;

invalid:
    SetLastError(ERROR_INVALID_PARAMETER);
    return NULL;
}

EVT_HANDLE WINAPI EvtOpenLog(EVT_HANDLE session, const WCHAR *path, DWORD flags)
{
    DWORD error;
    BOOL remote;

    FIXME("(%p %s %#lx) semi-stub\n", session, debugstr_w(path), flags);

    if ((flags != EvtOpenChannelPath && flags != EvtOpenFilePath) || !path)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!get_session(session, &remote)) return NULL;
    if (remote)
    {
        no_service();
        return NULL;
    }
    if (flags == EvtOpenFilePath)
        error = check_query(path, NULL, TRUE);
    else
        error = check_channel(path);
    if (error)
    {
        SetLastError(error);
        return NULL;
    }
    no_service();
    return NULL;
}

BOOL WINAPI EvtGetLogInfo(EVT_HANDLE log, EVT_LOG_PROPERTY_ID id, DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    FIXME("(%p %i %lu %p %p) semi-stub\n", log, id, size, buffer, used);

    /* no log is ever opened */
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtGetQueryInfo(EVT_HANDLE query, EVT_QUERY_PROPERTY_ID id, DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    FIXME("(%p %i %lu %p %p) semi-stub\n", query, id, size, buffer, used);

    /* no query or subscription exists */
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtExportLog(EVT_HANDLE session, const WCHAR *path, const WCHAR *query, const WCHAR *file, DWORD flags)
{
    DWORD error;
    BOOL remote;

    FIXME("(%p %s %s %s %#lx) semi-stub\n", session, debugstr_w(path), debugstr_w(query), debugstr_w(file), flags);

    if ((flags & 3) != EvtExportLogChannelPath && (flags & 3) != EvtExportLogFilePath)
        return fail(ERROR_INVALID_PARAMETER);
    if (flags & ~(0x3 | EvtExportLogTolerateQueryErrors | EvtExportLogOverwrite))
        return fail(ERROR_INVALID_PARAMETER);
    if (!get_session(session, &remote)) return FALSE;
    if (remote) return no_service();
    if ((error = check_query(path, query, (flags & 3) == EvtExportLogFilePath))) return fail(error);
    if (!file) return fail(ERROR_INVALID_PARAMETER);
    return no_service();
}

BOOL WINAPI EvtClearLog(EVT_HANDLE session, const WCHAR *path, const WCHAR *file, DWORD flags)
{
    DWORD error;
    BOOL remote;

    FIXME("(%p %s %s %#lx) semi-stub\n", session, debugstr_w(path), debugstr_w(file), flags);

    if (flags) return fail(ERROR_INVALID_PARAMETER);
    if (!get_session(session, &remote)) return FALSE;
    if (remote) return no_service();
    if ((error = check_channel(path))) return fail(error);
    return no_service();
}

EVT_HANDLE WINAPI EvtCreateRenderContext(DWORD count, const WCHAR **paths, DWORD flags)
{
    DWORD i;

    FIXME("(%lu %p %#lx) semi-stub\n", count, paths, flags);

    if (flags == EvtRenderContextValues)
    {
        if (!count || !paths) goto invalid;
        for (i = 0; i < count; i++)
            if (!paths[i] || !paths[i][0]) goto invalid;
    }
    else if (flags == EvtRenderContextSystem || flags == EvtRenderContextUser)
    {
        if (count || paths) goto invalid;
    }
    else goto invalid;

    return new_object(OBJECT_RENDER_CONTEXT, FALSE, NULL);

invalid:
    SetLastError(ERROR_INVALID_PARAMETER);
    return NULL;
}

/* A bookmark list in the format that Windows writes: Bookmark elements with up to three attributes */

enum { BOOKMARK_OK, BOOKMARK_MALFORMED, BOOKMARK_INVALID };

/* The digits at the start of a record number; no digit makes zero, and a number that is too large is the largest one */
static ULONGLONG parse_record_id(const WCHAR *text)
{
    ULONGLONG number = 0;

    for (; *text >= '0' && *text <= '9'; text++)
    {
        if (number > (MAXLONGLONG - (*text - '0')) / 10) return MAXLONGLONG;
        number = number * 10 + (*text - '0');
    }
    return number;
}

static BOOL is_channel_text(const WCHAR *name)
{
    for (; *name; name++)
        if (is_bad_channel_char(*name) || *name == '\'') return FALSE;
    return TRUE;
}

static void free_marks(struct bookmark_entry *marks, DWORD count)
{
    DWORD i;

    for (i = 0; i < count; i++)
        free(marks[i].channel);
    free(marks);
}

static int parse_bookmark_list(const WCHAR *xml, struct bookmark_entry **marks, DWORD *count, WCHAR **direction)
{
    struct bookmark_entry *list = NULL, *entry;
    const WCHAR *p = xml, *value;
    struct xml_tag tag;
    BOOL root = FALSE, closed = FALSE;
    DWORD n = 0, i;
    int ret, result = BOOKMARK_MALFORMED;

    *marks = NULL;
    *count = 0;
    *direction = NULL;

    while ((ret = next_tag(&p, &tag)) == XML_TAG)
    {
        int status = BOOKMARK_INVALID;

        if (tag.junk) { free_tag(&tag); result = BOOKMARK_MALFORMED; goto done; }
        if (closed || has_duplicate_attr(&tag)) goto invalid;

        if (!root)
        {
            if (tag.closing || wcscmp(tag.name, L"BookmarkList")) goto invalid;
            for (i = 0; i < tag.attr_count; i++)
                if (wcscmp(tag.attrs[i].name, L"Direction")) goto invalid;
            if ((value = find_attr(&tag, L"Direction")) && !(*direction = _wcsdup(value))) goto invalid;
            root = TRUE;
            closed = tag.empty;
        }
        else if (tag.closing)
        {
            if (wcscmp(tag.name, L"BookmarkList")) goto invalid;
            closed = TRUE;
        }
        else if (!wcscmp(tag.name, L"Bookmark") && tag.empty)
        {
            if (!(entry = realloc(list, (n + 1) * sizeof(*list)))) goto invalid;
            list = entry;
            entry = &list[n++];
            memset(entry, 0, sizeof(*entry));

            value = find_attr(&tag, L"Channel");
            if (value && !is_channel_text(value)) goto invalid;
            if (!(entry->channel = _wcsdup(value ? value : L""))) goto invalid;
            if ((value = find_attr(&tag, L"RecordId")))
            {
                entry->record_id = parse_record_id(value);
                entry->has_record_id = TRUE;
            }
            if ((value = find_attr(&tag, L"IsCurrent")))
            {
                if (wcscmp(value, L"true") && wcscmp(value, L"false")) goto invalid;
                entry->is_current = !wcscmp(value, L"true");
                entry->has_is_current = TRUE;
            }
        }
        else goto invalid;

        free_tag(&tag);
        continue;

    invalid:
        free_tag(&tag);
        result = status;
        goto done;
    }
    /* text without a list at all, text that is cut off or text that is not XML */
    if (ret == XML_END && root && closed && !tag.junk) result = BOOKMARK_OK;

done:
    if (result != BOOKMARK_OK)
    {
        free_marks(list, n);
        free(*direction);
        *direction = NULL;
        return result;
    }
    *marks = list;
    *count = n;
    return BOOKMARK_OK;
}

EVT_HANDLE WINAPI EvtCreateBookmark(const WCHAR *bookmark_xml)
{
    struct bookmark_entry *marks = NULL;
    WCHAR *direction = NULL;
    DWORD count = 0;
    struct object *object;
    EVT_HANDLE handle;
    int result;

    FIXME("(%s) semi-stub\n", debugstr_w(bookmark_xml));

    if (bookmark_xml && (result = parse_bookmark_list(bookmark_xml, &marks, &count, &direction)))
    {
        SetLastError(result == BOOKMARK_MALFORMED ? ERROR_EVT_MALFORMED_XML_TEXT : ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (!(handle = new_object(OBJECT_BOOKMARK, FALSE, NULL)))
    {
        free_marks(marks, count);
        free(direction);
        return NULL;
    }
    object = (struct object *)handle;
    object->marks = marks;
    object->mark_count = count;
    object->direction = direction;
    return handle;
}

struct builder
{
    WCHAR *buf;   /* NULL to only measure */
    SIZE_T len;
};

static void add_text(struct builder *b, const WCHAR *text)
{
    SIZE_T len = lstrlenW(text);

    if (b->buf) memcpy(b->buf + b->len, text, len * sizeof(WCHAR));
    b->len += len;
}

/* The text of an attribute value, with the characters that XML reserves written as references */
static void add_escaped(struct builder *b, const WCHAR *text)
{
    for (; *text; text++)
    {
        switch (*text)
        {
        case '&': add_text(b, L"&amp;"); break;
        case '<': add_text(b, L"&lt;"); break;
        case '>': add_text(b, L"&gt;"); break;
        case '\'': add_text(b, L"&apos;"); break;
        case '"': add_text(b, L"&quot;"); break;
        default:
            if (b->buf) b->buf[b->len] = *text;
            b->len++;
        }
    }
}

static void add_number(struct builder *b, ULONGLONG value)
{
    WCHAR digits[24], *p = digits + ARRAY_SIZE(digits) - 1;

    *p = 0;
    do *--p = '0' + (value % 10); while ((value /= 10));
    add_text(b, p);
}

/* The XML that Windows renders for a bookmark. Returns its length in characters. */
static SIZE_T render_bookmark(const struct object *bookmark, WCHAR *out)
{
    struct builder b = { out, 0 };
    DWORD i;

    add_text(&b, L"<BookmarkList");
    if (bookmark->direction)
    {
        add_text(&b, L" Direction='");
        add_escaped(&b, bookmark->direction);
        add_text(&b, L"'");
    }
    add_text(&b, L">\r\n");
    for (i = 0; i < bookmark->mark_count; i++)
    {
        const struct bookmark_entry *entry = &bookmark->marks[i];

        add_text(&b, L"  <Bookmark Channel='");
        add_escaped(&b, entry->channel);
        add_text(&b, L"'");
        if (entry->has_record_id)
        {
            add_text(&b, L" RecordId='");
            add_number(&b, entry->record_id);
            add_text(&b, L"'");
        }
        if (entry->has_is_current)
            add_text(&b, entry->is_current ? L" IsCurrent='true'" : L" IsCurrent='false'");
        add_text(&b, L"/>\r\n");
    }
    add_text(&b, L"</BookmarkList>");
    if (out) out[b.len] = 0;
    return b.len;
}

BOOL WINAPI EvtRender(EVT_HANDLE context, EVT_HANDLE fragment, DWORD flags, DWORD buffer_size, void *buffer,
                      DWORD *buffer_used, DWORD *property_count)
{
    struct object *object;
    DWORD needed;
    BOOL ret = FALSE;

    FIXME("(%p %p %#lx %lu %p %p %p) semi-stub\n", context, fragment, flags, buffer_size, buffer, buffer_used,
          property_count);

    EnterCriticalSection(&object_cs);
    if (flags > EvtRenderBookmark || !buffer_used || (buffer_size && !buffer)) SetLastError(ERROR_INVALID_PARAMETER);
    else if (flags == EvtRenderEventValues && !context) SetLastError(ERROR_INVALID_PARAMETER);
    else if (flags == EvtRenderBookmark && context) SetLastError(ERROR_INVALID_PARAMETER);
    /* a bookmark is the only fragment that can exist, and only a bookmark is rendered as a bookmark */
    else if (!(object = find_object(fragment, OBJECT_BOOKMARK)) || flags != EvtRenderBookmark) SetLastError(ERROR_INVALID_HANDLE);
    else
    {
        SIZE_T length = render_bookmark(object, NULL);

        if (length >= MAXDWORD / sizeof(WCHAR)) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        else
        {
            needed = (length + 1) * sizeof(WCHAR);
            *buffer_used = needed;
            if (property_count) *property_count = 0;
            if (buffer_size < needed) SetLastError(ERROR_INSUFFICIENT_BUFFER);
            else
            {
                render_bookmark(object, buffer);
                ret = TRUE;
            }
        }
    }
    LeaveCriticalSection(&object_cs);
    return ret;
}

BOOL WINAPI EvtUpdateBookmark(EVT_HANDLE bookmark, EVT_HANDLE event)
{
    FIXME("(%p %p) semi-stub\n", bookmark, event);

    /* no event exists to update a bookmark with */
    return fail(ERROR_INVALID_HANDLE);
}

EVT_HANDLE WINAPI EvtOpenPublisherEnum(EVT_HANDLE session, DWORD flags)
{
    BOOL remote;

    FIXME("(%p %#lx) semi-stub\n", session, flags);

    if (flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!get_session(session, &remote)) return NULL;
    if (remote)
    {
        no_service();
        return NULL;
    }
    return new_object(OBJECT_PUBLISHER_ENUM, FALSE, NULL);
}

BOOL WINAPI EvtNextPublisherId(EVT_HANDLE publisher_enum, DWORD buffer_len, WCHAR *buffer, DWORD *used)
{
    BOOL found;

    FIXME("(%p %lu %p %p) semi-stub\n", publisher_enum, buffer_len, buffer, used);

    EnterCriticalSection(&object_cs);
    found = find_object(publisher_enum, OBJECT_PUBLISHER_ENUM) != NULL;
    LeaveCriticalSection(&object_cs);

    if (!found) return fail(ERROR_INVALID_HANDLE);
    if (!used || (buffer_len && !buffer)) return fail(ERROR_INVALID_PARAMETER);

    /* no publisher is registered */
    return fail(ERROR_NO_MORE_ITEMS);
}

EVT_HANDLE WINAPI EvtOpenPublisherMetadata(EVT_HANDLE session, const WCHAR *publisher, const WCHAR *log_file,
                                           LCID locale, DWORD flags)
{
    BOOL remote;

    FIXME("(%p %s %s %#lx %#lx) semi-stub\n", session, debugstr_w(publisher), debugstr_w(log_file), locale, flags);

    if (flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!get_session(session, &remote)) return NULL;
    if (remote)
    {
        no_service();
        return NULL;
    }
    /* no publisher is registered, and Windows reports the missing manifest as a missing file */
    SetLastError(ERROR_FILE_NOT_FOUND);
    return NULL;
}

/*
 * The functions for publisher metadata, event metadata and events themselves take handles that no
 * function here makes: no publisher has metadata, and no event can be read.
 */

BOOL WINAPI EvtFormatMessage(EVT_HANDLE metadata, EVT_HANDLE event, DWORD message_id, DWORD count, PEVT_VARIANT values,
                             DWORD flags, DWORD size, WCHAR *buffer, DWORD *used)
{
    FIXME("(%p %p %lu %lu %p %#lx %lu %p %p) semi-stub\n", metadata, event, message_id, count, values, flags, size, buffer, used);
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtGetEventInfo(EVT_HANDLE event, EVT_EVENT_PROPERTY_ID id, DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    FIXME("(%p %u %lu %p %p) semi-stub\n", event, id, size, buffer, used);
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtGetEventMetadataProperty(EVT_HANDLE metadata, EVT_EVENT_METADATA_PROPERTY_ID id, DWORD flags, DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    FIXME("(%p %u %#lx %lu %p %p) semi-stub\n", metadata, id, flags, size, buffer, used);
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtGetPublisherMetadataProperty(EVT_HANDLE metadata, EVT_PUBLISHER_METADATA_PROPERTY_ID id, DWORD flags, DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    FIXME("(%p %u %#lx %lu %p %p) semi-stub\n", metadata, id, flags, size, buffer, used);
    return fail(ERROR_INVALID_HANDLE);
}

EVT_HANDLE WINAPI EvtOpenEventMetadataEnum(EVT_HANDLE metadata, DWORD flags)
{
    FIXME("(%p %#lx) semi-stub\n", metadata, flags);
    SetLastError(ERROR_INVALID_HANDLE);
    return NULL;
}

EVT_HANDLE WINAPI EvtNextEventMetadata(EVT_HANDLE metadata_enum, DWORD flags)
{
    FIXME("(%p %#lx) semi-stub\n", metadata_enum, flags);
    SetLastError(ERROR_INVALID_HANDLE);
    return NULL;
}

BOOL WINAPI EvtGetObjectArraySize(HANDLE array, DWORD *size)
{
    FIXME("(%p %p) semi-stub\n", array, size);
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtGetObjectArrayProperty(HANDLE array, DWORD id, DWORD index, DWORD flags, DWORD size, PEVT_VARIANT buffer, DWORD *used)
{
    FIXME("(%p %lu %lu %#lx %lu %p %p) semi-stub\n", array, id, index, flags, size, buffer, used);
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtSetObjectArrayProperty(HANDLE array, DWORD id, DWORD index, DWORD flags, PEVT_VARIANT value)
{
    FIXME("(%p %lu %lu %#lx %p) semi-stub\n", array, id, index, flags, value);
    return fail(ERROR_INVALID_HANDLE);
}

BOOL WINAPI EvtArchiveExportedLog(EVT_HANDLE session, const WCHAR *path, LCID locale, DWORD flags)
{
    BOOL remote;

    FIXME("(%p %s %#lx %#lx) semi-stub\n", session, debugstr_w(path), locale, flags);

    if (flags || !path) return fail(ERROR_INVALID_PARAMETER);
    if (!get_session(session, &remote)) return FALSE;
    if (remote) return no_service();
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    return no_service();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        break;
    case DLL_PROCESS_DETACH:
        if (!reserved)
        {
            struct object *object, *next;

            LIST_FOR_EACH_ENTRY_SAFE(object, next, &object_list, struct object, entry)
            {
                list_remove(&object->entry);
                free_object(object);
            }
            DeleteCriticalSection(&object_cs);
        }
        break;
    }
    return TRUE;
}
