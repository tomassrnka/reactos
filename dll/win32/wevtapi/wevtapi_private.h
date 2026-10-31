/*
 * Classic event log backend of wevtapi: the records of the logs that the eventlog service keeps
 * (Application, System, Security and the logs that others register), seen as events.
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

#ifndef __WEVTAPI_PRIVATE_H
#define __WEVTAPI_PRIVATE_H

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winerror.h"
#include "winevt.h"

/* The ways that a name or a value in a record's text can be limited */
#define EVT_MAX_XPATH_LEN   (64 * 1024)
#define EVT_MAX_XPATH_DEPTH 64

/* xpath.c: the subset of XPath 1.0 that event queries use, over the XML of one event */

struct xpath;
struct xnode;

/* NULL when the text is not an expression of the subset */
struct xpath *xpath_compile(const WCHAR *text);
void xpath_free(struct xpath *path);
/* TRUE when the expression selects at least one node of the document, or is true */
BOOL xpath_match(const struct xpath *path, struct xnode *document);

/* The document of an event as xml_render_event writes it. NULL for text that is not that XML. */
struct xnode *xml_parse_event(const WCHAR *xml);
void xml_free(struct xnode *document);

/* classic.c: records, sources and the text of messages */

/* The fields of one record of a classic log, seen as the system section of an event */
struct classic_event
{
    const EVENTLOGRECORD *rec;
    const WCHAR *channel;
    const WCHAR *source;
    const WCHAR *computer;
    WORD id;
    WORD qualifiers;
    BYTE level;
    WORD task;
    ULONGLONG keywords;
    ULONGLONG time;                 /* FILETIME units */
    ULONGLONG record_id;
    DWORD string_count;
    const WCHAR **strings;          /* allocated, the strings are in the record */
    const BYTE *data;
    DWORD data_len;
    PSID sid;
};

BOOL classic_event_init(struct classic_event *event, const EVENTLOGRECORD *rec, const WCHAR *channel);
void classic_event_free(struct classic_event *event);

/* A source (publisher) of the classic logs, with the files that hold its message texts */
struct classic_pub
{
    WCHAR *name;
    WCHAR *event_file;              /* the raw values of the registry, which may name environment variables */
    WCHAR *category_file;
    WCHAR *parameter_file;
    WCHAR **logs;                   /* the logs that register it */
    DWORD log_count;
};

struct classic_pub *classic_pub_open(const WCHAR *name);
struct classic_pub *classic_pub_default(void);
void classic_pub_free(struct classic_pub *pub);
/* The names of all sources of all logs, without duplicates. Returns the count, and NULL in *names for none. */
DWORD classic_pub_list(WCHAR ***names);
/* The names of the sources that one log registers. */
DWORD classic_pub_list_for_log(const WCHAR *log, WCHAR ***names);
void classic_names_free(WCHAR **names, DWORD count);
/* The names of the channels: the classic logs, and the channels that an installed manifest registers */
DWORD classic_channel_list(WCHAR ***names);
/* Whether an installed manifest registers the channel (HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WINEVT\\Channels) */
BOOL classic_registered_channel(const WCHAR *name);
/* A copy of the raw value with its environment variables expanded, or NULL */
WCHAR *classic_expand(const WCHAR *value);

/* The system section as the variants of EvtRender. Returns ERROR_SUCCESS or the error to report. */
DWORD classic_render_values(DWORD context_flags, const DWORD *indexes, DWORD index_count, const struct classic_event *event,
                            DWORD size, void *buffer, DWORD *used, DWORD *count);
/* The index of the system value that a path such as Event/System/EventID names, or -1 */
int classic_value_index(const WCHAR *path);

/* The XML of the event, as a heap string. rendering is the text of a RenderingInfo element to add, or NULL. */
WCHAR *classic_render_xml(const struct classic_event *event, const WCHAR *rendering);

/* EvtFormatMessage for the classic logs. Returns the text in *text (heap) and ERROR_SUCCESS, or the error to report. */
DWORD classic_format(const struct classic_pub *pub, const struct classic_event *event, DWORD message_id, DWORD flags,
                     DWORD value_count, const EVT_VARIANT *values, WCHAR **text, DWORD *length);

/* The properties of publishers, of the elements of their arrays, and of events */
DWORD classic_pub_property(const struct classic_pub *pub, DWORD id, DWORD size, void *buffer, DWORD *used);
DWORD classic_array_property(const WCHAR *item, DWORD index, DWORD id, DWORD size, void *buffer, DWORD *used);
DWORD classic_event_info(const struct classic_event *event, DWORD id, DWORD size, void *buffer, DWORD *used);

/* The properties of the channels and logs that the registry describes */
DWORD classic_channel_property(const WCHAR *channel, DWORD id, DWORD size, void *buffer, DWORD *used);
DWORD classic_log_property(const WCHAR *channel, DWORD id, DWORD size, void *buffer, DWORD *used);

/* A reader of the records of one or more logs, with the filters of a query */
struct classic_query;

struct classic_select
{
    WCHAR *channel;
    WCHAR *xpath;                   /* NULL for every event */
    DWORD group;                    /* the query that it belongs to */
    BOOL suppress;
};

DWORD classic_query_open(const struct classic_select *selects, DWORD count, BOOL reverse, struct classic_query **query);
void classic_query_free(struct classic_query *query);
/* The next event, in the order of the time that records were generated at. NULL for the end; the error is set. */
EVENTLOGRECORD *classic_query_next(struct classic_query *query, const WCHAR **channel, DWORD *error);
DWORD classic_query_names(const struct classic_query *query, DWORD size, void *buffer, DWORD *used);
DWORD classic_query_statuses(const struct classic_query *query, DWORD size, void *buffer, DWORD *used);
/* Starts again from the first event, and skips the given number of events */
DWORD classic_query_rewind(struct classic_query *query, ULONGLONG skip);

#endif
