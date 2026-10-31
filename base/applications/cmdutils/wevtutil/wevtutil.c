/*
 * PROJECT:     ReactOS Windows Events Command Line Utility
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     wevtutil: install and uninstall event manifests, enumerate logs and publishers,
 *              read the configuration of a log, query events
 * COPYRIGHT:   Copyright 2026 the reactos-nitro authors
 *
 * The syntax, the text of the output and the exit codes are those of wevtutil.exe of Windows 10 22H2,
 * as measured on that system. The commands that this tool does not have (sl, gp, epl, al, cl) check their
 * arguments and report that they are not supported.
 */

#define COBJMACROS

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winerror.h>
#include <winnls.h>
#include <wincon.h>
#include <shlwapi.h>
#include <sddl.h>
#include <winevt.h>
#include <xmllite.h>

#include "help.h"

#undef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The limits of what is read from a manifest or an argument */
#define MAX_TEXT        4096        /* any string of a manifest */
#define MAX_PROVIDERS   256
#define MAX_CHANNELS    1024
#define MAX_DEPTH       32
#define MAX_NAMES       200000      /* the logs or publishers that are listed */
#define MAX_QUERY_FILE  (1024 * 1024)
#define MAX_VALUES      256         /* the data values of an event that a message shows */

#define EVENTS_NS       L"http://schemas.microsoft.com/win/2004/08/events"
#define WINEVT_KEY      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WINEVT"

/* Output: the text goes to the console, or to a file in the OEM code page, or as UTF-16 with /uni:true */

static HANDLE std_handle[3];
static BOOL std_console[3];
static BOOL g_unicode;
static BOOL bom_written;

static void put_text(int fd, const WCHAR *s, size_t len)
{
    WCHAR *buf;
    size_t i, n = 0;
    DWORD written;

    if (!len) return;
    if (!std_handle[fd])
    {
        std_handle[fd] = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        std_console[fd] = std_handle[fd] && std_handle[fd] != INVALID_HANDLE_VALUE && GetConsoleMode(std_handle[fd], &written);
    }
    if (!std_handle[fd] || std_handle[fd] == INVALID_HANDLE_VALUE) return;

    /* each line feed becomes a carriage return and a line feed, and a pair is left alone */
    if (!(buf = malloc((len * 2 + 1) * sizeof(WCHAR)))) return;
    for (i = 0; i < len; i++)
    {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) buf[n++] = '\r';
        buf[n++] = s[i];
    }

    if (std_console[fd])
    {
        for (i = 0; i < n; i += 8192)
            WriteConsoleW(std_handle[fd], buf + i, (DWORD)min(n - i, 8192), &written, NULL);
    }
    else if (g_unicode)
    {
        if (fd == 1 && !bom_written)
        {
            static const WCHAR bom = 0xfeff;

            WriteFile(std_handle[fd], &bom, sizeof(bom), &written, NULL);
            bom_written = TRUE;
        }
        WriteFile(std_handle[fd], buf, (DWORD)(n * sizeof(WCHAR)), &written, NULL);
    }
    else
    {
        int size = WideCharToMultiByte(CP_OEMCP, 0, buf, (int)n, NULL, 0, NULL, NULL);
        char *mb = size > 0 ? malloc(size) : NULL;

        if (mb && WideCharToMultiByte(CP_OEMCP, 0, buf, (int)n, mb, size, NULL, NULL))
            WriteFile(std_handle[fd], mb, size, &written, NULL);
        free(mb);
    }
    free(buf);
}

static void out_s(int fd, const WCHAR *s)
{
    put_text(fd, s, wcslen(s));
}

static void outf(int fd, const WCHAR *format, ...)
{
    va_list args;
    size_t cap = 512;
    WCHAR *buf = NULL;
    int n;

    while (cap <= 1024 * 1024)
    {
        WCHAR *p = realloc(buf, cap * sizeof(WCHAR));

        if (!p) break;
        buf = p;
        va_start(args, format);
        n = _vsnwprintf(buf, cap, format, args);
        va_end(args);
        if (n >= 0 && (size_t)n < cap)
        {
            put_text(fd, buf, n);
            free(buf);
            return;
        }
        cap *= 2;
    }
    free(buf);
}

/* The text of an error, as the system or wevtapi has it */

static const struct { DWORD code; const WCHAR *text; } evt_errors[] =
{
    { 15000, L"The specified channel path is invalid." },
    { 15001, L"The specified query is invalid." },
    { 15002, L"The publisher metadata cannot be found in the resource." },
    { 15004, L"The specified publisher name is invalid." },
    { 15007, L"The specified channel could not be found." },
    { 15027, L"The message resource is present but the message was not found in the message table." },
    { 15028, L"The message ID for the desired message could not be found." },
};

static void print_error(DWORD error)
{
    WCHAR text[512];
    DWORD i, len;

    for (i = 0; i < ARRAYSIZE(evt_errors); i++)
    {
        if (evt_errors[i].code == error)
        {
            outf(2, L"%ls\n", evt_errors[i].text);
            return;
        }
    }
    len = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, text, ARRAYSIZE(text), NULL);
    while (len && (text[len - 1] == '\r' || text[len - 1] == '\n' || text[len - 1] == ' ')) len--;
    if (len)
    {
        text[len] = 0;
        outf(2, L"%ls\n", text);
    }
    else
        outf(2, L"Error 0x%08lx.\n", error);
}

/* A failure of a command: the message of the command, then the text of the error. Returns the exit code. */
static DWORD fail_with(DWORD error, const WCHAR *format, ...)
{
    va_list args;
    WCHAR text[1024];

    if (format)
    {
        va_start(args, format);
        _vsnwprintf(text, ARRAYSIZE(text) - 1, format, args);
        va_end(args);
        text[ARRAYSIZE(text) - 1] = 0;
        out_s(2, text);
    }
    print_error(error);
    return error;
}

static DWORD bad_parameter(const WCHAR *format, ...)
{
    va_list args;
    WCHAR text[1024];

    va_start(args, format);
    _vsnwprintf(text, ARRAYSIZE(text) - 1, format, args);
    va_end(args);
    text[ARRAYSIZE(text) - 1] = 0;
    out_s(2, text);
    print_error(ERROR_INVALID_PARAMETER);
    return ERROR_INVALID_PARAMETER;
}

static WCHAR *dup_n(const WCHAR *s, size_t len)
{
    WCHAR *p = malloc((len + 1) * sizeof(WCHAR));

    if (!p) return NULL;
    memcpy(p, s, len * sizeof(WCHAR));
    p[len] = 0;
    return p;
}

static WCHAR *dup_s(const WCHAR *s)
{
    return dup_n(s, wcslen(s));
}

/* Arguments and options */

enum { OPT_VALUE, OPT_BOOL };

struct optdef
{
    const WCHAR *name;
    const WCHAR *long_name;
    int kind;
};

#define MAX_OPTS 16

struct options
{
    const struct optdef *defs;
    DWORD count;
    WCHAR *value[MAX_OPTS];
    BOOL set[MAX_OPTS];
    const WCHAR *args[4];
    DWORD arg_count;
};

static const struct optdef common_opts[] =
{
    { L"r", L"remote", OPT_VALUE }, { L"u", L"username", OPT_VALUE }, { L"p", L"password", OPT_VALUE },
    { L"a", L"authentication", OPT_VALUE }, { L"uni", L"unicode", OPT_BOOL },
};
#define NUM_COMMON ARRAYSIZE(common_opts)

