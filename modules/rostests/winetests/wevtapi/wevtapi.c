/*
 * Copyright 2021 Dmitry Timoshkov
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

#include "windef.h"
#include "winbase.h"
#include "winevt.h"
#include "wine/test.h"

static void test_EvtGetChannelConfigProperty(void)
{
    HKEY key;
    EVT_HANDLE config;
    DWORD size, ret;
    struct
    {
        EVT_VARIANT var;
        WCHAR buf[MAX_PATH];
    } path;

    ret = RegCreateKeyW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\EventLog\\Winetest", &key);
    if (ret == ERROR_ACCESS_DENIED)
    {
        skip("Not enough privileges to modify HKLM\n");
        return;
    }
    ok(ret == ERROR_SUCCESS, "RegCreateKey error %lu\n", ret);

    config = EvtOpenChannelConfig(NULL, L"Winetest", 0);
    ok(config != NULL, "EvtOpenChannelConfig error %lu\n", GetLastError());

    ret = EvtGetChannelConfigProperty(config, EvtChannelLoggingConfigLogFilePath, 0, 0, NULL, &size);
    ok(!ret, "EvtGetChannelConfigProperty should fail\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got %lu\n", GetLastError());
    ok(size < sizeof(path), "got %lu\n", size);

    memset(&path, 0, sizeof(path));
    path.var.Count = 0xdeadbeef;
    path.var.Type = 0xdeadbeef;
    ret = EvtGetChannelConfigProperty(config, EvtChannelLoggingConfigLogFilePath, 0, size, (EVT_VARIANT *)&path, &size);
    ok(ret, "EvtGetChannelConfigProperty error %lu\n", GetLastError());
    ok(path.var.Count == 0xdeadbeef, "got %lu\n", path.var.Count);
    ok(path.var.Type == EvtVarTypeString, "got %lu\n", path.var.Type);
    ok(size == sizeof(EVT_VARIANT) + (wcslen(path.var.StringVal) + 1) * sizeof(WCHAR), "got %lu\n", size);
    ok(path.var.StringVal == path.buf, "path.var.StringVal = %p, path.buf = %p\n", path.var.StringVal, path.buf);

    EvtClose(config);

    RegDeleteKeyW(key, L"");
    RegCloseKey(key);
}

static BOOL render_xml(EVT_HANDLE event, WCHAR **xml)
{
    DWORD size = 0, count, error;
    BOOL ret;

    *xml = NULL;
    SetLastError(0xdeadbeef);
    ret = EvtRender(NULL, event, EvtRenderEventXml, 0, NULL, &size, &count);
    error = GetLastError();
    ok(!ret, "EvtRender should fail\n");
    ok(error == ERROR_INSUFFICIENT_BUFFER, "got %lu\n", error);
    if (error != ERROR_INSUFFICIENT_BUFFER) return FALSE;
    ok(size >= sizeof(WCHAR) && !(size % sizeof(WCHAR)), "got %lu\n", size);

    *xml = HeapAlloc(GetProcessHeap(), 0, size);
    ret = EvtRender(NULL, event, EvtRenderEventXml, size, *xml, &size, &count);
    ok(ret, "EvtRender error %lu\n", GetLastError());
    ok(size == (wcslen(*xml) + 1) * sizeof(WCHAR), "got %lu for %lu characters\n", size, (DWORD)wcslen(*xml));
    return ret;
}

static void test_query_and_render(void)
{
    static const WCHAR *provider_path[] = { L"Event/System/Provider/@Name" };
    static const WCHAR *no_such_event = L"*[System[Level=99]]";
    EVT_HANDLE query, forward, event = NULL, event2, context;
    EVT_VARIANT *values;
    DWORD count, size, error, id, level, i;
    ULONGLONG newest, oldest;
    WCHAR *xml, *filter, name[256];
    BOOL ret;
    struct { EVT_VARIANT var; WCHAR buf[256]; } info;

    SetLastError(0xdeadbeef);
    query = EvtQuery(NULL, L"System", L"*[System[bogus", EvtQueryChannelPath);
    error = GetLastError();
    ok(!query, "EvtQuery should fail\n");
    ok(error == ERROR_EVT_INVALID_QUERY, "got %lu\n", error);

    SetLastError(0xdeadbeef);
    query = EvtQuery(NULL, L"wevtapi_winetest_no_such_log", NULL, EvtQueryChannelPath);
    ok(!query, "EvtQuery should fail\n");
    ok(GetLastError() == ERROR_EVT_CHANNEL_NOT_FOUND, "got %lu\n", GetLastError());

    query = EvtQuery(NULL, L"System", L"*", EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query)
    {
        skip("Cannot query the System log, error %lu\n", GetLastError());
        return;
    }

    /* the name of the log */
    memset(&info, 0, sizeof(info));
    ret = EvtGetQueryInfo(query, EvtQueryNames, sizeof(info), &info.var, &size);
    ok(ret, "EvtGetQueryInfo error %lu\n", GetLastError());
    ok(info.var.Type == (EvtVarTypeString | EVT_VARIANT_TYPE_ARRAY), "got %lu\n", info.var.Type);
    ok(info.var.Count == 1, "got %lu\n", info.var.Count);
    ok(info.var.StringArr && !_wcsicmp(info.var.StringArr[0], L"System"), "got %s\n",
       wine_dbgstr_w(info.var.StringArr ? info.var.StringArr[0] : NULL));

    SetLastError(0xdeadbeef);
    ret = EvtNext(query, 1, &event, 1000, 0, &count);
    if (!ret)
    {
        ok(GetLastError() == ERROR_NO_MORE_ITEMS, "got %lu\n", GetLastError());
        skip("The System log has no event\n");
        EvtClose(query);
        return;
    }
    ok(count == 1, "got %lu\n", count);

    ret = render_xml(event, &xml);
    if (ret)
    {
        static const WCHAR prefix[] = L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='";

        ok(!wcsncmp(xml, prefix, ARRAY_SIZE(prefix) - 1), "got %s\n", wine_dbgstr_w(xml));
        ok(wcsstr(xml, L"<Channel>System</Channel>") != NULL, "got %s\n", wine_dbgstr_w(xml));
        ok(wcsstr(xml, L"<TimeCreated SystemTime='") != NULL, "got %s\n", wine_dbgstr_w(xml));
        ok(wcsstr(xml, L"</System>") != NULL, "got %s\n", wine_dbgstr_w(xml));
    }
    HeapFree(GetProcessHeap(), 0, xml);

    /* the system properties */
    context = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
    ok(context != NULL, "EvtCreateRenderContext error %lu\n", GetLastError());
    size = 0;
    SetLastError(0xdeadbeef);
    ret = EvtRender(context, event, EvtRenderEventValues, 0, NULL, &size, &count);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got %d, %lu\n", ret, GetLastError());
    ok(count == EvtSystemPropertyIdEND, "got %lu\n", count);
    values = HeapAlloc(GetProcessHeap(), 0, size);
    ret = EvtRender(context, event, EvtRenderEventValues, size, values, &size, &count);
    ok(ret, "EvtRender error %lu\n", GetLastError());
    id = level = 0;
    newest = 0;
    if (ret && count == EvtSystemPropertyIdEND)
    {
        ok(values[EvtSystemProviderName].Type == EvtVarTypeString, "got %lu\n", values[EvtSystemProviderName].Type);
        ok(values[EvtSystemEventID].Type == EvtVarTypeUInt16, "got %lu\n", values[EvtSystemEventID].Type);
        ok(values[EvtSystemLevel].Type == EvtVarTypeByte || values[EvtSystemLevel].Type == EvtVarTypeNull, "got %lu\n",
           values[EvtSystemLevel].Type);
        ok(values[EvtSystemKeywords].Type == EvtVarTypeHexInt64, "got %lu\n", values[EvtSystemKeywords].Type);
        ok(values[EvtSystemTimeCreated].Type == EvtVarTypeFileTime, "got %lu\n", values[EvtSystemTimeCreated].Type);
        ok(values[EvtSystemTimeCreated].FileTimeVal != 0, "no time\n");
        ok(values[EvtSystemEventRecordId].Type == EvtVarTypeUInt64, "got %lu\n", values[EvtSystemEventRecordId].Type);
        ok(values[EvtSystemChannel].Type == EvtVarTypeString && !_wcsicmp(values[EvtSystemChannel].StringVal, L"System"),
           "got %lu\n", values[EvtSystemChannel].Type);
        ok(values[EvtSystemComputer].Type == EvtVarTypeString, "got %lu\n", values[EvtSystemComputer].Type);
        id = values[EvtSystemEventID].UInt16Val;
        level = values[EvtSystemLevel].Type == EvtVarTypeByte ? values[EvtSystemLevel].ByteVal : 0;
        newest = values[EvtSystemEventRecordId].UInt64Val;
    }
    HeapFree(GetProcessHeap(), 0, values);
    EvtClose(context);

    /* the text of the message is for the provider of the event to give */
    if (level >= 1 && level <= 5)
    {
        EVT_HANDLE meta;
        WCHAR provider[256];

        context = EvtCreateRenderContext(1, provider_path, EvtRenderContextValues);
        ok(context != NULL, "EvtCreateRenderContext error %lu\n", GetLastError());
        values = HeapAlloc(GetProcessHeap(), 0, 512);
        ret = EvtRender(context, event, EvtRenderEventValues, 512, values, &size, &count);
        ok(ret && count == 1 && values[0].Type == EvtVarTypeString, "got %d, %lu, %lu\n", ret, count, values[0].Type);
        provider[0] = 0;
        if (ret) lstrcpynW(provider, values[0].StringVal, ARRAY_SIZE(provider));
        HeapFree(GetProcessHeap(), 0, values);
        EvtClose(context);

        meta = EvtOpenPublisherMetadata(NULL, provider, NULL, 0, 0);
        if (meta)
        {
            ret = EvtFormatMessage(meta, event, 0, 0, NULL, EvtFormatMessageLevel, ARRAY_SIZE(name), name, &size);
            ok(ret, "EvtFormatMessage error %lu\n", GetLastError());
            ok(size == wcslen(name) + 1, "got %lu for %s\n", size, wine_dbgstr_w(name));
            ok(!wcscmp(name, level == 2 ? L"Error" : level == 3 ? L"Warning" : level == 4 ? L"Information" : name),
               "level %lu: %s\n", level, wine_dbgstr_w(name));

            SetLastError(0xdeadbeef);
            ret = EvtFormatMessage(meta, event, 0, 0, NULL, EvtFormatMessageLevel, 1, name, &size);
            ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got %d, %lu\n", ret, GetLastError());
            ok(size > 1, "got %lu\n", size);
            EvtClose(meta);
        }
        else skip("No publisher %s, error %lu\n", wine_dbgstr_w(provider), GetLastError());
    }

    /* the oldest event is the first of the other direction, and has the lowest record number */
    forward = EvtQuery(NULL, L"System", L"*", EvtQueryChannelPath | EvtQueryForwardDirection);
    ok(forward != NULL, "EvtQuery error %lu\n", GetLastError());
    if (forward)
    {
        event2 = NULL;
        ret = EvtNext(forward, 1, &event2, 1000, 0, &count);
        ok(ret, "EvtNext error %lu\n", GetLastError());
        if (ret)
        {
            context = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
            values = HeapAlloc(GetProcessHeap(), 0, 4096);
            ret = EvtRender(context, event2, EvtRenderEventValues, 4096, values, &size, &count);
            ok(ret, "EvtRender error %lu\n", GetLastError());
            oldest = ret ? values[EvtSystemEventRecordId].UInt64Val : 0;
            ok(oldest <= newest, "got %I64u, %I64u\n", oldest, newest);
            HeapFree(GetProcessHeap(), 0, values);
            EvtClose(context);
            EvtClose(event2);
        }
        EvtClose(forward);
    }

    /* filters */
    filter = HeapAlloc(GetProcessHeap(), 0, 128 * sizeof(WCHAR));
    _snwprintf(filter, 128, L"*[System[(EventID=%lu)]]", id);
    forward = EvtQuery(NULL, L"System", filter, EvtQueryChannelPath);
    ok(forward != NULL, "EvtQuery error %lu\n", GetLastError());
    if (forward)
    {
        for (i = 0;; i++)
        {
            EVT_HANDLE e = NULL;

            if (!EvtNext(forward, 1, &e, 1000, 0, &count)) break;
            EvtClose(e);
            if (i >= 3) break;
        }
        ok(i >= 1, "the event with the id of %lu is not found\n", id);
        EvtClose(forward);
    }
    HeapFree(GetProcessHeap(), 0, filter);

    forward = EvtQuery(NULL, L"System", no_such_event, EvtQueryChannelPath);
    ok(forward != NULL, "EvtQuery error %lu\n", GetLastError());
    if (forward)
    {
        EVT_HANDLE e = NULL;

        SetLastError(0xdeadbeef);
        ret = EvtNext(forward, 1, &e, 1000, 0, &count);
        ok(!ret && GetLastError() == ERROR_NO_MORE_ITEMS, "got %d, %lu\n", ret, GetLastError());
        EvtClose(forward);
    }

    /* the structured form of the same query */
    forward = EvtQuery(NULL, NULL, L"<QueryList><Query Id='0'><Select Path='System'>*[System[Level=99]]</Select>"
                       L"<Suppress Path='System'>*[System[EventID=1]]</Suppress></Query></QueryList>", EvtQueryChannelPath);
    ok(forward != NULL, "EvtQuery error %lu\n", GetLastError());
    EvtClose(forward);

    /* a bookmark follows the event that it is updated with */
    {
        EVT_HANDLE bookmark = EvtCreateBookmark(NULL);
        WCHAR text[512];

        ok(bookmark != NULL, "EvtCreateBookmark error %lu\n", GetLastError());
        ret = EvtUpdateBookmark(bookmark, event);
        ok(ret, "EvtUpdateBookmark error %lu\n", GetLastError());
        ret = EvtRender(NULL, bookmark, EvtRenderBookmark, sizeof(text), text, &size, &count);
        ok(ret, "EvtRender error %lu\n", GetLastError());
        if (ret)
        {
            ok(wcsstr(text, L"<Bookmark Channel='System' RecordId='") != NULL, "got %s\n", wine_dbgstr_w(text));
            ok(wcsstr(text, L"IsCurrent='true'") != NULL, "got %s\n", wine_dbgstr_w(text));
        }
        EvtClose(bookmark);
    }

    /* room for no event is room for the end of the results */
    SetLastError(0xdeadbeef);
    ret = EvtNext(query, 0, &event2, 1000, 0, &count);
    ok(!ret && GetLastError() == ERROR_NO_MORE_ITEMS, "got %d, %lu\n", ret, GetLastError());

    EvtClose(event);
    EvtClose(query);
}

