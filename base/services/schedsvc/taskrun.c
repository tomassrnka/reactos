/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Task Scheduler 2.0: run the actions of registered tasks when their triggers fire
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * The ITaskSchedulerService server (schrpc.c, from Wine) stores the task XML under
 * %SystemRoot%\System32\Tasks. When a task is registered, the service also records it in
 * HKLM\SYSTEM\CurrentControlSet\Services\Schedule\TaskCache, together with the SID of the
 * caller that registered it and whether that caller was LocalSystem or an administrator.
 * The trigger thread below only runs tasks from that record, never from the files.
 *
 * Identity: a task runs as its principal (Principal/UserId), or as the registering user when
 * it has none. LocalSystem tasks run only when an administrator or LocalSystem registered
 * them; a task registered by anybody else runs only as that same user. A user task needs the
 * user to be logged on: its process gets the token of the user's shell (no stored passwords).
 *
 * Triggers: TimeTrigger and CalendarTrigger/ScheduleByDay (with Repetition, StartBoundary and
 * EndBoundary), LogonTrigger and BootTrigger (with Delay). The other trigger types are ignored.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

#define COBJMACROS

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <rpc.h>
#include <xmllite.h>
#include <taskschd.h>

#include <wine/debug.h>
#include <wine/list.h>

#include "schedsvc_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(schedsvc);

DWORD TaskSchedulerStart(VOID);
VOID TaskSchedulerStop(VOID);

#define TASKCACHE_KEY L"SYSTEM\\CurrentControlSet\\Services\\Schedule\\TaskCache"

#define TICKS_PER_SECOND 10000000ULL
#define NO_TIME (~0ULL)
#define MAX_TRIGGERS 32
#define MAX_ACTIONS 32
#define MAX_DEPTH 16
/* boot triggers, and logons that predate the service, only count this soon after boot */
#define BOOT_WINDOW_MS (10 * 60 * 1000)
#define LOGON_POLL_MS 5000
#define MAX_WAIT_MS 60000

struct trigger
{
    TASK_TRIGGER_TYPE2 type;
    BOOL supported;
    BOOL enabled;
    ULONGLONG start;        /* UTC, 0 if absent */
    ULONGLONG end;          /* UTC, 0 if absent */
    ULONGLONG period;       /* CalendarTrigger/ScheduleByDay */
    ULONGLONG rep_interval;
    ULONGLONG rep_duration;
    ULONGLONG delay;
    WCHAR *user_id;         /* LogonTrigger */
};

struct action
{
    WCHAR *command;
    WCHAR *arguments;
    WCHAR *directory;
};

struct task
{
    struct list entry;
    WCHAR *path;
    PSID registrant;        /* NULL if unknown */
    BOOL privileged;        /* registered by LocalSystem or an administrator */
    BOOL enabled;
    WCHAR *user_id;         /* Principal/UserId */
    unsigned int n_triggers;
    unsigned int n_actions;
    struct trigger triggers[MAX_TRIGGERS];
    struct action actions[MAX_ACTIONS];
    ULONGLONG pending;      /* a delayed logon or boot start, NO_TIME if none */
    HANDLE process;         /* the last instance, to skip a start while it still runs */
};

struct logon
{
    LUID id;
    BOOL alive;
};

static struct list tasks = LIST_INIT(tasks);
static struct logon logons[64];
static unsigned int n_logons;
static HANDLE stop_event, update_event, scheduler_thread;

