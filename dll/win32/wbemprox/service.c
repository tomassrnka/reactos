/*
 * Win32_Service methods implementation
 *
 * Copyright 2012 Hans Leidekker for CodeWeavers
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
#include "oleauto.h"
#endif
#include "winsvc.h"

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

static UINT map_error( DWORD error )
{
    switch (error)
    {
    case ERROR_SUCCESS:       return 0;
    case ERROR_ACCESS_DENIED: return 2;
    case ERROR_DEPENDENT_SERVICES_RUNNING: return 3;
    case ERROR_INVALID_SERVICE_CONTROL:    return 4;
    case ERROR_SERVICE_CANNOT_ACCEPT_CTRL: return 5;
    case ERROR_SERVICE_NOT_ACTIVE:         return 6;
    case ERROR_SERVICE_REQUEST_TIMEOUT:    return 7;
    case ERROR_SERVICE_ALREADY_RUNNING:    return 10;
    default:
        WARN("unknown error %u\n", error);
        break;
    }
    return 8;
}

static HRESULT control_service( const WCHAR *name, DWORD control, VARIANT *retval )
{
    SC_HANDLE manager, service = NULL;
    SERVICE_STATUS status;
    UINT error = 0;

    if (!(manager = OpenSCManagerW( NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE )))
    {
        error = map_error( GetLastError() );
        goto done;
    }
    if (!(service = OpenServiceW( manager, name, SERVICE_STOP|SERVICE_START|SERVICE_PAUSE_CONTINUE )))
    {
        error = map_error( GetLastError() );
        goto done;
    }
    if (!ControlService( service, control, &status )) error = map_error( GetLastError() );
    CloseServiceHandle( service );

done:
    set_variant( VT_UI4, error, NULL, retval );
    if (manager) CloseServiceHandle( manager );
    return S_OK;
}

HRESULT service_pause_service( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    VARIANT name, retval;
    IWbemClassObject *sig, *out_params = NULL;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    hr = IWbemClassObject_Get( obj, prop_nameW, 0, &name, NULL, NULL );
    if (hr != S_OK) return hr;

    hr = create_signature( class_serviceW, method_pauseserviceW, PARAM_OUT, &sig );
    if (hr != S_OK)
    {
        VariantClear( &name );
        return hr;
    }
    if (out)
    {
        hr = IWbemClassObject_SpawnInstance( sig, 0, &out_params );
        if (hr != S_OK)
        {
            VariantClear( &name );
            IWbemClassObject_Release( sig );
            return hr;
        }
    }
    hr = control_service( V_BSTR(&name), SERVICE_CONTROL_PAUSE, &retval );
    if (hr != S_OK) goto done;

    if (out_params)
        hr = IWbemClassObject_Put( out_params, param_returnvalueW, 0, &retval, CIM_UINT32 );

done:
    VariantClear( &name );
    IWbemClassObject_Release( sig );
    if (hr == S_OK && out)
    {
        *out = out_params;
        IWbemClassObject_AddRef( out_params );
    }
    if (out_params) IWbemClassObject_Release( out_params );
    return hr;
}

HRESULT service_resume_service( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    VARIANT name, retval;
    IWbemClassObject *sig, *out_params = NULL;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    hr = IWbemClassObject_Get( obj, prop_nameW, 0, &name, NULL, NULL );
    if (hr != S_OK) return hr;

    hr = create_signature( class_serviceW, method_resumeserviceW, PARAM_OUT, &sig );
    if (hr != S_OK)
    {
        VariantClear( &name );
        return hr;
    }
    if (out)
    {
        hr = IWbemClassObject_SpawnInstance( sig, 0, &out_params );
        if (hr != S_OK)
        {
            VariantClear( &name );
            IWbemClassObject_Release( sig );
            return hr;
        }
    }
    hr = control_service( V_BSTR(&name), SERVICE_CONTROL_CONTINUE, &retval );
    if (hr != S_OK) goto done;

    if (out_params)
        hr = IWbemClassObject_Put( out_params, param_returnvalueW, 0, &retval, CIM_UINT32 );

done:
    VariantClear( &name );
    IWbemClassObject_Release( sig );
    if (hr == S_OK && out)
    {
        *out = out_params;
        IWbemClassObject_AddRef( out_params );
    }
    if (out_params) IWbemClassObject_Release( out_params );
    return hr;
}

static HRESULT start_service( const WCHAR *name, VARIANT *retval )
{
    SC_HANDLE manager, service = NULL;
    UINT error = 0;

    if (!(manager = OpenSCManagerW( NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE )))
    {
        error = map_error( GetLastError() );
        goto done;
    }
    if (!(service = OpenServiceW( manager, name, SERVICE_START )))
    {
        error = map_error( GetLastError() );
        goto done;
    }
    if (!StartServiceW( service, 0, NULL )) error = map_error( GetLastError() );
    CloseServiceHandle( service );

done:
    set_variant( VT_UI4, error, NULL, retval );
    if (manager) CloseServiceHandle( manager );
    return S_OK;
}

HRESULT service_start_service( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    VARIANT name, retval;
    IWbemClassObject *sig, *out_params = NULL;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    hr = IWbemClassObject_Get( obj, prop_nameW, 0, &name, NULL, NULL );
    if (hr != S_OK) return hr;

    hr = create_signature( class_serviceW, method_startserviceW, PARAM_OUT, &sig );
    if (hr != S_OK)
    {
        VariantClear( &name );
        return hr;
    }
    if (out)
    {
        hr = IWbemClassObject_SpawnInstance( sig, 0, &out_params );
        if (hr != S_OK)
        {
            VariantClear( &name );
            IWbemClassObject_Release( sig );
            return hr;
        }
    }
    hr = start_service( V_BSTR(&name), &retval );
    if (hr != S_OK) goto done;

    if (out_params)
        hr = IWbemClassObject_Put( out_params, param_returnvalueW, 0, &retval, CIM_UINT32 );

done:
    VariantClear( &name );
    IWbemClassObject_Release( sig );
    if (hr == S_OK && out)
    {
        *out = out_params;
        IWbemClassObject_AddRef( out_params );
    }
    if (out_params) IWbemClassObject_Release( out_params );
    return hr;
}

HRESULT service_stop_service( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    VARIANT name, retval;
    IWbemClassObject *sig, *out_params = NULL;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    hr = IWbemClassObject_Get( obj, prop_nameW, 0, &name, NULL, NULL );
    if (hr != S_OK) return hr;

    hr = create_signature( class_serviceW, method_stopserviceW, PARAM_OUT, &sig );
    if (hr != S_OK)
    {
        VariantClear( &name );
        return hr;
    }
    if (out)
    {
        hr = IWbemClassObject_SpawnInstance( sig, 0, &out_params );
        if (hr != S_OK)
        {
            VariantClear( &name );
            IWbemClassObject_Release( sig );
            return hr;
        }
    }
    hr = control_service( V_BSTR(&name), SERVICE_CONTROL_STOP, &retval );
    if (hr != S_OK) goto done;

    if (out_params)
        hr = IWbemClassObject_Put( out_params, param_returnvalueW, 0, &retval, CIM_UINT32 );

done:
    VariantClear( &name );
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
static WCHAR *get_string_param( IWbemClassObject *in, const WCHAR *name )
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

static BOOL get_int_param( IWbemClassObject *in, const WCHAR *name, LONG *value )
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

/* string array parameter -> double-null-terminated list; *present FALSE when the parameter is NULL */
static WCHAR *get_multisz_param( IWbemClassObject *in, const WCHAR *name, BOOL *present )
{
    VARIANT var;
    WCHAR *ret = NULL, *p;
    LONG lb, ub, i;
    SIZE_T len = 1;

    *present = FALSE;
    if (!in) return NULL;
    VariantInit( &var );
    if (IWbemClassObject_Get( in, name, 0, &var, NULL, NULL ) != S_OK || V_VT( &var ) != (VT_BSTR|VT_ARRAY) ||
        !V_ARRAY( &var ) || SafeArrayGetLBound( V_ARRAY( &var ), 1, &lb ) || SafeArrayGetUBound( V_ARRAY( &var ), 1, &ub ))
        goto done;
    *present = TRUE;
    for (i = lb; i <= ub; i++)
    {
        BSTR str;
        if (SafeArrayGetElement( V_ARRAY( &var ), &i, &str ) == S_OK)
        {
            len += SysStringLen( str ) + 1;
            SysFreeString( str );
        }
    }
    if (len > 0x10000 || !(ret = heap_alloc( (len + 1) * sizeof(WCHAR) ))) goto done;
    p = ret;
    for (i = lb; i <= ub; i++)
    {
        BSTR str;
        if (SafeArrayGetElement( V_ARRAY( &var ), &i, &str ) == S_OK)
        {
            if (str && str[0])
            {
                lstrcpyW( p, str );
                p += lstrlenW( str ) + 1;
            }
            SysFreeString( str );
        }
    }
    *p = 0;
done:
    VariantClear( &var );
    return ret;
}