static const struct optdef gl_opts[] = { { L"f", L"format", OPT_VALUE } };
static const struct optdef gli_opts[] = { { L"lf", L"logfile", OPT_BOOL } };
static const struct optdef gp_opts[] = { { L"ge", L"getevents", OPT_BOOL }, { L"gm", L"getmessage", OPT_BOOL }, { L"f", L"format", OPT_VALUE } };
static const struct optdef im_opts[] =
{
    { L"rf", L"resourceFilePath", OPT_VALUE }, { L"mf", L"messageFilePath", OPT_VALUE }, { L"pf", L"parameterFilePath", OPT_VALUE },
};
static const struct optdef qe_opts[] =
{
    { L"lf", L"logfile", OPT_BOOL }, { L"sq", L"structuredquery", OPT_BOOL }, { L"q", L"query", OPT_VALUE },
    { L"bm", L"bookmark", OPT_VALUE }, { L"sbm", L"savebookmark", OPT_VALUE }, { L"rd", L"reversedirection", OPT_BOOL },
    { L"f", L"format", OPT_VALUE }, { L"l", L"locale", OPT_VALUE }, { L"c", L"count", OPT_VALUE }, { L"e", L"element", OPT_VALUE },
};
/* the commands that are not here accept what Windows accepts for them, to report that they are not supported */
static const struct optdef sl_opts[] =
{
    { L"e", L"enabled", OPT_VALUE }, { L"i", L"isolation", OPT_VALUE }, { L"lfn", L"logfilename", OPT_VALUE },
    { L"rt", L"retention", OPT_VALUE }, { L"ab", L"autobackup", OPT_VALUE }, { L"ms", L"maxsize", OPT_VALUE },
    { L"l", L"level", OPT_VALUE }, { L"k", L"keywords", OPT_VALUE }, { L"ca", L"channelaccess", OPT_VALUE },
    { L"ova", L"operationalaccess", OPT_VALUE }, { L"c", L"config", OPT_VALUE },
};
static const struct optdef epl_opts[] =
{
    { L"lf", L"logfile", OPT_BOOL }, { L"sq", L"structuredquery", OPT_BOOL }, { L"q", L"query", OPT_VALUE }, { L"ow", L"overwrite", OPT_BOOL },
};
static const struct optdef al_opts[] = { { L"l", L"locale", OPT_VALUE } };
static const struct optdef cl_opts[] = { { L"bu", L"backup", OPT_VALUE } };

enum { CMD_EL, CMD_GL, CMD_SL, CMD_EP, CMD_GP, CMD_IM, CMD_UM, CMD_QE, CMD_GLI, CMD_EPL, CMD_AL, CMD_CL };

static const struct command
{
    const WCHAR *name;
    const WCHAR *long_name;
    int id;
    DWORD min_args, max_args;
    const struct optdef *opts;
    DWORD opt_count;
    const WCHAR *help;
} commands[] =
{
    { L"el", L"enum-logs", CMD_EL, 0, 0, NULL, 0, help_el },
    { L"gl", L"get-log", CMD_GL, 1, 1, gl_opts, ARRAYSIZE(gl_opts), help_gl },
    { L"sl", L"set-log", CMD_SL, 1, 1, sl_opts, ARRAYSIZE(sl_opts), NULL },
    { L"ep", L"enum-publishers", CMD_EP, 0, 0, NULL, 0, help_ep },
    { L"gp", L"get-publisher", CMD_GP, 1, 1, gp_opts, ARRAYSIZE(gp_opts), help_gp },
    { L"im", L"install-manifest", CMD_IM, 1, 1, im_opts, ARRAYSIZE(im_opts), help_im },
    { L"um", L"uninstall-manifest", CMD_UM, 1, 1, NULL, 0, help_um },
    { L"qe", L"query-events", CMD_QE, 1, 1, qe_opts, ARRAYSIZE(qe_opts), help_qe },
    { L"gli", L"get-log-info", CMD_GLI, 1, 1, gli_opts, ARRAYSIZE(gli_opts), help_gli },
    { L"gli", L"get-loginfo", CMD_GLI, 1, 1, gli_opts, ARRAYSIZE(gli_opts), help_gli },
    { L"epl", L"export-log", CMD_EPL, 2, 2, epl_opts, ARRAYSIZE(epl_opts), NULL },
    { L"al", L"archive-log", CMD_AL, 1, 1, al_opts, ARRAYSIZE(al_opts), NULL },
    { L"cl", L"clear-log", CMD_CL, 1, 1, cl_opts, ARRAYSIZE(cl_opts), NULL },
};

static const struct command *find_command(const WCHAR *name)
{
    DWORD i;

    for (i = 0; i < ARRAYSIZE(commands); i++)
        if (!_wcsicmp(name, commands[i].name) || !_wcsicmp(name, commands[i].long_name)) return &commands[i];
    return NULL;
}

/* The option that the name stands for: the common ones come after those of the command */
static int find_option(const struct options *o, const WCHAR *name, const struct optdef **def)
{
    DWORD i;

    for (i = 0; i < o->count; i++)
    {
        const struct optdef *d = i < o->count - NUM_COMMON ? &o->defs[i] : &common_opts[i - (o->count - NUM_COMMON)];

        if (!_wcsicmp(name, d->name) || !_wcsicmp(name, d->long_name))
        {
            *def = d;
            return i;
        }
    }
    return -1;
}

static BOOL parse_bool(const WCHAR *value, BOOL *result)
{
    if (!_wcsicmp(value, L"true")) *result = TRUE;
    else if (!_wcsicmp(value, L"false")) *result = FALSE;
    else return FALSE;
    return TRUE;
}

static void free_options(struct options *o)
{
    DWORD i;

    for (i = 0; i < MAX_OPTS; i++) free(o->value[i]);
}

/* Reads the arguments after the command. Returns ERROR_SUCCESS, or the exit code after the message was written. */
static DWORD parse_options(const struct command *cmd, int argc, WCHAR **argv, struct options *o)
{
    DWORD error = ERROR_SUCCESS;
    int i;

    memset(o, 0, sizeof(*o));
    o->defs = cmd->opts;
    o->count = cmd->opt_count + NUM_COMMON;
    if (o->count > MAX_OPTS) return ERROR_INVALID_PARAMETER;

    for (i = 0; i < argc; i++)
    {
        const WCHAR *a = argv[i];

        if (a[0] == '/' || (a[0] == '-' && a[1]))
        {
            const struct optdef *def;
            WCHAR *name, *colon;
            const WCHAR *value = NULL;
            int index;

            name = dup_s(a + 1);
            if (!name) return ERROR_OUTOFMEMORY;
            if ((colon = wcschr(name, ':')))
            {
                *colon = 0;
                value = argv[i] + 1 + (colon - name) + 1;
            }
            if ((index = find_option(o, name, &def)) < 0)
            {
                error = bad_parameter(L"Invalid option %ls. Option is not supported.\n", name);
                free(name);
                return error;
            }
            if (def->kind == OPT_VALUE && (!value || !value[0]))
            {
                error = bad_parameter(L"Invalid option %ls. Option value is not specified.\n", name);
                free(name);
                return error;
            }
            if (def->kind == OPT_BOOL)
            {
                BOOL b;

                if (value && !parse_bool(value, &b))
                {
                    error = bad_parameter(L"Invalid option %ls. Option is not Boolean.\n", name);
                    free(name);
                    return error;
                }
                value = value ? value : L"true";
            }
            free(name);
            free(o->value[index]);
            if (!(o->value[index] = dup_s(value))) return ERROR_OUTOFMEMORY;
            o->set[index] = TRUE;
        }
        else
        {
            if (o->arg_count >= ARRAYSIZE(o->args)) return bad_parameter(L"Too many arguments are specified.\n");
            o->args[o->arg_count++] = a;
        }
    }
    if (o->arg_count < cmd->min_args) return bad_parameter(L"Required argument(s) is/are not specified.\n");
    if (o->arg_count > cmd->max_args) return bad_parameter(L"Too many arguments are specified.\n");
    return ERROR_SUCCESS;
}

/* The value of an option of the command, by its short name, or NULL when it was not given */
static const WCHAR *option(const struct options *o, const WCHAR *name)
{
    DWORD i;

    for (i = 0; i < o->count; i++)
    {
        const struct optdef *d = i < o->count - NUM_COMMON ? &o->defs[i] : &common_opts[i - (o->count - NUM_COMMON)];

        if (!wcscmp(d->name, name)) return o->set[i] ? o->value[i] : NULL;
    }
    return NULL;
}

static BOOL option_true(const struct options *o, const WCHAR *name)
{
    const WCHAR *v = option(o, name);

    return v && !_wcsicmp(v, L"true");
}

/* Lists of names */

static int compare_names(const void *a, const void *b)
{
    return _wcsicmp(*(WCHAR * const *)a, *(WCHAR * const *)b);
}

static void free_names(WCHAR **names, DWORD count)
{
    DWORD i;

    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}