static ULONGLONG get_time(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static WCHAR *strdupW(const WCHAR *str)
{
    return str ? wcsdup(str) : NULL;
}

/* "YYYY-MM-DDTHH:MM:SS[.fff][Z|+HH:MM|-HH:MM]"; no zone means local time */
static BOOL parse_datetime(const WCHAR *str, ULONGLONG *time)
{
    SYSTEMTIME st = {0}, utc;
    FILETIME ft;
    int year, month, day, hour = 0, minute = 0, second = 0, n = 0;
    LONGLONG offset = 0;
    BOOL zone = FALSE;
    const WCHAR *p;

    if (swscanf(str, L"%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute, &second, &n) < 6 ||
        !n)
    {
        WARN("unsupported date %s\n", debugstr_w(str));
        return FALSE;
    }
    p = str + n;
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++);
    if (*p == 'Z')
        zone = TRUE;
    else if (*p == '+' || *p == '-')
    {
        int oh = 0, om = 0;
        if (swscanf(p + 1, L"%2d:%2d", &oh, &om) < 1) return FALSE;
        offset = (LONGLONG)(oh * 60 + om) * 60 * TICKS_PER_SECOND;
        if (*p == '-') offset = -offset;
        zone = TRUE;
    }

    st.wYear = year;
    st.wMonth = month;
    st.wDay = day;
    st.wHour = hour;
    st.wMinute = minute;
    st.wSecond = second;
    if (!zone)
    {
        if (!TzSpecificLocalTimeToSystemTime(NULL, &st, &utc)) return FALSE;
        st = utc;
    }
    if (!SystemTimeToFileTime(&st, &ft)) return FALSE;
    *time = (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - offset;
    return TRUE;
}

/* ISO 8601 duration "PnYnMnWnDTnHnMnS", in 100 ns units; 0 if invalid */
static ULONGLONG parse_duration(const WCHAR *str)
{
    ULONGLONG seconds = 0, n;
    BOOL time_part = FALSE;
    WCHAR *end;

    if (*str++ != 'P') return 0;
    while (*str)
    {
        if (*str == 'T')
        {
            time_part = TRUE;
            str++;
            continue;
        }
        n = wcstoul(str, &end, 10);
        if (end == str) return 0;
        str = end;
        switch (*str++)
        {
        case 'Y': seconds += n * 365 * 86400; break;
        case 'M': seconds += time_part ? n * 60 : n * 30 * 86400; break;
        case 'W': seconds += n * 7 * 86400; break;
        case 'D': seconds += n * 86400; break;
        case 'H': seconds += n * 3600; break;
        case 'S': seconds += n; break;
        default: return 0;
        }
    }
    return seconds * TICKS_PER_SECOND;
}

static BOOL parse_bool(const WCHAR *str)
{
    return !wcscmp(str, L"true") || !wcscmp(str, L"1");
}

/* the first run of a time-based trigger strictly after 'after', NO_TIME if none */
static ULONGLONG trigger_next_run(const struct trigger *trigger, ULONGLONG after)
{
    ULONGLONG base, next, j;

    if (!trigger->supported || !trigger->enabled || !trigger->start) return NO_TIME;
    if (trigger->type != TASK_TRIGGER_TIME && trigger->type != TASK_TRIGGER_DAILY) return NO_TIME;

    if (after < trigger->start)
        next = trigger->start;
    else
    {
        base = trigger->start;
        if (trigger->type == TASK_TRIGGER_DAILY)
            base += (after - trigger->start) / trigger->period * trigger->period;

        next = NO_TIME;
        if (trigger->rep_interval)
        {
            j = (after - base) / trigger->rep_interval + 1;
            if (!trigger->rep_duration || j * trigger->rep_interval < trigger->rep_duration)
                next = base + j * trigger->rep_interval;
        }
        if (trigger->type == TASK_TRIGGER_DAILY && base + trigger->period < next)
            next = base + trigger->period;
    }

    if (trigger->end && next != NO_TIME && next > trigger->end) return NO_TIME;
    return next;
}

static void free_task(struct task *task)
{
    unsigned int i;

    for (i = 0; i < task->n_triggers; i++)
        free(task->triggers[i].user_id);
    for (i = 0; i < task->n_actions; i++)
    {
        free(task->actions[i].command);
        free(task->actions[i].arguments);
        free(task->actions[i].directory);
    }
    free(task->path);
    free(task->user_id);
    LocalFree(task->registrant);
    if (task->process) CloseHandle(task->process);
    free(task);
}

static struct trigger *current_trigger(struct task *task)
{
    return task->n_triggers ? &task->triggers[task->n_triggers - 1] : NULL;
}

static struct action *current_action(struct task *task)
{
    return task->n_actions ? &task->actions[task->n_actions - 1] : NULL;
}

/* an element opened; names[] is its path below the document, names[0] = "Task" */
static void begin_element(struct task *task, const WCHAR **names, unsigned int depth)
{
    static const struct { const WCHAR *name; TASK_TRIGGER_TYPE2 type; BOOL supported; } trigger_types[] =
    {
        { L"TimeTrigger", TASK_TRIGGER_TIME, TRUE },
        { L"CalendarTrigger", TASK_TRIGGER_DAILY, TRUE },
        { L"LogonTrigger", TASK_TRIGGER_LOGON, TRUE },
        { L"BootTrigger", TASK_TRIGGER_BOOT, TRUE },
        { L"IdleTrigger", TASK_TRIGGER_IDLE, FALSE },
        { L"RegistrationTrigger", TASK_TRIGGER_REGISTRATION, FALSE },
        { L"EventTrigger", TASK_TRIGGER_EVENT, FALSE },
        { L"SessionStateChangeTrigger", TASK_TRIGGER_SESSION_STATE_CHANGE, FALSE },
    };
    struct trigger *trigger;
    unsigned int i;

    if (depth == 3 && !wcscmp(names[1], L"Triggers"))
    {
        if (task->n_triggers >= MAX_TRIGGERS)
        {
            FIXME("too many triggers in %s\n", debugstr_w(task->path));
            return;
        }
        trigger = &task->triggers[task->n_triggers++];
        memset(trigger, 0, sizeof(*trigger));
        trigger->enabled = TRUE;
        trigger->type = TASK_TRIGGER_TIME;
        trigger->period = 86400 * TICKS_PER_SECOND;
        for (i = 0; i < ARRAYSIZE(trigger_types); i++)
        {
            if (wcscmp(names[2], trigger_types[i].name)) continue;
            trigger->type = trigger_types[i].type;
            trigger->supported = trigger_types[i].supported;
            break;
        }
        if (!trigger->supported)
            FIXME("%s: %s is not supported\n", debugstr_w(task->path), debugstr_w(names[2]));
    }
    else if (depth == 4 && !wcscmp(names[1], L"Triggers") && !wcscmp(names[2], L"CalendarTrigger") &&
             wcscmp(names[3], L"ScheduleByDay") && !wcsncmp(names[3], L"ScheduleBy", 10))
    {
        FIXME("%s: CalendarTrigger/%s is not supported\n", debugstr_w(task->path), debugstr_w(names[3]));
        if ((trigger = current_trigger(task))) trigger->supported = FALSE;
    }
    else if (depth == 3 && !wcscmp(names[1], L"Actions"))
    {
        if (wcscmp(names[2], L"Exec"))
        {
            FIXME("%s: action %s is not supported\n", debugstr_w(task->path), debugstr_w(names[2]));
            return;
        }
        if (task->n_actions >= MAX_ACTIONS)
        {
            FIXME("too many actions in %s\n", debugstr_w(task->path));
            return;
        }
        memset(&task->actions[task->n_actions++], 0, sizeof(task->actions[0]));
    }
}

static void element_text(struct task *task, const WCHAR **names, unsigned int depth, const WCHAR *value)
{
    struct trigger *trigger;
    struct action *action;
    const WCHAR *name = names[depth - 1];

    if (depth == 3 && !wcscmp(names[1], L"Settings"))
    {
        if (!wcscmp(name, L"Enabled")) task->enabled = parse_bool(value);
    }
    else if (depth == 4 && !wcscmp(names[1], L"Principals") && !wcscmp(names[2], L"Principal"))
    {
        if (!wcscmp(name, L"UserId"))
        {
            free(task->user_id);
            task->user_id = strdupW(value);
        }
    }
    else if (depth >= 4 && !wcscmp(names[1], L"Triggers") && (trigger = current_trigger(task)))
    {
        if (depth == 4)
        {
            if (!wcscmp(name, L"Enabled")) trigger->enabled = parse_bool(value);
            else if (!wcscmp(name, L"StartBoundary"))
            {
                if (!parse_datetime(value, &trigger->start)) trigger->supported = FALSE;
            }
            else if (!wcscmp(name, L"EndBoundary"))
            {
                if (!parse_datetime(value, &trigger->end)) trigger->end = 0;
            }
            else if (!wcscmp(name, L"Delay")) trigger->delay = parse_duration(value);
            else if (!wcscmp(name, L"UserId"))
            {
                free(trigger->user_id);
                trigger->user_id = strdupW(value);
            }
        }
        else if (depth == 5 && !wcscmp(names[3], L"Repetition"))
        {
            if (!wcscmp(name, L"Interval")) trigger->rep_interval = parse_duration(value);
            else if (!wcscmp(name, L"Duration")) trigger->rep_duration = parse_duration(value);
        }
        else if (depth == 5 && !wcscmp(names[3], L"ScheduleByDay") && !wcscmp(name, L"DaysInterval"))
        {
            int days = _wtoi(value);
            if (days > 0) trigger->period = days * 86400 * TICKS_PER_SECOND;
        }
    }
    else if (depth == 4 && !wcscmp(names[1], L"Actions") && !wcscmp(names[2], L"Exec") &&
             (action = current_action(task)))
    {
        if (!wcscmp(name, L"Command"))
        {
            free(action->command);
            action->command = strdupW(value);
        }
        else if (!wcscmp(name, L"Arguments"))
        {
            free(action->arguments);
            action->arguments = strdupW(value);
        }
        else if (!wcscmp(name, L"WorkingDirectory"))
        {
            free(action->directory);
            action->directory = strdupW(value);
        }
    }
}

static HRESULT parse_task_xml(struct task *task, const WCHAR *xml)
{
    WCHAR *names[MAX_DEPTH];
    unsigned int depth = 0, skip = 0, i;
    IXmlReader *reader = NULL;
    IStream *stream = NULL;
    XmlNodeType type;
    const WCHAR *name, *value;
    HGLOBAL hmem;
    SIZE_T size;
    HRESULT hr;

    task->enabled = TRUE;

    size = wcslen(xml) * sizeof(WCHAR);
    if (!(hmem = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1))) return E_OUTOFMEMORY;
    memcpy(GlobalLock(hmem), xml, size);
    GlobalUnlock(hmem);
    hr = CreateStreamOnHGlobal(hmem, TRUE, &stream);
    if (FAILED(hr))
    {
        GlobalFree(hmem);
        return hr;
    }
    /* the stream is shorter than the buffer GlobalAlloc may have rounded up to */
    {
        ULARGE_INTEGER len;
        len.QuadPart = size;
        IStream_SetSize(stream, len);
    }

    hr = CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL);
    if (SUCCEEDED(hr)) hr = IXmlReader_SetInput(reader, (IUnknown *)stream);

    while (SUCCEEDED(hr) && IXmlReader_Read(reader, &type) == S_OK)
    {
        switch (type)
        {
        case XmlNodeType_Element:
        {
            BOOL empty = IXmlReader_IsEmptyElement(reader);

            if (FAILED(hr = IXmlReader_GetLocalName(reader, &name, NULL))) break;
            if (skip || depth >= MAX_DEPTH)
            {
                if (!empty) skip++;
                break;
            }
            if (!(names[depth] = wcsdup(name)))
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            depth++;
            if (wcscmp(names[0], L"Task"))
            {
                hr = SCHED_E_MALFORMEDXML;
                break;
            }
            begin_element(task, (const WCHAR **)names, depth);
            if (empty) free(names[--depth]);
            break;
        }
        case XmlNodeType_EndElement:
            if (skip) skip--;
            else if (depth) free(names[--depth]);
            break;

        case XmlNodeType_Text:
        case XmlNodeType_CDATA:
            if (skip || !depth) break;
            if (FAILED(hr = IXmlReader_GetValue(reader, &value, NULL))) break;
            element_text(task, (const WCHAR **)names, depth, value);
            break;

        default:
            break;
        }
    }

    for (i = 0; i < depth; i++) free(names[i]);
    if (reader) IXmlReader_Release(reader);
    IStream_Release(stream);
    if (FAILED(hr)) WARN("%s: failed to parse the task, hr %#lx\n", debugstr_w(task->path), hr);
    return FAILED(hr) ? hr : S_OK;
}

