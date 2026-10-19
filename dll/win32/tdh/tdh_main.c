/*
 * Copyright 2017 Austin English
 * Copyright 2025 Paul Gofman for CodeWeavers (TdhEnumerateProviders)
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
#include "winerror.h"
#include "tdh.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(tdh);

/*
 * There is no ETW provider registry, no manifest store and no WBEM class store behind this
 * library. Every lookup therefore finds nothing: the functions validate their arguments the way
 * Windows does and then report that the schema or provider is not found, so that a consumer
 * sees a failure instead of garbage.
 */

static BOOL buffer_size_mismatch(const void *buffer, const ULONG *buffer_size)
{
    return (*buffer_size == 0) != (buffer == NULL);
}

ULONG WINAPI TdhLoadManifest(PWSTR manifest)
{
    FIXME("(%s): stub\n", debugstr_w(manifest));

    if (!manifest) return ERROR_INVALID_PARAMETER;
    if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES) return GetLastError();
    return ERROR_NOT_SUPPORTED;
}

ULONG WINAPI TdhUnloadManifest(PWSTR manifest)
{
    FIXME("(%s): stub\n", debugstr_w(manifest));

    if (!manifest) return ERROR_INVALID_PARAMETER;
    if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES) return GetLastError();
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhEnumerateProviders( PROVIDER_ENUMERATION_INFO *buffer, ULONG *buffer_size )
{
    FIXME( "%p %p semi-stub.\n", buffer, buffer_size );

    if (!buffer_size) return ERROR_INVALID_PARAMETER;
    if (*buffer_size && !buffer) return ERROR_INVALID_PARAMETER;

    if (*buffer_size < offsetof(PROVIDER_ENUMERATION_INFO, TraceProviderInfoArray))
    {
        *buffer_size = offsetof(PROVIDER_ENUMERATION_INFO, TraceProviderInfoArray);
        return ERROR_INSUFFICIENT_BUFFER;
    }
    buffer->NumberOfProviders = 0;
    return ERROR_SUCCESS;
}

ULONG WINAPI TdhEnumerateProviderFieldInformation(LPGUID guid, EVENT_FIELD_TYPE type,
        PROVIDER_FIELD_INFOARRAY *buffer, ULONG *buffer_size)
{
    FIXME("%s, %d, %p, %p: stub\n", debugstr_guid(guid), type, buffer, buffer_size);

    if (!guid || !buffer_size) return ERROR_INVALID_PARAMETER;
    if ((ULONG)type >= EventOpcodeInformation) return ERROR_NOT_SUPPORTED;
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhQueryProviderFieldInformation(LPGUID guid, ULONGLONG value, EVENT_FIELD_TYPE type,
        PROVIDER_FIELD_INFOARRAY *buffer, ULONG *buffer_size)
{
    FIXME("%s, %s, %d, %p, %p: stub\n", debugstr_guid(guid), wine_dbgstr_longlong(value), type,
            buffer, buffer_size);

    if (!guid || !buffer_size) return ERROR_INVALID_PARAMETER;
    if ((ULONG)type >= EventInformationMax) return ERROR_NOT_SUPPORTED;
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhEnumerateProviderFilters(LPGUID guid, ULONG context_count, PTDH_CONTEXT context,
        ULONG *filter_count, PROVIDER_FILTER_INFO **buffer, ULONG *buffer_size)
{
    FIXME("%s, %lu, %p, %p, %p, %p: stub\n", debugstr_guid(guid), context_count, context,
            filter_count, buffer, buffer_size);

    if (!guid || !filter_count || !buffer_size) return ERROR_INVALID_PARAMETER;
    if (context_count && !context) return ERROR_INVALID_PARAMETER;
    if (buffer && !*buffer_size) return ERROR_INVALID_PARAMETER;
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhGetEventInformation(PEVENT_RECORD event, ULONG context_count, PTDH_CONTEXT context,
        PTRACE_EVENT_INFO buffer, ULONG *buffer_size)
{
    FIXME("%p, %lu, %p, %p, %p: stub\n", event, context_count, context, buffer, buffer_size);

    if (!event || !buffer_size) return ERROR_INVALID_PARAMETER;
    if (context_count && !context) return ERROR_INVALID_PARAMETER;
    if (buffer_size_mismatch(buffer, buffer_size)) return ERROR_INVALID_PARAMETER;
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhGetEventMapInformation(PEVENT_RECORD event, LPWSTR map_name, PEVENT_MAP_INFO buffer,
        ULONG *buffer_size)
{
    FIXME("%p, %s, %p, %p: stub\n", event, debugstr_w(map_name), buffer, buffer_size);

    if (!event || !map_name || !buffer_size) return ERROR_INVALID_PARAMETER;
    if (buffer_size_mismatch(buffer, buffer_size)) return ERROR_INVALID_PARAMETER;
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhGetPropertySize(PEVENT_RECORD event, ULONG context_count, PTDH_CONTEXT context,
        ULONG property_count, PPROPERTY_DATA_DESCRIPTOR property_data, ULONG *property_size)
{
    FIXME("%p, %lu, %p, %lu, %p, %p: stub\n", event, context_count, context, property_count,
            property_data, property_size);

    if (!event || !property_count || !property_data || !property_size) return ERROR_INVALID_PARAMETER;
    if (context_count && !context) return ERROR_INVALID_PARAMETER;
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhGetProperty(PEVENT_RECORD event, ULONG context_count, PTDH_CONTEXT context,
        ULONG property_count, PPROPERTY_DATA_DESCRIPTOR property_data, ULONG buffer_size, PBYTE buffer)
{
    FIXME("%p, %lu, %p, %lu, %p, %lu, %p: stub\n", event, context_count, context, property_count,
            property_data, buffer_size, buffer);

    if (!event || !property_count || !property_data || !buffer_size || !buffer) return ERROR_INVALID_PARAMETER;
    if (context_count && !context) return ERROR_INVALID_PARAMETER;
    return ERROR_NOT_FOUND;
}

ULONG WINAPI TdhFormatProperty(PTRACE_EVENT_INFO event_info, PEVENT_MAP_INFO map_info,
        ULONG pointer_size, USHORT in_type, USHORT out_type, USHORT property_length,
        USHORT user_data_length, PBYTE user_data, ULONG *buffer_size, PWCHAR buffer,
        USHORT *user_data_consumed)
{
    FIXME("%p, %p, %lu, %u, %u, %u, %u, %p, %p, %p, %p: stub\n", event_info, map_info, pointer_size,
            in_type, out_type, property_length, user_data_length, user_data, buffer_size, buffer,
            user_data_consumed);

    if (!event_info || !buffer_size || !user_data_consumed) return ERROR_INVALID_PARAMETER;
    return ERROR_NOT_SUPPORTED;
}