static void test_log_info(void)
{
    EVT_HANDLE log;
    EVT_VARIANT value;
    DWORD size;
    BOOL ret;

    SetLastError(0xdeadbeef);
    log = EvtOpenLog(NULL, L"wevtapi_winetest_no_such_log", EvtOpenChannelPath);
    ok(!log && GetLastError() == ERROR_EVT_CHANNEL_NOT_FOUND, "got %p, %lu\n", log, GetLastError());

    log = EvtOpenLog(NULL, L"System", EvtOpenChannelPath);
    if (!log)
    {
        skip("Cannot open the System log, error %lu\n", GetLastError());
        return;
    }
    memset(&value, 0, sizeof(value));
    ret = EvtGetLogInfo(log, EvtLogNumberOfLogRecords, sizeof(value), &value, &size);
    ok(ret, "EvtGetLogInfo error %lu\n", GetLastError());
    ok(value.Type == EvtVarTypeUInt64, "got %lu\n", value.Type);
    ok(size == sizeof(value), "got %lu\n", size);
    ok(value.UInt64Val > 0, "the System log has no record\n");

    ret = EvtGetLogInfo(log, EvtLogFull, sizeof(value), &value, &size);
    ok(ret && value.Type == EvtVarTypeBoolean, "got %d, %lu\n", ret, value.Type);

    SetLastError(0xdeadbeef);
    ret = EvtGetLogInfo(log, EvtLogLastWriteTime, 0, NULL, &size);
    ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got %d, %lu\n", ret, GetLastError());
    ok(size == sizeof(value), "got %lu\n", size);

    SetLastError(0xdeadbeef);
    ret = EvtGetLogInfo(log, EvtLogPropertyIdEND, sizeof(value), &value, &size);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "got %d, %lu\n", ret, GetLastError());
    EvtClose(log);
}