/* the TaskCache key name of a task path: no leading separator, '\' and '/' become '|' */
static WCHAR *cache_name(const WCHAR *path)
{
    WCHAR *name, *p;

    while (*path == '\\' || *path == '/') path++;
    if (!*path || !(name = wcsdup(path))) return NULL;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/') *p = '|';
    return name;
}

static WCHAR *reg_get_string(HKEY key, const WCHAR *value)
{
    DWORD type, size = 0;
    WCHAR *str;

    if (RegQueryValueExW(key, value, NULL, &type, NULL, &size) || type != REG_SZ) return NULL;
    if (!(str = calloc(1, size + sizeof(WCHAR)))) return NULL;
    if (RegQueryValueExW(key, value, NULL, NULL, (BYTE *)str, &size))
    {
        free(str);
        return NULL;
    }
    return str;
}

static struct task *load_task(HKEY cache, const WCHAR *name)
{
    struct task *task;
    WCHAR *xml, *sid, *p;
    DWORD privileged = 0, size = sizeof(privileged);
    HKEY key;

    if (RegOpenKeyExW(cache, name, 0, KEY_READ, &key)) return NULL;
    if (!(task = calloc(1, sizeof(*task))))
    {
        RegCloseKey(key);
        return NULL;
    }
    task->pending = NO_TIME;
    task->path = wcsdup(name);
    if (task->path)
        for (p = task->path; *p; p++)
            if (*p == '|') *p = '\\';

