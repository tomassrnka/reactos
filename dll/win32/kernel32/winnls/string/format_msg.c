/*
 * FormatMessage implementation
 *
 * Copyright 1996 Marcus Meissner
 * Copyright 2009, 2020 Alexandre Julliard
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

#ifdef __REACTOS__

#include <k32.h>

#define NDEBUG
#include <debug.h>
DEBUG_CHANNEL(resource);

extern HMODULE kernel32_handle;

#else /* __REACTOS__ */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winternl.h"
#include "winuser.h"
#include "winnls.h"
#include "wine/unicode.h"
#include "kernel_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(resource);

#endif /* __REACTOS__ */

static inline BOOL set_ntstatus( NTSTATUS status )
{
    if (status) SetLastError( RtlNtStatusToDosError( status ));
    return !status;
}

static const WCHAR *get_message( DWORD flags, const void *src, UINT id, UINT lang,
                                 BOOL ansi, WCHAR **buffer )
{
    DWORD len;

    if (!(flags & FORMAT_MESSAGE_FROM_STRING))
    {
#ifdef __REACTOS__
        MESSAGE_RESOURCE_ENTRY *entry;
#else
        const MESSAGE_RESOURCE_ENTRY *entry;
#endif
        NTSTATUS status = STATUS_INVALID_PARAMETER;

        if (flags & FORMAT_MESSAGE_FROM_HMODULE)
        {
            HMODULE module = (HMODULE)src;
            if (!module) module = GetModuleHandleW( 0 );
            status = RtlFindMessage( module, (ULONG_PTR)RT_MESSAGETABLE, lang, id, &entry );
        }
        if (status && (flags & FORMAT_MESSAGE_FROM_SYSTEM))
        {
            /* Fold win32 hresult to its embedded error code. */
            if (HRESULT_SEVERITY(id) == SEVERITY_ERROR && HRESULT_FACILITY(id) == FACILITY_WIN32)
                id = HRESULT_CODE( id );
            status = RtlFindMessage( kernel32_handle, (ULONG_PTR)RT_MESSAGETABLE, lang, id, &entry );
        }
        if (!set_ntstatus( status )) return NULL;

        src = entry->Text;
        ansi = !(entry->Flags & MESSAGE_RESOURCE_UNICODE);
    }

    if (!ansi) return src;
    len = MultiByteToWideChar( CP_ACP, 0, src, -1, NULL, 0 );
    if (!(*buffer = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) ))) return NULL;
    MultiByteToWideChar( CP_ACP, 0, src, -1, *buffer, len );
    return *buffer;
}

/***********************************************************************
 *           FormatMessageA   (KERNEL32.@)
 */
DWORD WINAPI FormatMessageA( DWORD flags, const void *source, DWORD msgid, DWORD langid,
                             char *buffer, DWORD size, __ms_va_list *args )
{
    DWORD ret = 0;
    ULONG len, retsize = 0;
    ULONG width = (flags & FORMAT_MESSAGE_MAX_WIDTH_MASK);
    const WCHAR *src;
    WCHAR *result, *message = NULL;
    NTSTATUS status;

    TRACE( "(0x%x,%p,%d,0x%x,%p,%d,%p)\n", flags, source, msgid, langid, buffer, size, args );

    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        if (!buffer)
        {
            SetLastError( ERROR_NOT_ENOUGH_MEMORY );
            return 0;
        }
        *(char **)buffer = NULL;
    }
    if (size >= 32768)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return 0;
    }

    if (width == 0xff) width = ~0u;

    if (!(src = get_message( flags, source, msgid, langid, TRUE, &message ))) return 0;

    if (!(result = HeapAlloc( GetProcessHeap(), 0, 65536 )))
        status = STATUS_NO_MEMORY;
    else
        status = RtlFormatMessage( (WCHAR *)src, width, !!(flags & FORMAT_MESSAGE_IGNORE_INSERTS),
                                   TRUE, !!(flags & FORMAT_MESSAGE_ARGUMENT_ARRAY), args,
                                   result, 65536, &retsize );

    HeapFree( GetProcessHeap(), 0, message );

    if (status == STATUS_BUFFER_OVERFLOW)
    {
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        goto done;
    }
    if (!set_ntstatus( status )) goto done;

    len = WideCharToMultiByte( CP_ACP, 0, result, retsize / sizeof(WCHAR), NULL, 0, NULL, NULL );
    if (len <= 1)
    {
        SetLastError( ERROR_NO_WORK_DONE );
        goto done;
    }

    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        char *buf = LocalAlloc( LMEM_ZEROINIT, max( size, len ));
        if (!buf)
        {
            SetLastError( ERROR_NOT_ENOUGH_MEMORY );
            goto done;
        }
        *(char **)buffer = buf;
        WideCharToMultiByte( CP_ACP, 0, result, retsize / sizeof(WCHAR), buf, max( size, len ), NULL, NULL );
    }
    else if (len > size)
    {
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        goto done;
    }
    else WideCharToMultiByte( CP_ACP, 0, result, retsize / sizeof(WCHAR), buffer, size, NULL, NULL );

    ret = len - 1;