static void test_channel_properties(void)
{
    EVT_HANDLE config;
    struct { EVT_VARIANT var; WCHAR buf[1024]; } data;
    DWORD size;
    BOOL ret;

    config = EvtOpenChannelConfig(NULL, L"System", 0);
    if (!config)
    {
        skip("Cannot open the System channel, error %lu\n", GetLastError());
        return;
    }
    memset(&data, 0, sizeof(data));
    ret = EvtGetChannelConfigProperty(config, EvtChannelConfigClassicEventlog, 0, sizeof(data), &data.var, &size);
    ok(ret && data.var.Type == EvtVarTypeBoolean && data.var.BooleanVal, "got %d, %lu\n", ret, data.var.Type);
    ret = EvtGetChannelConfigProperty(config, EvtChannelConfigIsolation, 0, sizeof(data), &data.var, &size);
    ok(ret && data.var.Type == EvtVarTypeUInt32 && data.var.UInt32Val == 1, "got %d, %lu\n", ret, data.var.Type);
    ret = EvtGetChannelConfigProperty(config, EvtChannelConfigType, 0, sizeof(data), &data.var, &size);
    ok(ret && data.var.Type == EvtVarTypeUInt32 && data.var.UInt32Val == 0, "got %d, %lu\n", ret, data.var.Type);
    ret = EvtGetChannelConfigProperty(config, EvtChannelLoggingConfigMaxSize, 0, sizeof(data), &data.var, &size);
    ok(ret && data.var.Type == EvtVarTypeUInt64 && data.var.UInt64Val > 0, "got %d, %lu\n", ret, data.var.Type);
    ret = EvtGetChannelConfigProperty(config, EvtChannelLoggingConfigRetention, 0, sizeof(data), &data.var, &size);
    ok(ret && data.var.Type == EvtVarTypeBoolean && !data.var.BooleanVal, "got %d, %lu\n", ret, data.var.Type);
    ret = EvtGetChannelConfigProperty(config, EvtChannelConfigAccess, 0, sizeof(data), &data.var, &size);
    ok(ret && data.var.Type == EvtVarTypeString && !wcsncmp(data.var.StringVal, L"O:", 2), "got %d, %lu\n", ret, data.var.Type);
    ret = EvtGetChannelConfigProperty(config, EvtChannelPublishingConfigLevel, 0, sizeof(data), &data.var, &size);
    ok(ret && data.var.Type == EvtVarTypeNull, "got %d, %lu\n", ret, data.var.Type);
    EvtClose(config);
}