    xml = reg_get_string(key, L"Xml");
    if ((sid = reg_get_string(key, L"Registrant")))
    {
        if (!ConvertStringSidToSidW(sid, &task->registrant)) task->registrant = NULL;
        free(sid);
    }
    if (!RegQueryValueExW(key, L"Privileged", NULL, NULL, (BYTE *)&privileged, &size))
        task->privileged = privileged != 0;
    RegCloseKey(key);

    if (!task->path || !xml || parse_task_xml(task, xml) != S_OK)
    {
        free(xml);
        free_task(task);
        return NULL;
    }
    free(xml);
    return task;
}

static void load_tasks(void)
{
    struct list old = LIST_INIT(old);
    struct task *task, *prev, *next;
    WCHAR name[256];
    DWORD index, len;
    HKEY cache;

    list_move_tail(&old, &tasks);

    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, TASKCACHE_KEY, 0, KEY_READ, &cache))
    {
        for (index = 0; len = ARRAYSIZE(name), !RegEnumKeyExW(cache, index, name, &len, NULL, NULL, NULL, NULL); index++)
        {
            if (!(task = load_task(cache, name))) continue;
            /* keep what the previous copy of the task was doing */
            LIST_FOR_EACH_ENTRY(prev, &old, struct task, entry)
            {
                if (wcsicmp(prev->path, task->path)) continue;
                task->process = prev->process;
                task->pending = prev->pending;
                prev->process = NULL;
                break;
            }
            TRACE("task %s: %u triggers, %u actions, enabled %d\n", debugstr_w(task->path),
                  task->n_triggers, task->n_actions, task->enabled);
            list_add_tail(&tasks, &task->entry);
        }
        RegCloseKey(cache);
    }

    LIST_FOR_EACH_ENTRY_SAFE(prev, next, &old, struct task, entry)
    {
        list_remove(&prev->entry);
        free_task(prev);
    }
}