static DWORD collect_names(BOOL channels, WCHAR ***names, DWORD *count)
{
    EVT_HANDLE handle;
    WCHAR *buf = NULL;
    DWORD cap = 0, used, error = ERROR_SUCCESS;
    WCHAR **list = NULL;
    DWORD n = 0;

    *names = NULL;
    *count = 0;
    handle = channels ? EvtOpenChannelEnum(NULL, 0) : EvtOpenPublisherEnum(NULL, 0);
    if (!handle) return GetLastError();

    for (;;)
    {
        BOOL ok = channels ? EvtNextChannelPath(handle, cap, buf, &used) : EvtNextPublisherId(handle, cap, buf, &used);

        if (!ok)
        {
            error = GetLastError();
            if (error == ERROR_INSUFFICIENT_BUFFER && used > cap && used <= 65536)
            {
                WCHAR *p = realloc(buf, used * sizeof(WCHAR));

                if (!p) { error = ERROR_OUTOFMEMORY; break; }
                buf = p;
                cap = used;
                continue;
            }
            if (error == ERROR_NO_MORE_ITEMS) error = ERROR_SUCCESS;
            break;
        }
        if (n >= MAX_NAMES) break;
        {
            WCHAR **p = realloc(list, (n + 1) * sizeof(*p));

            if (!p) { error = ERROR_OUTOFMEMORY; break; }
            list = p;
            if (!(list[n] = dup_s(buf))) { error = ERROR_OUTOFMEMORY; break; }
            n++;
        }
    }
    free(buf);
    EvtClose(handle);
    if (error)
    {
        free_names(list, n);
        return error;
    }
    if (n) qsort(list, n, sizeof(*list), compare_names);
    *names = list;
    *count = n;
    return ERROR_SUCCESS;
}

static DWORD cmd_enum(BOOL channels)
{
    WCHAR **names;
    DWORD i, count, error;

    if ((error = collect_names(channels, &names, &count)))
        return fail_with(error, channels ? L"Failed to enumerate logs.\n" : L"Failed to enumerate publishers.\n");
    for (i = 0; i < count; i++) outf(1, L"%ls\n", names[i]);
    free_names(names, count);
    return ERROR_SUCCESS;
}

/* gl and gli */

static void *get_config(EVT_HANDLE config, EVT_CHANNEL_CONFIG_PROPERTY_ID id, EVT_VARIANT **v)
{
    DWORD used = 0;
    EVT_VARIANT *buf;

    *v = NULL;
    if (EvtGetChannelConfigProperty(config, id, 0, 0, NULL, &used) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !used || used > 1024 * 1024)
        return NULL;
    if (!(buf = malloc(used))) return NULL;
    if (!EvtGetChannelConfigProperty(config, id, 0, used, buf, &used))
    {
        free(buf);
        return NULL;
    }
    *v = buf;
    return buf;
}

static const WCHAR *config_string(EVT_HANDLE config, EVT_CHANNEL_CONFIG_PROPERTY_ID id, EVT_VARIANT **v)
{
    if (!get_config(config, id, v)) return L"";
    if (((*v)->Type & EVT_VARIANT_TYPE_MASK) != EvtVarTypeString || !(*v)->StringVal) return L"";
    return (*v)->StringVal;
}

static UINT64 config_number(EVT_HANDLE config, EVT_CHANNEL_CONFIG_PROPERTY_ID id, UINT64 def)
{
    EVT_VARIANT *v;
    UINT64 result = def;

    if (get_config(config, id, &v))
    {
        switch (v->Type & EVT_VARIANT_TYPE_MASK)
        {
        case EvtVarTypeBoolean: result = v->BooleanVal != 0; break;
        case EvtVarTypeUInt32: result = v->UInt32Val; break;
        case EvtVarTypeUInt64: result = v->UInt64Val; break;
        default: break;
        }
        free(v);
    }
    return result;
}

static void out_xml_escaped(const WCHAR *s)
{
    for (; *s; s++)
    {
        switch (*s)
        {
        case '&': out_s(1, L"&amp;"); break;
        case '<': out_s(1, L"&lt;"); break;
        case '>': out_s(1, L"&gt;"); break;
        case '"': out_s(1, L"&quot;"); break;
        case '\'': out_s(1, L"&apos;"); break;
        default: put_text(1, s, 1); break;
        }
    }
}

static BOOL get_format(const struct options *o, BOOL *xml)
{
    const WCHAR *f = option(o, L"f");

    *xml = FALSE;
    if (!f) return TRUE;
    if (!_wcsicmp(f, L"xml")) *xml = TRUE;
    else if (_wcsicmp(f, L"text")) return FALSE;
    return TRUE;
}

static DWORD cmd_gl(const struct options *o)
{
    static const WCHAR *const types[] = { L"Admin", L"Operational", L"Analytic", L"Debug" };
    static const WCHAR *const isolations[] = { L"Application", L"System", L"Custom" };
    const WCHAR *name = o->args[0];
    EVT_VARIANT *v1 = NULL, *v2 = NULL, *v3 = NULL;
    const WCHAR *publisher, *access, *file;
    UINT64 type, isolation;
    BOOL xml, enabled, retention, backup;
    EVT_HANDLE config;

    if (!get_format(o, &xml)) return bad_parameter(L"Invalid value for option f.\n");
    if (!(config = EvtOpenChannelConfig(NULL, name, 0)))
        return fail_with(GetLastError(), L"Failed to read configuration for log %ls.\n", name);

    enabled = config_number(config, EvtChannelConfigEnabled, 0) != 0;
    type = config_number(config, EvtChannelConfigType, 0);
    isolation = config_number(config, EvtChannelConfigIsolation, 0);
    publisher = config_string(config, EvtChannelConfigOwningPublisher, &v1);
    access = config_string(config, EvtChannelConfigAccess, &v2);
    file = config_string(config, EvtChannelLoggingConfigLogFilePath, &v3);
    retention = config_number(config, EvtChannelLoggingConfigRetention, 0) != 0;
    backup = config_number(config, EvtChannelLoggingConfigAutoBackup, 0) != 0;

    if (xml)
    {
        out_s(1, L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<channel name=\"");
        out_xml_escaped(name);
        outf(1, L"\" enabled=\"%ls\" type=\"%ls\" owningPublisher=\"", enabled ? L"true" : L"false", types[type < 4 ? type : 0]);
        out_xml_escaped(publisher);
        outf(1, L"\" isolation=\"%ls\" channelAccess=\"", isolations[isolation < 3 ? isolation : 0]);
        out_xml_escaped(access);
        out_s(1, L"\" xmlns=\"" EVENTS_NS L"\">\n  <logging>\n    <logFileName>");
        out_xml_escaped(file);
        outf(1, L"</logFileName>\n    <retention>%ls</retention>\n    <autoBackup>%ls</autoBackup>\n    <maxSize>%I64u</maxSize>\n"
             L"  </logging>\n  <publishing>\n    <fileMax>%I64u</fileMax>\n  </publishing>\n</channel>\n",
             retention ? L"true" : L"false", backup ? L"true" : L"false", config_number(config, EvtChannelLoggingConfigMaxSize, 0),
             config_number(config, EvtChannelPublishingConfigFileMax, 1));
    }
    else
    {
        outf(1, L"name: %ls\nenabled: %ls\ntype: %ls\nowningPublisher: %ls\nisolation: %ls\nchannelAccess: %ls\nlogging:\n"
             L"  logFileName: %ls\n  retention: %ls\n  autoBackup: %ls\n  maxSize: %I64u\npublishing:\n  fileMax: %I64u\n",
             name, enabled ? L"true" : L"false", types[type < 4 ? type : 0], publisher, isolations[isolation < 3 ? isolation : 0], access,
             file, retention ? L"true" : L"false", backup ? L"true" : L"false", config_number(config, EvtChannelLoggingConfigMaxSize, 0),
             config_number(config, EvtChannelPublishingConfigFileMax, 1));
    }
    free(v1);
    free(v2);
    free(v3);
    EvtClose(config);
    return ERROR_SUCCESS;
}

