/*
 * The classic event logs of the eventlog service, as the events and publishers of wevtapi.
 *
 * Copyright 2026 the reactos-nitro authors
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

#include "wevtapi_private.h"
#include "sddl.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

/*
 * There is no manifest-based publisher here and no .evtx file: what the library reads are the
 * records that ReadEventLog gives, which are what a classic provider wrote with ReportEvent. They
 * are presented the way Windows presents such records: the event id is the low word of the
 * record's id and the qualifiers its high word, the level follows from the type of the record,
 * the keywords say "classic", the task is the category and the data are the insertion strings
 * and a binary. The text of a message comes from the message files of the source.
 */

#define KEYWORD_AUDIT_FAILURE 0x0010000000000000ULL
#define KEYWORD_AUDIT_SUCCESS 0x0020000000000000ULL
#define KEYWORD_CLASSIC       0x0080000000000000ULL

#define MAX_LOG_BUFFER (16 * 1024 * 1024)

static const WCHAR eventlog_key[] = L"System\\CurrentControlSet\\Services\\EventLog";
static const WCHAR default_log_dir[] = L"%SystemRoot%\\System32\\Winevt\\Logs\\";

static WCHAR *dup_string(const WCHAR *s)
{
    SIZE_T len = wcslen(s) + 1;
    WCHAR *copy = malloc(len * sizeof(WCHAR));

    if (copy) memcpy(copy, s, len * sizeof(WCHAR));
    return copy;
}

/* The registry */

/* The value as a string with a terminator, or NULL when it is missing or no string */
static WCHAR *reg_string(HKEY key, const WCHAR *name)
{
    DWORD type, size = 0;
    WCHAR *value;

    if (RegQueryValueExW(key, name, NULL, &type, NULL, &size) || (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ))
        return NULL;
    /* the terminator is added, as a value is not guaranteed to have one */
    if (!(value = calloc(size / sizeof(WCHAR) + 2, sizeof(WCHAR)))) return NULL;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size) || (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ))
    {
        free(value);
        return NULL;
    }
    return value;
}

static BOOL reg_dword(HKEY key, const WCHAR *name, DWORD *value)
{
    DWORD type, size = sizeof(*value);

    return !RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size) && type == REG_DWORD && size == sizeof(*value);
}

static BOOL is_bad_name(const WCHAR *name)
{
    return !name || !name[0] || wcschr(name, '\\') || wcslen(name) >= 256;
}

static LSTATUS open_log_key(const WCHAR *log, HKEY *key)
{
    HKEY root;
    LSTATUS status;

    if (is_bad_name(log)) return ERROR_FILE_NOT_FOUND;
    if ((status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, eventlog_key, 0, KEY_READ, &root))) return status;
    status = RegOpenKeyExW(root, log, 0, KEY_READ, key);
    RegCloseKey(root);
    return status;
}

/*
 * What wevtutil im registers: the publishers (WINEVT\\Publishers\\{guid}, the name is the default value) and
 * the channels (WINEVT\\Channels\\name). There are no records behind them: the log of such a channel is empty.
 */

static const WCHAR winevt_key[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WINEVT";

static LSTATUS open_winevt_key(const WCHAR *kind, const WCHAR *name, HKEY *key)
{
    WCHAR path[ARRAY_SIZE(winevt_key) + 16 + 256 + 2];

    if (name && is_bad_name(name)) return ERROR_FILE_NOT_FOUND;
    if (name) _snwprintf(path, ARRAY_SIZE(path), L"%ls\\%ls\\%ls", winevt_key, kind, name);
    else _snwprintf(path, ARRAY_SIZE(path), L"%ls\\%ls", winevt_key, kind);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, key);
}

BOOL classic_registered_channel(const WCHAR *name)
{
    HKEY key;

    if (open_winevt_key(L"Channels", name, &key)) return FALSE;
    RegCloseKey(key);
    return TRUE;
}

/* The name of the publisher that has the guid, or NULL */
static WCHAR *registered_publisher_name(const WCHAR *guid)
{
    HKEY key;
    WCHAR *name;

    if (!guid || !guid[0] || open_winevt_key(L"Publishers", guid, &key)) return NULL;
    name = reg_string(key, NULL);
    RegCloseKey(key);
    return name;
}

WCHAR *classic_expand(const WCHAR *value)
{
    DWORD len = ExpandEnvironmentStringsW(value, NULL, 0);
    WCHAR *out;

    if (!len) return dup_string(value);
    if (!(out = calloc(len + 1, sizeof(WCHAR)))) return NULL;
    if (!ExpandEnvironmentStringsW(value, out, len + 1))
    {
        free(out);
        return dup_string(value);
    }
    return out;
}

void classic_names_free(WCHAR **names, DWORD count)
{
    DWORD i;

    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}

static BOOL add_name(WCHAR ***names, DWORD *count, const WCHAR *name, BOOL unique)
{
    WCHAR **p, *copy;
    DWORD i;

    if (unique)
        for (i = 0; i < *count; i++)
            if (!_wcsicmp((*names)[i], name)) return TRUE;
    if (!(copy = dup_string(name))) return FALSE;
    if (!(p = realloc(*names, (*count + 1) * sizeof(*p)))) { free(copy); return FALSE; }
    *names = p;
    p[(*count)++] = copy;
    return TRUE;
}

/* The names of the keys below a key, which are the sources of a log */
static DWORD enum_sources(HKEY log_key, WCHAR ***names, DWORD *count, BOOL unique)
{
    DWORD i;

    for (i = 0;; i++)
    {
        WCHAR name[256];
        DWORD size = ARRAY_SIZE(name);
        LSTATUS status = RegEnumKeyExW(log_key, i, name, &size, NULL, NULL, NULL, NULL);

        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_MORE_DATA) continue;
        if (status) return status;
        if (!add_name(names, count, name, unique)) return ERROR_OUTOFMEMORY;
    }
    return ERROR_SUCCESS;
}

DWORD classic_channel_list(WCHAR ***names)
{
    HKEY key;
    DWORD count = 0;

    *names = NULL;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, eventlog_key, 0, KEY_READ, &key))
    {
        enum_sources(key, names, &count, TRUE);
        RegCloseKey(key);
    }
    if (!open_winevt_key(L"Channels", NULL, &key))
    {
        enum_sources(key, names, &count, TRUE);
        RegCloseKey(key);
    }
    return count;
}

/* The names of the publishers that installed manifests register */
static void enum_registered_publishers(WCHAR ***names, DWORD *count)
{
    HKEY root, key;
    DWORD i;

    if (open_winevt_key(L"Publishers", NULL, &root)) return;
    for (i = 0;; i++)
    {
        WCHAR guid[64], *name;
        DWORD size = ARRAY_SIZE(guid);
        LSTATUS status = RegEnumKeyExW(root, i, guid, &size, NULL, NULL, NULL, NULL);

        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_MORE_DATA) continue;
        if (status) break;
        if (RegOpenKeyExW(root, guid, 0, KEY_READ, &key)) continue;
        name = reg_string(key, NULL);
        RegCloseKey(key);
        if (name && name[0]) add_name(names, count, name, TRUE);
        free(name);
    }
    RegCloseKey(root);
}

DWORD classic_pub_list_for_log(const WCHAR *log, WCHAR ***names)
{
    DWORD count = 0;
    HKEY key;

    *names = NULL;
    if (open_log_key(log, &key)) return 0;
    if (enum_sources(key, names, &count, TRUE))
    {
        classic_names_free(*names, count);
        *names = NULL;
        count = 0;
    }
    RegCloseKey(key);
    return count;
}

DWORD classic_pub_list(WCHAR ***names)
{
    HKEY root;
    DWORD i, count = 0;

    *names = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, eventlog_key, 0, KEY_READ, &root)) return 0;
    for (i = 0;; i++)
    {
        WCHAR log[256];
        DWORD size = ARRAY_SIZE(log);
        LSTATUS status = RegEnumKeyExW(root, i, log, &size, NULL, NULL, NULL, NULL);
        HKEY key;

        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_MORE_DATA) continue;
        if (status) break;
        if (RegOpenKeyExW(root, log, 0, KEY_READ, &key)) continue;
        enum_sources(key, names, &count, TRUE);
        RegCloseKey(key);
    }
    RegCloseKey(root);
    enum_registered_publishers(names, &count);
    return count;
}

void classic_pub_free(struct classic_pub *pub)
{
    if (!pub) return;
    classic_names_free(pub->logs, pub->log_count);
    free(pub->name);
    free(pub->event_file);
    free(pub->category_file);
    free(pub->parameter_file);
    free(pub);
}