static PSID resolve_account(const WCHAR *name)
{
    static const WCHAR *system_names[] = { L"SYSTEM", L"NT AUTHORITY\\SYSTEM", L"LocalSystem", L".\\LocalSystem" };
    WCHAR domain[256];
    DWORD size = 0, domain_size = ARRAYSIZE(domain);
    SID_NAME_USE use;
    PSID sid = NULL;
    unsigned int i;

    if (!name || !*name) return NULL;

    if (!wcsnicmp(name, L"S-1-", 4))
    {
        if (!ConvertStringSidToSidW(name, &sid)) return NULL;
        return sid;
    }

    for (i = 0; i < ARRAYSIZE(system_names); i++)
    {
        if (wcsicmp(name, system_names[i])) continue;
        size = SECURITY_MAX_SID_SIZE;
        if (!(sid = LocalAlloc(LPTR, size))) return NULL;
        if (!CreateWellKnownSid(WinLocalSystemSid, NULL, sid, &size))
        {
            LocalFree(sid);
            return NULL;
        }
        return sid;
    }

    LookupAccountNameW(NULL, name, NULL, &size, domain, &domain_size, &use);
    if (!size || !(sid = LocalAlloc(LPTR, size))) return NULL;
    domain_size = ARRAYSIZE(domain);
    if (!LookupAccountNameW(NULL, name, sid, &size, domain, &domain_size, &use))
    {
        WARN("unknown account %s (%lu)\n", debugstr_w(name), GetLastError());
        LocalFree(sid);
        return NULL;
    }
    return sid;
}

static PSID copy_sid(PSID sid)
{
    DWORD size;
    PSID copy;

    if (!sid) return NULL;
    size = GetLengthSid(sid);
    if ((copy = LocalAlloc(LPTR, size))) CopySid(size, copy, sid);
    return copy;
}

typedef BOOL (*shell_token_callback)(HANDLE token, const LUID *logon, PSID user, void *context);

/* calls 'callback' with the token of every running shell (explorer.exe) until it returns FALSE */
static void enum_shell_tokens(shell_token_callback callback, void *context)
{
    PROCESSENTRY32W entry;
    HANDLE snapshot, process, token;
    TOKEN_STATISTICS stats;
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)buffer;
    DWORD size;
    BOOL more = TRUE;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (wcsicmp(entry.szExeFile, L"explorer.exe")) continue;
            if (!(process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, entry.th32ProcessID))) continue;
            if (OpenProcessToken(process, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &token))
            {
                if (GetTokenInformation(token, TokenStatistics, &stats, sizeof(stats), &size) &&
                    GetTokenInformation(token, TokenUser, user, sizeof(buffer), &size))
                    more = callback(token, &stats.AuthenticationId, user->User.Sid, context);
                CloseHandle(token);
            }
            CloseHandle(process);
        } while (more && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

struct find_token
{
    PSID sid;
    HANDLE token;
};

static BOOL find_token_callback(HANDLE token, const LUID *logon, PSID user, void *context)
{
    struct find_token *find = context;

    if (!EqualSid(user, find->sid)) return TRUE;
    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &find->token))
        find->token = NULL;
    return find->token == NULL;
}

/* a primary token of a logged-on user, NULL if the user has no shell running */
static HANDLE get_user_token(PSID sid)
{
    struct find_token find = { sid, NULL };
    enum_shell_tokens(find_token_callback, &find);
    return find.token;
}

static WCHAR *expand_string(HANDLE token, const WCHAR *str)
{
    WCHAR *buffer;
    DWORD size = 4096;

    if (!str) return NULL;
    if (!(buffer = malloc(size * sizeof(WCHAR)))) return NULL;
    if (token ? !ExpandEnvironmentStringsForUserW(token, str, buffer, size)
              : !ExpandEnvironmentStringsW(str, buffer, size))
        lstrcpynW(buffer, str, size);
    return buffer;
}