static void format_time(UINT64 ft, int digits)
{
    SYSTEMTIME st;
    FILETIME f;
    UINT64 frac = ft % 10000000;

    f.dwLowDateTime = (DWORD)ft;
    f.dwHighDateTime = (DWORD)(ft >> 32);
    if (!FileTimeToSystemTime(&f, &st))
    {
        out_s(1, L"N/A");
        return;
    }
    if (digits == 7)
        outf(1, L"%04u-%02u-%02uT%02u:%02u:%02u.%07I64uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, frac);
    else
        outf(1, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static BOOL get_log_info(EVT_HANDLE log, EVT_LOG_PROPERTY_ID id, EVT_VARIANT *v)
{
    DWORD used;

    return EvtGetLogInfo(log, id, sizeof(*v), v, &used) && v->Type != EvtVarTypeNull;
}

static DWORD cmd_gli(const struct options *o)
{
    const WCHAR *name = o->args[0];
    BOOL file = option_true(o, L"lf");
    EVT_HANDLE log;
    EVT_VARIANT v;

    if (!(log = EvtOpenLog(NULL, name, file ? EvtOpenFilePath : EvtOpenChannelPath)))
        return fail_with(GetLastError(), L"Failed to read log status information for log %ls.\n", name);

    out_s(1, L"creationTime: ");
    if (get_log_info(log, EvtLogCreationTime, &v)) format_time(v.FileTimeVal, 3);
    out_s(1, L"\nlastAccessTime: ");
    if (get_log_info(log, EvtLogLastAccessTime, &v)) format_time(v.FileTimeVal, 3);
    out_s(1, L"\nlastWriteTime: ");
    if (get_log_info(log, EvtLogLastWriteTime, &v)) format_time(v.FileTimeVal, 3);
    out_s(1, L"\n");
    if (get_log_info(log, EvtLogFileSize, &v)) outf(1, L"fileSize: %I64u\n", v.UInt64Val);
    if (get_log_info(log, EvtLogAttributes, &v)) outf(1, L"attributes: %lu\n", v.UInt32Val);
    if (get_log_info(log, EvtLogNumberOfLogRecords, &v)) outf(1, L"numberOfLogRecords: %I64u\n", v.UInt64Val);
    if (get_log_info(log, EvtLogOldestRecordNumber, &v)) outf(1, L"oldestRecordNumber: %I64u\n", v.UInt64Val);
    EvtClose(log);
    return ERROR_SUCCESS;
}

/* qe */

/* The system values of an event, as an array of EVT_VARIANT that the caller frees */
static EVT_VARIANT *render_system(EVT_HANDLE context, EVT_HANDLE event, DWORD flags, DWORD *count)
{
    DWORD used = 0, props = 0;
    EVT_VARIANT *buf;

    *count = 0;
    if (EvtRender(context, event, flags, 0, NULL, &used, &props) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !used || used > 16 * 1024 * 1024)
        return NULL;
    if (!(buf = malloc(used))) return NULL;
    if (!EvtRender(context, event, flags, used, buf, &used, &props))
    {
        free(buf);
        return NULL;
    }
    *count = props;
    return buf;
}

static WCHAR *render_text(EVT_HANDLE event, DWORD flags)
{
    DWORD used = 0, props = 0;
    WCHAR *buf;

    if (EvtRender(NULL, event, flags, 0, NULL, &used, &props) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !used || used > 64 * 1024 * 1024)
        return NULL;
    if (!(buf = malloc(used + sizeof(WCHAR)))) return NULL;
    if (!EvtRender(NULL, event, flags, used, buf, &used, &props))
    {
        free(buf);
        return NULL;
    }
    buf[used / sizeof(WCHAR)] = 0;
    return buf;
}

static WCHAR *format_event(EVT_HANDLE meta, EVT_HANDLE event, DWORD flags)
{
    DWORD used = 0;
    WCHAR *buf;

    if (EvtFormatMessage(meta, event, 0, 0, NULL, flags, 0, NULL, &used) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !used || used > 16 * 1024 * 1024)
        return NULL;
    if (!(buf = malloc((used + 1) * sizeof(WCHAR)))) return NULL;
    if (!EvtFormatMessage(meta, event, 0, 0, NULL, flags, used, buf, &used))
    {
        free(buf);
        return NULL;
    }
    buf[used] = 0;
    return buf;
}

/* The text without the white space at its end */
static void trim_end(WCHAR *s)
{
    size_t len = wcslen(s);

    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = 0;
}

static const WCHAR *level_name(UINT64 level)
{
    switch (level)
    {
    case 1: return L"Critical";
    case 2: return L"Error";
    case 3: return L"Warning";
    case 4: return L"Information";
    case 5: return L"Verbose";
    default: return L"N/A";
    }
}

/* The name of a publisher's metadata is opened again only when the publisher changes */
struct meta_cache
{
    WCHAR name[256];
    EVT_HANDLE handle;
    BOOL valid;
};

static EVT_HANDLE get_meta(struct meta_cache *cache, const WCHAR *provider)
{
    if (cache->valid && !wcscmp(cache->name, provider)) return cache->handle;
    if (cache->handle) EvtClose(cache->handle);
    cache->handle = NULL;
    cache->valid = FALSE;
    if (!provider || !provider[0] || wcslen(provider) >= ARRAYSIZE(cache->name)) return NULL;
    cache->handle = EvtOpenPublisherMetadata(NULL, provider, NULL, 0, 0);
    wcscpy(cache->name, provider);
    cache->valid = TRUE;
    return cache->handle;
}

/* The description of an event whose publisher has no message for it, as the classic logs show it */
static void out_missing_description(UINT64 id, const WCHAR *provider, EVT_HANDLE event)
{
    EVT_HANDLE context = EvtCreateRenderContext(0, NULL, EvtRenderContextUser);
    EVT_VARIANT *values = NULL;
    DWORD i, count = 0;

    outf(1, L"The description for Event ID %I64u from source %ls cannot be found. Either the component that raises this event is not installed on "
         L"your local computer or the installation is corrupted. You can install or repair the component on the local computer.\n\n"
         L"If the event originated on another computer, the display information had to be saved with the event.\n\n"
         L"The following information was included with the event: \n\n", id, provider);
    if (context)
    {
        values = render_system(context, event, EvtRenderEventValues, &count);
        for (i = 0; values && i < count && i < MAX_VALUES; i++)
            if ((values[i].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && values[i].StringVal)
                outf(1, L"%ls\n", values[i].StringVal);
        free(values);
        EvtClose(context);
    }
}

static void out_event_text(EVT_HANDLE context, EVT_HANDLE event, DWORD index, struct meta_cache *cache)
{
    EVT_VARIANT *v;
    DWORD count;
    const WCHAR *channel = L"", *provider = L"", *computer = L"";
    UINT64 id = 0, level = 0, ft = 0;
    PSID sid = NULL;
    EVT_HANDLE meta;
    WCHAR *text;

    if (!(v = render_system(context, event, EvtRenderEventValues, &count)) || count <= EvtSystemUserID)
    {
        free(v);
        outf(1, L"Event[%lu]:\n\n", index);
        return;
    }
    if ((v[EvtSystemProviderName].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && v[EvtSystemProviderName].StringVal)
        provider = v[EvtSystemProviderName].StringVal;
    if ((v[EvtSystemChannel].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && v[EvtSystemChannel].StringVal)
        channel = v[EvtSystemChannel].StringVal;
    if ((v[EvtSystemComputer].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && v[EvtSystemComputer].StringVal)
        computer = v[EvtSystemComputer].StringVal;
    if (v[EvtSystemEventID].Type != EvtVarTypeNull) id = v[EvtSystemEventID].UInt16Val;
    if (v[EvtSystemLevel].Type != EvtVarTypeNull) level = v[EvtSystemLevel].ByteVal;
    if (v[EvtSystemTimeCreated].Type != EvtVarTypeNull) ft = v[EvtSystemTimeCreated].FileTimeVal;
    if (v[EvtSystemUserID].Type == EvtVarTypeSid) sid = v[EvtSystemUserID].SidVal;

    meta = get_meta(cache, provider);
    outf(1, L"Event[%lu]:\n  Log Name: %ls\n  Source: %ls\n  Date: ", index, channel, provider);
    format_time(ft, 7);
    outf(1, L"\n  Event ID: %I64u\n", id);

    out_s(1, L"  Task: ");
    if ((text = format_event(meta, event, EvtFormatMessageTask)) && (trim_end(text), text[0])) out_s(1, text);
    else out_s(1, L"N/A");
    free(text);

    out_s(1, L"\n  Level: ");
    if ((text = format_event(meta, event, EvtFormatMessageLevel)) && (trim_end(text), text[0])) out_s(1, text);
    else out_s(1, level_name(level));
    free(text);

    out_s(1, L"\n  Opcode: ");
    if ((text = format_event(meta, event, EvtFormatMessageOpcode)) && (trim_end(text), text[0])) out_s(1, text);
    else out_s(1, L"N/A");
    free(text);

    out_s(1, L"\n  Keyword: ");
    if ((text = format_event(meta, event, EvtFormatMessageKeyword)) && (trim_end(text), text[0])) out_s(1, text);
    else out_s(1, L"N/A");
    free(text);

    out_s(1, L"\n  User: ");
    if (sid && IsValidSid(sid))
    {
        WCHAR *string = NULL, name[256], domain[256];
        DWORD name_len = ARRAYSIZE(name), domain_len = ARRAYSIZE(domain);
        SID_NAME_USE use;

        if (ConvertSidToStringSidW(sid, &string))
        {
            out_s(1, string);
            LocalFree(string);
        }
        else out_s(1, L"N/A");
        out_s(1, L"\n  User Name: ");
        if (LookupAccountSidW(NULL, sid, name, &name_len, domain, &domain_len, &use))
        {
            if (domain[0]) outf(1, L"%ls\\%ls", domain, name);
            else out_s(1, name);
        }
        else out_s(1, L"N/A");
    }
    else out_s(1, L"N/A\n  User Name: N/A");
    outf(1, L"\n  Computer: %ls\n  Description: \n", computer);

    if ((text = format_event(meta, event, EvtFormatMessageEvent)) && (trim_end(text), text[0])) out_s(1, text);
    else out_missing_description(id, provider, event);
    free(text);
    out_s(1, L"\n\n");
    free(v);
}

static void out_event_rendered_xml(EVT_HANDLE context, EVT_HANDLE event, struct meta_cache *cache)
{
    EVT_VARIANT *v;
    DWORD count;
    const WCHAR *provider = L"";
    WCHAR *text;

    if ((v = render_system(context, event, EvtRenderEventValues, &count)) && count > EvtSystemProviderName &&
        (v[EvtSystemProviderName].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && v[EvtSystemProviderName].StringVal)
        provider = v[EvtSystemProviderName].StringVal;
    if (!(text = format_event(get_meta(cache, provider), event, EvtFormatMessageXml))) text = render_text(event, EvtRenderEventXml);
    if (text) outf(1, L"%ls\n", text);
    free(text);
    free(v);
}

/* The content of a file as text: UTF-16 with a mark, or UTF-8 */
static WCHAR *read_text_file(const WCHAR *path, DWORD *error)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    BYTE *data;
    DWORD read;
    WCHAR *text = NULL;

    if (file == INVALID_HANDLE_VALUE) { *error = GetLastError(); return NULL; }
    if (!GetFileSizeEx(file, &size) || size.QuadPart > MAX_QUERY_FILE) { *error = size.QuadPart > MAX_QUERY_FILE ? ERROR_FILE_TOO_LARGE : GetLastError(); CloseHandle(file); return NULL; }
    if (!(data = malloc((size_t)size.QuadPart + 2)))
    {
        *error = ERROR_OUTOFMEMORY;
        CloseHandle(file);
        return NULL;
    }
    if (!ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL)) { *error = GetLastError(); free(data); CloseHandle(file); return NULL; }
    CloseHandle(file);
    data[read] = data[read + 1] = 0;
    if (read >= 2 && data[0] == 0xff && data[1] == 0xfe) text = dup_s((WCHAR *)(data + 2));
    else
    {
        BYTE *src = data;
        int n;

        if (read >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) { src += 3; read -= 3; }
        n = MultiByteToWideChar(CP_UTF8, 0, (char *)src, read, NULL, 0);
        if ((text = malloc((n + 1) * sizeof(WCHAR))))
        {
            n = MultiByteToWideChar(CP_UTF8, 0, (char *)src, read, text, n);
            text[n] = 0;
        }
    }
    free(data);
    *error = text ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
    return text;
}

static DWORD cmd_qe(const struct options *o)
{
    const WCHAR *path = o->args[0], *query = option(o, L"q"), *f = option(o, L"f"), *c = option(o, L"c"), *element = option(o, L"e");
    BOOL logfile = option_true(o, L"lf"), structured = option_true(o, L"sq"), reverse = option_true(o, L"rd");
    enum { FORMAT_XML, FORMAT_TEXT, FORMAT_RENDERED } format = FORMAT_XML;
    DWORD limit = 0, error, flags, extended = 0;
    WCHAR *file_query = NULL;
    EVT_HANDLE result, context = NULL;
    struct meta_cache cache = { { 0 }, NULL, FALSE };
    DWORD index = 0;

    if (f)
    {
        if (!_wcsicmp(f, L"text")) format = FORMAT_TEXT;
        else if (!_wcsicmp(f, L"renderedxml")) format = FORMAT_RENDERED;
        else if (_wcsicmp(f, L"xml")) return bad_parameter(L"Invalid value for option f.\n");
    }
    if (c)
    {
        WCHAR *end;
        unsigned long n = wcstoul(c, &end, 10);

        if (!c[0] || *end || n == 0 || n > 0x7fffffff || c[0] == '-' || c[0] == '+') return bad_parameter(L"Invalid value for property count.\n");
        limit = n;
    }
    if (option(o, L"bm") || option(o, L"sbm")) return fail_with(ERROR_NOT_SUPPORTED, L"Bookmarks are not supported.\n");
    if (structured && query) return bad_parameter(L"Option q is not available when option sq is true.\n");

    flags = (logfile ? EvtQueryFilePath : EvtQueryChannelPath) | (reverse ? EvtQueryReverseDirection : EvtQueryForwardDirection);
    if (structured)
    {
        if (!(file_query = read_text_file(path, &error))) return fail_with(error, L"Failed to read the structured query file %ls.\n", path);
        result = EvtQuery(NULL, NULL, file_query, flags);
    }
    else
        result = EvtQuery(NULL, path, query ? query : L"*", flags);
    free(file_query);
    if (!result)
    {
        WCHAR status[1024];

        error = GetLastError();
        /* the detail that the query gave, or the text of the error that Windows shows in its place */
        status[0] = 0;
        if (EvtGetExtendedStatus(ARRAYSIZE(status), status, &extended)) extended = 0;
        if (extended > 1 && status[0]) outf(2, L"%ls\n\n", status);
        else if (error == ERROR_EVT_CHANNEL_NOT_FOUND || error == ERROR_EVT_INVALID_QUERY)
        {
            print_error(error);
            out_s(2, L"\n");
        }
        return fail_with(error, L"Failed to open event query.\n");
    }

    if (element) outf(1, L"<%ls>\n", element);
    context = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);

    error = ERROR_SUCCESS;
    for (;;)
    {
        EVT_HANDLE events[16];
        DWORD want = ARRAYSIZE(events), got = 0, i;

        if (limit && limit - index < want) want = limit - index;
        if (!want) break;
        if (!EvtNext(result, want, events, INFINITE, 0, &got))
        {
            error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) error = ERROR_SUCCESS;
            break;
        }
        for (i = 0; i < got; i++)
        {
            if (format == FORMAT_TEXT) out_event_text(context, events[i], index, &cache);
            else if (format == FORMAT_RENDERED) out_event_rendered_xml(context, events[i], &cache);
            else
            {
                WCHAR *xml = render_text(events[i], EvtRenderEventXml);

                if (xml) outf(1, L"%ls\n", xml);
                free(xml);
            }
            index++;
            EvtClose(events[i]);
        }
    }
    if (element) outf(1, L"</%ls>\n", element);
    if (cache.handle) EvtClose(cache.handle);
    if (context) EvtClose(context);
    EvtClose(result);
    if (error) return fail_with(error, L"Failed to read events.\n");
    return ERROR_SUCCESS;
}

/* Manifests */

struct channel
{
    WCHAR *name;
    WCHAR *access;
    DWORD id;
    BOOL has_id;
    BOOL imported;
    int type;           /* -1 when the manifest has none */
    int enabled;
    int isolation;
};

struct provider
{
    WCHAR *name;
    WCHAR guid[40];     /* in braces, lower case */
    WCHAR *resource;
    WCHAR *message;
    WCHAR *parameter;
    struct channel *channels;
    DWORD channel_count;
};

struct manifest
{
    struct provider *providers;
    DWORD count;
    BOOL found_events;
    WCHAR *error_text;  /* the text of a message that the parser wants shown with the error */
};

static void free_manifest(struct manifest *m)
{
    DWORD i, j;

    for (i = 0; i < m->count; i++)
    {
        struct provider *p = &m->providers[i];

        free(p->name);
        free(p->resource);
        free(p->message);
        free(p->parameter);
        for (j = 0; j < p->channel_count; j++)
        {
            free(p->channels[j].name);
            free(p->channels[j].access);
        }
        free(p->channels);
    }
    free(m->providers);
    free(m->error_text);
    memset(m, 0, sizeof(*m));
}

static BOOL is_guid_text(const WCHAR *s)
{
    int i;

    if (wcslen(s) != 38 || s[0] != '{' || s[37] != '}') return FALSE;
    for (i = 1; i < 37; i++)
    {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (s[i] != '-') return FALSE; }
        else if (!iswxdigit(s[i])) return FALSE;
    }
    return TRUE;
}

/* The unprefixed attributes of the current element that the caller wants, by name */
struct attr_want
{
    const WCHAR *name;
    WCHAR **value;
};

static HRESULT read_attributes(IXmlReader *reader, const struct attr_want *want, DWORD count)
{
    HRESULT hr = IXmlReader_MoveToFirstAttribute(reader);

    while (hr == S_OK)
    {
        const WCHAR *name, *prefix, *value;
        UINT len;
        DWORD i;

        if (FAILED(hr = IXmlReader_GetPrefix(reader, &prefix, &len))) return hr;
        if (FAILED(hr = IXmlReader_GetLocalName(reader, &name, &len))) return hr;
        if (!prefix[0] && wcscmp(name, L"xmlns"))
        {
            for (i = 0; i < count; i++)
            {
                if (!wcscmp(name, want[i].name))
                {
                    if (FAILED(hr = IXmlReader_GetValue(reader, &value, &len))) return hr;
                    if (len >= MAX_TEXT) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    free(*want[i].value);
                    if (!(*want[i].value = dup_n(value, len))) return E_OUTOFMEMORY;
                    break;
                }
            }
        }
        hr = IXmlReader_MoveToNextAttribute(reader);
    }
    IXmlReader_MoveToElement(reader);
    return FAILED(hr) ? hr : S_OK;
}

static int find_word(const WCHAR *value, const WCHAR *const *words, int count)
{
    int i;

    for (i = 0; i < count; i++)
        if (!_wcsicmp(value, words[i])) return i;
    return -1;
}

static HRESULT add_provider(struct manifest *m, IXmlReader *reader, struct provider **result)
{
    WCHAR *name = NULL, *guid = NULL, *res = NULL, *msg = NULL, *par = NULL;
    struct attr_want want[] =
    {
        { L"name", &name }, { L"guid", &guid }, { L"resourceFileName", &res }, { L"messageFileName", &msg }, { L"parameterFileName", &par },
    };
    struct provider *list, *p;
    HRESULT hr;
    DWORD i;

    if (m->count >= MAX_PROVIDERS) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (FAILED(hr = read_attributes(reader, want, ARRAYSIZE(want)))) goto error;
    if (!name || !name[0] || wcschr(name, '\\') || wcslen(name) > 255 || !guid || !is_guid_text(guid))
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        goto error;
    }
    if (!(list = realloc(m->providers, (m->count + 1) * sizeof(*list)))) { hr = E_OUTOFMEMORY; goto error; }
    m->providers = list;
    p = &list[m->count++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->resource = res;
    p->message = msg;
    p->parameter = par;
    for (i = 0; i < 38; i++) p->guid[i] = towlower(guid[i]);
    free(guid);
    *result = p;
    return S_OK;

error:
    free(name);
    free(guid);
    free(res);
    free(msg);
    free(par);
    return hr;
}

static HRESULT add_channel(struct provider *p, IXmlReader *reader, BOOL imported, struct channel **result)
{
    WCHAR *name = NULL, *access = NULL, *value = NULL, *type = NULL, *enabled = NULL, *isolation = NULL;
    struct attr_want want[] =
    {
        { L"name", &name }, { L"access", &access }, { L"value", &value }, { L"type", &type }, { L"enabled", &enabled }, { L"isolation", &isolation },
    };
    static const WCHAR *const types[] = { L"Admin", L"Operational", L"Analytic", L"Debug" };
    static const WCHAR *const isolations[] = { L"Application", L"System", L"Custom" };
    struct channel *list, *c;
    HRESULT hr;

    *result = NULL;
    if (p->channel_count >= MAX_CHANNELS) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (FAILED(hr = read_attributes(reader, want, ARRAYSIZE(want)))) goto error;
    if (!name || !name[0] || wcschr(name, '\\') || wcslen(name) > 255) { hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); goto error; }
    if (!(list = realloc(p->channels, (p->channel_count + 1) * sizeof(*list)))) { hr = E_OUTOFMEMORY; goto error; }
    p->channels = list;
    c = &list[p->channel_count++];
    memset(c, 0, sizeof(*c));
    c->name = name;
    c->access = access;
    c->imported = imported;
    c->type = -1;
    c->enabled = 0;
    c->isolation = 0;
    if (type) c->type = find_word(type, types, ARRAYSIZE(types));
    if (isolation && (c->isolation = find_word(isolation, isolations, ARRAYSIZE(isolations))) < 0) c->isolation = 0;
    if (enabled) c->enabled = !_wcsicmp(enabled, L"true") || !wcscmp(enabled, L"1");
    if (value)
    {
        WCHAR *end;
        unsigned long n = wcstoul(value, &end, 10);

        if (value[0] && !*end && n <= 255) { c->id = n; c->has_id = TRUE; }
    }
    free(value);
    free(type);
    free(enabled);
    free(isolation);
    *result = c;
    return S_OK;

error:
    free(name);
    free(access);
    free(value);
    free(type);
    free(enabled);
    free(isolation);
    return hr;
}

/* Reads the providers of the manifest. A failure of the parser is returned as its HRESULT. */
static HRESULT parse_manifest(const WCHAR *path, struct manifest *m, IXmlReader **keep_reader)
{
    struct { WCHAR name[64]; BOOL ok; } stack[MAX_DEPTH];
    IXmlReader *reader = NULL;
    IStream *stream = NULL;
    struct provider *provider = NULL;
    XmlNodeType type;
    HRESULT hr;
    int depth = 0;

    memset(m, 0, sizeof(*m));
    if (FAILED(hr = SHCreateStreamOnFileW(path, STGM_READ | STGM_SHARE_DENY_WRITE, &stream))) return hr;
    if (FAILED(hr = CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL))) goto done;
    IXmlReader_SetProperty(reader, XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    IXmlReader_SetProperty(reader, XmlReaderProperty_MaxElementDepth, MAX_DEPTH);
    if (FAILED(hr = IXmlReader_SetInput(reader, (IUnknown *)stream))) goto done;

    while ((hr = IXmlReader_Read(reader, &type)) == S_OK)
    {
        if (type == XmlNodeType_Element)
        {
            const WCHAR *name, *ns;
            BOOL empty = IXmlReader_IsEmptyElement(reader), ok;
            UINT len;

            if (depth >= MAX_DEPTH) { hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); break; }
            if (FAILED(hr = IXmlReader_GetLocalName(reader, &name, &len))) break;
            if (FAILED(hr = IXmlReader_GetNamespaceUri(reader, &ns, &len))) break;
            ok = depth == 0 ? (!wcscmp(name, L"instrumentationManifest") || !wcscmp(name, L"assembly")) :
                 stack[depth - 1].ok;
            if (depth == 1) ok = ok && !wcscmp(name, L"instrumentation");
            else if (depth == 2) ok = ok && !wcscmp(name, L"events") && !wcscmp(ns, EVENTS_NS);
            else if (depth == 3) ok = ok && !wcscmp(name, L"provider");
            else if (depth == 4) ok = ok && !wcscmp(name, L"channels");
            else if (depth == 5) ok = ok && (!wcscmp(name, L"channel") || !wcscmp(name, L"importChannel"));
            else if (depth > 5) ok = FALSE;
            if (depth == 2 && ok) m->found_events = TRUE;
            if (ok && depth == 3)
            {
                if (FAILED(hr = add_provider(m, reader, &provider))) break;
            }
            else if (ok && depth == 5 && provider)
            {
                struct channel *channel;

                if (FAILED(hr = add_channel(provider, reader, !wcscmp(name, L"importChannel"), &channel))) break;
            }
            if (!empty)
            {
                wcsncpy(stack[depth].name, name, ARRAYSIZE(stack[depth].name) - 1);
                stack[depth].name[ARRAYSIZE(stack[depth].name) - 1] = 0;
                stack[depth].ok = ok;
                depth++;
            }
        }
        else if (type == XmlNodeType_EndElement)
        {
            if (depth > 0) depth--;
        }
    }
    if (hr == S_FALSE) hr = S_OK;

done:
    if (FAILED(hr) && keep_reader && reader)
    {
        *keep_reader = reader;
        reader = NULL;
    }
    if (reader) IXmlReader_Release(reader);
    IStream_Release(stream);
    return hr;
}

/* Loads the manifest the way that im and um do, and writes the message of a failure. Returns the exit code. */
static DWORD load_manifest(const WCHAR *arg, struct manifest *m)
{
    WCHAR full[MAX_PATH];
    IXmlReader *reader = NULL;
    HRESULT hr;
    DWORD len = GetFullPathNameW(arg, ARRAYSIZE(full), full, NULL);

    if (!len || len >= ARRAYSIZE(full)) wcsncpy(full, arg, ARRAYSIZE(full) - 1), full[ARRAYSIZE(full) - 1] = 0;
    hr = parse_manifest(full, m, &reader);
    if (FAILED(hr))
    {
        DWORD error = HRESULT_CODE(hr);

        if (HRESULT_FACILITY(hr) == FACILITY_WIN32 || HRESULT_FACILITY(hr) == 0)
        {
            outf(2, L"Failed to load XML document %ls.\n", full);
            print_error(error);
        }
        else
        {
            UINT line = 0, column = 0;

            if (reader)
            {
                IXmlReader_GetLineNumber(reader, &line);
                IXmlReader_GetLinePosition(reader, &column);
            }
            outf(2, L"%ls(%u): Error 0x%08lx: At column=%u.\n\nFailed to load XML document %ls.\n", full, line, (DWORD)hr, column, full);
            error = (DWORD)hr;
        }
        if (reader) IXmlReader_Release(reader);
        free_manifest(m);
        return error ? error : ERROR_INVALID_DATA;
    }
    if (!m->found_events)
    {
        outf(2, L"assembly/instrumentation/events:events or \nevents:instrumentationManifest/events:instrumentation/events:events node\n"
             L"is not found in manifest file %ls.\nxmlns:events=\"" EVENTS_NS L"\"\n\n", full);
        free_manifest(m);
        return fail_with(ERROR_NOT_FOUND, NULL);
    }
    return ERROR_SUCCESS;
}

static const WCHAR default_access_application[] =
    L"O:BAG:SYD:(A;;0x2;;;S-1-15-2-1)(A;;0x2;;;S-1-15-3-1024-3153509613-960666767-3724611135-2725662640-12138253-543910227-1950414635-4190290187)"
    L"(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x7;;;SO)(A;;0x3;;;IU)(A;;0x3;;;SU)(A;;0x3;;;S-1-5-3)(A;;0x3;;;S-1-5-33)(A;;0x1;;;S-1-5-32-573)";
static const WCHAR default_access_system[] =
    L"O:BAG:SYD:(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x3;;;BO)(A;;0x5;;;SO)(A;;0x1;;;IU)(A;;0x3;;;SU)(A;;0x1;;;S-1-5-3)(A;;0x2;;;S-1-5-33)"
    L"(A;;0x1;;;S-1-5-32-573)";

/* Writes a value that is not there yet: a publisher or channel that is installed keeps what it has */
static void set_missing(HKEY key, const WCHAR *name, DWORD type, const void *data, DWORD size)
{
    if (RegQueryValueExW(key, name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, type, data, size);
}

static void set_missing_sz(HKEY key, const WCHAR *name, DWORD type, const WCHAR *value)
{
    set_missing(key, name, type, value, (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
}

static void set_missing_dword(HKEY key, const WCHAR *name, DWORD value)
{
    set_missing(key, name, REG_DWORD, &value, sizeof(value));
}

static DWORD open_key(HKEY parent, const WCHAR *sub, HKEY *key)
{
    DWORD disp;

    return RegCreateKeyExW(parent, sub, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, key, &disp);
}

static void delete_tree(HKEY root, const WCHAR *sub);

/* A publisher that is installed is written again from the manifest, a channel keeps the values that it has */
static DWORD register_provider(HKEY root, const struct provider *p)
{
    WCHAR path[600];
    HKEY pub = NULL, refs = NULL, key = NULL;
    DWORD error, i, next_id = 0x10;

    _snwprintf(path, ARRAYSIZE(path), L"Publishers\\%ls", p->guid);
    delete_tree(root, path);
    if ((error = open_key(root, path, &pub))) return error;
    set_missing_sz(pub, NULL, REG_SZ, p->name);
    if (p->resource) set_missing_sz(pub, L"ResourceFileName", REG_EXPAND_SZ, p->resource);
    if (p->message) set_missing_sz(pub, L"MessageFileName", REG_EXPAND_SZ, p->message);
    if (p->parameter) set_missing_sz(pub, L"ParameterFileName", REG_EXPAND_SZ, p->parameter);
    set_missing_dword(pub, L"Enabled", 1);
    if ((error = open_key(pub, L"ChannelReferences", &refs))) goto done;
    set_missing_dword(refs, L"Count", p->channel_count);

    for (i = 0; i < p->channel_count; i++)
    {
        const struct channel *c = &p->channels[i];
        WCHAR index[16];
        DWORD id;

        if (c->imported)
        {
            id = !_wcsicmp(c->name, L"System") ? 8 : !_wcsicmp(c->name, L"Application") ? 9 : !_wcsicmp(c->name, L"Security") ? 10 : 0;
        }
        else
            id = c->has_id ? c->id : next_id++;

        _snwprintf(index, ARRAYSIZE(index), L"%lu", i);
        if ((error = open_key(refs, index, &key))) goto done;
        set_missing_sz(key, NULL, REG_SZ, c->name);
        set_missing_dword(key, L"Id", id);
        set_missing_dword(key, L"Flags", c->imported ? EvtChannelReferenceImported : 0);
        RegCloseKey(key);
        key = NULL;

        if (c->imported) continue;
        _snwprintf(path, ARRAYSIZE(path), L"Channels\\%ls", c->name);
        if ((error = open_key(root, path, &key))) goto done;
        set_missing_sz(key, L"OwningPublisher", REG_SZ, p->guid);
        set_missing_dword(key, L"Enabled", c->enabled);
        set_missing_dword(key, L"Isolation", c->isolation);
        set_missing_sz(key, L"ChannelAccess", REG_SZ, c->access ? c->access : c->isolation == 1 ? default_access_system : default_access_application);
        set_missing_dword(key, L"Type", c->type < 0 ? 0 : c->type);
        RegCloseKey(key);
        key = NULL;
    }
done:
    if (key) RegCloseKey(key);
    if (refs) RegCloseKey(refs);
    RegCloseKey(pub);
    return error;
}

/* The resource file of a publisher: what Windows warns about when it is not there or has no metadata */
static BOOL memory_contains(const BYTE *data, DWORD size, const BYTE *pattern, DWORD pattern_size)
{
    DWORD i;

    if (size < pattern_size) return FALSE;
    for (i = 0; i + pattern_size <= size; i++)
        if (data[i] == pattern[0] && !memcmp(data + i, pattern, pattern_size)) return TRUE;
    return FALSE;
}

static BOOL guid_from_text(const WCHAR *text, GUID *guid)
{
    DWORD d1, d2, d3, b[8], i;

    if (swscanf(text, L"{%8lx-%4lx-%4lx-%2lx%2lx-%2lx%2lx%2lx%2lx%2lx%2lx}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return FALSE;
    guid->Data1 = d1;
    guid->Data2 = (WORD)d2;
    guid->Data3 = (WORD)d3;
    for (i = 0; i < 8; i++) guid->Data4[i] = (BYTE)b[i];
    return TRUE;
}

static void check_resource(const struct provider *p)
{
    WCHAR path[MAX_PATH];
    DWORD len;
    HMODULE module;
    HRSRC res;
    BOOL found = FALSE, has_metadata = FALSE, has_provider = FALSE;

    if (!p->resource || !p->resource[0])
    {
        out_s(2, L"Failed to load resource .\n");
        return;
    }
    len = ExpandEnvironmentStringsW(p->resource, path, ARRAYSIZE(path));
    if (!len || len > ARRAYSIZE(path)) wcsncpy(path, p->resource, ARRAYSIZE(path) - 1), path[ARRAYSIZE(path) - 1] = 0;

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
        (module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE)))
    {
        found = TRUE;
        if ((res = FindResourceW(module, MAKEINTRESOURCEW(1), L"WEVT_TEMPLATE")))
        {
            HGLOBAL mem = LoadResource(module, res);
            DWORD size = SizeofResource(module, res);
            const BYTE *data = mem ? LockResource(mem) : NULL;
            GUID guid;

            has_metadata = TRUE;
            if (data && guid_from_text(p->guid, &guid)) has_provider = memory_contains(data, size, (const BYTE *)&guid, sizeof(guid));
        }
        FreeLibrary(module);
    }

    if (!found)
        outf(1, L"**** Warning: The resource file for publisher %ls was not found or could not be opened.\n\nresourceFileName: %ls\n\n", p->name, path);
    else if (!has_metadata)
        outf(1, L"**** Warning: The resource file for publisher %ls does not contain the metadata resource.\n"
             L"Make sure to link the .bin file generated by the Message Compiler into the\nspecified binary.\n\nresourceFileName: %ls\n\n", p->name, path);
    else if (!has_provider)
        outf(1, L"**** Warning: Publisher %ls was not found in the resource file.\n\nresourceFileName: %ls\n\n", p->name, path);
    else
        return;
    outf(1, L"**** Warning: Publisher %ls resources could not be found or are not accessible\nto the EventLog service account (NT SERVICE\\EventLog).\n\n", p->name);
}

static void replace_attr(WCHAR **attr, const WCHAR *value)
{
    if (!value) return;
    free(*attr);
    *attr = dup_s(value);
}

static DWORD cmd_im(const struct options *o)
{
    struct manifest m;
    HKEY root, check;
    DWORD i, j, error;

    if ((error = load_manifest(o->args[0], &m))) return error;

    /* a channel without a type is refused before anything is written */
    for (i = 0; i < m.count; i++)
        for (j = 0; j < m.providers[i].channel_count; j++)
            if (!m.providers[i].channels[j].imported && m.providers[i].channels[j].type < 0)
            {
                error = fail_with(ERROR_INVALID_DATA, L"Provider %ls in the manifest contains channel %ls that is missing the type attribute.\n\n",
                                  m.providers[i].guid, m.providers[i].channels[j].name);
                free_manifest(&m);
                return error;
            }

    if ((error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, WINEVT_KEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &root, NULL)))
    {
        free_manifest(&m);
        return fail_with(error, L"Failed to install the manifest.\n");
    }
    for (i = 0; i < m.count; i++)
    {
        struct provider *p = &m.providers[i];
        WCHAR path[600];

        replace_attr(&p->resource, option(o, L"rf"));
        replace_attr(&p->message, option(o, L"mf"));
        replace_attr(&p->parameter, option(o, L"pf"));

        _snwprintf(path, ARRAYSIZE(path), L"Publishers\\%ls", p->guid);
        if (!RegOpenKeyExW(root, path, 0, KEY_READ, &check))
        {
            RegCloseKey(check);
            outf(1, L"**** Warning: Publisher %ls is installed on\nthe system. Only new values would be added. If you want to update previous \n"
                 L"settings, uninstall the manifest first.\n\n", p->guid);
        }
        if ((error = register_provider(root, p)))
        {
            RegCloseKey(root);
            free_manifest(&m);
            return fail_with(error, L"Failed to install the manifest.\n");
        }
        check_resource(p);
    }
    RegCloseKey(root);
    free_manifest(&m);
    return ERROR_SUCCESS;
}

static void delete_tree(HKEY root, const WCHAR *sub)
{
    HKEY key;
    WCHAR last[256];

    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WRITE, &key)) return;
    last[0] = 0;
    for (;;)
    {
        WCHAR name[256];
        DWORD size = ARRAYSIZE(name);

        if (RegEnumKeyExW(key, 0, name, &size, NULL, NULL, NULL, NULL)) break;
        /* a key that cannot be removed would be listed for ever */
        if (!wcscmp(name, last)) break;
        wcscpy(last, name);
        delete_tree(key, name);
    }
    RegCloseKey(key);
    RegDeleteKeyW(root, sub);
}

static DWORD cmd_um(const struct options *o)
{
    struct manifest m;
    HKEY root;
    DWORD i, j, error;

    if ((error = load_manifest(o->args[0], &m))) return error;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WINEVT_KEY, 0, KEY_READ | KEY_WRITE, &root))
    {
        free_manifest(&m);
        return ERROR_SUCCESS;
    }
    for (i = 0; i < m.count; i++)
    {
        const struct provider *p = &m.providers[i];
        WCHAR path[600];

        _snwprintf(path, ARRAYSIZE(path), L"Publishers\\%ls", p->guid);
        delete_tree(root, path);
        for (j = 0; j < p->channel_count; j++)
        {
            if (p->channels[j].imported) continue;
            _snwprintf(path, ARRAYSIZE(path), L"Channels\\%ls", p->channels[j].name);
            delete_tree(root, path);
        }
    }
    RegCloseKey(root);
    free_manifest(&m);
    return ERROR_SUCCESS;
}