static void test_publishers(void)
{
    EVT_HANDLE publishers, meta, array;
    struct { EVT_VARIANT var; WCHAR buf[1024]; } data;
    WCHAR name[256];
    DWORD size, count = 0, array_size;
    BOOL ret, found = FALSE;

    publishers = EvtOpenPublisherEnum(NULL, 0);
    if (!publishers)
    {
        skip("Cannot enumerate the publishers, error %lu\n", GetLastError());
        return;
    }
    while (EvtNextPublisherId(publishers, ARRAY_SIZE(name), name, &size))
    {
        ok(size == wcslen(name) + 1, "got %lu for %s\n", size, wine_dbgstr_w(name));
        if (!_wcsicmp(name, L"Service Control Manager")) found = TRUE;
        count++;
    }
    ok(GetLastError() == ERROR_NO_MORE_ITEMS, "got %lu\n", GetLastError());
    ok(count > 0, "no publisher\n");
    ok(found, "Service Control Manager is not among %lu publishers\n", count);
    EvtClose(publishers);

    SetLastError(0xdeadbeef);
    meta = EvtOpenPublisherMetadata(NULL, L"wevtapi_winetest_no_such_publisher", NULL, 0, 0);
    ok(!meta && GetLastError() == ERROR_FILE_NOT_FOUND, "got %p, %lu\n", meta, GetLastError());

    meta = EvtOpenPublisherMetadata(NULL, L"Service Control Manager", NULL, 0, 0);
    if (!meta)
    {
        skip("Cannot open the Service Control Manager publisher, error %lu\n", GetLastError());
        return;
    }
    memset(&data, 0, sizeof(data));
    ret = EvtGetPublisherMetadataProperty(meta, EvtPublisherMetadataMessageFilePath, 0, sizeof(data), &data.var, &size);
    ok(ret, "EvtGetPublisherMetadataProperty error %lu\n", GetLastError());
    ok(data.var.Type == EvtVarTypeString && wcslen(data.var.StringVal) > 4, "got %lu\n", data.var.Type);

    ret = EvtGetPublisherMetadataProperty(meta, EvtPublisherMetadataChannelReferences, 0, sizeof(data), &data.var, &size);
    ok(ret, "EvtGetPublisherMetadataProperty error %lu\n", GetLastError());
    ok(data.var.Type == EvtVarTypeEvtHandle, "got %lu\n", data.var.Type);
    if (ret && data.var.Type == EvtVarTypeEvtHandle)
    {
        array = data.var.EvtHandleVal;
        ret = EvtGetObjectArraySize(array, &array_size);
        ok(ret && array_size >= 1, "got %d, %lu\n", ret, array_size);
        memset(&data, 0, sizeof(data));
        ret = EvtGetObjectArrayProperty(array, EvtPublisherMetadataChannelReferencePath, 0, 0, sizeof(data), &data.var, &size);
        ok(ret, "EvtGetObjectArrayProperty error %lu\n", GetLastError());
        ok(data.var.Type == EvtVarTypeString && !_wcsicmp(data.var.StringVal, L"System"), "got %lu\n", data.var.Type);
        EvtClose(array);
    }

    /* the properties of one element are not those of the publisher */
    SetLastError(0xdeadbeef);
    ret = EvtGetPublisherMetadataProperty(meta, EvtPublisherMetadataChannelReferencePath, 0, sizeof(data), &data.var, &size);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "got %d, %lu\n", ret, GetLastError());
    EvtClose(meta);
}