static HANDLE start_action(const struct task *task, const struct action *action, HANDLE token)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR *command, *arguments, *directory, *cmdline;
    void *env = NULL;
    SIZE_T len;
    BOOL ret;

    if (!action->command || !*action->command) return NULL;

    command = expand_string(token, action->command);
    arguments = expand_string(token, action->arguments);
    directory = expand_string(token, action->directory);
    len = (command ? wcslen(command) : 0) + (arguments ? wcslen(arguments) : 0) + 4;
    cmdline = malloc(len * sizeof(WCHAR));
    if (!command || !cmdline || (action->arguments && !arguments) || (action->directory && !directory))
    {
        free(command);
        free(arguments);
        free(directory);
        free(cmdline);
        return NULL;
    }

    if (command[0] == '"')
        wcscpy(cmdline, command);
    else
    {
        wcscpy(cmdline, L"\"");
        wcscat(cmdline, command);
        wcscat(cmdline, L"\"");
    }
    if (arguments && *arguments)
    {
        wcscat(cmdline, L" ");
        wcscat(cmdline, arguments);
    }

    CreateEnvironmentBlock(&env, token, FALSE);
    if (token)
    {
        si.lpDesktop = L"winsta0\\default";
        ret = CreateProcessAsUserW(token, NULL, cmdline, NULL, NULL, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE, env,
                                   directory && *directory ? directory : NULL, &si, &pi);
    }
    else
    {
        ret = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                             CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env,
                             directory && *directory ? directory : NULL, &si, &pi);
    }
    if (env) DestroyEnvironmentBlock(env);

    if (ret)
    {
        TRACE("task %s: started %s, pid %lu\n", debugstr_w(task->path), debugstr_w(cmdline), pi.dwProcessId);
        CloseHandle(pi.hThread);
    }
    else
        ERR("task %s: failed to start %s (%lu)\n", debugstr_w(task->path), debugstr_w(cmdline), GetLastError());

    free(command);
    free(arguments);
    free(directory);
    free(cmdline);
    return ret ? pi.hProcess : NULL;
}

/* the account a task runs as: its principal, else the registering user */
static PSID task_identity(const struct task *task)
{
    PSID sid = resolve_account(task->user_id);
    return sid ? sid : copy_sid(task->registrant);
}

/* starts the actions of a task; returns the process of its last action, or NULL */
static HANDLE run_task(struct task *task)
{
    HANDLE token = NULL, process = NULL, last = NULL;
    PSID sid;
    unsigned int i;

    if (!task->enabled || !task->n_actions) return NULL;

    if (!(sid = task_identity(task)))
    {
        WARN("task %s: no account to run as\n", debugstr_w(task->path));
        return NULL;
    }

    if (IsWellKnownSid(sid, WinLocalSystemSid))
    {
        if (!task->privileged)
        {
            WARN("task %s: not registered by an administrator, not running it as LocalSystem\n",
                 debugstr_w(task->path));
            goto done;
        }
    }
    else if (IsWellKnownSid(sid, WinLocalServiceSid) || IsWellKnownSid(sid, WinNetworkServiceSid))
    {
        FIXME("task %s: service accounts other than LocalSystem are not supported\n", debugstr_w(task->path));
        goto done;
    }
    else
    {
        if (!task->privileged && (!task->registrant || !EqualSid(sid, task->registrant)))
        {
            WARN("task %s: registered by another user, not running it\n", debugstr_w(task->path));
            goto done;
        }
        if (!(token = get_user_token(sid)))
        {
            TRACE("task %s: its user is not logged on\n", debugstr_w(task->path));
            goto done;
        }
    }

    for (i = 0; i < task->n_actions; i++)
    {
        if (!(process = start_action(task, &task->actions[i], token))) continue;
        if (last) CloseHandle(last);
        last = process;
    }

done:
    if (token) CloseHandle(token);
    LocalFree(sid);
    return last;
}

/* the scheduled start of a task, respecting a still running instance */
static void start_task(struct task *task)
{
    HANDLE process;

    if (task->process)
    {
        if (WaitForSingleObject(task->process, 0) == WAIT_TIMEOUT)
        {
            TRACE("task %s: still running, not starting another instance\n", debugstr_w(task->path));
            return;
        }
        CloseHandle(task->process);
        task->process = NULL;
    }
    if ((process = run_task(task))) task->process = process;
}

static void schedule_event_trigger(struct task *task, const struct trigger *trigger, ULONGLONG now)
{
    ULONGLONG when = now + trigger->delay;
    if (when < task->pending) task->pending = when;
}

static BOOL is_boot_window(void)
{
    return GetTickCount64() < BOOT_WINDOW_MS;
}

static void on_boot(ULONGLONG now)
{
    struct task *task;
    unsigned int i;

    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
        for (i = 0; i < task->n_triggers; i++)
            if (task->triggers[i].type == TASK_TRIGGER_BOOT && task->triggers[i].supported &&
                task->triggers[i].enabled)
                schedule_event_trigger(task, &task->triggers[i], now);
}

static void on_logon(PSID user, ULONGLONG now)
{
    struct task *task;
    unsigned int i;
    PSID sid, trigger_sid;

    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
    {
        /* a user task runs only at its own user's logon */
        sid = task_identity(task);
        if (sid && !IsWellKnownSid(sid, WinLocalSystemSid) && !EqualSid(sid, user))
        {
            LocalFree(sid);
            continue;
        }
        LocalFree(sid);

        for (i = 0; i < task->n_triggers; i++)
        {
            const struct trigger *trigger = &task->triggers[i];

            if (trigger->type != TASK_TRIGGER_LOGON || !trigger->supported || !trigger->enabled) continue;
            if (trigger->user_id && *trigger->user_id)
            {
                BOOL match = (trigger_sid = resolve_account(trigger->user_id)) && EqualSid(trigger_sid, user);
                LocalFree(trigger_sid);
                if (!match) continue;
            }
            TRACE("task %s: logon trigger\n", debugstr_w(task->path));
            schedule_event_trigger(task, trigger, now);
        }
    }
}