/* The publisher of an installed manifest that has the name: its files and the channels that it references */
static BOOL fill_registered_publisher(struct classic_pub *pub, const WCHAR *name)
{
    HKEY root, key, refs, ref;
    DWORD i;
    BOOL found = FALSE;

    if (open_winevt_key(L"Publishers", NULL, &root)) return FALSE;
    for (i = 0; !found; i++)
    {
        WCHAR guid[64], *value;
        DWORD size = ARRAY_SIZE(guid);
        LSTATUS status = RegEnumKeyExW(root, i, guid, &size, NULL, NULL, NULL, NULL);

        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_MORE_DATA) continue;
        if (status) break;
        if (RegOpenKeyExW(root, guid, 0, KEY_READ, &key)) continue;
        value = reg_string(key, NULL);
        if (value && !_wcsicmp(value, name)) found = TRUE;
        free(value);
        if (found) break;
        RegCloseKey(key);
    }
    RegCloseKey(root);
    if (!found) return FALSE;

    pub->event_file = reg_string(key, L"MessageFileName");
    pub->parameter_file = reg_string(key, L"ParameterFileName");
    if (!RegOpenKeyExW(key, L"ChannelReferences", 0, KEY_READ, &refs))
    {
        for (i = 0; i < 1024; i++)
        {
            WCHAR index[16], *channel;

            _snwprintf(index, ARRAY_SIZE(index), L"%lu", i);
            if (RegOpenKeyExW(refs, index, 0, KEY_READ, &ref)) break;
            channel = reg_string(ref, NULL);
            RegCloseKey(ref);
            if (channel && channel[0]) add_name(&pub->logs, &pub->log_count, channel, FALSE);
            free(channel);
        }
        RegCloseKey(refs);
    }
    RegCloseKey(key);
    /* a publisher without channel references is still a publisher */
    if (!pub->log_count && !add_name(&pub->logs, &pub->log_count, L"Application", FALSE)) return FALSE;
    return TRUE;
}

/* The publisher that has no name, whose messages are the standard ones of Windows */
struct classic_pub *classic_pub_default(void)
{
    return calloc(1, sizeof(struct classic_pub));
}

struct classic_pub *classic_pub_open(const WCHAR *name)
{
    struct classic_pub *pub;
    HKEY root;
    DWORD i;

    if (is_bad_name(name)) return NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, eventlog_key, 0, KEY_READ, &root)) return NULL;
    if (!(pub = calloc(1, sizeof(*pub)))) { RegCloseKey(root); return NULL; }

    for (i = 0;; i++)
    {
        WCHAR log[256];
        DWORD size = ARRAY_SIZE(log);
        LSTATUS status = RegEnumKeyExW(root, i, log, &size, NULL, NULL, NULL, NULL);
        HKEY log_key, source_key;

        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_MORE_DATA) continue;
        if (status) break;
        if (RegOpenKeyExW(root, log, 0, KEY_READ, &log_key)) continue;
        if (!RegOpenKeyExW(log_key, name, 0, KEY_READ, &source_key))
        {
            if (!add_name(&pub->logs, &pub->log_count, log, FALSE)) { RegCloseKey(source_key); RegCloseKey(log_key); goto error; }
            /* the files of the first log that registers the source are the ones in use */
            if (pub->log_count == 1)
            {
                pub->event_file = reg_string(source_key, L"EventMessageFile");
                pub->category_file = reg_string(source_key, L"CategoryMessageFile");
                pub->parameter_file = reg_string(source_key, L"ParameterMessageFile");
            }
            RegCloseKey(source_key);
        }
        RegCloseKey(log_key);
    }
    RegCloseKey(root);
    if (!pub->log_count && !fill_registered_publisher(pub, name)) goto error_noroot;
    if (!(pub->name = dup_string(name))) goto error_noroot;
    return pub;

error:
    RegCloseKey(root);
error_noroot:
    classic_pub_free(pub);
    return NULL;
}

/* Records */

/* The string that starts at the pointer and ends before the end, or NULL; next is the pointer behind its terminator */
static const WCHAR *bounded_string(const BYTE *start, const BYTE *end, const BYTE **next)
{
    const WCHAR *s = (const WCHAR *)start;

    while ((const BYTE *)(s + 1) <= end)
    {
        if (!*s)
        {
            *next = (const BYTE *)(s + 1);
            return (const WCHAR *)start;
        }
        s++;
    }
    return NULL;
}

void classic_event_free(struct classic_event *event)
{
    free(event->strings);
    memset(event, 0, sizeof(*event));
}

BOOL classic_event_init(struct classic_event *event, const EVENTLOGRECORD *rec, const WCHAR *channel)
{
    const BYTE *base = (const BYTE *)rec, *end, *p;
    DWORD i, type;

    memset(event, 0, sizeof(*event));
    if (rec->Length < sizeof(EVENTLOGRECORD) + 4 * sizeof(WCHAR)) return FALSE;
    end = base + rec->Length;

    if (!(event->source = bounded_string(base + sizeof(EVENTLOGRECORD), end, &p))) return FALSE;
    if (!(event->computer = bounded_string(p, end, &p))) return FALSE;

    if (rec->NumStrings)
    {
        if (rec->StringOffset >= rec->Length || rec->NumStrings > rec->Length / sizeof(WCHAR)) return FALSE;
        if (!(event->strings = calloc(rec->NumStrings, sizeof(*event->strings)))) return FALSE;
        p = base + rec->StringOffset;
        for (i = 0; i < rec->NumStrings; i++)
        {
            if (!(event->strings[i] = bounded_string(p, end, &p))) { free(event->strings); event->strings = NULL; return FALSE; }
        }
        event->string_count = rec->NumStrings;
    }
    if (rec->DataLength)
    {
        if (rec->DataOffset > rec->Length || rec->DataLength > rec->Length - rec->DataOffset) { free(event->strings); event->strings = NULL; return FALSE; }
        event->data = base + rec->DataOffset;
        event->data_len = rec->DataLength;
    }
    if (rec->UserSidLength)
    {
        if (rec->UserSidOffset > rec->Length || rec->UserSidLength > rec->Length - rec->UserSidOffset ||
            !IsValidSid((PSID)(base + rec->UserSidOffset)) || GetLengthSid((PSID)(base + rec->UserSidOffset)) > rec->UserSidLength)
        {
            free(event->strings);
            event->strings = NULL;
            return FALSE;
        }
        event->sid = (PSID)(base + rec->UserSidOffset);
    }

    event->rec = rec;
    event->channel = channel;
    event->id = LOWORD(rec->EventID);
    event->qualifiers = HIWORD(rec->EventID);
    event->task = rec->EventCategory;
    event->record_id = rec->RecordNumber;
    event->time = (ULONGLONG)rec->TimeGenerated * 10000000 + 116444736000000000ULL;
    event->keywords = KEYWORD_CLASSIC;

    type = rec->EventType;
    switch (type)
    {
    case EVENTLOG_ERROR_TYPE: event->level = 2; break;
    case EVENTLOG_WARNING_TYPE: event->level = 3; break;
    case EVENTLOG_AUDIT_SUCCESS: event->level = 0; event->keywords |= KEYWORD_AUDIT_SUCCESS; break;
    case EVENTLOG_AUDIT_FAILURE: event->level = 0; event->keywords |= KEYWORD_AUDIT_FAILURE; break;
    default: event->level = 4; break;
    }
    return TRUE;
}

/* Variants for EvtRender and the properties */

struct vpack
{
    BYTE *buf;
    DWORD size;
    DWORD off;
    BOOL over;
};

static void vp_init(struct vpack *p, void *buf, DWORD size, DWORD slots)
{
    p->buf = buf;
    p->size = size;
    p->off = slots * sizeof(EVT_VARIANT);
    p->over = p->off > size || (size && !buf);
}

static EVT_VARIANT *vp_slot(struct vpack *p, DWORD i)
{
    return p->over ? NULL : (EVT_VARIANT *)p->buf + i;
}

/* Room for bytes behind what was used, aligned. The offset counts on when the buffer is too small, to tell how much is needed. */
static void *vp_alloc(struct vpack *p, SIZE_T bytes)
{
    SIZE_T start = (p->off + 7) & ~(SIZE_T)7;

    if (bytes > MAXDWORD / 2 || start + bytes > MAXDWORD / 2) { p->over = TRUE; p->off = MAXDWORD / 2; return NULL; }
    p->off = start + bytes;
    if (p->off > p->size) p->over = TRUE;
    return p->over ? NULL : p->buf + start;
}

static void vp_null(struct vpack *p, DWORD i)
{
    EVT_VARIANT *v = vp_slot(p, i);

    if (!v) return;
    v->UInt64Val = 0;
    v->Type = EvtVarTypeNull;
}