done:
    HeapFree( GetProcessHeap(), 0, result );
    return ret;
}

/***********************************************************************
 *           FormatMessageW   (KERNEL32.@)
 */
DWORD WINAPI FormatMessageW( DWORD flags, const void *source, DWORD msgid, DWORD langid,
                             WCHAR *buffer, DWORD size, __ms_va_list *args )
{
    ULONG retsize = 0;
    ULONG width = (flags & FORMAT_MESSAGE_MAX_WIDTH_MASK);
    const WCHAR *src;
    WCHAR *message = NULL;
    NTSTATUS status;

    TRACE( "(0x%x,%p,%d,0x%x,%p,%d,%p)\n", flags, source, msgid, langid, buffer, size, args );

    if (!buffer)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return 0;
    }

    if (width == 0xff) width = ~0u;

    /* size is a character count, RtlFormatMessage takes bytes in a ULONG */
    if (size > MAXULONG / sizeof(WCHAR)) size = MAXULONG / sizeof(WCHAR);

    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) *(LPWSTR *)buffer = NULL;

    if (!(src = get_message( flags, source, msgid, langid, FALSE, &message ))) return 0;

    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        WCHAR *result;
        __ms_va_list args_copy;
        ULONG alloc = max( size * sizeof(WCHAR), 65536 );

        for (;;)
        {
            if (!(result = HeapAlloc( GetProcessHeap(), 0, alloc )))
            {
                status = STATUS_NO_MEMORY;
                break;
            }
            if (args && !(flags & FORMAT_MESSAGE_ARGUMENT_ARRAY))
            {
                va_copy( args_copy, *args );
                status = RtlFormatMessage( (WCHAR *)src, width, !!(flags & FORMAT_MESSAGE_IGNORE_INSERTS),
                                           FALSE, FALSE, &args_copy, result, alloc, &retsize );
                va_end( args_copy );
            }
            else
                status = RtlFormatMessage( (WCHAR *)src, width, !!(flags & FORMAT_MESSAGE_IGNORE_INSERTS),
                                           FALSE, TRUE, args, result, alloc, &retsize );

            if (!status)
            {
                if (retsize > sizeof(WCHAR))
                {
                    WCHAR *buf = LocalAlloc( LMEM_ZEROINIT, max( retsize, size * sizeof(WCHAR) ));
                    if (buf) memcpy( buf, result, retsize );
                    else status = STATUS_NO_MEMORY;
                    *(WCHAR **)buffer = buf;
                }
                HeapFree( GetProcessHeap(), 0, result );
                break;
            }
            HeapFree( GetProcessHeap(), 0, result );
            if (status != STATUS_BUFFER_OVERFLOW) break;
            alloc *= 2;
        }
    }
    else status = RtlFormatMessage( (WCHAR *)src, width, !!(flags & FORMAT_MESSAGE_IGNORE_INSERTS),
                                    FALSE, !!(flags & FORMAT_MESSAGE_ARGUMENT_ARRAY), args,
                                    buffer, size * sizeof(WCHAR), &retsize );

    HeapFree( GetProcessHeap(), 0, message );

    if (status == STATUS_BUFFER_OVERFLOW)
    {
        if (size) buffer[size - 1] = 0;
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        return 0;
    }
    if (!set_ntstatus( status )) return 0;
    if (retsize <= sizeof(WCHAR)) SetLastError( ERROR_NO_WORK_DONE );
    return retsize / sizeof(WCHAR) - 1;
}