struct logon_scan
{
    ULONGLONG now;
    BOOL fire;
};

static BOOL logon_scan_callback(HANDLE token, const LUID *id, PSID user, void *context)
{
    struct logon_scan *scan = context;
    unsigned int i;

    for (i = 0; i < n_logons; i++)
    {
        if (logons[i].id.LowPart == id->LowPart && logons[i].id.HighPart == id->HighPart)
        {
            logons[i].alive = TRUE;
            return TRUE;
        }
    }
    if (n_logons < ARRAYSIZE(logons))
    {
        logons[n_logons].id = *id;
        logons[n_logons].alive = TRUE;
        n_logons++;
    }
    if (scan->fire) on_logon(user, scan->now);
    return TRUE;
}

/* new logon sessions with a shell fire the logon triggers */
static void scan_logons(ULONGLONG now, BOOL fire)
{
    struct logon_scan scan = { now, fire };
    unsigned int i, j;

    for (i = 0; i < n_logons; i++) logons[i].alive = FALSE;
    enum_shell_tokens(logon_scan_callback, &scan);
    for (i = j = 0; i < n_logons; i++)
        if (logons[i].alive) logons[j++] = logons[i];
    n_logons = j;
}

static BOOL have_logon_triggers(void)
{
    struct task *task;
    unsigned int i;

    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
        for (i = 0; i < task->n_triggers; i++)
            if (task->triggers[i].type == TASK_TRIGGER_LOGON && task->triggers[i].supported)
                return TRUE;
    return FALSE;
}

/* starts every task with a start in (last, now]; returns the next start after now */
static ULONGLONG run_due_tasks(ULONGLONG last, ULONGLONG now)
{
    ULONGLONG next = NO_TIME, t;
    struct task *task;
    unsigned int i;
    BOOL due;

    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
    {
        due = FALSE;
        if (task->pending <= now)
        {
            due = TRUE;
            task->pending = NO_TIME;
        }
        for (i = 0; i < task->n_triggers; i++)
            if (trigger_next_run(&task->triggers[i], last) <= now) due = TRUE;

        if (due && task->enabled)
        {
            TRACE("task %s is due\n", debugstr_w(task->path));
            start_task(task);
        }

        if (task->pending < next) next = task->pending;
        for (i = 0; i < task->n_triggers; i++)
            if ((t = trigger_next_run(&task->triggers[i], now)) < next) next = t;
    }
    return next;
}

static DWORD WINAPI scheduler_thread_proc(void *arg)
{
    ULONGLONG last, now, next;
    HANDLE events[2] = { stop_event, update_event };
    DWORD timeout, ret;
    BOOL boot = is_boot_window();

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    load_tasks();
    last = now = get_time();
    if (boot) on_boot(now);
    /* logons that predate the service count only at boot (autologon can win the race) */
    scan_logons(now, boot);
    next = run_due_tasks(last, now);

    for (;;)
    {
        timeout = MAX_WAIT_MS;
        now = get_time();
        if (next != NO_TIME)
            timeout = next <= now ? 0 : (DWORD)min((next - now + 9999) / 10000, MAX_WAIT_MS);
        if (have_logon_triggers() && timeout > LOGON_POLL_MS) timeout = LOGON_POLL_MS;

        ret = WaitForMultipleObjects(2, events, FALSE, timeout);
        if (ret == WAIT_OBJECT_0) break;

        now = get_time();
        if (now < last) last = now; /* the clock went back */
        if (ret == WAIT_OBJECT_0 + 1)
        {
            /* finish the old schedule up to now, then switch to the new one */
            run_due_tasks(last, now);
            last = now;
            load_tasks();
        }
        scan_logons(now, TRUE);
        next = run_due_tasks(last, now);
        last = now;
    }

    while (!list_empty(&tasks))
    {
        struct task *task = LIST_ENTRY(list_head(&tasks), struct task, entry);
        list_remove(&task->entry);
        free_task(task);
    }
    CoUninitialize();
    return 0;
}

/* who is calling the RPC server: their SID and whether they are LocalSystem or an administrator */
static void get_caller(WCHAR **sid_str, BOOL *privileged)
{
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)buffer;
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(admins);
    HANDLE token;
    BOOL member = FALSE;

    *sid_str = NULL;
    *privileged = FALSE;

    if (RpcImpersonateClient(NULL) != RPC_S_OK)
    {
        WARN("cannot impersonate the caller\n");
        return;
    }
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
    {
        if (GetTokenInformation(token, TokenUser, user, sizeof(buffer), &size))
        {
            ConvertSidToStringSidW(user->User.Sid, sid_str);
            if (IsWellKnownSid(user->User.Sid, WinLocalSystemSid))
                *privileged = TRUE;
        }
        size = sizeof(admins);
        if (!*privileged && CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admins, &size) &&
            CheckTokenMembership(token, admins, &member) && member)
            *privileged = TRUE;
        CloseHandle(token);
    }
    else
        WARN("cannot open the caller's token (%lu)\n", GetLastError());
    RpcRevertToSelf();
}

