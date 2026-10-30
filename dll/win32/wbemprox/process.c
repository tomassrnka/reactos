/*
 * Win32_Process methods implementation
 *
 * Copyright 2013 Hans Leidekker for CodeWeavers
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

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wbemcli.h"
#ifdef __REACTOS__
#include "winuser.h"
#include "oleauto.h"
#endif

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

static HRESULT get_owner( VARIANT *user, VARIANT *domain, VARIANT *retval )
{
    DWORD len;
    UINT error = 8;

    len = 0;
    GetUserNameW( NULL, &len );
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) goto done;
    if (!(V_BSTR( user ) = SysAllocStringLen( NULL, len - 1 ))) goto done;
    if (!GetUserNameW( V_BSTR( user ), &len )) goto done;
    V_VT( user ) = VT_BSTR;

    len = 0;
    GetComputerNameW( NULL, &len );
    if (GetLastError() != ERROR_BUFFER_OVERFLOW) goto done;
    if (!(V_BSTR( domain ) = SysAllocStringLen( NULL, len - 1 ))) goto done;
    if (!GetComputerNameW( V_BSTR( domain ), &len )) goto done;
    V_VT( domain ) = VT_BSTR;

    error = 0;

done:
    if (error)
    {
        VariantClear( user );
        VariantClear( domain );
    }
    set_variant( VT_UI4, error, NULL, retval );
    return S_OK;
}

HRESULT process_get_owner( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    VARIANT user, domain, retval;
    IWbemClassObject *sig, *out_params = NULL;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    hr = create_signature( class_processW, method_getownerW, PARAM_OUT, &sig );
    if (hr != S_OK) return hr;

    if (out)
    {
        hr = IWbemClassObject_SpawnInstance( sig, 0, &out_params );
        if (hr != S_OK)
        {
            IWbemClassObject_Release( sig );
            return hr;
        }
    }
    VariantInit( &user );
    VariantInit( &domain );
    hr = get_owner( &user, &domain, &retval );
    if (hr != S_OK) goto done;
    if (out_params)
    {
        if (!V_UI4( &retval ))
        {
            hr = IWbemClassObject_Put( out_params, param_userW, 0, &user, CIM_STRING );
            if (hr != S_OK) goto done;
            hr = IWbemClassObject_Put( out_params, param_domainW, 0, &domain, CIM_STRING );
            if (hr != S_OK) goto done;
        }
        hr = IWbemClassObject_Put( out_params, param_returnvalueW, 0, &retval, CIM_UINT32 );
    }

done:
    VariantClear( &user );
    VariantClear( &domain );
    IWbemClassObject_Release( sig );
    if (hr == S_OK && out)
    {
        *out = out_params;
        IWbemClassObject_AddRef( out_params );
    }
    if (out_params) IWbemClassObject_Release( out_params );
    return hr;
}

#ifdef __REACTOS__
#include <sddl.h>

/* the out-parameter object of a method; *out_params stays NULL when the caller wants no results */
HRESULT method_begin( const WCHAR *class, const WCHAR *method, IWbemClassObject **out,
                      IWbemClassObject **out_params )
{
    IWbemClassObject *sig;
    HRESULT hr;

    *out_params = NULL;
    if ((hr = create_signature( class, method, PARAM_OUT, &sig )) != S_OK) return hr;
    if (out && sig) hr = IWbemClassObject_SpawnInstance( sig, 0, out_params );
    if (sig) IWbemClassObject_Release( sig );
    return hr;
}

/* store ReturnValue and hand the out-parameters to the caller */
HRESULT method_end( HRESULT hr, IWbemClassObject *obj, IWbemClassObject *out_params, UINT retval,
                    IWbemClassObject **out )
{
    VARIANT var;

    (void)obj;
    if (hr == S_OK && out_params)
    {
        V_VT( &var ) = VT_I4;
        V_I4( &var ) = retval;
        hr = IWbemClassObject_Put( out_params, param_returnvalueW, 0, &var, CIM_UINT32 );
    }
    if (hr == S_OK && out)
    {
        *out = out_params;
        if (out_params) IWbemClassObject_AddRef( out_params );
    }
    if (out_params) IWbemClassObject_Release( out_params );
    return hr;
}

