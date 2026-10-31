/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Task Scheduler 2.0: the registry copy of the task store, as Windows keeps it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * Windows keeps, next to the task XML files under %SystemRoot%\System32\Tasks, an index of them in
 * HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache (shape measured on Windows 10
 * 22H2 with read-only queries):
 *
 *   Tree\<folders>\<task>   Id (REG_SZ, "{GUID}"), Index (REG_DWORD, 3), SD (REG_BINARY)
 *   Tasks\{GUID}            Path, URI, Author, Date, Description, Source (REG_SZ), Schema (REG_DWORD,
 *                           major << 16 | minor of the Task version), Hash, Triggers, Actions and
 *                           DynamicInfo (REG_BINARY, private formats)
 *   Plain|Boot|Logon\{GUID} empty keys: how the task is started (Maintenance and others are not used here)
 *
 * This service writes Tree, Tasks (the strings and Schema) and the Plain, Boot and Logon keys, so that
 * what reads the registry (tools, installers that look for their task) finds the task. Not written:
 * the security descriptor (this system does not enforce task ACLs), the hash and the three binary
 * blobs, whose formats are private to Windows and which nothing here reads.
 *
 * Nothing in this file is trusted: which tasks run, and as whom, comes only from the service's own
 * record (Services\Schedule\TaskCache, taskrun.c), never from these keys.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>

#include <wine/debug.h>

#include "schedsvc_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(schedsvc);

#define WIN_TASKCACHE_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache"

/* the text of the first <name> element, entities decoded; NULL if there is none */
static WCHAR *xml_element(const WCHAR *xml, const WCHAR *name)
{
    static const struct { const WCHAR *entity; WCHAR ch; } entities[] =
    {
        { L"&amp;", '&' }, { L"&lt;", '<' }, { L"&gt;", '>' }, { L"&quot;", '"' }, { L"&apos;", '\'' },
    };
    WCHAR open[64], close[64], *text, *out;
    const WCHAR *start, *end, *p;
    unsigned int i;

    if (wcslen(name) > 40) return NULL;
    _snwprintf(open, ARRAYSIZE(open), L"<%s>", name);
    _snwprintf(close, ARRAYSIZE(close), L"</%s>", name);
    if (!(start = wcsstr(xml, open))) return NULL;
    start += wcslen(open);
    if (!(end = wcsstr(start, close))) return NULL;

    if (!(text = calloc(end - start + 1, sizeof(WCHAR)))) return NULL;
    for (p = start, out = text; p < end;)
    {
        for (i = 0; i < ARRAYSIZE(entities); i++)
        {
            SIZE_T len = wcslen(entities[i].entity);
            if (!wcsncmp(p, entities[i].entity, len))
            {
                *out++ = entities[i].ch;
                p += len;
                break;
            }
        }
        if (i == ARRAYSIZE(entities)) *out++ = *p++;
    }
    return text;
}

/* "Folder\Task": no leading separator, '/' as '\' */
static WCHAR *tree_path(const WCHAR *path)
{
    WCHAR *copy, *p;

    while (*path == '\\' || *path == '/') path++;
    if (!*path || !(copy = wcsdup(path))) return NULL;
    for (p = copy; *p; p++) if (*p == '/') *p = '\\';
    for (p = copy + wcslen(copy); p > copy && p[-1] == '\\'; p--) p[-1] = 0;
    return copy;
}

static void set_string(HKEY key, const WCHAR *name, const WCHAR *value)
{
    if (value) RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (wcslen(value) + 1) * sizeof(WCHAR));
}

static void set_string_from_xml(HKEY key, const WCHAR *name, const WCHAR *xml)
{
    WCHAR *text = xml_element(xml, name);

    set_string(key, name, text);
    free(text);
}

/* the Schema value: the version attribute of the Task element, "1.2" -> 0x10002 */
static DWORD task_schema(const WCHAR *xml)
{
    const WCHAR *task = wcsstr(xml, L"<Task"), *end, *version;
    unsigned int major = 1, minor = 2;

    if (task && (end = wcschr(task, '>')) && (version = wcsstr(task, L"version=\"")) && version < end)
    {
        if (swscanf(version + 9, L"%u.%u", &major, &minor) != 2 || major > 0xffff || minor > 0xffff)
            major = 1, minor = 2;
    }
    return (major << 16) | minor;
}

static void delete_class_keys(HKEY cache, const WCHAR *id)
{
    static const WCHAR *classes[] = { L"Plain", L"Boot", L"Logon" };
    unsigned int i;
    HKEY key;

    for (i = 0; i < ARRAYSIZE(classes); i++)
    {
        if (RegOpenKeyExW(cache, classes[i], 0, KEY_ALL_ACCESS, &key)) continue;
        RegDeleteKeyW(key, id);
        RegCloseKey(key);
    }
}