static DWORD run_command(const struct command *cmd, int argc, WCHAR **argv)
{
    struct options o;
    DWORD ret;
    int i;

    for (i = 0; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"/?") || !wcscmp(argv[i], L"-?"))
        {
            if (cmd->help) out_s(1, cmd->help);
            else out_s(1, help_main);
            return ERROR_SUCCESS;
        }
    }
    if ((ret = parse_options(cmd, argc, argv, &o)))
    {
        free_options(&o);
        return ret;
    }
    if (option_true(&o, L"uni")) g_unicode = TRUE;
    if (option(&o, L"r"))
    {
        free_options(&o);
        return fail_with(ERROR_NOT_SUPPORTED, L"Remote operation is not supported.\n");
    }

    switch (cmd->id)
    {
    case CMD_EL: ret = cmd_enum(TRUE); break;
    case CMD_EP: ret = cmd_enum(FALSE); break;
    case CMD_GL: ret = cmd_gl(&o); break;
    case CMD_GLI: ret = cmd_gli(&o); break;
    case CMD_IM: ret = cmd_im(&o); break;
    case CMD_UM: ret = cmd_um(&o); break;
    case CMD_QE: ret = cmd_qe(&o); break;
    default:
        ret = fail_with(ERROR_NOT_SUPPORTED, L"Command %ls is not supported by this implementation.\n", cmd->name);
        break;
    }
    free_options(&o);
    return ret;
}

int wmain(int argc, WCHAR **argv)
{
    const struct command *cmd;

    if (argc < 2)
    {
        out_s(2, L"Command is not specified.\n");
        out_s(1, help_main);
        return 0;
    }
    if (!wcscmp(argv[1], L"/?") || !wcscmp(argv[1], L"-?"))
    {
        out_s(1, help_main);
        return 0;
    }
    if (!(cmd = find_command(argv[1])))
    {
        outf(2, L"Command %ls is not supported.\n", argv[1]);
        print_error(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }
    return (int)run_command(cmd, argc - 2, argv + 2);
}