static UINT map_process_error( DWORD error )
{
    switch (error)
    {
    case ERROR_SUCCESS:           return 0;
    case ERROR_ACCESS_DENIED:     return 2;
    case ERROR_PRIVILEGE_NOT_HELD: return 3;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:    return 9;
    case ERROR_INVALID_PARAMETER: return 21;
    default:                      return 8;
    }
}

/* the string value of a parameter (NULL when absent) */
static WCHAR *get_param_string( IWbemClassObject *in, const WCHAR *name )
{
    VARIANT var;
    WCHAR *ret = NULL;

    if (!in) return NULL;
    VariantInit( &var );
    if (IWbemClassObject_Get( in, name, 0, &var, NULL, NULL ) == S_OK && V_VT( &var ) == VT_BSTR && V_BSTR( &var ))
        ret = heap_strdupW( V_BSTR( &var ) );
    VariantClear( &var );
    return ret;
}

static BOOL get_param_int( IWbemClassObject *in, const WCHAR *name, LONG *value )
{
    VARIANT var, tmp;
    BOOL ret = FALSE;

    if (!in) return FALSE;
    VariantInit( &var );
    VariantInit( &tmp );
    if (IWbemClassObject_Get( in, name, 0, &var, NULL, NULL ) == S_OK && V_VT( &var ) != VT_NULL &&
        V_VT( &var ) != VT_EMPTY && VariantChangeType( &tmp, &var, 0, VT_I4 ) == S_OK)
    {
        *value = V_I4( &tmp );
        ret = TRUE;
    }
    VariantClear( &var );
    return ret;
}

static DWORD get_object_pid( IWbemClassObject *obj )
{
    VARIANT var;
    DWORD pid = 0;

    VariantInit( &var );
    if (IWbemClassObject_Get( obj, L"ProcessId", 0, &var, NULL, NULL ) == S_OK && V_VT( &var ) == VT_I4)
        pid = V_I4( &var );
    VariantClear( &var );
    return pid;
}

/* static Win32_Process.Create: the process runs as the caller (this provider runs in the caller's process) */
HRESULT process_create( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR *cmdline, *curdir;
    UINT retval = 21;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if ((hr = method_begin( class_processW, L"Create", out, &out_params )) != S_OK) return hr;
    cmdline = get_param_string( in, L"CommandLine" );
    curdir = get_param_string( in, L"CurrentDirectory" );
    if (cmdline && cmdline[0])
    {
        memset( &si, 0, sizeof(si) );
        si.cb = sizeof(si);
        if (CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, curdir, &si, &pi ))
        {
            retval = 0;
            if (out_params)
            {
                VARIANT var;
                V_VT( &var ) = VT_I4;
                V_I4( &var ) = pi.dwProcessId;
                hr = IWbemClassObject_Put( out_params, L"ProcessId", 0, &var, CIM_UINT32 );
            }
            CloseHandle( pi.hThread );
            CloseHandle( pi.hProcess );
        }
        else retval = map_process_error( GetLastError() );
    }
    heap_free( cmdline );
    heap_free( curdir );
    return method_end( hr, obj, out_params, retval, out );
}

HRESULT process_terminate( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    LONG reason = 0;
    HANDLE process;
    UINT retval;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if ((hr = method_begin( class_processW, L"Terminate", out, &out_params )) != S_OK) return hr;
    get_param_int( in, L"Reason", &reason );
    if (!(process = OpenProcess( PROCESS_TERMINATE, FALSE, get_object_pid( obj ) )))
        retval = map_process_error( GetLastError() );
    else
    {
        retval = TerminateProcess( process, reason ) ? 0 : map_process_error( GetLastError() );
        CloseHandle( process );
    }
    return method_end( hr, obj, out_params, retval, out );
}