static void test_backwards_read(void)
{
    BYTE *buffer = HeapAlloc(GetProcessHeap(), 0, 64 * 1024);
    DWORD read, needed, calls = 0, error = 0, last = 0, records = 0;
    HANDLE log = OpenEventLogW(NULL, L"System");

    if (!log)
    {
        skip("Cannot open the System log, error %lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, buffer);
        return;
    }
    /* a read backwards comes to the first record, and then to the end */
    while (calls < 100)
    {
        EVENTLOGRECORD *rec;
        DWORD pos;

        calls++;
        if (!ReadEventLogW(log, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0, buffer, 64 * 1024, &read, &needed))
        {
            error = GetLastError();
            break;
        }
        for (pos = 0; pos < read; pos += rec->Length)
        {
            rec = (EVENTLOGRECORD *)(buffer + pos);
            ok(!last || rec->RecordNumber < last, "record %lu follows record %lu\n", rec->RecordNumber, last);
            last = rec->RecordNumber;
            records++;
        }
    }
    ok(error == ERROR_HANDLE_EOF, "got %lu after %lu calls\n", error, calls);
    ok(records > 0, "no record\n");
    CloseEventLog(log);
    HeapFree(GetProcessHeap(), 0, buffer);
}

START_TEST(wevtapi)
{
    test_EvtGetChannelConfigProperty();
    test_log_info();
    test_channel_properties();
    test_publishers();
    test_query_and_render();
    test_backwards_read();
}