static DWORD map_start_mode( const WCHAR *mode )
{
    static const struct { const WCHAR *name; DWORD type; } modes[] =
    {
        { L"Boot", SERVICE_BOOT_START }, { L"System", SERVICE_SYSTEM_START }, { L"Automatic", SERVICE_AUTO_START },
        { L"Manual", SERVICE_DEMAND_START }, { L"Disabled", SERVICE_DISABLED },
    };
    UINT i;
    for (i = 0; i < ARRAY_SIZE(modes); i++) if (!wcsicmp( modes[i].name, mode )) return modes[i].type;
    return ~0u;
}

/* open the service of the instance for a configuration change; the SCM checks the caller's access */
static SC_HANDLE open_service_object( IWbemClassObject *obj, DWORD access, SC_HANDLE *manager, UINT *error )
{
    VARIANT name;
    SC_HANDLE service = NULL;

    *manager = NULL;
    VariantInit( &name );
    if (IWbemClassObject_Get( obj, prop_nameW, 0, &name, NULL, NULL ) != S_OK || V_VT( &name ) != VT_BSTR)
    {
        *error = 8;
        VariantClear( &name );
        return NULL;
    }
    if (!(*manager = OpenSCManagerW( NULL, NULL, SC_MANAGER_CONNECT ))) *error = map_error( GetLastError() );
    else if (!(service = OpenServiceW( *manager, V_BSTR( &name ), access ))) *error = map_error( GetLastError() );
    VariantClear( &name );
    return service;
}