HRESULT process_get_owner_sid( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    HANDLE process, token = NULL;
    TOKEN_USER *user = NULL;
    WCHAR *sid = NULL;
    DWORD size = 0;
    UINT retval = 8;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if ((hr = method_begin( class_processW, L"GetOwnerSid", out, &out_params )) != S_OK) return hr;
    if (!(process = OpenProcess( PROCESS_QUERY_INFORMATION, FALSE, get_object_pid( obj ) )))
        retval = map_process_error( GetLastError() );
    else
    {
        if (!OpenProcessToken( process, TOKEN_QUERY, &token )) retval = map_process_error( GetLastError() );
        else
        {
            GetTokenInformation( token, TokenUser, NULL, 0, &size );
            if (size && (user = heap_alloc( size )) && GetTokenInformation( token, TokenUser, user, size, &size ) &&
                ConvertSidToStringSidW( user->User.Sid, &sid ))
                retval = 0;
            CloseHandle( token );
        }
        CloseHandle( process );
    }
    if (!retval && out_params)
    {
        VARIANT var;
        V_VT( &var ) = VT_BSTR;
        V_BSTR( &var ) = SysAllocString( sid );
        hr = IWbemClassObject_Put( out_params, L"Sid", 0, &var, CIM_STRING );
        VariantClear( &var );
    }
    if (sid) LocalFree( sid );
    heap_free( user );
    return method_end( hr, obj, out_params, retval, out );
}

HRESULT process_set_priority( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    LONG priority;
    HANDLE process;
    UINT retval = 21;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if ((hr = method_begin( class_processW, L"SetPriority", out, &out_params )) != S_OK) return hr;
    if (get_param_int( in, L"Priority", &priority ))
    {
        if (!(process = OpenProcess( PROCESS_SET_INFORMATION, FALSE, get_object_pid( obj ) )))
            retval = map_process_error( GetLastError() );
        else
        {
            retval = SetPriorityClass( process, priority ) ? 0 : map_process_error( GetLastError() );
            CloseHandle( process );
        }
    }
    return method_end( hr, obj, out_params, retval, out );
}

HRESULT process_attach_debugger( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    HRESULT hr;

    FIXME("%p, %p, %p: starting the registered debugger is not supported\n", obj, in, out);

    if ((hr = method_begin( class_processW, L"AttachDebugger", out, &out_params )) != S_OK) return hr;
    return method_end( hr, obj, out_params, 8, out );
}
#endif

#ifdef __REACTOS__
/* Win32_OperatingSystem.Reboot/Shutdown/Win32Shutdown: the caller's own token decides (SeShutdownPrivilege
 * must be enabled by the caller, as on Windows); ReturnValue is the Win32 error */
static HRESULT os_exit_windows( IWbemClassObject *obj, const WCHAR *method, UINT flags, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    UINT retval;
    HRESULT hr;

    if ((hr = method_begin( L"Win32_OperatingSystem", method, out, &out_params )) != S_OK) return hr;
    retval = ExitWindowsEx( flags, 0 ) ? 0 : GetLastError();
    return method_end( hr, obj, out_params, retval, out );
}

HRESULT os_reboot( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    TRACE("%p, %p, %p\n", obj, in, out);
    return os_exit_windows( obj, L"Reboot", EWX_REBOOT, out );
}

HRESULT os_shutdown( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    TRACE("%p, %p, %p\n", obj, in, out);
    return os_exit_windows( obj, L"Shutdown", EWX_SHUTDOWN, out );
}

HRESULT os_win32shutdown( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    LONG flags = 0;
    UINT ewx;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if (!get_param_int( in, L"Flags", &flags ) || (flags & ~0x0f))
    {
        /* 0 log off, 1 shut down, 2 reboot, 8 power off; 4 = force */
        if ((hr = method_begin( L"Win32_OperatingSystem", L"Win32Shutdown", out, &out_params )) != S_OK) return hr;
        return method_end( hr, obj, out_params, ERROR_INVALID_PARAMETER, out );
    }
    if (flags & 8) ewx = EWX_POWEROFF;
    else if (flags & 2) ewx = EWX_REBOOT;
    else if (flags & 1) ewx = EWX_SHUTDOWN;
    else ewx = EWX_LOGOFF;
    if (flags & 4) ewx |= EWX_FORCE;
    return os_exit_windows( obj, L"Win32Shutdown", ewx, out );
}
#endif