static void vp_u64(struct vpack *p, DWORD i, DWORD type, ULONGLONG value)
{
    EVT_VARIANT *v = vp_slot(p, i);

    if (!v) return;
    v->UInt64Val = value;
    v->Type = type;
}

static void vp_string(struct vpack *p, DWORD i, const WCHAR *s)
{
    SIZE_T bytes = (wcslen(s) + 1) * sizeof(WCHAR);
    void *mem = vp_alloc(p, bytes);
    EVT_VARIANT *v = vp_slot(p, i);

    if (!mem || !v) return;
    memcpy(mem, s, bytes);
    v->UInt64Val = 0;
    v->StringVal = mem;
    v->Type = EvtVarTypeString;
}

static void vp_sid(struct vpack *p, DWORD i, PSID sid)
{
    DWORD len = GetLengthSid(sid);
    void *mem = vp_alloc(p, len);
    EVT_VARIANT *v = vp_slot(p, i);

    if (!mem || !v) return;
    memcpy(mem, sid, len);
    v->UInt64Val = 0;
    v->SidVal = mem;
    v->Type = EvtVarTypeSid;
}

static void vp_binary(struct vpack *p, DWORD i, const BYTE *data, DWORD len)
{
    void *mem = vp_alloc(p, len);
    EVT_VARIANT *v = vp_slot(p, i);

    if (!mem || !v) return;
    memcpy(mem, data, len);
    v->UInt64Val = 0;
    v->BinaryVal = mem;
    v->Count = len;
    v->Type = EvtVarTypeBinary;
}

static DWORD vp_finish(struct vpack *p, DWORD *used)
{
    *used = p->off;
    return p->over ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS;
}

/* A variant that is an array of strings, as the names of a query or the publishers of a channel */
static void vp_string_array(struct vpack *p, DWORD i, WCHAR *const *names, DWORD count)
{
    WCHAR **array = vp_alloc(p, (SIZE_T)count * sizeof(WCHAR *));
    EVT_VARIANT *v = vp_slot(p, i);
    DWORD n;

    for (n = 0; n < count; n++)
    {
        SIZE_T bytes = (wcslen(names[n]) + 1) * sizeof(WCHAR);
        WCHAR *mem = vp_alloc(p, bytes);

        if (mem && array)
        {
            memcpy(mem, names[n], bytes);
            array[n] = mem;
        }
    }
    if (!v || (count && !array)) return;
    v->UInt64Val = 0;
    v->StringArr = array;
    v->Count = count;
    v->Type = EvtVarTypeString | EVT_VARIANT_TYPE_ARRAY;
}

static void fill_system_value(struct vpack *p, DWORD slot, int index, const struct classic_event *event)
{
    switch (index)
    {
    case 0: vp_string(p, slot, event->source); break;
    case 1: vp_null(p, slot); break;
    case 2: vp_u64(p, slot, EvtVarTypeUInt16, event->id); break;
    case 3: vp_u64(p, slot, EvtVarTypeUInt16, event->qualifiers); break;
    case 4: vp_u64(p, slot, EvtVarTypeByte, event->level); break;
    case 5: vp_u64(p, slot, EvtVarTypeUInt16, event->task); break;
    case 6: vp_u64(p, slot, EvtVarTypeByte, 0); break;
    case 7: vp_u64(p, slot, EvtVarTypeHexInt64, event->keywords); break;
    case 8: vp_u64(p, slot, EvtVarTypeFileTime, event->time); break;
    case 9: vp_u64(p, slot, EvtVarTypeUInt64, event->record_id); break;
    case 10: case 11: vp_null(p, slot); break;
    case 12: case 13: vp_u64(p, slot, EvtVarTypeUInt32, 0); break;
    case 14: vp_string(p, slot, event->channel); break;
    case 15: vp_string(p, slot, event->computer); break;
    case 16:
        if (event->sid) vp_sid(p, slot, event->sid);
        else vp_null(p, slot);
        break;
    case 17: vp_u64(p, slot, EvtVarTypeByte, 0); break;
    default:
        /* the strings of the event, from 100 on */
        if (index >= 100 && (DWORD)(index - 100) < event->string_count) vp_string(p, slot, event->strings[index - 100]);
        else vp_null(p, slot);
        break;
    }
}

static const WCHAR *const value_paths[] =
{
    L"Event/System/Provider/@Name", L"Event/System/Provider/@Guid", L"Event/System/EventID",
    L"Event/System/EventID/@Qualifiers", L"Event/System/Level", L"Event/System/Task", L"Event/System/Opcode",
    L"Event/System/Keywords", L"Event/System/TimeCreated/@SystemTime", L"Event/System/EventRecordID",
    L"Event/System/Correlation/@ActivityID", L"Event/System/Correlation/@RelatedActivityID",
    L"Event/System/Execution/@ProcessID", L"Event/System/Execution/@ThreadID", L"Event/System/Channel",
    L"Event/System/Computer", L"Event/System/Security/@UserID", L"Event/System/Version",
};

int classic_value_index(const WCHAR *path)
{
    DWORD i, n;
    const WCHAR *p;

    for (i = 0; i < ARRAY_SIZE(value_paths); i++)
        if (!wcscmp(path, value_paths[i])) return i;

    /* Event/EventData/Data[n], with n counting from one */
    if (!wcsncmp(path, L"Event/EventData/Data[", 21))
    {
        for (p = path + 21, n = 0; *p >= '0' && *p <= '9' && n < 100000; p++) n = n * 10 + (*p - '0');
        if (p > path + 21 && !wcscmp(p, L"]") && n >= 1 && n <= 99999) return 100 + n - 1;
    }
    return -1;
}

DWORD classic_render_values(DWORD context_flags, const DWORD *indexes, DWORD index_count, const struct classic_event *event,
                            DWORD size, void *buffer, DWORD *used, DWORD *count)
{
    struct vpack p;
    DWORD i, slots;

    if (context_flags == EvtRenderContextSystem)
    {
        slots = ARRAY_SIZE(value_paths);
        vp_init(&p, buffer, size, slots);
        for (i = 0; i < slots; i++) fill_system_value(&p, i, i, event);
    }
    else if (context_flags == EvtRenderContextUser)
    {
        slots = event->string_count + (event->data_len ? 1 : 0);
        vp_init(&p, buffer, size, slots);
        for (i = 0; i < event->string_count; i++) vp_string(&p, i, event->strings[i]);
        if (event->data_len) vp_binary(&p, event->string_count, event->data, event->data_len);
    }
    else
    {
        slots = index_count;
        vp_init(&p, buffer, size, slots);
        for (i = 0; i < slots; i++) fill_system_value(&p, i, indexes[i], event);
    }
    *count = slots;
    return vp_finish(&p, used);
}

/* The XML of an event */

struct sb
{
    WCHAR *buf;
    SIZE_T len, cap;
    BOOL fail;
};

static void sb_add_n(struct sb *b, const WCHAR *text, SIZE_T len)
{
    if (b->fail) return;
    if (b->len + len + 1 > b->cap)
    {
        SIZE_T cap = b->cap ? b->cap : 1024;
        WCHAR *p;

        while (cap < b->len + len + 1) cap *= 2;
        if (cap > MAXDWORD / 4 || !(p = realloc(b->buf, cap * sizeof(WCHAR)))) { b->fail = TRUE; return; }
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, text, len * sizeof(WCHAR));
    b->len += len;
    b->buf[b->len] = 0;
}

static void sb_add(struct sb *b, const WCHAR *text)
{
    sb_add_n(b, text, wcslen(text));
}

static void sb_escape(struct sb *b, const WCHAR *text, BOOL attribute)
{
    for (; *text; text++)
    {
        switch (*text)
        {
        case '&': sb_add(b, L"&amp;"); break;
        case '<': sb_add(b, L"&lt;"); break;
        case '>': sb_add(b, L"&gt;"); break;
        case '\'': if (attribute) sb_add(b, L"&apos;"); else sb_add_n(b, text, 1); break;
        case '"': if (attribute) sb_add(b, L"&quot;"); else sb_add_n(b, text, 1); break;
        default: sb_add_n(b, text, 1); break;
        }
    }
}

static void sb_printf_u(struct sb *b, const WCHAR *format, ULONGLONG value)
{
    WCHAR text[64];

    _snwprintf(text, ARRAY_SIZE(text), format, value);
    text[ARRAY_SIZE(text) - 1] = 0;
    sb_add(b, text);
}