HRESULT service_change( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    SC_HANDLE manager = NULL, service;
    WCHAR *display, *group, *path, *startname, *password, *mode, *groupdeps, *deps;
    DWORD type = SERVICE_NO_CHANGE, start = SERVICE_NO_CHANGE, errctl = SERVICE_NO_CHANGE;
    BOOL has_groupdeps, has_deps, ok = TRUE;
    LONG value;
    UINT error = 0;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if ((hr = method_begin( class_serviceW, L"Change", out, &out_params )) != S_OK) return hr;
    display = get_string_param( in, L"DisplayName" );
    group = get_string_param( in, L"LoadOrderGroup" );
    path = get_string_param( in, L"PathName" );
    startname = get_string_param( in, L"StartName" );
    password = get_string_param( in, L"StartPassword" );
    mode = get_string_param( in, L"StartMode" );
    groupdeps = get_multisz_param( in, L"LoadOrderGroupDependencies", &has_groupdeps );
    deps = get_multisz_param( in, L"ServiceDependencies", &has_deps );

    if (get_int_param( in, L"ServiceType", &value )) type = (DWORD)value;
    if (get_int_param( in, L"ErrorControl", &value )) errctl = (DWORD)value;
    if (get_int_param( in, L"DesktopInteract", &value ) && value)
    {
        /* only meaningful for an own/shared process service */
        if (type == SERVICE_NO_CHANGE) ok = FALSE;
        else type |= SERVICE_INTERACTIVE_PROCESS;
    }
    if (mode && (start = map_start_mode( mode )) == ~0u) ok = FALSE;
    if ((has_groupdeps && !groupdeps) || (has_deps && !deps)) ok = FALSE;
    if (has_groupdeps && has_deps) ok = FALSE; /* one dependency list per call (both share lpDependencies) */

    if (!ok) error = 21;
    else if ((service = open_service_object( obj, SERVICE_CHANGE_CONFIG, &manager, &error )))
    {
        if (!ChangeServiceConfigW( service, type, start, errctl, path, group, NULL,
                                   has_deps ? deps : (has_groupdeps ? groupdeps : NULL),
                                   startname, password, display ))
            error = map_error( GetLastError() );
        CloseServiceHandle( service );
    }
    if (manager) CloseServiceHandle( manager );
    if (password) SecureZeroMemory( password, lstrlenW( password ) * sizeof(WCHAR) );
    heap_free( display );
    heap_free( group );
    heap_free( path );
    heap_free( startname );
    heap_free( password );
    heap_free( mode );
    heap_free( groupdeps );
    heap_free( deps );
    return method_end( hr, obj, out_params, error, out );
}

HRESULT service_change_start_mode( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    SC_HANDLE manager = NULL, service;
    WCHAR *mode;
    DWORD start;
    UINT error = 0;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if ((hr = method_begin( class_serviceW, L"ChangeStartMode", out, &out_params )) != S_OK) return hr;
    mode = get_string_param( in, L"StartMode" );
    if (!mode || (start = map_start_mode( mode )) == ~0u) error = 21;
    else if ((service = open_service_object( obj, SERVICE_CHANGE_CONFIG, &manager, &error )))
    {
        if (!ChangeServiceConfigW( service, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL ))
            error = map_error( GetLastError() );
        CloseServiceHandle( service );
    }
    if (manager) CloseServiceHandle( manager );
    heap_free( mode );
    return method_end( hr, obj, out_params, error, out );
}

HRESULT service_delete( IWbemClassObject *obj, IWbemClassObject *in, IWbemClassObject **out )
{
    IWbemClassObject *out_params;
    SC_HANDLE manager = NULL, service;
    UINT error = 0;
    HRESULT hr;

    TRACE("%p, %p, %p\n", obj, in, out);

    if ((hr = method_begin( class_serviceW, L"Delete", out, &out_params )) != S_OK) return hr;
    if ((service = open_service_object( obj, DELETE, &manager, &error )))
    {
        if (!DeleteService( service )) error = map_error( GetLastError() );
        CloseServiceHandle( service );
    }
    if (manager) CloseServiceHandle( manager );
    return method_end( hr, obj, out_params, error, out );
}
#endif