void schedsvc_task_registered(const WCHAR *path, const WCHAR *xml)
{
    WCHAR *name, *sid;
    BOOL privileged;
    DWORD value;
    HKEY cache, key;
    LONG ret;

    if (!(name = cache_name(path))) return;
    get_caller(&sid, &privileged);
    TRACE("%s registered by %s, privileged %d\n", debugstr_w(path), debugstr_w(sid), privileged);

    ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE, TASKCACHE_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &cache, NULL);
    if (!ret)
    {
        RegDeleteKeyW(cache, name);
        ret = RegCreateKeyExW(cache, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL);
        if (!ret)
        {
            value = privileged;
            RegSetValueExW(key, L"Xml", 0, REG_SZ, (const BYTE *)xml, (wcslen(xml) + 1) * sizeof(WCHAR));
            if (sid)
                RegSetValueExW(key, L"Registrant", 0, REG_SZ, (const BYTE *)sid, (wcslen(sid) + 1) * sizeof(WCHAR));
            RegSetValueExW(key, L"Privileged", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
            RegCloseKey(key);
        }
        RegCloseKey(cache);
    }
    if (ret) ERR("failed to record task %s (%ld)\n", debugstr_w(path), ret);

    LocalFree(sid);
    free(name);
    if (update_event) SetEvent(update_event);
}

void schedsvc_task_deleted(const WCHAR *path)
{
    WCHAR *name;
    HKEY cache;

    if (!(name = cache_name(path))) return;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, TASKCACHE_KEY, 0, KEY_ALL_ACCESS, &cache))
    {
        RegDeleteKeyW(cache, name);
        RegCloseKey(cache);
    }
    free(name);
    if (update_event) SetEvent(update_event);
}

/* IRegisteredTask::Run: only the registering user or an administrator may start a task */
HRESULT schedsvc_run_task(const WCHAR *path, GUID *guid)
{
    struct task *task = NULL;
    WCHAR *name, *sid_str;
    PSID caller = NULL;
    BOOL privileged;
    HANDLE process;
    HKEY cache;

    memset(guid, 0, sizeof(*guid));
    if (!(name = cache_name(path))) return E_INVALIDARG;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, TASKCACHE_KEY, 0, KEY_READ, &cache))
    {
        task = load_task(cache, name);
        RegCloseKey(cache);
    }
    free(name);
    if (!task) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    get_caller(&sid_str, &privileged);
    if (sid_str && !ConvertStringSidToSidW(sid_str, &caller)) caller = NULL;
    LocalFree(sid_str);

    if (!privileged && (!caller || !task->registrant || !EqualSid(caller, task->registrant)))
    {
        LocalFree(caller);
        free_task(task);
        return E_ACCESSDENIED;
    }
    LocalFree(caller);

    if (!task->enabled)
    {
        free_task(task);
        return SCHED_E_TASK_DISABLED;
    }

    process = run_task(task);
    free_task(task);
    if (!process) return E_FAIL;
    CloseHandle(process);
    UuidCreate(guid);
    return S_OK;
}

/* a task or folder path stays inside the task store: no "." or ".." components, no streams or devices */
BOOL schedsvc_valid_path(const WCHAR *path)
{
    const WCHAR *p, *start;

    if (!path) return TRUE;

    for (start = p = path; ; p++)
    {
        if (!*p || *p == '\\' || *p == '/')
        {
            if ((p - start == 1 && start[0] == '.') ||
                (p - start == 2 && start[0] == '.' && start[1] == '.'))
                return FALSE;
            if (!*p) return TRUE;
            start = p + 1;
        }
        else if (*p < 32 || wcschr(L":*?\"<>|", *p))
            return FALSE;
    }
}

void schedsvc_auto_start(void)
{
    /* the Schedule service is an auto-start service already */
}

DWORD TaskSchedulerStart(VOID)
{
    WCHAR path[MAX_PATH];

    /* the task store, %SystemRoot%\System32\Tasks */
    if (GetSystemDirectoryW(path, MAX_PATH - 6))
    {
        wcscat(path, L"\\Tasks");
        if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            ERR("cannot create %s (%lu)\n", debugstr_w(path), GetLastError());
    }

    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    update_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!stop_event || !update_event) return GetLastError();

    scheduler_thread = CreateThread(NULL, 0, scheduler_thread_proc, NULL, 0, NULL);
    if (!scheduler_thread) return GetLastError();
    return ERROR_SUCCESS;
}

VOID TaskSchedulerStop(VOID)
{
    if (scheduler_thread)
    {
        SetEvent(stop_event);
        WaitForSingleObject(scheduler_thread, 10000);
        CloseHandle(scheduler_thread);
        scheduler_thread = NULL;
    }
}