void schedsvc_mirror_registered(const WCHAR *path, const WCHAR *xml)
{
    WCHAR *rel, id[64] = L"", *text;
    SYSTEMTIME st;
    HKEY cache = NULL, tree = NULL, key = NULL, tasks = NULL, klass = NULL;
    DWORD size, value;
    const WCHAR *class_name;
    GUID guid;

    if (!(rel = tree_path(path))) return;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, WIN_TASKCACHE_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &cache, NULL))
        goto done;
    if (RegCreateKeyExW(cache, L"Tree", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &tree, NULL) ||
        RegCreateKeyExW(tree, rel, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
        goto done;

    /* an update keeps the Id of the task */
    size = sizeof(id) - sizeof(WCHAR);
    if (RegQueryValueExW(key, L"Id", NULL, NULL, (BYTE *)id, &size) || id[0] != '{')
    {
        UuidCreate(&guid);
        StringFromGUID2(&guid, id, ARRAYSIZE(id));
        set_string(key, L"Id", id);
    }
    value = 3;
    RegSetValueExW(key, L"Index", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(key);
    key = NULL;

    if (RegCreateKeyExW(cache, L"Tasks", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &tasks, NULL) ||
        RegCreateKeyExW(tasks, id, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
        goto done;

    text = malloc((wcslen(rel) + 2) * sizeof(WCHAR));
    if (text)
    {
        text[0] = '\\';
        wcscpy(text + 1, rel);
        set_string(key, L"Path", text);
        free(text);
    }
    value = task_schema(xml);
    RegSetValueExW(key, L"Schema", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    set_string_from_xml(key, L"Author", xml);
    set_string_from_xml(key, L"Description", xml);
    set_string_from_xml(key, L"Source", xml);
    if ((text = xml_element(xml, L"URI")))
    {
        set_string(key, L"URI", text);
        free(text);
    }
    if ((text = xml_element(xml, L"Date")))
    {
        set_string(key, L"Date", text);
        free(text);
    }
    else
    {
        WCHAR now[32];

        GetLocalTime(&st);
        _snwprintf(now, ARRAYSIZE(now), L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
        set_string(key, L"Date", now);
    }

    /* how the task starts: at boot, at logon, or otherwise (Plain) */
    delete_class_keys(cache, id);
    class_name = wcsstr(xml, L"<BootTrigger") ? L"Boot" : wcsstr(xml, L"<LogonTrigger") ? L"Logon" : L"Plain";
    if (!RegCreateKeyExW(cache, class_name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &klass, NULL))
    {
        HKEY entry;

        if (!RegCreateKeyExW(klass, id, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &entry, NULL)) RegCloseKey(entry);
        RegCloseKey(klass);
    }

done:
    if (key) RegCloseKey(key);
    if (tasks) RegCloseKey(tasks);
    if (tree) RegCloseKey(tree);
    if (cache) RegCloseKey(cache);
    free(rel);
}

void schedsvc_mirror_deleted(const WCHAR *path)
{
    WCHAR *rel, id[64] = L"";
    HKEY cache = NULL, tree = NULL, key = NULL, tasks;
    DWORD size;

    if (!(rel = tree_path(path))) return;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WIN_TASKCACHE_KEY, 0, KEY_ALL_ACCESS, &cache) ||
        RegOpenKeyExW(cache, L"Tree", 0, KEY_ALL_ACCESS, &tree) ||
        RegOpenKeyExW(tree, rel, 0, KEY_ALL_ACCESS, &key))
        goto done;

    size = sizeof(id) - sizeof(WCHAR);
    if (!RegQueryValueExW(key, L"Id", NULL, NULL, (BYTE *)id, &size) && id[0] == '{')
    {
        if (!RegOpenKeyExW(cache, L"Tasks", 0, KEY_ALL_ACCESS, &tasks))
        {
            RegDeleteKeyW(tasks, id);
            RegCloseKey(tasks);
        }
        delete_class_keys(cache, id);
    }
    RegDeleteValueW(key, L"Id");
    RegDeleteValueW(key, L"Index");
    RegCloseKey(key);
    key = NULL;
    /* the key of a task has no subkeys: a folder's does, and stays */
    RegDeleteKeyW(tree, rel);

done:
    if (key) RegCloseKey(key);
    if (tree) RegCloseKey(tree);
    if (cache) RegCloseKey(cache);
    free(rel);
}