WCHAR *classic_render_xml(const struct classic_event *event, const WCHAR *rendering)
{
    struct sb b = { NULL, 0, 0, FALSE };
    WCHAR text[96];
    SYSTEMTIME st;
    FILETIME ft;
    DWORD i;
    WCHAR *sid_text = NULL;

    ft.dwLowDateTime = (DWORD)event->time;
    ft.dwHighDateTime = (DWORD)(event->time >> 32);
    if (!FileTimeToSystemTime(&ft, &st)) memset(&st, 0, sizeof(st));

    sb_add(&b, L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='");
    sb_escape(&b, event->source, TRUE);
    sb_add(&b, L"'/><EventID Qualifiers='");
    sb_printf_u(&b, L"%I64u", event->qualifiers);
    sb_add(&b, L"'>");
    sb_printf_u(&b, L"%I64u", event->id);
    sb_add(&b, L"</EventID><Version>0</Version><Level>");
    sb_printf_u(&b, L"%I64u", event->level);
    sb_add(&b, L"</Level><Task>");
    sb_printf_u(&b, L"%I64u", event->task);
    sb_add(&b, L"</Task><Opcode>0</Opcode><Keywords>");
    sb_printf_u(&b, L"0x%I64x", event->keywords);
    sb_add(&b, L"</Keywords><TimeCreated SystemTime='");
    _snwprintf(text, ARRAY_SIZE(text), L"%04u-%02u-%02uT%02u:%02u:%02u.%07uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, (unsigned)(event->time % 10000000));
    text[ARRAY_SIZE(text) - 1] = 0;
    sb_add(&b, text);
    sb_add(&b, L"'/><EventRecordID>");
    sb_printf_u(&b, L"%I64u", event->record_id);
    sb_add(&b, L"</EventRecordID><Correlation/><Execution ProcessID='0' ThreadID='0'/><Channel>");
    sb_escape(&b, event->channel, FALSE);
    sb_add(&b, L"</Channel><Computer>");
    sb_escape(&b, event->computer, FALSE);
    sb_add(&b, L"</Computer>");
    if (event->sid && ConvertSidToStringSidW(event->sid, &sid_text))
    {
        sb_add(&b, L"<Security UserID='");
        sb_escape(&b, sid_text, TRUE);
        sb_add(&b, L"'/>");
        LocalFree(sid_text);
    }
    else sb_add(&b, L"<Security/>");
    sb_add(&b, L"</System>");

    if (event->string_count || event->data_len)
    {
        sb_add(&b, L"<EventData>");
        for (i = 0; i < event->string_count; i++)
        {
            sb_add(&b, L"<Data>");
            sb_escape(&b, event->strings[i], FALSE);
            sb_add(&b, L"</Data>");
        }
        if (event->data_len)
        {
            static const WCHAR hex[] = L"0123456789ABCDEF";

            sb_add(&b, L"<Binary>");
            for (i = 0; i < event->data_len; i++)
            {
                WCHAR two[2] = { hex[event->data[i] >> 4], hex[event->data[i] & 15] };

                sb_add_n(&b, two, 2);
            }
            sb_add(&b, L"</Binary>");
        }
        sb_add(&b, L"</EventData>");
    }
    if (rendering) sb_add(&b, rendering);
    sb_add(&b, L"</Event>");

    if (b.fail)
    {
        free(b.buf);
        return NULL;
    }
    return b.buf;
}

/* Message texts */

static void trim_end(WCHAR *text)
{
    SIZE_T len = wcslen(text);

    while (len && (text[len - 1] == '\r' || text[len - 1] == '\n' || text[len - 1] == ' ')) text[--len] = 0;
}

/* The text of FormatMessage as a heap string, and the error that tells why not */
static DWORD format_module(HMODULE module, DWORD flags, DWORD id, WCHAR **args, WCHAR **text)
{
    WCHAR *buffer = NULL;
    DWORD ret;

    /* a message with inserts needs the strings for them, or has to leave them alone */
    if (!args) flags |= FORMAT_MESSAGE_IGNORE_INSERTS;
    SetLastError(0);
    ret = FormatMessageW(flags | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_ARGUMENT_ARRAY, module, id, 0,
                         (WCHAR *)&buffer, 0, (va_list *)args);
    if (!ret || !buffer) return GetLastError() ? GetLastError() : ERROR_MR_MID_NOT_FOUND;
    *text = dup_string(buffer);
    LocalFree(buffer);
    return *text ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
}

/* A message of the files that a list names, separated by semicolons */
static DWORD format_from_files(const WCHAR *list, DWORD id, WCHAR **args, WCHAR **text)
{
    WCHAR *copy, *p, *next;
    DWORD error = ERROR_EVT_MESSAGE_NOT_FOUND;

    if (!list || !list[0]) return ERROR_EVT_MESSAGE_NOT_FOUND;
    if (!(copy = dup_string(list))) return ERROR_OUTOFMEMORY;

    for (p = copy; p; p = next)
    {
        WCHAR *expanded;
        HMODULE module;
        DWORD ret;

        if ((next = wcschr(p, ';'))) *next++ = 0;
        while (*p == ' ') p++;
        if (!*p) continue;
        if (!(expanded = classic_expand(p))) continue;
        module = LoadLibraryExW(expanded, NULL, LOAD_LIBRARY_AS_DATAFILE);
        free(expanded);
        if (!module) continue;
        ret = format_module(module, FORMAT_MESSAGE_FROM_HMODULE, id, args, text);
        FreeLibrary(module);
        if (!ret) { free(copy); return ERROR_SUCCESS; }
        /* a file that is there but has no such message */
        error = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
    }
    free(copy);
    return error;
}

/* The text of a message of the parameter file, the message file or the system, for %%n inserts */
static WCHAR *parameter_text(const struct classic_pub *pub, DWORD id)
{
    WCHAR *text = NULL;
    const WCHAR *files = pub->parameter_file && pub->parameter_file[0] ? pub->parameter_file : pub->event_file;

    if (files && !format_from_files(files, id, NULL, &text)) { trim_end(text); return text; }
    if (!format_module(NULL, FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, id, NULL, &text))
    {
        trim_end(text);
        return text;
    }
    return NULL;
}

/* The insertion string with its %%n replaced, as a heap string */
static WCHAR *expand_parameters(const struct classic_pub *pub, const WCHAR *s)
{
    struct sb b = { NULL, 0, 0, FALSE };

    while (*s)
    {
        if (s[0] == '%' && s[1] == '%' && s[2] >= '0' && s[2] <= '9')
        {
            const WCHAR *p = s + 2;
            DWORD id = 0;
            WCHAR *text;

            while (*p >= '0' && *p <= '9' && id < 100000000) id = id * 10 + (*p++ - '0');
            if ((text = parameter_text(pub, id)))
            {
                sb_add(&b, text);
                free(text);
                s = p;
                continue;
            }
        }
        sb_add_n(&b, s, 1);
        s++;
    }
    if (b.fail) { free(b.buf); return NULL; }
    if (!b.buf) return dup_string(L"");
    return b.buf;
}

static const WCHAR *level_name(DWORD level)
{
    switch (level)
    {
    case 0: return L"Log Always";
    case 1: return L"Critical";
    case 2: return L"Error";
    case 3: return L"Warning";
    case 4: return L"Information";
    case 5: return L"Verbose";
    default: return NULL;
    }
}

/* The messages that Windows has for its levels, opcodes, categories and kinds of trace, which the publisher without a name gives */
static void standard_message(DWORD id, WCHAR **text)
{
    static const WCHAR *const categories[] = { L"Devices", L"Disk", L"Printers", L"Services", L"Shell", L"System Event", L"Network" };
    const WCHAR *name = NULL;

    if (id >= 1 && id <= ARRAY_SIZE(categories)) name = categories[id - 1];
    else if (id >= 0x50000001 && id <= 0x50000005) name = level_name(id == 0x50000001 ? 1 : id == 0x50000002 ? 2 : id == 0x50000003 ? 3 : id == 0x50000004 ? 4 : 5);
    else if (id == 0x30000000) name = L"Info";
    else if (id == 0x90000001) name = L"TraceClassic";
    else if (id == 0x90000002) name = L"TraceLogging";
    if (!name) return;
    free(*text);
    *text = dup_string(name);
}

/* The description of an event: the message of the source's file, with the strings of the record inserted */
static DWORD event_message(const struct classic_pub *pub, const struct classic_event *event, DWORD message_id,
                           DWORD value_count, const EVT_VARIANT *values, WCHAR **text)
{
    WCHAR **args = NULL, **owned = NULL, *pool = NULL;
    DWORD i, count, error;

    if (value_count)
    {
        for (i = 0; i < value_count; i++)
            if ((values[i].Type & EVT_VARIANT_TYPE_MASK) != EvtVarTypeString || (values[i].Type & EVT_VARIANT_TYPE_ARRAY))
                return ERROR_INVALID_PARAMETER;
        count = value_count;
    }
    else count = event ? event->string_count : 0;

    /* a message may insert up to 99 strings; the ones that the record lacks stay as they were written */
    if (!(args = calloc(100, sizeof(*args))) || !(owned = calloc(count + 1, sizeof(*owned))) ||
        !(pool = calloc(100, 5 * sizeof(WCHAR))))
    {
        error = ERROR_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < 99; i++)
    {
        WCHAR *slot = pool + i * 5;

        _snwprintf(slot, 5, L"%%%u", (unsigned)(i + 1));
        slot[4] = 0;
        args[i] = slot;
    }
    args[99] = (WCHAR *)L"";
    for (i = 0; i < count && i < 99; i++)
    {
        const WCHAR *s = value_count ? values[i].StringVal : event->strings[i];

        if (!s) s = L"";
        if (!(owned[i] = expand_parameters(pub, s))) { error = ERROR_OUTOFMEMORY; goto done; }
        args[i] = owned[i];
    }

    error = format_from_files(pub->event_file, message_id, args, text);
    if (!error) trim_end(*text);

done:
    if (owned)
        for (i = 0; i < count; i++) free(owned[i]);
    free(owned);
    free(args);
    free(pool);
    return error;
}

static WCHAR *multi_string(const WCHAR *const *names, DWORD count, DWORD *length)
{
    SIZE_T total = 1, pos = 0;
    DWORD i;
    WCHAR *out;

    for (i = 0; i < count; i++) total += wcslen(names[i]) + 1;
    if (!(out = calloc(total, sizeof(WCHAR)))) return NULL;
    for (i = 0; i < count; i++)
    {
        SIZE_T len = wcslen(names[i]) + 1;

        memcpy(out + pos, names[i], len * sizeof(WCHAR));
        pos += len;
    }
    out[pos] = 0;
    *length = total;
    return out;
}

/* The element of the rendered text that EvtFormatMessageXml adds to the event */
static WCHAR *rendering_info(const struct classic_pub *pub, const struct classic_event *event)
{
    struct sb b = { NULL, 0, 0, FALSE };
    WCHAR *text = NULL;
    const WCHAR *name;

    sb_add(&b, L"<RenderingInfo Culture='en-US'>");
    if (!event_message(pub, event, MAKELONG(event->id, event->qualifiers), 0, NULL, &text))
    {
        sb_add(&b, L"<Message>");
        sb_escape(&b, text, FALSE);
        sb_add(&b, L"</Message>");
        free(text);
    }
    sb_add(&b, L"<Level>");
    if ((name = level_name(event->level))) sb_add(&b, name);
    sb_add(&b, L"</Level><Task>");
    text = NULL;
    if (event->task && !format_from_files(pub->category_file, event->task, NULL, &text))
    {
        trim_end(text);
        sb_escape(&b, text, FALSE);
        free(text);
    }
    sb_add(&b, L"</Task><Opcode></Opcode><Channel></Channel><Provider></Provider><Keywords>");
    if (event->keywords & KEYWORD_AUDIT_FAILURE) sb_add(&b, L"<Keyword>Audit Failure</Keyword>");
    if (event->keywords & KEYWORD_AUDIT_SUCCESS) sb_add(&b, L"<Keyword>Audit Success</Keyword>");
    if (event->keywords & KEYWORD_CLASSIC) sb_add(&b, L"<Keyword>Classic</Keyword>");
    sb_add(&b, L"</Keywords></RenderingInfo>");
    if (b.fail) { free(b.buf); return NULL; }
    return b.buf;
}

DWORD classic_format(const struct classic_pub *pub, const struct classic_event *event, DWORD message_id, DWORD flags,
                     DWORD value_count, const EVT_VARIANT *values, WCHAR **text, DWORD *length)
{
    DWORD error = ERROR_SUCCESS, id;

    *text = NULL;
    switch (flags)
    {
    case EvtFormatMessageEvent:
        if (!event) return ERROR_INVALID_PARAMETER;
        error = event_message(pub, event, MAKELONG(event->id, event->qualifiers), value_count, values, text);
        break;

    case EvtFormatMessageId:
        if (!(*text = dup_string(L""))) return ERROR_OUTOFMEMORY;
        standard_message(message_id, text);
        if (!*text) return ERROR_OUTOFMEMORY;
        if ((*text)[0]) break;
        free(*text);
        *text = NULL;
        error = event_message(pub, event, message_id, value_count, values, text);
        /* what Windows says of a message that the files of a classic source lack */
        if (error == ERROR_EVT_MESSAGE_NOT_FOUND || error == ERROR_EVT_MESSAGE_ID_NOT_FOUND)
            error = ERROR_EVT_MESSAGE_LOCALE_NOT_FOUND;
        break;

    case EvtFormatMessageLevel:
    {
        const WCHAR *name = level_name(event ? event->level : message_id);

        if (!name) return ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        *text = dup_string(name);
        break;
    }

    case EvtFormatMessageTask:
        id = event ? event->task : message_id;
        if (!pub->name)
        {
            /* the standard messages of Windows have a name for no task and for the categories that classic sources share */
            if (!(*text = dup_string(L""))) return ERROR_OUTOFMEMORY;
            if (!id) { free(*text); *text = dup_string(L"None"); }
            else standard_message(id, text);
            if (!*text || !(*text)[0])
            {
                free(*text);
                *text = NULL;
                return ERROR_EVT_MESSAGE_ID_NOT_FOUND;
            }
            break;
        }
        if (!id) return ERROR_EVT_MESSAGE_NOT_FOUND;
        error = format_from_files(pub->category_file, id, NULL, text);
        if (!error) trim_end(*text);
        break;

    case EvtFormatMessageKeyword:
    {
        const WCHAR *names[3];
        DWORD count = 0;
        ULONGLONG keywords = event ? event->keywords : 0;

        if (keywords & KEYWORD_AUDIT_FAILURE) names[count++] = L"Audit Failure";
        if (keywords & KEYWORD_AUDIT_SUCCESS) names[count++] = L"Audit Success";
        if (keywords & KEYWORD_CLASSIC) names[count++] = L"Classic";
        if (!count) return ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        if (!(*text = multi_string(names, count, length))) return ERROR_OUTOFMEMORY;
        return ERROR_SUCCESS;
    }

    case EvtFormatMessageXml:
    {
        WCHAR *rendering, *xml;

        if (!event) return ERROR_INVALID_PARAMETER;
        if (!(rendering = rendering_info(pub, event))) return ERROR_OUTOFMEMORY;
        xml = classic_render_xml(event, rendering);
        free(rendering);
        if (!(*text = xml)) return ERROR_OUTOFMEMORY;
        break;
    }

    default:
        /* opcodes, channels and providers have no messages of their own in a classic source */
        return ERROR_EVT_MESSAGE_ID_NOT_FOUND;
    }

    if (error) return error;
    if (!*text) return ERROR_OUTOFMEMORY;
    *length = wcslen(*text) + 1;
    return ERROR_SUCCESS;
}

/* Properties of a publisher, an element of its arrays and an event */

DWORD classic_pub_property(const struct classic_pub *pub, DWORD id, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;
    WCHAR *file;

    vp_init(&p, buffer, size, 1);
    switch (id)
    {
    case EvtPublisherMetadataPublisherGuid:
    {
        GUID *guid = vp_alloc(&p, sizeof(*guid));
        EVT_VARIANT *v = vp_slot(&p, 0);

        if (guid && v)
        {
            memset(guid, 0, sizeof(*guid));
            v->UInt64Val = 0;
            v->GuidVal = guid;
            v->Type = EvtVarTypeGuid;
        }
        break;
    }
    case EvtPublisherMetadataResourceFilePath:
    case EvtPublisherMetadataHelpLink:
        vp_null(&p, 0);
        break;
    case EvtPublisherMetadataParameterFilePath:
    case EvtPublisherMetadataMessageFilePath:
        file = id == EvtPublisherMetadataMessageFilePath ? pub->event_file : pub->parameter_file;
        if (file && file[0] && (file = classic_expand(file)))
        {
            vp_string(&p, 0, file);
            free(file);
        }
        else vp_null(&p, 0);
        break;
    case EvtPublisherMetadataPublisherMessageID:
        vp_u64(&p, 0, EvtVarTypeUInt32, 0xffffffff);
        break;
    default:
        return ERROR_INVALID_PARAMETER;
    }
    return vp_finish(&p, used);
}

DWORD classic_array_property(const WCHAR *item, DWORD index, DWORD id, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;

    vp_init(&p, buffer, size, 1);
    switch (id)
    {
    case EvtPublisherMetadataChannelReferencePath:
        vp_string(&p, 0, item);
        break;
    case EvtPublisherMetadataChannelReferenceIndex:
        /* a classic source has no manifest to number its channels in */
        vp_u64(&p, 0, EvtVarTypeUInt32, 0xffffffff);
        break;
    case EvtPublisherMetadataChannelReferenceID:
        vp_u64(&p, 0, EvtVarTypeUInt32, !_wcsicmp(item, L"System") ? 8 : !_wcsicmp(item, L"Application") ? 9 :
                                        !_wcsicmp(item, L"Security") ? 10 : 0);
        break;
    case EvtPublisherMetadataChannelReferenceFlags:
        vp_u64(&p, 0, EvtVarTypeUInt32, EvtChannelReferenceImported);
        break;
    case EvtPublisherMetadataChannelReferenceMessageID:
        vp_u64(&p, 0, EvtVarTypeUInt32, 0xffffffff);
        break;
    default:
        return ERROR_INVALID_PARAMETER;
    }
    return vp_finish(&p, used);
}

DWORD classic_event_info(const struct classic_event *event, DWORD id, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;

    vp_init(&p, buffer, size, 1);
    switch (id)
    {
    case EvtEventQueryIDs:
    {
        UINT32 *ids = vp_alloc(&p, sizeof(*ids));
        EVT_VARIANT *v = vp_slot(&p, 0);

        if (ids && v)
        {
            *ids = 0;
            v->UInt64Val = 0;
            v->UInt32Arr = ids;
            v->Count = 1;
            v->Type = EvtVarTypeUInt32 | EVT_VARIANT_TYPE_ARRAY;
        }
        break;
    }
    case EvtEventPath:
        vp_string(&p, 0, event->channel);
        break;
    default:
        return ERROR_INVALID_PARAMETER;
    }
    return vp_finish(&p, used);
}

/* Channels and logs */

static BOOL get_log_file(HKEY key, const WCHAR *log, WCHAR **path)
{
    WCHAR *value = reg_string(key, L"File"), *full;
    SIZE_T len;

    if (value && value[0]) { *path = value; return TRUE; }
    free(value);
    len = wcslen(default_log_dir) + wcslen(log) + ARRAY_SIZE(L".evtx");
    if (!(full = calloc(len, sizeof(WCHAR)))) return FALSE;
    memcpy(full, default_log_dir, sizeof(default_log_dir) - sizeof(WCHAR));
    memcpy(full + ARRAY_SIZE(default_log_dir) - 1, log, wcslen(log) * sizeof(WCHAR));
    memcpy(full + ARRAY_SIZE(default_log_dir) - 1 + wcslen(log), L".evtx", sizeof(L".evtx"));
    *path = full;
    return TRUE;
}

static const WCHAR default_sddl_system[] =
    L"O:BAG:SYD:(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x3;;;BO)(A;;0x5;;;SO)(A;;0x1;;;IU)(A;;0x3;;;SU)(A;;0x1;;;S-1-5-3)"
    L"(A;;0x2;;;S-1-5-33)(A;;0x1;;;S-1-5-32-573)";
static const WCHAR default_sddl_security[] = L"O:BAG:SYD:(A;;0xf0005;;;SY)(A;;0x5;;;BA)(A;;0x1;;;S-1-5-32-573)";
static const WCHAR default_sddl_other[] =
    L"O:BAG:SYD:(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x7;;;SO)(A;;0x3;;;IU)(A;;0x3;;;SU)(A;;0x3;;;S-1-5-3)"
    L"(A;;0x3;;;S-1-5-33)(A;;0x1;;;S-1-5-32-573)";

/* A channel that an installed manifest registers: Type 0 to 3 is Admin, Operational, Analytic and Debug, Isolation 0 to 2 Application, System, Custom */
static DWORD registered_channel_property(const WCHAR *channel, DWORD id, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;
    HKEY key;
    DWORD value, type = 0, isolation = 0, enabled = 0;
    WCHAR *text = NULL, *owner = NULL, *file = NULL, *names[1];
    SIZE_T len, i;
    BOOL file_log;

    if (open_winevt_key(L"Channels", channel, &key)) return ERROR_EVT_CHANNEL_NOT_FOUND;
    if (reg_dword(key, L"Type", &value) && value < 4) type = value;
    if (reg_dword(key, L"Isolation", &value) && value < 3) isolation = value;
    if (reg_dword(key, L"Enabled", &value)) enabled = value != 0;
    file_log = type < 2;
    vp_init(&p, buffer, size, 1);

    switch (id)
    {
    case EvtChannelConfigEnabled:
        vp_u64(&p, 0, EvtVarTypeBoolean, enabled);
        break;
    case EvtChannelConfigClassicEventlog:
        vp_u64(&p, 0, EvtVarTypeBoolean, FALSE);
        break;
    case EvtChannelConfigIsolation:
        vp_u64(&p, 0, EvtVarTypeUInt32, isolation);
        break;
    case EvtChannelConfigType:
        vp_u64(&p, 0, EvtVarTypeUInt32, type);
        break;
    case EvtChannelConfigOwningPublisher:
    case EvtChannelPublisherList:
        text = reg_string(key, L"OwningPublisher");
        owner = registered_publisher_name(text);
        if (id == EvtChannelConfigOwningPublisher) vp_string(&p, 0, owner ? owner : L"");
        else
        {
            names[0] = owner;
            vp_string_array(&p, 0, names, owner ? 1 : 0);
        }
        break;
    case EvtChannelConfigAccess:
        text = reg_string(key, L"ChannelAccess");
        vp_string(&p, 0, text && text[0] ? text : isolation == 1 ? default_sddl_system : default_sddl_other);
        break;
    case EvtChannelLoggingConfigRetention:
        vp_u64(&p, 0, EvtVarTypeBoolean, !file_log);
        break;
    case EvtChannelLoggingConfigAutoBackup:
        vp_u64(&p, 0, EvtVarTypeBoolean, FALSE);
        break;
    case EvtChannelLoggingConfigMaxSize:
        vp_u64(&p, 0, EvtVarTypeUInt64, 1052672);
        break;
    case EvtChannelLoggingConfigLogFilePath:
        /* the name of the file is the name of the channel, with the slash as %4 */
        len = ARRAY_SIZE(default_log_dir) + wcslen(channel) * 2 + ARRAY_SIZE(L".evtx");
        if ((file = calloc(len, sizeof(WCHAR))))
        {
            wcscpy(file, default_log_dir);
            for (i = 0; channel[i]; i++)
            {
                if (channel[i] == '/') wcscat(file, L"%4");
                else file[wcslen(file)] = channel[i];
            }
            wcscat(file, file_log ? L".evtx" : L".etl");
        }
        vp_string(&p, 0, file ? file : L"");
        break;
    case EvtChannelPublishingConfigLevel:
    case EvtChannelPublishingConfigKeywords:
    case EvtChannelPublishingConfigControlGuid:
        vp_null(&p, 0);
        break;
    case EvtChannelPublishingConfigBufferSize:
        vp_u64(&p, 0, EvtVarTypeUInt32, 64);
        break;
    case EvtChannelPublishingConfigMinBuffers:
    case EvtChannelPublishingConfigClockType:
        vp_u64(&p, 0, EvtVarTypeUInt32, 0);
        break;
    case EvtChannelPublishingConfigMaxBuffers:
        vp_u64(&p, 0, EvtVarTypeUInt32, 16);
        break;
    case EvtChannelPublishingConfigLatency:
        vp_u64(&p, 0, EvtVarTypeUInt32, 1000);
        break;
    case EvtChannelPublishingConfigSidType:
    case EvtChannelPublishingConfigFileMax:
        vp_u64(&p, 0, EvtVarTypeUInt32, 1);
        break;
    default:
        RegCloseKey(key);
        return ERROR_INVALID_PARAMETER;
    }
    RegCloseKey(key);
    free(text);
    free(owner);
    free(file);
    return vp_finish(&p, used);
}

DWORD classic_channel_property(const WCHAR *channel, DWORD id, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;
    HKEY key;
    DWORD value, status;
    WCHAR *text = NULL, **names = NULL;
    DWORD count = 0;
    BOOL is_system = !_wcsicmp(channel, L"System"), is_security = !_wcsicmp(channel, L"Security"), ret;

    if ((status = open_log_key(channel, &key))) return registered_channel_property(channel, id, size, buffer, used);
    vp_init(&p, buffer, size, 1);

    switch (id)
    {
    case EvtChannelConfigEnabled:
    case EvtChannelConfigClassicEventlog:
        vp_u64(&p, 0, EvtVarTypeBoolean, TRUE);
        break;
    case EvtChannelConfigIsolation:
        vp_u64(&p, 0, EvtVarTypeUInt32, is_system ? 1 : is_security ? 2 : 0);
        break;
    case EvtChannelConfigType:
        vp_u64(&p, 0, EvtVarTypeUInt32, 0);
        break;
    case EvtChannelConfigOwningPublisher:
        vp_string(&p, 0, L"");
        break;
    case EvtChannelConfigAccess:
        text = reg_string(key, L"CustomSD");
        vp_string(&p, 0, text && text[0] ? text : is_system ? default_sddl_system : is_security ? default_sddl_security : default_sddl_other);
        break;
    case EvtChannelLoggingConfigRetention:
        /* a log that keeps what it has and does not overwrite is the one with all bits of the retention */
        ret = reg_dword(key, L"Retention", &value) && value == 0xffffffff;
        vp_u64(&p, 0, EvtVarTypeBoolean, ret);
        break;
    case EvtChannelLoggingConfigAutoBackup:
        ret = reg_dword(key, L"AutoBackupLogFiles", &value) && value;
        vp_u64(&p, 0, EvtVarTypeBoolean, ret);
        break;
    case EvtChannelLoggingConfigMaxSize:
        if (!reg_dword(key, L"MaxSize", &value)) value = 20971520;
        vp_u64(&p, 0, EvtVarTypeUInt64, value);
        break;
    case EvtChannelLoggingConfigLogFilePath:
        if (!get_log_file(key, channel, &text)) { RegCloseKey(key); return ERROR_OUTOFMEMORY; }
        vp_string(&p, 0, text);
        break;
    case EvtChannelPublishingConfigLevel:
    case EvtChannelPublishingConfigKeywords:
    case EvtChannelPublishingConfigControlGuid:
        vp_null(&p, 0);
        break;
    case EvtChannelPublishingConfigBufferSize:
        vp_u64(&p, 0, EvtVarTypeUInt32, 64);
        break;
    case EvtChannelPublishingConfigMinBuffers:
        vp_u64(&p, 0, EvtVarTypeUInt32, 0);
        break;
    case EvtChannelPublishingConfigMaxBuffers:
        vp_u64(&p, 0, EvtVarTypeUInt32, !_wcsicmp(channel, L"Application") ? 64 : 16);
        break;
    case EvtChannelPublishingConfigLatency:
        vp_u64(&p, 0, EvtVarTypeUInt32, 1000);
        break;
    case EvtChannelPublishingConfigClockType:
        vp_u64(&p, 0, EvtVarTypeUInt32, 0);
        break;
    case EvtChannelPublishingConfigSidType:
    case EvtChannelPublishingConfigFileMax:
        vp_u64(&p, 0, EvtVarTypeUInt32, 1);
        break;
    case EvtChannelPublisherList:
        enum_sources(key, &names, &count, TRUE);
        vp_string_array(&p, 0, names, count);
        classic_names_free(names, count);
        break;
    default:
        RegCloseKey(key);
        return ERROR_INVALID_PARAMETER;
    }
    RegCloseKey(key);
    free(text);
    return vp_finish(&p, used);
}

DWORD classic_log_property(const WCHAR *channel, DWORD id, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;
    HKEY key;
    WCHAR *file = NULL, *expanded = NULL;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    BOOL have_attr = FALSE;
    HANDLE log;
    DWORD value = 0;
    ULONGLONG time = 0;
    BOOL registered = FALSE;

    if (open_log_key(channel, &key))
    {
        /* the log of a channel that a manifest registers has no file and no records */
        if (!classic_registered_channel(channel)) return ERROR_EVT_CHANNEL_NOT_FOUND;
        registered = TRUE;
    }
    if (id >= EvtLogPropertyIdEND) { if (!registered) RegCloseKey(key); return ERROR_INVALID_PARAMETER; }
    if (!registered)
    {
        if (get_log_file(key, channel, &file) && (expanded = classic_expand(file)))
            have_attr = GetFileAttributesExW(expanded, GetFileExInfoStandard, &attr);
        RegCloseKey(key);
    }
    free(file);
    free(expanded);

    vp_init(&p, buffer, size, 1);
    switch (id)
    {
    case EvtLogCreationTime:
    case EvtLogLastAccessTime:
    case EvtLogLastWriteTime:
    {
        const FILETIME *ft = !have_attr ? NULL : id == EvtLogCreationTime ? &attr.ftCreationTime :
                             id == EvtLogLastAccessTime ? &attr.ftLastAccessTime : &attr.ftLastWriteTime;

        if (ft) time = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
        vp_u64(&p, 0, EvtVarTypeFileTime, time);
        break;
    }
    case EvtLogFileSize:
        vp_u64(&p, 0, EvtVarTypeUInt64, have_attr ? ((ULONGLONG)attr.nFileSizeHigh << 32) | attr.nFileSizeLow : 0);
        break;
    case EvtLogAttributes:
        vp_u64(&p, 0, EvtVarTypeUInt32, have_attr ? attr.dwFileAttributes : 0);
        break;
    case EvtLogNumberOfLogRecords:
    case EvtLogOldestRecordNumber:
        if (!registered)
        {
            if (!(log = OpenEventLogW(NULL, channel))) return GetLastError();
            if (id == EvtLogNumberOfLogRecords) GetNumberOfEventLogRecords(log, &value);
            else GetOldestEventLogRecord(log, &value);
            CloseEventLog(log);
        }
        vp_u64(&p, 0, EvtVarTypeUInt64, value);
        break;
    case EvtLogFull:
        vp_u64(&p, 0, EvtVarTypeBoolean, FALSE);
        break;
    }
    return vp_finish(&p, used);
}

/* Queries */

struct rule
{
    DWORD group;
    BOOL suppress;
    struct xpath *path;             /* NULL for every event */
};

struct stream
{
    WCHAR *channel;
    HANDLE log;
    struct rule *rules;
    DWORD rule_count;
    BYTE *buf;
    DWORD cap, len, pos;
    BOOL eof;
    DWORD error;
    DWORD last_number;              /* the number of the record that was read last */
    BOOL have_last;
    EVENTLOGRECORD *next;           /* the next event that passes, read ahead */
};

struct classic_query
{
    struct stream *streams;
    DWORD count;
    BOOL reverse;
};

static void stream_close(struct stream *s)
{
    DWORD i;

    if (s->log) CloseEventLog(s->log);
    s->log = NULL;
    free(s->buf);
    s->buf = NULL;
    s->cap = s->len = s->pos = 0;
    free(s->next);
    s->next = NULL;
    s->eof = FALSE;
    s->error = 0;
    s->have_last = FALSE;
    for (i = 0; i < s->rule_count; i++) xpath_free(s->rules[i].path);
}

void classic_query_free(struct classic_query *query)
{
    DWORD i;

    if (!query) return;
    for (i = 0; i < query->count; i++)
    {
        struct stream *s = &query->streams[i];

        stream_close(s);
        free(s->rules);
        free(s->channel);
    }
    free(query->streams);
    free(query);
}

static DWORD stream_open(struct stream *s)
{
    HKEY key;

    /* a channel that only a manifest registers has no records */
    if (open_log_key(s->channel, &key))
    {
        if (classic_registered_channel(s->channel))
        {
            s->eof = TRUE;
            return ERROR_SUCCESS;
        }
    }
    else RegCloseKey(key);
    if (!(s->log = OpenEventLogW(NULL, s->channel))) return GetLastError();
    s->cap = 64 * 1024;
    if (!(s->buf = malloc(s->cap))) return ERROR_OUTOFMEMORY;
    return ERROR_SUCCESS;
}

DWORD classic_query_open(const struct classic_select *selects, DWORD count, BOOL reverse, struct classic_query **ret)
{
    struct classic_query *query;
    DWORD i, j, error = ERROR_SUCCESS;

    *ret = NULL;
    if (!(query = calloc(1, sizeof(*query)))) return ERROR_OUTOFMEMORY;
    query->reverse = reverse;
    if (!(query->streams = calloc(count ? count : 1, sizeof(*query->streams)))) { free(query); return ERROR_OUTOFMEMORY; }

    /* one stream for each log, with all the rules that apply to it */
    for (i = 0; i < count; i++)
    {
        struct stream *s = NULL;
        struct rule *rules, *rule;

        if (selects[i].suppress)
        {
            BOOL selected = FALSE;

            for (j = 0; j < count; j++)
                if (!selects[j].suppress && selects[j].group == selects[i].group && !_wcsicmp(selects[j].channel, selects[i].channel)) selected = TRUE;
            if (!selected) continue;
        }
        for (j = 0; j < query->count; j++)
            if (!_wcsicmp(query->streams[j].channel, selects[i].channel)) { s = &query->streams[j]; break; }
        if (!s)
        {
            s = &query->streams[query->count];
            if (!(s->channel = dup_string(selects[i].channel))) { error = ERROR_OUTOFMEMORY; goto fail; }
            query->count++;
        }
        if (!(rules = realloc(s->rules, (s->rule_count + 1) * sizeof(*rules)))) { error = ERROR_OUTOFMEMORY; goto fail; }
        s->rules = rules;
        rule = &rules[s->rule_count];
        rule->group = selects[i].group;
        rule->suppress = selects[i].suppress;
        rule->path = NULL;
        if (selects[i].xpath && wcscmp(selects[i].xpath, L"*") && !(rule->path = xpath_compile(selects[i].xpath)))
        {
            error = ERROR_EVT_INVALID_QUERY;
            goto fail;
        }
        s->rule_count++;
    }
    for (i = 0; i < query->count; i++)
        if ((error = stream_open(&query->streams[i]))) goto fail;
    *ret = query;
    return ERROR_SUCCESS;

fail:
    classic_query_free(query);
    return error;
}

DWORD classic_query_rewind(struct classic_query *query, ULONGLONG skip)
{
    DWORD i, error = ERROR_SUCCESS;

    for (i = 0; i < query->count; i++)
    {
        struct stream *s = &query->streams[i];

        /* the compiled paths stay, and only the position of the reader goes back */
        if (s->log) CloseEventLog(s->log);
        s->log = NULL;
        free(s->buf);
        s->buf = NULL;
        free(s->next);
        s->next = NULL;
        s->cap = s->len = s->pos = 0;
        s->eof = FALSE;
        s->error = 0;
        s->have_last = FALSE;
        if ((error = stream_open(s))) return error;
    }
    while (skip--)
    {
        DWORD e;
        const WCHAR *channel;
        EVENTLOGRECORD *rec = classic_query_next(query, &channel, &e);

        if (!rec) break;
        free(rec);
    }
    return ERROR_SUCCESS;
}

/* Whether the event passes the rules of the stream */
static BOOL stream_matches(struct stream *s, const EVENTLOGRECORD *rec)
{
    struct classic_event event;
    struct xnode *doc = NULL;
    BOOL result = FALSE, parsed = FALSE;
    DWORD i, g;

    for (i = 0; i < s->rule_count; i++)
        if (s->rules[i].path) break;
    if (i == s->rule_count)
    {
        /* no rule looks into the event: a group selects it when it has a selection, and no suppression stands against it */
        for (i = 0; i < s->rule_count; i++)
        {
            if (s->rules[i].suppress) continue;
            for (g = 0; g < s->rule_count; g++)
                if (s->rules[g].suppress && s->rules[g].group == s->rules[i].group) break;
            if (g == s->rule_count) return TRUE;
        }
        return FALSE;
    }

    if (!classic_event_init(&event, rec, s->channel)) return FALSE;
    for (i = 0; i < s->rule_count && !result; i++)
    {
        struct rule *sel = &s->rules[i];
        BOOL selected, suppressed = FALSE;

        if (sel->suppress) continue;
        if (sel->path && !parsed)
        {
            WCHAR *xml = classic_render_xml(&event, NULL);

            parsed = TRUE;
            if (xml) doc = xml_parse_event(xml);
            free(xml);
        }
        selected = !sel->path || (doc && xpath_match(sel->path, doc));
        if (!selected) continue;
        for (g = 0; g < s->rule_count; g++)
        {
            struct rule *sup = &s->rules[g];

            if (!sup->suppress || sup->group != sel->group) continue;
            if (!sup->path) { suppressed = TRUE; break; }
            if (!parsed)
            {
                WCHAR *xml = classic_render_xml(&event, NULL);

                parsed = TRUE;
                if (xml) doc = xml_parse_event(xml);
                free(xml);
            }
            if (doc && xpath_match(sup->path, doc)) { suppressed = TRUE; break; }
        }
        result = !suppressed;
    }
    xml_free(doc);
    classic_event_free(&event);
    return result;
}

/* Reads on until a record passes, or the log ends */
static void stream_advance(struct classic_query *query, struct stream *s)
{
    while (!s->next && !s->eof)
    {
        const EVENTLOGRECORD *rec;
        DWORD read, needed;

        if (s->pos >= s->len)
        {
            s->pos = s->len = 0;
            if (!ReadEventLogW(s->log, EVENTLOG_SEQUENTIAL_READ | (query->reverse ? EVENTLOG_BACKWARDS_READ : EVENTLOG_FORWARDS_READ),
                               0, s->buf, s->cap, &read, &needed))
            {
                DWORD error = GetLastError();

                if (error == ERROR_INSUFFICIENT_BUFFER && needed > s->cap && needed <= MAX_LOG_BUFFER)
                {
                    BYTE *buf = realloc(s->buf, needed);

                    if (buf)
                    {
                        s->buf = buf;
                        s->cap = needed;
                        continue;
                    }
                    error = ERROR_OUTOFMEMORY;
                }
                s->eof = TRUE;
                if (error != ERROR_HANDLE_EOF) s->error = error;
                return;
            }
            if (!read) { s->eof = TRUE; return; }
            s->len = min(read, s->cap);
        }

        rec = (const EVENTLOGRECORD *)(s->buf + s->pos);
        if (s->len - s->pos < sizeof(DWORD) || rec->Length < sizeof(EVENTLOGRECORD) || rec->Length > s->len - s->pos)
        {
            /* a block that does not hold whole records ends what can be read */
            s->eof = TRUE;
            s->error = ERROR_EVT_INVALID_EVENT_DATA;
            return;
        }
        s->pos += (rec->Length + 3) & ~3;
        /* the records of a log come in the order of their numbers; a reader that goes round again has ended */
        if (s->have_last && (query->reverse ? rec->RecordNumber >= s->last_number : rec->RecordNumber <= s->last_number))
        {
            s->eof = TRUE;
            return;
        }
        s->last_number = rec->RecordNumber;
        s->have_last = TRUE;
        if (!stream_matches(s, rec)) continue;
        if (!(s->next = malloc(rec->Length))) { s->eof = TRUE; s->error = ERROR_OUTOFMEMORY; return; }
        memcpy(s->next, rec, rec->Length);
    }
}

EVENTLOGRECORD *classic_query_next(struct classic_query *query, const WCHAR **channel, DWORD *error)
{
    struct stream *best = NULL;
    EVENTLOGRECORD *rec;
    DWORD i;

    *error = ERROR_NO_MORE_ITEMS;
    for (i = 0; i < query->count; i++)
    {
        struct stream *s = &query->streams[i];

        stream_advance(query, s);
        if (!s->next) continue;
        if (!best) best = s;
        else if (query->reverse ? s->next->TimeGenerated > best->next->TimeGenerated :
                                  s->next->TimeGenerated < best->next->TimeGenerated) best = s;
    }
    if (!best) return NULL;
    rec = best->next;
    best->next = NULL;
    *channel = best->channel;
    *error = ERROR_SUCCESS;
    return rec;
}

DWORD classic_query_names(const struct classic_query *query, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;
    WCHAR **names;
    DWORD i;

    if (!(names = calloc(query->count ? query->count : 1, sizeof(*names)))) return ERROR_OUTOFMEMORY;
    for (i = 0; i < query->count; i++) names[i] = query->streams[i].channel;
    vp_init(&p, buffer, size, 1);
    vp_string_array(&p, 0, names, query->count);
    free(names);
    return vp_finish(&p, used);
}

DWORD classic_query_statuses(const struct classic_query *query, DWORD size, void *buffer, DWORD *used)
{
    struct vpack p;
    UINT32 *array;
    EVT_VARIANT *v;
    DWORD i;

    vp_init(&p, buffer, size, 1);
    array = vp_alloc(&p, (SIZE_T)query->count * sizeof(UINT32));
    v = vp_slot(&p, 0);
    if (array)
        for (i = 0; i < query->count; i++) array[i] = query->streams[i].error;
    if (v && (array || !query->count))
    {
        v->UInt64Val = 0;
        v->UInt32Arr = array;
        v->Count = query->count;
        v->Type = EvtVarTypeUInt32 | EVT_VARIANT_TYPE_ARRAY;
    }
    return vp_finish(&p, used);
}
