/*
 * PROJECT:     ReactOS Management Infrastructure client (mi.dll)
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     MI_Session and MI_Operation over IWbemServices
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Every operation runs on its own worker thread (in the COM multithreaded
 * apartment): it connects to the namespace with IWbemLocator, runs the WMI call
 * and converts each IWbemClassObject into an MI_Instance / MI_Class. Results are
 * then delivered either through the caller's MI_OperationCallbacks (push) or
 * from MI_Operation_GetInstance/GetClass (pull), as the MI API specifies.
 */

#include "mi_private.h"
#include <wchar.h>

WINE_DEFAULT_DEBUG_CHANNEL(mi);

#ifndef WBEM_FLAG_DEEP
#define WBEM_FLAG_DEEP 0
#endif

#define MI_SESSION_MAGIC 0x53534d49 /* "IMSS" */
#define MI_OPERATION_MAGIC 0x504f4d49 /* "IMOP" */

/* wbemprox keeps one process-global table per class and refills it on every query: the WMI phase
 * of every operation (connect, query, drain the enumerator, read classes) runs under this lock, so
 * a second operation can never refill a table another one still has row indices into. No client
 * callback ever runs while it is held. */
static CRITICAL_SECTION wmi_cs;

void mi_init_locks(void) { InitializeCriticalSection(&wmi_cs); }
void mi_free_locks(void) { DeleteCriticalSection(&wmi_cs); }

struct mi_session
{
    LONG magic;
    LONG refs;
    LONG closed;
    WCHAR *protocol;
    WCHAR *destination;
    MI_Session client;          /* copy of the caller's handle for MI_Operation_GetSession */
};

enum op_kind
{
    OP_GET_INSTANCE,
    OP_ENUMERATE,
    OP_QUERY,
    OP_GET_CLASS,
    OP_ENUMERATE_CLASSES,
    OP_INVOKE,
    OP_TEST_CONNECTION,
    OP_FAILED,
};

struct mi_operation
{
    LONG magic;
    LONG refs;
    enum op_kind kind;
    struct mi_session *session;
    MI_Operation *client;
    MI_Uint32 flags;
    BOOL push;
    MI_OperationCallbacks callbacks;
    WCHAR *nameSpace;           /* as given by the caller, '/' separators */
    WCHAR *class_name;
    WCHAR *method;
    WCHAR *query;
    BOOL keys_only;
    struct mi_instance *inbound;
    struct mi_instance *params;

    CRITICAL_SECTION cs;
    HANDLE ready;               /* signalled when a result is added or the operation completes */
    HANDLE ack;                 /* manual-ack pacing for push delivery */
    HANDLE thread;
    DWORD thread_id;
    HMODULE module;             /* reference on mi.dll held for the life of the worker thread */
    BOOL cancelled;
    BOOL closed;
    BOOL done;
    BOOL final_delivered;
    UINT count, size, next;
    struct mi_instance **instances;
    MI_Class **classes;
    MI_Result result;
    WCHAR *error_message;
    struct mi_instance *error_details;
};

MI_Result hresult_to_mi(HRESULT hr)
{
    switch (hr)
    {
    case S_OK:
    case WBEM_S_FALSE: return MI_RESULT_OK;
    case WBEM_E_ACCESS_DENIED:
    case E_ACCESSDENIED: return MI_RESULT_ACCESS_DENIED;
    case WBEM_E_INVALID_NAMESPACE: return MI_RESULT_INVALID_NAMESPACE;
    case WBEM_E_INVALID_PARAMETER:
    case WBEM_E_INVALID_OBJECT_PATH:
    case E_INVALIDARG: return MI_RESULT_INVALID_PARAMETER;
    case WBEM_E_INVALID_CLASS: return MI_RESULT_INVALID_CLASS;
    case WBEM_E_NOT_FOUND: return MI_RESULT_NOT_FOUND;
    case WBEM_E_NOT_SUPPORTED:
    case WBEM_E_PROVIDER_NOT_CAPABLE:
    case E_NOTIMPL: return MI_RESULT_NOT_SUPPORTED;
    case WBEM_E_ALREADY_EXISTS: return MI_RESULT_ALREADY_EXISTS;
    case WBEM_E_TYPE_MISMATCH: return MI_RESULT_TYPE_MISMATCH;
    case WBEM_E_INVALID_QUERY_TYPE: return MI_RESULT_QUERY_LANGUAGE_NOT_SUPPORTED;
    case WBEM_E_INVALID_QUERY: return MI_RESULT_INVALID_QUERY;
    case WBEM_E_INVALID_METHOD: return MI_RESULT_METHOD_NOT_FOUND;
    case WBEM_E_METHOD_NOT_IMPLEMENTED:
    case WBEM_E_METHOD_DISABLED: return MI_RESULT_METHOD_NOT_AVAILABLE;
    case WBEM_E_OUT_OF_MEMORY:
    case E_OUTOFMEMORY: return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    case WBEM_E_SHUTTING_DOWN: return MI_RESULT_SERVER_IS_SHUTTING_DOWN;
    default: return MI_RESULT_FAILED;
    }
}

/* helpers */

static WCHAR *local_server_name(void)
{
    WCHAR buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAYSIZE(buf);
    if (!GetComputerNameW(buf, &len)) return mi_strdup(L"localhost");
    return mi_strdup(buf);
}

static BOOL is_local_destination(const WCHAR *dest)
{
    WCHAR buf[256];
    DWORD len;

    if (!dest || !dest[0] || !wcscmp(dest, L".") || !_wcsicmp(dest, L"localhost") ||
        !wcscmp(dest, L"127.0.0.1") || !wcscmp(dest, L"::1") || !wcscmp(dest, L"[::1]"))
        return TRUE;
    len = ARRAYSIZE(buf);
    if (GetComputerNameW(buf, &len) && !_wcsicmp(dest, buf)) return TRUE;
    len = ARRAYSIZE(buf);
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &len) && !_wcsicmp(dest, buf)) return TRUE;
    len = ARRAYSIZE(buf);
    if (GetComputerNameExW(ComputerNameDnsFullyQualified, buf, &len) && !_wcsicmp(dest, buf)) return TRUE;
    return FALSE;
}

/* namespace as MI reports it: forward slashes */
static WCHAR *mi_namespace(const WCHAR *ns)
{
    WCHAR *ret, *p;
    if (!ns || !ns[0]) ns = L"root/cimv2";
    while (*ns == '/' || *ns == '\\') ns++;
    if (!(ret = mi_strdup(ns))) return NULL;
    for (p = ret; *p; p++) if (*p == '\\') *p = '/';
    return ret;
}

/* namespace as IWbemLocator wants it */
static BSTR wmi_namespace(const WCHAR *ns)
{
    BSTR ret;
    WCHAR *p;
    if (!ns || !ns[0]) ns = L"root\\cimv2";
    while (*ns == '/' || *ns == '\\') ns++;
    if (!(ret = SysAllocString(ns))) return NULL;
    for (p = ret; *p; p++) if (*p == '/') *p = '\\';
    return ret;
}

/* CIM/VARIANT -> MI conversion */

static MI_Type cimtype_to_mi(CIMTYPE type)
{
    MI_Type ret;
    switch (type & ~CIM_FLAG_ARRAY)
    {
    case CIM_SINT8: ret = MI_SINT8; break;
    case CIM_UINT8: ret = MI_UINT8; break;
    case CIM_SINT16: ret = MI_SINT16; break;
    case CIM_UINT16: ret = MI_UINT16; break;
    case CIM_SINT32: ret = MI_SINT32; break;
    case CIM_UINT32: ret = MI_UINT32; break;
    case CIM_SINT64: ret = MI_SINT64; break;
    case CIM_UINT64: ret = MI_UINT64; break;
    case CIM_REAL32: ret = MI_REAL32; break;
    case CIM_REAL64: ret = MI_REAL64; break;
    case CIM_BOOLEAN: ret = MI_BOOLEAN; break;
    case CIM_DATETIME: ret = MI_DATETIME; break;
    case CIM_REFERENCE: ret = MI_REFERENCE; break;
    case CIM_CHAR16: ret = MI_CHAR16; break;
    case CIM_OBJECT: ret = MI_INSTANCE; break;
    case CIM_STRING:
    default: ret = MI_STRING; break;
    }
    if (type & CIM_FLAG_ARRAY) ret |= MI_ARRAY;
    return ret;
}

static MI_Type vartype_to_mi(VARTYPE vt)
{
    MI_Type ret;
    switch (vt & ~VT_ARRAY)
    {
    case VT_BOOL: ret = MI_BOOLEAN; break;
    case VT_UI1: ret = MI_UINT8; break;
    case VT_I1: ret = MI_SINT8; break;
    case VT_I2: ret = MI_SINT16; break;
    case VT_UI2: ret = MI_UINT16; break;
    case VT_I4: case VT_INT: ret = MI_SINT32; break;
    case VT_UI4: case VT_UINT: ret = MI_UINT32; break;
    case VT_I8: ret = MI_SINT64; break;
    case VT_UI8: ret = MI_UINT64; break;
    case VT_R4: ret = MI_REAL32; break;
    case VT_R8: ret = MI_REAL64; break;
    case VT_UNKNOWN: case VT_DISPATCH: ret = MI_INSTANCE; break;
    default: ret = MI_STRING; break;
    }
    if (vt & VT_ARRAY) ret |= MI_ARRAY;
    return ret;
}

static UINT parse_digits(const WCHAR **p, int count)
{
    UINT ret = 0;
    while (count--)
    {
        if (**p >= '0' && **p <= '9') ret = ret * 10 + (**p - '0');
        else ret = ret * 10; /* '*' wildcard */
        if (**p) (*p)++;
    }
    return ret;
}

/* DMTF datetime "yyyymmddHHMMSS.mmmmmmsUUU" or interval "ddddddddHHMMSS.mmmmmm:000" */
static BOOL parse_datetime(const WCHAR *str, MI_Datetime *dt)
{
    const WCHAR *p = str;

    memset(dt, 0, sizeof(*dt));
    if (!str || wcslen(str) < 25) return FALSE;
    if (str[21] == ':')
    {
        dt->isTimestamp = MI_FALSE;
        dt->u.interval.days = parse_digits(&p, 8);
        dt->u.interval.hours = parse_digits(&p, 2);
        dt->u.interval.minutes = parse_digits(&p, 2);
        dt->u.interval.seconds = parse_digits(&p, 2);
        p++;
        dt->u.interval.microseconds = parse_digits(&p, 6);
        return TRUE;
    }
    dt->isTimestamp = MI_TRUE;
    dt->u.timestamp.year = parse_digits(&p, 4);
    dt->u.timestamp.month = parse_digits(&p, 2);
    dt->u.timestamp.day = parse_digits(&p, 2);
    dt->u.timestamp.hour = parse_digits(&p, 2);
    dt->u.timestamp.minute = parse_digits(&p, 2);
    dt->u.timestamp.second = parse_digits(&p, 2);
    p++;
    dt->u.timestamp.microseconds = parse_digits(&p, 6);
    if (*p == '+' || *p == '-')
    {
        int sign = (*p == '-') ? -1 : 1;
        p++;
        dt->u.timestamp.utc = sign * (int)parse_digits(&p, 3);
    }
    return TRUE;
}

static void format_datetime(const MI_Datetime *dt, WCHAR *buf, size_t len)
{
    if (dt->isTimestamp)
    {
        int utc = dt->u.timestamp.utc;
        _snwprintf(buf, len, L"%04u%02u%02u%02u%02u%02u.%06u%c%03d",
                   dt->u.timestamp.year, dt->u.timestamp.month, dt->u.timestamp.day, dt->u.timestamp.hour,
                   dt->u.timestamp.minute, dt->u.timestamp.second, dt->u.timestamp.microseconds,
                   utc < 0 ? '-' : '+', utc < 0 ? -utc : utc);
    }
    else
    {
        _snwprintf(buf, len, L"%08u%02u%02u%02u.%06u:000",
                   dt->u.interval.days, dt->u.interval.hours, dt->u.interval.minutes,
                   dt->u.interval.seconds, dt->u.interval.microseconds);
    }
    buf[len - 1] = 0;
}

static ULONGLONG parse_u64(const WCHAR *str)
{
    ULONGLONG ret = 0;
    if (!str) return 0;
    while (*str == ' ') str++;
    while (*str >= '0' && *str <= '9') ret = ret * 10 + (*str++ - '0');
    return ret;
}

static LONGLONG parse_s64(const WCHAR *str)
{
    BOOL neg = FALSE;
    if (!str) return 0;
    while (*str == ' ') str++;
    if (*str == '-') { neg = TRUE; str++; }
    else if (*str == '+') str++;
    return neg ? -(LONGLONG)parse_u64(str) : (LONGLONG)parse_u64(str);
}

static struct mi_instance *convert_object(IWbemClassObject *obj, const WCHAR *ns, const WCHAR *server, BOOL keys_from_path);
static struct mi_instance *reference_to_instance(const WCHAR *path, const WCHAR *ns, const WCHAR *server);

/* one scalar VARIANT -> MI value of the given (non-array) type; *is_null set for VT_NULL/VT_EMPTY */
static MI_Result variant_to_value(const VARIANT *var, MI_Type type, MI_Value *value, BOOL *is_null,
                                  const WCHAR *ns, const WCHAR *server)
{
    VARIANT tmp;
    HRESULT hr;

    memset(value, 0, sizeof(*value));
    *is_null = FALSE;
    if (V_VT(var) == VT_NULL || V_VT(var) == VT_EMPTY) { *is_null = TRUE; return MI_RESULT_OK; }

    switch (type)
    {
    case MI_STRING:
        if (V_VT(var) == VT_BSTR) { value->string = mi_strdup(V_BSTR(var) ? V_BSTR(var) : L""); return MI_RESULT_OK; }
        VariantInit(&tmp);
        if (FAILED(VariantChangeType(&tmp, (VARIANT *)var, 0, VT_BSTR))) { *is_null = TRUE; return MI_RESULT_OK; }
        value->string = mi_strdup(V_BSTR(&tmp));
        VariantClear(&tmp);
        return MI_RESULT_OK;

    case MI_DATETIME:
        if (V_VT(var) != VT_BSTR || !parse_datetime(V_BSTR(var), &value->datetime)) *is_null = TRUE;
        return MI_RESULT_OK;

    case MI_UINT64:
        if (V_VT(var) == VT_BSTR) value->uint64 = parse_u64(V_BSTR(var));
        else
        {
            VariantInit(&tmp);
            if (FAILED(VariantChangeType(&tmp, (VARIANT *)var, 0, VT_UI8))) { *is_null = TRUE; return MI_RESULT_OK; }
            value->uint64 = V_UI8(&tmp);
        }
        return MI_RESULT_OK;

    case MI_SINT64:
        if (V_VT(var) == VT_BSTR) value->sint64 = parse_s64(V_BSTR(var));
        else
        {
            VariantInit(&tmp);
            if (FAILED(VariantChangeType(&tmp, (VARIANT *)var, 0, VT_I8))) { *is_null = TRUE; return MI_RESULT_OK; }
            value->sint64 = V_I8(&tmp);
        }
        return MI_RESULT_OK;

    case MI_REAL32:
    case MI_REAL64:
        VariantInit(&tmp);
        if (FAILED(VariantChangeType(&tmp, (VARIANT *)var, 0, VT_R8))) { *is_null = TRUE; return MI_RESULT_OK; }
        if (type == MI_REAL32) value->real32 = (float)V_R8(&tmp);
        else value->real64 = V_R8(&tmp);
        return MI_RESULT_OK;

    case MI_BOOLEAN:
        VariantInit(&tmp);
        if (FAILED(VariantChangeType(&tmp, (VARIANT *)var, 0, VT_BOOL))) { *is_null = TRUE; return MI_RESULT_OK; }
        value->boolean = V_BOOL(&tmp) ? MI_TRUE : MI_FALSE;
        return MI_RESULT_OK;

    case MI_REFERENCE:
        if (V_VT(var) != VT_BSTR || !V_BSTR(var)) { *is_null = TRUE; return MI_RESULT_OK; }
        {
            struct mi_instance *ref = reference_to_instance(V_BSTR(var), ns, server);
            if (!ref) { *is_null = TRUE; return MI_RESULT_OK; }
            value->reference = &ref->base;
        }
        return MI_RESULT_OK;

    case MI_INSTANCE:
        if ((V_VT(var) == VT_UNKNOWN || V_VT(var) == VT_DISPATCH) && V_UNKNOWN(var))
        {
            IWbemClassObject *obj;
            struct mi_instance *inst = NULL;
            if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(var), &IID_IWbemClassObject, (void **)&obj)))
            {
                inst = convert_object(obj, ns, server, FALSE);
                IWbemClassObject_Release(obj);
            }
            if (inst) { value->instance = &inst->base; return MI_RESULT_OK; }
        }
        *is_null = TRUE;
        return MI_RESULT_OK;

    default:
        /* 8/16/32-bit integers and char16 */
        VariantInit(&tmp);
        if (V_VT(var) == VT_BSTR)
        {
            V_VT(&tmp) = VT_I8;
            V_I8(&tmp) = parse_s64(V_BSTR(var));
            if (V_BSTR(var) && V_BSTR(var)[0] != '-') V_I8(&tmp) = (LONGLONG)parse_u64(V_BSTR(var));
        }
        else if (FAILED(hr = VariantChangeType(&tmp, (VARIANT *)var, 0, VT_I8)))
        {
            *is_null = TRUE;
            return MI_RESULT_OK;
        }
        switch (type)
        {
        case MI_UINT8: value->uint8 = (MI_Uint8)V_I8(&tmp); break;
        case MI_SINT8: value->sint8 = (MI_Sint8)V_I8(&tmp); break;
        case MI_UINT16: value->uint16 = (MI_Uint16)V_I8(&tmp); break;
        case MI_SINT16: value->sint16 = (MI_Sint16)V_I8(&tmp); break;
        case MI_UINT32: value->uint32 = (MI_Uint32)V_I8(&tmp); break;
        case MI_SINT32: value->sint32 = (MI_Sint32)V_I8(&tmp); break;
        case MI_CHAR16: value->char16 = (MI_Char16)V_I8(&tmp); break;
        default: *is_null = TRUE; break;
        }
        return MI_RESULT_OK;
    }
}

static SIZE_T mi_element_size(MI_Type type)
{
    switch (type & ~MI_ARRAY)
    {
    case MI_BOOLEAN: case MI_UINT8: case MI_SINT8: return 1;
    case MI_UINT16: case MI_SINT16: case MI_CHAR16: return 2;
    case MI_UINT32: case MI_SINT32: case MI_REAL32: return 4;
    case MI_UINT64: case MI_SINT64: case MI_REAL64: return 8;
    case MI_DATETIME: return sizeof(MI_Datetime);
    default: return sizeof(void *);
    }
}

static void store_array_element(MI_Value *array, MI_Type type, ULONG i, const MI_Value *elem)
{
    BYTE *data = array->array.data;
    SIZE_T size = mi_element_size(type);
    switch (type & ~MI_ARRAY)
    {
    case MI_BOOLEAN: data[i] = elem->boolean; break;
    case MI_UINT8: case MI_SINT8: data[i] = elem->uint8; break;
    case MI_UINT16: case MI_SINT16: case MI_CHAR16: ((MI_Uint16 *)data)[i] = elem->uint16; break;
    case MI_UINT32: case MI_SINT32: case MI_REAL32: ((MI_Uint32 *)data)[i] = elem->uint32; break;
    case MI_UINT64: case MI_SINT64: case MI_REAL64: ((MI_Uint64 *)data)[i] = elem->uint64; break;
    case MI_DATETIME: memcpy(data + i * size, &elem->datetime, size); break;
    case MI_STRING: ((MI_Char **)data)[i] = elem->string; break;
    default: ((MI_Instance **)data)[i] = elem->instance; break;
    }
}

static MI_Result variant_to_mi(const VARIANT *var, MI_Type type, MI_Value *value, BOOL *is_null,
                               const WCHAR *ns, const WCHAR *server)
{
    SAFEARRAY *sa;
    VARTYPE vt;
    LONG lb, ub, i;
    SIZE_T esize;
    MI_Uint32 out, count;
    MI_Type elem_type = type & ~MI_ARRAY;
    MI_Result res = MI_RESULT_OK;
    BOOL *present;

    if (!(type & MI_ARRAY))
    {
        res = variant_to_value(var, type, value, is_null, ns, server);
        /* an out-of-memory string must not look like a valid value */
        if (type == MI_STRING && !*is_null && !value->string) *is_null = TRUE;
        return res;
    }

    memset(value, 0, sizeof(*value));
    *is_null = FALSE;
    if (!(V_VT(var) & VT_ARRAY) || !(sa = V_ARRAY(var))) { *is_null = TRUE; return MI_RESULT_OK; }
    if (FAILED(SafeArrayGetVartype(sa, &vt)) || FAILED(SafeArrayGetLBound(sa, 1, &lb)) ||
        FAILED(SafeArrayGetUBound(sa, 1, &ub)))
    {
        *is_null = TRUE;
        return MI_RESULT_OK;
    }
    if (ub < lb) return MI_RESULT_OK;
    count = (MI_Uint32)((LONGLONG)ub - lb + 1);
    esize = mi_element_size(type);
    *is_null = TRUE;
    if (count > MAXLONG || count > (~(SIZE_T)0) / esize) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    if (!(present = mi_alloc(count * sizeof(*present)))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    if (!(value->array.data = mi_alloc(esize * count))) { mi_free(present); return MI_RESULT_SERVER_LIMITS_EXCEEDED; }
    value->array.size = count;
    for (i = lb; i <= ub; i++)
    {
        VARIANT elem;
        MI_Value v;
        BOOL null;

        VariantInit(&elem);
        if (vt == VT_VARIANT)
        {
            if (FAILED(SafeArrayGetElement(sa, &i, &elem))) continue;
        }
        else
        {
            V_VT(&elem) = vt;
            if (FAILED(SafeArrayGetElement(sa, &i, &V_I8(&elem)))) { V_VT(&elem) = VT_EMPTY; continue; }
        }
        variant_to_value(&elem, elem_type, &v, &null, ns, server);
        if (!null)
        {
            store_array_element(value, type, i - lb, &v);
            present[i - lb] = TRUE;
        }
        VariantClear(&elem);
    }

    if (elem_type == MI_STRING)
    {
        /* no NULL entries: a missing string becomes "" */
        for (i = 0; i < (LONG)count && res == MI_RESULT_OK; i++)
            if (!value->stringa.data[i] && !(value->stringa.data[i] = mi_strdup(L""))) res = MI_RESULT_SERVER_LIMITS_EXCEEDED;
    }
    else if (elem_type == MI_INSTANCE || elem_type == MI_REFERENCE)
    {
        /* instance arrays cannot hold NULL either: drop the elements that did not convert */
        for (out = 0, i = 0; i < (LONG)count; i++)
        {
            if (!present[i] || !value->instancea.data[i]) continue;
            value->instancea.data[out++] = value->instancea.data[i];
        }
        for (i = out; i < (LONG)count; i++) value->instancea.data[i] = NULL;
        value->array.size = out;
    }
    mi_free(present);
    if (res != MI_RESULT_OK)
    {
        free_value(value, type);
        return res;
    }
    *is_null = FALSE;
    return MI_RESULT_OK;
}

static BSTR get_string_prop(IWbemClassObject *obj, const WCHAR *name)
{
    VARIANT var;
    BSTR ret = NULL;
    VariantInit(&var);
    if (SUCCEEDED(IWbemClassObject_Get(obj, name, 0, &var, NULL, NULL)) && V_VT(&var) == VT_BSTR)
    {
        ret = V_BSTR(&var);
        V_BSTR(&var) = NULL;
    }
    VariantClear(&var);
    return ret;
}

/* key names from a relative path "Class.Key1=\"v\",Key2=1" */
static BOOL path_has_key(const WCHAR *relpath, const WCHAR *name)
{
    const WCHAR *p;
    size_t len = wcslen(name);
    BOOL quoted = FALSE;

    if (!relpath || !(p = wcschr(relpath, '.'))) return FALSE;
    p++;
    while (*p)
    {
        if (!_wcsnicmp(p, name, len) && p[len] == '=') return TRUE;
        /* skip to the next key */
        while (*p && (quoted || *p != ','))
        {
            if (*p == '\\' && quoted && p[1]) p++;
            else if (*p == '"') quoted = !quoted;
            p++;
        }
        if (*p == ',') p++;
    }
    return FALSE;
}

static void read_qualifiers(IWbemQualifierSet *qs, struct mi_qualifier_set *set, BOOL *is_key, BOOL *is_readonly)
{
    SAFEARRAY *names = NULL;
    LONG lb, ub, i;

    if (FAILED(IWbemQualifierSet_GetNames(qs, 0, &names)) || !names) return;
    if (FAILED(SafeArrayGetLBound(names, 1, &lb)) || FAILED(SafeArrayGetUBound(names, 1, &ub)) || ub < lb) goto done;
    if (!(set->items = mi_alloc((ub - lb + 1) * sizeof(*set->items)))) goto done;
    for (i = lb; i <= ub; i++)
    {
        BSTR name = NULL;
        VARIANT var;
        LONG flavor = 0;
        BOOL null;
        struct mi_qualifier *q;

        if (FAILED(SafeArrayGetElement(names, &i, &name)) || !name) continue;
        VariantInit(&var);
        if (SUCCEEDED(IWbemQualifierSet_Get(qs, name, 0, &var, &flavor)))
        {
            q = &set->items[set->count];
            q->name = mi_strdup(name);
            q->type = vartype_to_mi(V_VT(&var));
            if (flavor & WBEM_FLAVOR_FLAG_PROPAGATE_TO_DERIVED_CLASS) q->flags |= MI_FLAG_TOSUBCLASS;
            if (flavor & WBEM_FLAVOR_NOT_OVERRIDABLE) q->flags |= MI_FLAG_DISABLEOVERRIDE;
            else q->flags |= MI_FLAG_ENABLEOVERRIDE;
            if (flavor & WBEM_FLAVOR_AMENDED) q->flags |= MI_FLAG_TRANSLATABLE;
            variant_to_mi(&var, q->type, &q->value, &null, NULL, NULL);
            if (!_wcsicmp(name, L"key") && is_key && q->type == MI_BOOLEAN && q->value.boolean) *is_key = TRUE;
            if (!_wcsicmp(name, L"read") && is_readonly && q->type == MI_BOOLEAN && q->value.boolean) *is_readonly = TRUE;
            set->count++;
        }
        VariantClear(&var);
        SysFreeString(name);
    }
done:
    SafeArrayDestroy(names);
}

static BOOL qualifier_is_true(const struct mi_qualifier_set *set, const WCHAR *name)
{
    UINT i;
    for (i = 0; i < set->count; i++)
        if (!_wcsicmp(set->items[i].name, name)) return set->items[i].type == MI_BOOLEAN && set->items[i].value.boolean;
    return FALSE;
}

/* properties of a signature object (method parameters) or class */
static void read_properties(IWbemClassObject *obj, struct mi_property **props, UINT *count, BOOL with_qualifiers,
                            MI_Uint32 base_flags, const WCHAR *skip)
{
    SAFEARRAY *names = NULL;
    LONG lb, ub, i;

    *props = NULL;
    *count = 0;
    if (!obj) return;
    if (FAILED(IWbemClassObject_GetNames(obj, NULL, WBEM_FLAG_NONSYSTEM_ONLY, NULL, &names)) || !names) return;
    if (FAILED(SafeArrayGetLBound(names, 1, &lb)) || FAILED(SafeArrayGetUBound(names, 1, &ub)) || ub < lb) goto done;
    if (!(*props = mi_alloc((ub - lb + 1) * sizeof(**props)))) goto done;
    for (i = lb; i <= ub; i++)
    {
        BSTR name = NULL;
        VARIANT var;
        CIMTYPE type = CIM_STRING;
        struct mi_property *p;
        BOOL null, key = FALSE, readonly = FALSE;

        if (FAILED(SafeArrayGetElement(names, &i, &name)) || !name) continue;
        if (skip && !_wcsicmp(name, skip)) { SysFreeString(name); continue; }
        VariantInit(&var);
        if (FAILED(IWbemClassObject_Get(obj, name, 0, &var, &type, NULL))) { SysFreeString(name); continue; }
        p = &(*props)[*count];
        p->name = mi_strdup(name);
        p->type = cimtype_to_mi(type);
        p->flags = base_flags;
        variant_to_mi(&var, p->type, &p->value, &null, NULL, NULL);
        p->has_value = !null;
        if (with_qualifiers)
        {
            IWbemQualifierSet *qs;
            if (SUCCEEDED(IWbemClassObject_GetPropertyQualifierSet(obj, name, &qs)))
            {
                read_qualifiers(qs, &p->quals, &key, &readonly);
                IWbemQualifierSet_Release(qs);
            }
            if (key) p->flags |= MI_FLAG_KEY;
            /* Windows: read-only unless the property also has the write qualifier */
            if (readonly && !qualifier_is_true(&p->quals, L"write")) p->flags |= MI_FLAG_READONLY;
        }
        (*count)++;
        VariantClear(&var);
        SysFreeString(name);
    }
done:
    SafeArrayDestroy(names);
}

static void free_property_contents(struct mi_property *p)
{
    mi_free(p->name);
    mi_free(p->refclass);
    if (p->has_value) free_value(&p->value, p->type);
    qualifier_set_free(&p->quals);
}

static void read_methods(IWbemClassObject *obj, struct mi_class_data *cls)
{
    BSTR name;
    IWbemClassObject *in, *out;
    UINT size = 0;

    if (FAILED(IWbemClassObject_BeginMethodEnumeration(obj, 0))) return;
    for (;;)
    {
        struct mi_method *m;
        struct mi_property *in_props, *out_props;
        UINT in_count, out_count, i;
        IWbemQualifierSet *qs;

        name = NULL;
        in = out = NULL;
        if (IWbemClassObject_NextMethod(obj, 0, &name, &in, &out) != S_OK || !name) break;
        if (cls->method_count == size)
        {
            UINT new_size = size ? size * 2 : 8;
            struct mi_method *tmp = mi_alloc(new_size * sizeof(*tmp));
            if (!tmp)
            {
                if (in) IWbemClassObject_Release(in);
                if (out) IWbemClassObject_Release(out);
                SysFreeString(name);
                break;
            }
            if (cls->methods) memcpy(tmp, cls->methods, cls->method_count * sizeof(*tmp));
            mi_free(cls->methods);
            cls->methods = tmp;
            size = new_size;
        }
        m = &cls->methods[cls->method_count++];
        if (!(m->name = mi_strdup(name)))
        {
            cls->method_count--;
            if (in) IWbemClassObject_Release(in);
            if (out) IWbemClassObject_Release(out);
            SysFreeString(name);
            break;
        }
        m->return_type = MI_UINT32;
        if (SUCCEEDED(IWbemClassObject_GetMethodQualifierSet(obj, name, &qs)))
        {
            read_qualifiers(qs, &m->quals, NULL, NULL);
            IWbemQualifierSet_Release(qs);
        }
        read_properties(in, &in_props, &in_count, TRUE, MI_FLAG_PARAMETER | MI_FLAG_IN, NULL);
        read_properties(out, &out_props, &out_count, TRUE, MI_FLAG_PARAMETER | MI_FLAG_OUT, L"ReturnValue");
        if (out)
        {
            CIMTYPE rt;
            if (SUCCEEDED(IWbemClassObject_Get(out, L"ReturnValue", 0, NULL, &rt, NULL))) m->return_type = cimtype_to_mi(rt);
        }
        if ((m->params = mi_alloc((in_count + out_count + 1) * sizeof(*m->params))))
        {
            for (i = 0; i < in_count; i++) m->params[m->count++] = in_props[i];
            for (i = 0; i < out_count; i++)
            {
                UINT j;
                for (j = 0; j < in_count; j++)
                    if (!_wcsicmp(m->params[j].name, out_props[i].name)) break;
                if (j < in_count)
                {
                    m->params[j].flags |= MI_FLAG_OUT;
                    mi_free(out_props[i].name);
                    if (out_props[i].has_value) free_value(&out_props[i].value, out_props[i].type);
                    qualifier_set_free(&out_props[i].quals);
                }
                else m->params[m->count++] = out_props[i];
            }
        }
        else
        {
            /* no room for the merged parameter list: free what read_properties built */
            for (i = 0; i < in_count; i++) free_property_contents(&in_props[i]);
            for (i = 0; i < out_count; i++) free_property_contents(&out_props[i]);
        }
        mi_free(in_props);
        mi_free(out_props);
        if (in) IWbemClassObject_Release(in);
        if (out) IWbemClassObject_Release(out);
        SysFreeString(name);
    }
    IWbemClassObject_EndMethodEnumeration(obj);
}

/* the class objects an operation has read, by name; the cache owns one reference to each */
struct class_cache
{
    UINT count, size;
    struct mi_class_data **items;
};

#define MAX_CLASS_DEPTH 16

static struct mi_class_data *get_class_data(struct class_cache *cache, IWbemServices *services, const WCHAR *name,
                                            const WCHAR *ns, const WCHAR *server, UINT depth);

static struct mi_class_data *convert_class(IWbemClassObject *obj, const WCHAR *ns, const WCHAR *server,
                                           IWbemServices *services, struct class_cache *cache, UINT depth)
{
    struct mi_class_data *cls;
    BSTR name, parent;
    IWbemQualifierSet *qs;

    if (!(name = get_string_prop(obj, L"__CLASS"))) return NULL;
    cls = class_data_create(name, ns, server);
    SysFreeString(name);
    if (!cls) return NULL;
    if ((parent = get_string_prop(obj, L"__SUPERCLASS")))
    {
        if (parent[0]) cls->parent = mi_strdup(parent);
        SysFreeString(parent);
    }
    if (SUCCEEDED(IWbemClassObject_GetQualifierSet(obj, &qs)))
    {
        read_qualifiers(qs, &cls->quals, NULL, NULL);
        IWbemQualifierSet_Release(qs);
    }
    read_properties(obj, &cls->props, &cls->count, TRUE, MI_FLAG_PROPERTY, NULL);
    read_methods(obj, cls);
    /* the superclass chain (MI_Class_GetParentClass, CimClass.CimSuperClass), bounded */
    if (cls->parent && services && cache && depth < MAX_CLASS_DEPTH)
        cls->parent_data = get_class_data(cache, services, cls->parent, ns, server, depth + 1);
    class_data_finish(cls);
    return cls;
}

static BOOL cache_add(struct class_cache *cache, struct mi_class_data *cls)
{
    if (cache->count == cache->size)
    {
        UINT size = cache->size ? cache->size * 2 : 16;
        struct mi_class_data **tmp = mi_alloc(size * sizeof(*tmp));
        if (!tmp) return FALSE;
        if (cache->count) memcpy(tmp, cache->items, cache->count * sizeof(*tmp));
        mi_free(cache->items);
        cache->items = tmp;
        cache->size = size;
    }
    cache->items[cache->count++] = cls;
    return TRUE;
}

static void cache_free(struct class_cache *cache)
{
    UINT i;
    for (i = 0; i < cache->count; i++) class_data_release(cache->items[i]);
    mi_free(cache->items);
    memset(cache, 0, sizeof(*cache));
}

/* a new reference to the class data of name (from the cache, else read from WMI and cached) */
static struct mi_class_data *get_class_data(struct class_cache *cache, IWbemServices *services, const WCHAR *name,
                                            const WCHAR *ns, const WCHAR *server, UINT depth)
{
    IWbemClassObject *obj = NULL;
    struct mi_class_data *cls = NULL;
    BSTR bname;
    UINT i;

    for (i = 0; i < cache->count; i++)
    {
        if (_wcsicmp(cache->items[i]->name, name)) continue;
        InterlockedIncrement(&cache->items[i]->refs);
        return cache->items[i];
    }
    if (!(bname = SysAllocString(name))) return NULL;
    if (SUCCEEDED(IWbemServices_GetObject(services, bname, 0, NULL, &obj, NULL)) && obj)
    {
        cls = convert_class(obj, ns, server, services, cache, depth);
        IWbemClassObject_Release(obj);
    }
    SysFreeString(bname);
    if (cls)
    {
        InterlockedIncrement(&cls->refs);
        if (!cache_add(cache, cls)) InterlockedDecrement(&cls->refs);
    }
    return cls;
}

static struct mi_instance *convert_object(IWbemClassObject *obj, const WCHAR *ns, const WCHAR *server, BOOL keys_from_path)
{
    struct mi_instance *inst;
    BSTR class_name, relpath = NULL;
    SAFEARRAY *names = NULL;
    LONG lb, ub, i;

    if (!(class_name = get_string_prop(obj, L"__CLASS"))) return NULL;
    inst = instance_create(class_name, ns, server);
    SysFreeString(class_name);
    if (!inst) return NULL;
    if (keys_from_path) relpath = get_string_prop(obj, L"__RELPATH");

    if (SUCCEEDED(IWbemClassObject_GetNames(obj, NULL, WBEM_FLAG_NONSYSTEM_ONLY, NULL, &names)) && names &&
        SUCCEEDED(SafeArrayGetLBound(names, 1, &lb)) && SUCCEEDED(SafeArrayGetUBound(names, 1, &ub)))
    {
        for (i = lb; i <= ub; i++)
        {
            BSTR name = NULL;
            VARIANT var;
            CIMTYPE type = CIM_STRING;
            MI_Value value;
            MI_Type mt;
            MI_Uint32 flags = 0;
            BOOL null;

            if (FAILED(SafeArrayGetElement(names, &i, &name)) || !name) continue;
            VariantInit(&var);
            if (SUCCEEDED(IWbemClassObject_Get(obj, name, 0, &var, &type, NULL)))
            {
                mt = cimtype_to_mi(type);
                variant_to_mi(&var, mt, &value, &null, ns, server);
                flags = MI_FLAG_PROPERTY | MI_FLAG_NOT_MODIFIED;
                if (relpath && path_has_key(relpath, name)) flags |= MI_FLAG_KEY;
                if (null) flags |= MI_FLAG_NULL;
                /* an adopted value that was not stored (duplicate name, out of memory) is still ours */
                if (instance_add(inst, name, null ? NULL : &value, mt, flags | MI_FLAG_ADOPT) != MI_RESULT_OK && !null)
                    free_value(&value, mt);
            }
            VariantClear(&var);
            SysFreeString(name);
        }
    }
    SafeArrayDestroy(names);
    SysFreeString(relpath);
    return inst;
}

/* object path "\\server\ns:Class.Key="v",Key2=3" -> keys-only instance */
static struct mi_instance *reference_to_instance(const WCHAR *path, const WCHAR *ns, const WCHAR *server)
{
    struct mi_instance *inst;
    const WCHAR *p = path, *colon, *dot, *start;
    WCHAR *class_name, *buf;
    size_t len;

    if ((colon = wcschr(p, ':'))) p = colon + 1;
    dot = wcschr(p, '.');
    len = dot ? (size_t)(dot - p) : wcslen(p);
    if (!(class_name = mi_alloc((len + 1) * sizeof(WCHAR)))) return NULL;
    memcpy(class_name, p, len * sizeof(WCHAR));
    inst = instance_create(class_name, ns, server);
    mi_free(class_name);
    if (!inst || !dot) return inst;
    if (!(buf = mi_alloc((wcslen(dot) + 1) * sizeof(WCHAR)))) return inst;

    p = dot + 1;
    while (*p)
    {
        WCHAR *name, *out;
        MI_Value v;

        start = p;
        while (*p && *p != '=') p++;
        if (!*p) break;
        len = p - start;
        if (!(name = mi_alloc((len + 1) * sizeof(WCHAR)))) break;
        memcpy(name, start, len * sizeof(WCHAR));
        p++;
        out = buf;
        if (*p == '"')
        {
            p++;
            while (*p && *p != '"')
            {
                if (*p == '\\' && p[1]) p++;
                *out++ = *p++;
            }
            if (*p == '"') p++;
        }
        else while (*p && *p != ',') *out++ = *p++;
        *out = 0;
        v.string = buf;
        instance_add(inst, name, &v, MI_STRING, MI_FLAG_KEY);
        mi_free(name);
        if (*p == ',') p++;
    }
    mi_free(buf);
    return inst;
}

/* MI value -> VARIANT for method parameters and object paths */
static HRESULT value_to_variant(const MI_Value *value, MI_Type type, VARIANT *var)
{
    WCHAR buf[64];

    VariantInit(var);
    if (type & MI_ARRAY)
    {
        SAFEARRAY *sa;
        MI_Uint32 i;
        VARTYPE vt;
        switch (type & ~MI_ARRAY)
        {
        case MI_STRING: case MI_DATETIME: case MI_UINT64: case MI_SINT64: vt = VT_BSTR; break;
        case MI_BOOLEAN: vt = VT_BOOL; break;
        case MI_UINT8: vt = VT_UI1; break;
        case MI_REAL32: case MI_REAL64: vt = VT_R8; break;
        default: vt = VT_I4; break;
        }
        if (!(sa = SafeArrayCreateVector(vt, 0, value->array.size))) return E_OUTOFMEMORY;
        for (i = 0; i < value->array.size; i++)
        {
            MI_Value elem;
            VARIANT v;
            LONG idx = i;
            const BYTE *data = value->array.data;
            memset(&elem, 0, sizeof(elem));
            memcpy(&elem, data + i * mi_element_size(type), mi_element_size(type));
            if (FAILED(value_to_variant(&elem, type & ~MI_ARRAY, &v))) continue;
            if (vt == VT_BSTR) SafeArrayPutElement(sa, &idx, V_BSTR(&v));
            else if (vt == VT_R8) { VariantChangeType(&v, &v, 0, VT_R8); SafeArrayPutElement(sa, &idx, &V_R8(&v)); }
            else if (vt == VT_BOOL) SafeArrayPutElement(sa, &idx, &V_BOOL(&v));
            else if (vt == VT_UI1) { VariantChangeType(&v, &v, 0, VT_UI1); SafeArrayPutElement(sa, &idx, &V_UI1(&v)); }
            else { VariantChangeType(&v, &v, 0, VT_I4); SafeArrayPutElement(sa, &idx, &V_I4(&v)); }
            VariantClear(&v);
        }
        V_VT(var) = vt | VT_ARRAY;
        V_ARRAY(var) = sa;
        return S_OK;
    }
    switch (type)
    {
    case MI_BOOLEAN: V_VT(var) = VT_BOOL; V_BOOL(var) = value->boolean ? VARIANT_TRUE : VARIANT_FALSE; break;
    case MI_UINT8: V_VT(var) = VT_UI1; V_UI1(var) = value->uint8; break;
    case MI_SINT8: V_VT(var) = VT_I2; V_I2(var) = value->sint8; break;
    case MI_UINT16: V_VT(var) = VT_I4; V_I4(var) = value->uint16; break;
    case MI_SINT16: V_VT(var) = VT_I2; V_I2(var) = value->sint16; break;
    case MI_UINT32: V_VT(var) = VT_I4; V_I4(var) = (LONG)value->uint32; break;
    case MI_SINT32: V_VT(var) = VT_I4; V_I4(var) = value->sint32; break;
    case MI_CHAR16: V_VT(var) = VT_I2; V_I2(var) = (SHORT)value->char16; break;
    case MI_REAL32: V_VT(var) = VT_R4; V_R4(var) = value->real32; break;
    case MI_REAL64: V_VT(var) = VT_R8; V_R8(var) = value->real64; break;
    case MI_UINT64:
        _snwprintf(buf, ARRAYSIZE(buf) - 1, L"%I64u", value->uint64);
        buf[ARRAYSIZE(buf) - 1] = 0;
        V_VT(var) = VT_BSTR; V_BSTR(var) = SysAllocString(buf);
        break;
    case MI_SINT64:
        _snwprintf(buf, ARRAYSIZE(buf) - 1, L"%I64d", value->sint64);
        buf[ARRAYSIZE(buf) - 1] = 0;
        V_VT(var) = VT_BSTR; V_BSTR(var) = SysAllocString(buf);
        break;
    case MI_DATETIME:
        format_datetime(&value->datetime, buf, ARRAYSIZE(buf));
        V_VT(var) = VT_BSTR; V_BSTR(var) = SysAllocString(buf);
        break;
    case MI_STRING:
        V_VT(var) = VT_BSTR; V_BSTR(var) = SysAllocString(value->string ? value->string : L"");
        break;
    default:
        return WBEM_E_TYPE_MISMATCH;
    }
    return S_OK;
}

/* "Class.Key1=\"v\",Key2=3" from the key properties of an instance */
static BSTR instance_path(const struct mi_instance *inst)
{
    WCHAR *buf, *p;
    SIZE_T size = 64 + wcslen(inst->class_name);
    UINT i, keys = 0, pass;
    BOOL any_key = FALSE;
    BSTR ret;

    for (i = 0; i < inst->count; i++) if (inst->elems[i].flags & MI_FLAG_KEY) any_key = TRUE;
    for (i = 0; i < inst->count; i++)
    {
        const struct mi_element *e = &inst->elems[i];
        if (any_key ? !(e->flags & MI_FLAG_KEY) : (e->flags & MI_FLAG_NULL)) continue;
        size += wcslen(e->name) + 80;
        if (e->type == MI_STRING && e->value.string && !(e->flags & MI_FLAG_NULL)) size += 2 * wcslen(e->value.string);
    }
    if (!(buf = mi_alloc(size * sizeof(WCHAR)))) return NULL;
    wcscpy(buf, inst->class_name);
    p = buf + wcslen(buf);
    for (pass = 0, i = 0; i < inst->count; i++)
    {
        const struct mi_element *e = &inst->elems[i];
        VARIANT var;
        if (any_key ? !(e->flags & MI_FLAG_KEY) : (e->flags & MI_FLAG_NULL)) continue;
        if (e->flags & MI_FLAG_NULL) continue;
        *p++ = keys++ ? ',' : '.';
        wcscpy(p, e->name);
        p += wcslen(p);
        *p++ = '=';
        if (e->type == MI_STRING || e->type == MI_DATETIME || e->type == MI_REFERENCE)
        {
            const WCHAR *s;
            WCHAR tmp[64];
            if (e->type == MI_DATETIME) { format_datetime(&e->value.datetime, tmp, ARRAYSIZE(tmp)); s = tmp; }
            else if (e->type == MI_STRING) s = e->value.string ? e->value.string : L"";
            else s = L"";
            *p++ = '"';
            for (; *s; s++)
            {
                if (*s == '"' || *s == '\\') *p++ = '\\';
                *p++ = *s;
            }
            *p++ = '"';
        }
        else if (SUCCEEDED(value_to_variant(&e->value, e->type, &var)))
        {
            if (V_VT(&var) == VT_BOOL) wcscpy(p, V_BOOL(&var) ? L"TRUE" : L"FALSE");
            else if (SUCCEEDED(VariantChangeType(&var, &var, 0, VT_BSTR))) wcscpy(p, V_BSTR(&var));
            p += wcslen(p);
            VariantClear(&var);
        }
        *p = 0;
    }
    (void)pass;
    *p = 0;
    ret = SysAllocString(buf);
    mi_free(buf);
    return ret;
}

/* operation results */

static void op_lock(struct mi_operation *op) { EnterCriticalSection(&op->cs); }
static void op_unlock(struct mi_operation *op) { LeaveCriticalSection(&op->cs); }

static BOOL op_add_result(struct mi_operation *op, struct mi_instance *inst, MI_Class *cls)
{
    BOOL ok = TRUE;
    op_lock(op);
    if (op->count == op->size)
    {
        UINT size = op->size ? op->size * 2 : 16;
        struct mi_instance **ti = mi_alloc(size * sizeof(*ti));
        MI_Class **tc = mi_alloc(size * sizeof(*tc));
        if (!ti || !tc) { mi_free(ti); mi_free(tc); ok = FALSE; }
        else
        {
            if (op->count)
            {
                memcpy(ti, op->instances, op->count * sizeof(*ti));
                memcpy(tc, op->classes, op->count * sizeof(*tc));
            }
            mi_free(op->instances);
            mi_free(op->classes);
            op->instances = ti;
            op->classes = tc;
            op->size = size;
        }
    }
    if (ok)
    {
        op->instances[op->count] = inst;
        op->classes[op->count] = cls;
        op->count++;
    }
    op_unlock(op);
    if (!ok)
    {
        instance_delete(inst);
        class_delete(cls);
    }
    else SetEvent(op->ready);
    return ok;
}

static void op_complete(struct mi_operation *op, MI_Result result, HRESULT hr, const WCHAR *message)
{
    op_lock(op);
    if (!op->done)
    {
        op->result = result;
        if (result != MI_RESULT_OK)
        {
            if (!message || !message[0]) message = result_message(result);
            op->error_message = mi_strdup(message);
            op->error_details = make_error_instance(result, hr, message);
        }
        op->done = TRUE;
    }
    op_unlock(op);
    SetEvent(op->ready);
}

static void session_release(struct mi_session *s)
{
    if (InterlockedDecrement(&s->refs)) return;
    mi_free(s->protocol);
    mi_free(s->destination);
    s->magic = 0;
    mi_free(s);
}

static void op_release(struct mi_operation *op)
{
    UINT i;
    if (InterlockedDecrement(&op->refs)) return;
    for (i = 0; i < op->count; i++)
    {
        instance_delete(op->instances[i]);
        class_delete(op->classes[i]);
    }
    mi_free(op->instances);
    mi_free(op->classes);
    instance_delete(op->error_details);
    mi_free(op->error_message);
    instance_delete(op->inbound);
    instance_delete(op->params);
    mi_free(op->nameSpace);
    mi_free(op->class_name);
    mi_free(op->method);
    mi_free(op->query);
    if (op->ready) CloseHandle(op->ready);
    if (op->ack) CloseHandle(op->ack);
    if (op->thread) CloseHandle(op->thread);
    DeleteCriticalSection(&op->cs);
    if (op->session) session_release(op->session);
    op->magic = 0;
    mi_free(op);
}

/* WMI work (worker thread) */

static void set_blanket(IUnknown *proxy)
{
    CoSetProxyBlanket(proxy, RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT, COLE_DEFAULT_PRINCIPAL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_DEFAULT);
}

static HRESULT connect_namespace(const WCHAR *ns, IWbemServices **services)
{
    IWbemLocator *locator;
    BSTR path;
    HRESULT hr;

    *services = NULL;
    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator);
    if (FAILED(hr))
    {
        ERR("no WMI locator: %08lx\n", hr);
        return hr;
    }
    path = wmi_namespace(ns);
    hr = IWbemLocator_ConnectServer(locator, path, NULL, NULL, NULL, 0, NULL, NULL, services);
    SysFreeString(path);
    IWbemLocator_Release(locator);
    /* an out-of-process WMI service needs impersonation; in-process providers have no proxy */
    if (SUCCEEDED(hr)) set_blanket((IUnknown *)*services);
    return hr;
}

static BOOL op_cancelled(struct mi_operation *op)
{
    BOOL ret;
    op_lock(op);
    ret = op->cancelled;
    op_unlock(op);
    return ret;
}

/* take the WMI lock, giving up when the operation is cancelled or closed while it waits */
static BOOL wmi_lock(struct mi_operation *op)
{
    while (!TryEnterCriticalSection(&wmi_cs))
    {
        if (op_cancelled(op)) return FALSE;
        Sleep(5);
    }
    return TRUE;
}

/* one class object per class name for the instances of an operation: a pointer the cache keeps alive
 * (NULL when the class could not be read or cached) */
static struct mi_class_data *cached_class(struct class_cache *cache, IWbemServices *services, const WCHAR *name,
                                          const WCHAR *ns, const WCHAR *server)
{
    struct mi_class_data *cls = get_class_data(cache, services, name, ns, server, 0);
    BOOL cached = FALSE;
    UINT i;

    if (!cls) return NULL;
    for (i = 0; i < cache->count; i++) if (cache->items[i] == cls) cached = TRUE;
    class_data_release(cls);
    return cached ? cls : NULL;
}

/* attach the class and the schema flags to an already converted instance and add it to the result.
 * cached_class issues a new WMI query for the class, which refills the class table: never call this
 * while an enumerator over the same table is still being read. */
static void finalize_instance(struct mi_operation *op, IWbemServices *services, struct class_cache *cache,
                              struct mi_instance *inst, const WCHAR *server)
{
    struct mi_class_data *cls;
    UINT i;

    if (!inst) return;
    if ((cls = cached_class(cache, services, inst->class_name, op->nameSpace, server)))
    {
        instance_set_class(inst, cls);
        /* key and read-only flags from the schema */
        for (i = 0; i < inst->count; i++)
        {
            UINT j;
            for (j = 0; j < cls->count; j++)
            {
                if (_wcsicmp(cls->props[j].name, inst->elems[i].name)) continue;
                inst->elems[i].flags |= cls->props[j].flags & (MI_FLAG_KEY | MI_FLAG_READONLY);
                break;
            }
        }
    }
    if (op->keys_only)
    {
        /* drop non-key properties */
        UINT out = 0;
        for (i = 0; i < inst->count; i++)
        {
            if (inst->elems[i].flags & MI_FLAG_KEY) inst->elems[out++] = inst->elems[i];
            else
            {
                mi_free(inst->elems[i].name);
                free_value(&inst->elems[i].value, inst->elems[i].type);
            }
        }
        inst->count = out;
    }
    op_add_result(op, inst, NULL);
}

/* Convert every object first (the enumerator indexes rows of the shared wbemprox table), and only
 * then, with the enumerator drained, look up the class data. */
static void run_enum(struct mi_operation *op, IWbemServices *services, IEnumWbemClassObject *enumerator, const WCHAR *server)
{
    struct class_cache cache;
    struct mi_instance **list = NULL;
    IWbemClassObject *obj;
    MI_Result result = MI_RESULT_OK;
    const WCHAR *message = NULL;
    HRESULT hr, final_hr = 0;
    UINT count = 0, size = 0, i;
    ULONG got;

    memset(&cache, 0, sizeof(cache));
    set_blanket((IUnknown *)enumerator);
    for (;;)
    {
        struct mi_instance *inst;

        if (op_cancelled(op)) { result = MI_RESULT_FAILED; message = L"The operation was cancelled"; break; }
        obj = NULL;
        got = 0;
        hr = IEnumWbemClassObject_Next(enumerator, WBEM_INFINITE, 1, &obj, &got);
        if (hr == WBEM_S_FALSE || (hr == S_OK && !got)) break;
        if (FAILED(hr)) { result = hresult_to_mi(hr); final_hr = hr; break; }
        inst = convert_object(obj, op->nameSpace, server, TRUE);
        IWbemClassObject_Release(obj);
        if (!inst) continue;
        if (count == size)
        {
            UINT new_size = size ? size * 2 : 64;
            struct mi_instance **tmp = mi_alloc(new_size * sizeof(*tmp));
            if (!tmp)
            {
                instance_delete(inst);
                result = MI_RESULT_SERVER_LIMITS_EXCEEDED;
                final_hr = E_OUTOFMEMORY;
                break;
            }
            if (list) memcpy(tmp, list, count * sizeof(*tmp));
            mi_free(list);
            list = tmp;
            size = new_size;
        }
        list[count++] = inst;
    }

    for (i = 0; i < count; i++)
    {
        if (result == MI_RESULT_FAILED && message) instance_delete(list[i]); /* cancelled: nobody wants the rest */
        else finalize_instance(op, services, &cache, list[i], server);
    }
    mi_free(list);
    op_complete(op, result, final_hr, message);
    cache_free(&cache);
}

/* MI_Session_EnumerateClasses: every class derived from class_name (all classes when NULL) */
static void run_enum_classes(struct mi_operation *op, IWbemServices *services, const WCHAR *server)
{
    IEnumWbemClassObject *enumerator = NULL;
    struct class_cache cache;
    IWbemClassObject *obj;
    BSTR super = NULL;
    HRESULT hr;
    ULONG got;

    memset(&cache, 0, sizeof(cache));
    if (op->class_name && !(super = SysAllocString(op->class_name)))
    {
        op_complete(op, MI_RESULT_SERVER_LIMITS_EXCEEDED, E_OUTOFMEMORY, NULL);
        return;
    }
    hr = IWbemServices_CreateClassEnum(services, super, WBEM_FLAG_DEEP | WBEM_FLAG_FORWARD_ONLY, NULL, &enumerator);
    SysFreeString(super);
    if (FAILED(hr))
    {
        op_complete(op, hresult_to_mi(hr), hr, NULL);
        return;
    }
    set_blanket((IUnknown *)enumerator);
    for (;;)
    {
        struct mi_class_data *data = NULL;
        MI_Class *cls;

        if (op_cancelled(op))
        {
            IEnumWbemClassObject_Release(enumerator);
            cache_free(&cache);
            op_complete(op, MI_RESULT_FAILED, 0, L"The operation was cancelled");
            return;
        }
        obj = NULL;
        got = 0;
        hr = IEnumWbemClassObject_Next(enumerator, WBEM_INFINITE, 1, &obj, &got);
        if (FAILED(hr) || !got || !obj) break;
        if (op->keys_only)
        {
            /* classNamesOnly: the name and the superclass name */
            BSTR name = get_string_prop(obj, L"__CLASS"), parent = get_string_prop(obj, L"__SUPERCLASS");
            if (name && (data = class_data_create(name, op->nameSpace, server)))
            {
                if (parent && parent[0]) data->parent = mi_strdup(parent);
                class_data_finish(data);
            }
            SysFreeString(name);
            SysFreeString(parent);
        }
        else data = convert_class(obj, op->nameSpace, server, services, &cache, 0);
        IWbemClassObject_Release(obj);
        if (!data) continue;
        cls = class_create(data);
        class_data_release(data);
        if (cls && !op_add_result(op, NULL, cls)) break;
    }
    IEnumWbemClassObject_Release(enumerator);
    cache_free(&cache);
    op_complete(op, FAILED(hr) ? hresult_to_mi(hr) : MI_RESULT_OK, FAILED(hr) ? hr : 0, NULL);
}

static BOOL method_exists(IWbemClassObject *cls, const WCHAR *method)
{
    BSTR name;
    IWbemClassObject *in, *out;
    BOOL found = FALSE;

    if (FAILED(IWbemClassObject_BeginMethodEnumeration(cls, 0))) return TRUE; /* cannot tell: let WMI decide */
    for (;;)
    {
        name = NULL;
        in = out = NULL;
        if (IWbemClassObject_NextMethod(cls, 0, &name, &in, &out) != S_OK || !name) break;
        if (!_wcsicmp(name, method)) found = TRUE;
        if (in) IWbemClassObject_Release(in);
        if (out) IWbemClassObject_Release(out);
        SysFreeString(name);
        if (found) break;
    }
    IWbemClassObject_EndMethodEnumeration(cls);
    return found;
}

static void run_invoke(struct mi_operation *op, IWbemServices *services, const WCHAR *server)
{
    IWbemClassObject *cls = NULL, *in_sig = NULL, *out_sig = NULL, *in = NULL, *out = NULL;
    BSTR class_name = NULL, path = NULL, method = NULL;
    struct mi_instance *result;
    HRESULT hr;
    UINT i;

    if (op->inbound) path = instance_path(op->inbound);
    else path = SysAllocString(op->class_name);
    class_name = SysAllocString(op->inbound ? op->inbound->class_name : op->class_name);
    method = SysAllocString(op->method);
    if (!path || !class_name || !method) { op_complete(op, MI_RESULT_SERVER_LIMITS_EXCEEDED, E_OUTOFMEMORY, NULL); goto done; }

    hr = IWbemServices_GetObject(services, class_name, 0, NULL, &cls, NULL);
    if (FAILED(hr)) { op_complete(op, hresult_to_mi(hr), hr, NULL); goto done; }
    hr = IWbemClassObject_GetMethod(cls, method, 0, &in_sig, &out_sig);
    if (FAILED(hr) || !method_exists(cls, method))
    {
        /* as WMI reports it: Not found, HRESULT 0x80041002 */
        WCHAR msg[160];
        _snwprintf(msg, ARRAYSIZE(msg) - 1, L"Method \"%.100s\" not found", op->method);
        msg[ARRAYSIZE(msg) - 1] = 0;
        op_complete(op, MI_RESULT_NOT_FOUND, WBEM_E_NOT_FOUND, msg);
        goto done;
    }
    if (in_sig && op->params && op->params->count)
    {
        if (FAILED(hr = IWbemClassObject_SpawnInstance(in_sig, 0, &in))) { op_complete(op, hresult_to_mi(hr), hr, NULL); goto done; }
        for (i = 0; i < op->params->count; i++)
        {
            const struct mi_element *e = &op->params->elems[i];
            VARIANT var;
            if (e->flags & MI_FLAG_NULL) continue;
            if (FAILED(value_to_variant(&e->value, e->type, &var))) continue;
            hr = IWbemClassObject_Put(in, e->name, 0, &var, 0);
            VariantClear(&var);
            if (FAILED(hr)) { op_complete(op, MI_RESULT_INVALID_PARAMETER, hr, NULL); goto done; }
        }
    }
    hr = IWbemServices_ExecMethod(services, path, method, 0, NULL, in, &out, NULL);
    if (FAILED(hr)) { op_complete(op, hresult_to_mi(hr), hr, NULL); goto done; }
    if (out && (result = convert_object(out, op->nameSpace, server, FALSE)))
    {
        mi_free(result->class_name);
        result->class_name = mi_strdup(L"__PARAMETERS");
        result->decl.name = result->class_name;
        op_add_result(op, result, NULL);
    }
    op_complete(op, MI_RESULT_OK, 0, NULL);

done:
    if (out) IWbemClassObject_Release(out);
    if (in) IWbemClassObject_Release(in);
    if (in_sig) IWbemClassObject_Release(in_sig);
    if (out_sig) IWbemClassObject_Release(out_sig);
    if (cls) IWbemClassObject_Release(cls);
    SysFreeString(path);
    SysFreeString(class_name);
    SysFreeString(method);
}

static void run_operation(struct mi_operation *op)
{
    IWbemServices *services = NULL;
    IEnumWbemClassObject *enumerator = NULL;
    IWbemClassObject *obj = NULL;
    WCHAR *server = local_server_name();
    BSTR str = NULL, lang = NULL;
    HRESULT hr;
    BOOL locked = FALSE;

    if (op->kind == OP_FAILED || op->done) goto done; /* completed at creation */
    /* connecting does not touch the class tables, so it runs outside the WMI lock */
    if (FAILED(hr = connect_namespace(op->kind == OP_TEST_CONNECTION ? L"root\\cimv2" : op->nameSpace, &services)))
    {
        op_complete(op, hresult_to_mi(hr), hr, NULL);
        goto done;
    }
    if (op->kind == OP_TEST_CONNECTION)
    {
        op_complete(op, MI_RESULT_OK, 0, NULL);
        goto done;
    }
    /* a long fill by another operation must not hold up Cancel and Close of the ones queued behind it */
    if (!wmi_lock(op))
    {
        op_complete(op, MI_RESULT_FAILED, 0, L"The operation was cancelled");
        goto done;
    }
    locked = TRUE;
    switch (op->kind)
    {
    case OP_ENUMERATE:
        str = SysAllocString(op->class_name);
        hr = IWbemServices_CreateInstanceEnum(services, str, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                              NULL, &enumerator);
        if (FAILED(hr)) op_complete(op, hresult_to_mi(hr), hr, NULL);
        else run_enum(op, services, enumerator, server);
        break;

    case OP_QUERY:
        lang = SysAllocString(L"WQL");
        str = SysAllocString(op->query);
        hr = IWbemServices_ExecQuery(services, lang, str, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                     NULL, &enumerator);
        if (FAILED(hr)) op_complete(op, hresult_to_mi(hr), hr, NULL);
        else run_enum(op, services, enumerator, server);
        break;

    case OP_GET_INSTANCE:
        if (!(str = instance_path(op->inbound))) { op_complete(op, MI_RESULT_SERVER_LIMITS_EXCEEDED, E_OUTOFMEMORY, NULL); break; }
        hr = IWbemServices_GetObject(services, str, 0, NULL, &obj, NULL);
        if (FAILED(hr)) op_complete(op, hresult_to_mi(hr), hr, NULL);
        else
        {
            struct class_cache cache;
            memset(&cache, 0, sizeof(cache));
            finalize_instance(op, services, &cache, convert_object(obj, op->nameSpace, server, TRUE), server);
            cache_free(&cache);
            op_complete(op, MI_RESULT_OK, 0, NULL);
        }
        break;

    case OP_GET_CLASS:
        str = SysAllocString(op->class_name);
        hr = IWbemServices_GetObject(services, str, 0, NULL, &obj, NULL);
        if (FAILED(hr)) op_complete(op, hresult_to_mi(hr), hr, NULL);
        else
        {
            struct class_cache cache;
            struct mi_class_data *data;
            MI_Class *cls;

            memset(&cache, 0, sizeof(cache));
            data = convert_class(obj, op->nameSpace, server, services, &cache, 0);
            cls = data ? class_create(data) : NULL;
            class_data_release(data);
            cache_free(&cache);
            if (cls) op_add_result(op, NULL, cls);
            op_complete(op, cls ? MI_RESULT_OK : MI_RESULT_FAILED, 0, NULL);
        }
        break;

    case OP_ENUMERATE_CLASSES:
        run_enum_classes(op, services, server);
        break;

    case OP_INVOKE:
        run_invoke(op, services, server);
        break;

    default:
        op_complete(op, MI_RESULT_NOT_SUPPORTED, 0, NULL);
        break;
    }

done:
    if (obj) IWbemClassObject_Release(obj);
    if (enumerator) IEnumWbemClassObject_Release(enumerator);
    if (services) IWbemServices_Release(services);
    if (locked) LeaveCriticalSection(&wmi_cs);
    SysFreeString(str);
    SysFreeString(lang);
    mi_free(server);
}

/* push delivery through the caller's callbacks */

static MI_Result MI_CALL op_result_ack(MI_Operation *operation);

static void deliver_push(struct mi_operation *op)
{
    BOOL manual = (op->flags & MI_OPERATIONFLAGS_MANUAL_ACK_RESULTS) != 0;
    BOOL classes = (op->kind == OP_GET_CLASS || op->kind == OP_ENUMERATE_CLASSES);
    UINT i;

    for (i = 0;; i++)
    {
        struct mi_instance *inst = NULL;
        MI_Class *cls = NULL;
        BOOL last, stop;
        MI_Result result = MI_RESULT_OK;
        const WCHAR *message = NULL;
        const MI_Instance *details = NULL;

        op_lock(op);
        stop = op->closed;
        last = (i >= op->count);
        if (!last) { inst = op->instances[i]; cls = op->classes[i]; }
        else { result = op->result; message = op->error_message; details = op->error_details ? &op->error_details->base : NULL; }
        op_unlock(op);
        if (stop) break;

        if (manual) ResetEvent(op->ack);
        if (classes)
        {
            if (op->callbacks.classResult)
                op->callbacks.classResult(op->client, op->callbacks.callbackContext, cls, last ? MI_FALSE : MI_TRUE,
                                          result, message, details, op_result_ack);
        }
        else if (op->callbacks.instanceResult)
        {
            op->callbacks.instanceResult(op->client, op->callbacks.callbackContext, inst ? &inst->base : NULL,
                                         last ? MI_FALSE : MI_TRUE, result, message, details, op_result_ack);
        }
        if (last)
        {
            /* only now may MI_Operation_Close from another thread return: the client frees its callback
             * context after Close, so the final callback must have returned. Close from inside the
             * callback (same thread) never waits. */
            op_lock(op);
            op->final_delivered = TRUE;
            op_unlock(op);
            break;
        }
        if (manual)
        {
            HANDLE handles[1] = { op->ack };
            WaitForMultipleObjects(1, handles, FALSE, INFINITE);
        }
    }
}

static DWORD WINAPI operation_thread(void *arg)
{
    struct mi_operation *op = arg;
    HMODULE module = op->module;
    HRESULT hr_init;

    hr_init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    run_operation(op);
    if (SUCCEEDED(hr_init)) CoUninitialize();
    if (op->push) deliver_push(op);
    op_release(op);
    /* the worker runs mi.dll code until its last instruction: a FreeLibrary by the client (which runs
     * DllMain and deletes the locks) must wait for that, so drop the module reference on the way out */
    if (module) FreeLibraryAndExitThread(module, 0);
    return 0;
}

/* MI_Operation */

static struct mi_operation *get_operation(const MI_Operation *operation)
{
    struct mi_operation *op;
    if (!operation || operation->ft != &mi_operation_ft) return NULL;
    op = (struct mi_operation *)operation->reserved2;
    if (!op || op->magic != MI_OPERATION_MAGIC) return NULL;
    return op;
}

static MI_Result MI_CALL op_result_ack(MI_Operation *operation)
{
    struct mi_operation *op = get_operation(operation);
    if (!op) return MI_RESULT_INVALID_PARAMETER;
    SetEvent(op->ack);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL op_Close(MI_Operation *operation)
{
    struct mi_operation *op = get_operation(operation);
    HANDLE thread = NULL;

    TRACE("%p\n", operation);
    if (!op) return MI_RESULT_INVALID_PARAMETER;
    op_lock(op);
    if (op->closed) { op_unlock(op); return MI_RESULT_INVALID_PARAMETER; }
    op->closed = TRUE;
    op->cancelled = TRUE;
    if (op->thread && op->thread_id != GetCurrentThreadId() && !op->final_delivered)
        DuplicateHandle(GetCurrentProcess(), op->thread, GetCurrentProcess(), &thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    op_unlock(op);
    SetEvent(op->ack);
    /* no callbacks after MI_Operation_Close returns */
    if (thread)
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    operation->ft = NULL;
    operation->reserved2 = 0;
    op->client = NULL;
    op_release(op);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL op_Cancel(MI_Operation *operation, MI_CancellationReason reason)
{
    struct mi_operation *op = get_operation(operation);
    TRACE("%p %d\n", operation, reason);
    if (!op) return MI_RESULT_INVALID_PARAMETER;
    /* the ack event belongs to the client's manual-ack pacing: Close wakes the worker, Cancel must not */
    op_lock(op);
    op->cancelled = TRUE;
    op_unlock(op);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL op_GetSession(MI_Operation *operation, MI_Session *session)
{
    struct mi_operation *op = get_operation(operation);
    if (!op || !session || !op->session) return MI_RESULT_INVALID_PARAMETER;
    *session = op->session->client;
    return MI_RESULT_OK;
}

/* pull: wait for the next result (or the end) */
static MI_Result pull_next(struct mi_operation *op, struct mi_instance **inst, MI_Class **cls, MI_Boolean *more,
                           MI_Result *result, const MI_Char **message, const MI_Instance **details)
{
    if (op->push) return MI_RESULT_INVALID_PARAMETER;
    for (;;)
    {
        op_lock(op);
        if (op->final_delivered)
        {
            op_unlock(op);
            return MI_RESULT_INVALID_PARAMETER;
        }
        if (op->next < op->count)
        {
            if (inst) *inst = op->instances[op->next];
            if (cls) *cls = op->classes[op->next];
            op->next++;
            *more = MI_TRUE;
            *result = MI_RESULT_OK;
            if (message) *message = NULL;
            if (details) *details = NULL;
            op_unlock(op);
            return MI_RESULT_OK;
        }
        if (op->done)
        {
            op->final_delivered = TRUE;
            if (inst) *inst = NULL;
            if (cls) *cls = NULL;
            *more = MI_FALSE;
            *result = op->result;
            if (message) *message = op->error_message;
            if (details) *details = op->error_details ? &op->error_details->base : NULL;
            op_unlock(op);
            return MI_RESULT_OK;
        }
        ResetEvent(op->ready);
        op_unlock(op);
        WaitForSingleObject(op->ready, INFINITE);
    }
}

static MI_Result MI_CALL op_GetInstance(MI_Operation *operation, const MI_Instance **instance, MI_Boolean *more,
                                        MI_Result *result, const MI_Char **message, const MI_Instance **details)
{
    struct mi_operation *op = get_operation(operation);
    struct mi_instance *inst = NULL;
    MI_Result res;

    if (!op || !instance || !more || !result) return MI_RESULT_INVALID_PARAMETER;
    if (op->kind == OP_GET_CLASS || op->kind == OP_ENUMERATE_CLASSES) return MI_RESULT_INVALID_PARAMETER;
    res = pull_next(op, &inst, NULL, more, result, message, details);
    *instance = inst ? &inst->base : NULL;
    return res;
}

static MI_Result MI_CALL op_GetIndication(MI_Operation *operation, const MI_Instance **instance, const MI_Char **bookmark,
                                          const MI_Char **machineID, MI_Boolean *more, MI_Result *result,
                                          const MI_Char **message, const MI_Instance **details)
{
    /* no subscriptions are ever created, so no operation delivers indications */
    return MI_RESULT_INVALID_PARAMETER;
}

static MI_Result MI_CALL op_GetClass(MI_Operation *operation, const MI_Class **cls, MI_Boolean *more,
                                     MI_Result *result, const MI_Char **message, const MI_Instance **details)
{
    struct mi_operation *op = get_operation(operation);
    MI_Class *c = NULL;
    MI_Result res;

    if (!op || !cls || !more || !result) return MI_RESULT_INVALID_PARAMETER;
    if (op->kind != OP_GET_CLASS && op->kind != OP_ENUMERATE_CLASSES && op->kind != OP_FAILED) return MI_RESULT_INVALID_PARAMETER;
    res = pull_next(op, NULL, &c, more, result, message, details);
    *cls = c;
    return res;
}

const MI_OperationFT mi_operation_ft =
{
    op_Close,
    op_Cancel,
    op_GetSession,
    op_GetInstance,
    op_GetIndication,
    op_GetClass,
};

/* MI_Session */

static struct mi_session *get_session(const MI_Session *session)
{
    struct mi_session *s;
    if (!session || session->ft != &mi_session_ft) return NULL;
    s = (struct mi_session *)session->reserved2;
    if (!s || s->magic != MI_SESSION_MAGIC) return NULL;
    return s;
}

MI_Result session_new(MI_Application *app, const MI_Char *protocol, const MI_Char *destination,
                      MI_Instance **extendedError, MI_Session *session)
{
    struct mi_session *s;

    memset(session, 0, sizeof(*session));
    if (protocol && _wcsicmp(protocol, L"WMIDCOM") && _wcsicmp(protocol, L"WinRM"))
    {
        WARN("unsupported protocol %s\n", debugstr_w(protocol));
        if (extendedError)
        {
            struct mi_instance *err = make_error_instance(MI_RESULT_NOT_SUPPORTED, 0, L"The protocol is not supported ");
            *extendedError = err ? &err->base : NULL;
        }
        return MI_RESULT_NOT_SUPPORTED;
    }
    if (!is_local_destination(destination))
    {
        FIXME("remote destination %s not supported\n", debugstr_w(destination));
        if (extendedError)
        {
            struct mi_instance *err = make_error_instance(MI_RESULT_NOT_SUPPORTED, 0,
                                                          L"Remote CIM sessions are not supported on this system ");
            *extendedError = err ? &err->base : NULL;
        }
        return MI_RESULT_NOT_SUPPORTED;
    }
    if (!(s = mi_alloc(sizeof(*s)))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    s->magic = MI_SESSION_MAGIC;
    s->refs = 1;
    s->protocol = mi_strdup(protocol);
    s->destination = mi_strdup(destination);
    session->reserved1 = 1;
    session->reserved2 = (ptrdiff_t)s;
    session->ft = &mi_session_ft;
    s->client = *session;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL session_Close(MI_Session *session, void *context, void (MI_CALL *callback)(void *))
{
    struct mi_session *s = get_session(session);

    TRACE("%p %p %p\n", session, context, callback);
    if (!s) return MI_RESULT_INVALID_PARAMETER;
    if (InterlockedExchange(&s->closed, 1)) return MI_RESULT_INVALID_PARAMETER;
    session->ft = NULL;
    session->reserved2 = 0;
    session_release(s);
    if (callback) callback(context);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL session_GetApplication(MI_Session *session, MI_Application *application)
{
    if (!get_session(session) || !application) return MI_RESULT_INVALID_PARAMETER;
    return MI_RESULT_NOT_SUPPORTED;
}

static BOOL has_callbacks(const MI_OperationCallbacks *cb)
{
    return cb && (cb->instanceResult || cb->classResult || cb->indicationResult);
}

static struct mi_operation *op_create(struct mi_session *s, MI_Uint32 flags, const MI_Char *ns,
                                      MI_OperationCallbacks *callbacks, MI_Operation *operation, enum op_kind kind)
{
    struct mi_operation *op;

    memset(operation, 0, sizeof(*operation));
    if (!(op = mi_alloc(sizeof(*op)))) return NULL;
    op->magic = MI_OPERATION_MAGIC;
    op->refs = 1;
    op->kind = kind;
    op->flags = flags;
    op->client = operation;
    op->nameSpace = mi_namespace(ns);
    InitializeCriticalSection(&op->cs);
    op->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    op->ack = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!op->nameSpace || !op->ready || !op->ack)
    {
        mi_free(op->nameSpace);
        if (op->ready) CloseHandle(op->ready);
        if (op->ack) CloseHandle(op->ack);
        DeleteCriticalSection(&op->cs);
        op->magic = 0;
        mi_free(op);
        return NULL; /* *operation stays zeroed: it never looks like a live handle */
    }
    if (has_callbacks(callbacks))
    {
        op->push = TRUE;
        op->callbacks = *callbacks;
    }
    if (s)
    {
        InterlockedIncrement(&s->refs);
        op->session = s;
    }
    operation->reserved1 = 1;
    operation->reserved2 = (ptrdiff_t)op;
    operation->ft = &mi_operation_ft;
    return op;
}

/* start the worker; on failure the operation completes with an error */
static void op_start(struct mi_operation *op)
{
    HMODULE module = NULL;

    InterlockedIncrement(&op->refs); /* worker reference */
    /* pin mi.dll while the worker lives; if that fails the worker still runs, unprotected, as before */
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)operation_thread, &module);
    op->module = module;
    op_lock(op);
    op->thread = CreateThread(NULL, 0, operation_thread, op, 0, &op->thread_id);
    op_unlock(op);
    if (!op->thread)
    {
        op->module = NULL;
        if (module) FreeLibrary(module);
        op_complete(op, MI_RESULT_SERVER_LIMITS_EXCEEDED, HRESULT_FROM_WIN32(GetLastError()), NULL);
        if (op->push) deliver_push(op);
        op_release(op);
    }
}

/* an operation that fails before any WMI call: still delivered through the normal paths */
static void op_fail(struct mi_session *s, MI_Uint32 flags, MI_OperationCallbacks *callbacks, MI_Operation *operation,
                    MI_Result result, enum op_kind kind)
{
    struct mi_operation *op;
    if (!operation) return;
    if (!(op = op_create(s, flags, NULL, callbacks, operation, kind)))
    {
        memset(operation, 0, sizeof(*operation));
        return;
    }
    op_complete(op, result, 0, NULL);
    op->kind = (kind == OP_GET_CLASS || kind == OP_ENUMERATE_CLASSES) ? kind : OP_FAILED;
    op_start(op);
}

static BOOL check_options(MI_OperationOptions *options)
{
    return !options || options->ft == &mi_operation_options_ft;
}

static void MI_CALL session_GetInstance(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                        const MI_Char *ns, const MI_Instance *inbound, MI_OperationCallbacks *callbacks,
                                        MI_Operation *operation)
{
    struct mi_session *s = get_session(session);
    struct mi_instance *in = instance_from_mi(inbound);
    struct mi_operation *op;
    MI_Instance *copy = NULL;

    TRACE("%p %#x %p %s %p %p %p\n", session, flags, options, debugstr_w(ns), inbound, callbacks, operation);
    if (!operation) return;
    if (!s || !in || !in->class_name || !check_options(options))
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_INVALID_PARAMETER, OP_GET_INSTANCE);
        return;
    }
    if (!(op = op_create(s, flags, ns ? ns : in->nameSpace, callbacks, operation, OP_GET_INSTANCE))) return;
    if (in->base.ft->Clone(&in->base, &copy) == MI_RESULT_OK) op->inbound = instance_from_mi(copy);
    if (!op->inbound)
    {
        op_complete(op, MI_RESULT_SERVER_LIMITS_EXCEEDED, 0, NULL);
        op->kind = OP_FAILED;
    }
    op_start(op);
}

static void MI_CALL session_ModifyInstance(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                           const MI_Char *ns, const MI_Instance *inbound, MI_OperationCallbacks *callbacks,
                                           MI_Operation *operation)
{
    FIXME("%p: ModifyInstance not supported\n", session);
    op_fail(get_session(session), flags, callbacks, operation, MI_RESULT_NOT_SUPPORTED, OP_GET_INSTANCE);
}

static void MI_CALL session_CreateInstance(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                           const MI_Char *ns, const MI_Instance *inbound, MI_OperationCallbacks *callbacks,
                                           MI_Operation *operation)
{
    FIXME("%p: CreateInstance not supported\n", session);
    op_fail(get_session(session), flags, callbacks, operation, MI_RESULT_NOT_SUPPORTED, OP_GET_INSTANCE);
}

static void MI_CALL session_DeleteInstance(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                           const MI_Char *ns, const MI_Instance *inbound, MI_OperationCallbacks *callbacks,
                                           MI_Operation *operation)
{
    FIXME("%p: DeleteInstance not supported\n", session);
    op_fail(get_session(session), flags, callbacks, operation, MI_RESULT_NOT_SUPPORTED, OP_GET_INSTANCE);
}

static void MI_CALL session_Invoke(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options, const MI_Char *ns,
                                   const MI_Char *class_name, const MI_Char *method, const MI_Instance *inbound,
                                   const MI_Instance *params, MI_OperationCallbacks *callbacks, MI_Operation *operation)
{
    struct mi_session *s = get_session(session);
    struct mi_instance *in = instance_from_mi(inbound), *pin = instance_from_mi(params);
    struct mi_operation *op;
    MI_Instance *copy;

    TRACE("%p %#x %s %s %s %p %p\n", session, flags, debugstr_w(ns), debugstr_w(class_name), debugstr_w(method), inbound, params);
    if (!operation) return;
    if (!s || !method || (!class_name && !in) || (inbound && !in) || (params && !pin) || !check_options(options))
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_INVALID_PARAMETER, OP_INVOKE);
        return;
    }
    if (!(op = op_create(s, flags, ns ? ns : (in ? in->nameSpace : NULL), callbacks, operation, OP_INVOKE))) return;
    op->class_name = mi_strdup(class_name ? class_name : in->class_name);
    op->method = mi_strdup(method);
    if (in && in->base.ft->Clone(&in->base, &copy) == MI_RESULT_OK) op->inbound = instance_from_mi(copy);
    if (pin && pin->base.ft->Clone(&pin->base, &copy) == MI_RESULT_OK) op->params = instance_from_mi(copy);
    op_start(op);
}

static void MI_CALL session_EnumerateInstances(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                               const MI_Char *ns, const MI_Char *class_name, MI_Boolean keys_only,
                                               MI_OperationCallbacks *callbacks, MI_Operation *operation)
{
    struct mi_session *s = get_session(session);
    struct mi_operation *op;

    TRACE("%p %#x %p %s %s %d %p %p\n", session, flags, options, debugstr_w(ns), debugstr_w(class_name), keys_only,
          callbacks, operation);
    if (!operation) return;
    if (!s || !class_name || !class_name[0] || !check_options(options))
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_INVALID_PARAMETER, OP_ENUMERATE);
        return;
    }
    if (!(op = op_create(s, flags, ns, callbacks, operation, OP_ENUMERATE))) return;
    op->class_name = mi_strdup(class_name);
    op->keys_only = keys_only;
    op_start(op);
}

static void MI_CALL session_QueryInstances(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                           const MI_Char *ns, const MI_Char *dialect, const MI_Char *query,
                                           MI_OperationCallbacks *callbacks, MI_Operation *operation)
{
    struct mi_session *s = get_session(session);
    struct mi_operation *op;

    TRACE("%p %#x %p %s %s %s %p %p\n", session, flags, options, debugstr_w(ns), debugstr_w(dialect), debugstr_w(query),
          callbacks, operation);
    if (!operation) return;
    if (!s || !query || !check_options(options))
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_INVALID_PARAMETER, OP_QUERY);
        return;
    }
    if (dialect && _wcsicmp(dialect, L"WQL"))
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_QUERY_LANGUAGE_NOT_SUPPORTED, OP_QUERY);
        return;
    }
    if (!(op = op_create(s, flags, ns, callbacks, operation, OP_QUERY))) return;
    op->query = mi_strdup(query);
    op_start(op);
}

static void MI_CALL session_AssociatorInstances(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                                const MI_Char *ns, const MI_Instance *keys, const MI_Char *assoc,
                                                const MI_Char *result_class, const MI_Char *role, const MI_Char *result_role,
                                                MI_Boolean keys_only, MI_OperationCallbacks *callbacks, MI_Operation *operation)
{
    FIXME("%p: AssociatorInstances not supported\n", session);
    op_fail(get_session(session), flags, callbacks, operation, MI_RESULT_NOT_SUPPORTED, OP_ENUMERATE);
}

static void MI_CALL session_ReferenceInstances(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                               const MI_Char *ns, const MI_Instance *keys, const MI_Char *result_class,
                                               const MI_Char *role, MI_Boolean keys_only, MI_OperationCallbacks *callbacks,
                                               MI_Operation *operation)
{
    FIXME("%p: ReferenceInstances not supported\n", session);
    op_fail(get_session(session), flags, callbacks, operation, MI_RESULT_NOT_SUPPORTED, OP_ENUMERATE);
}

static void MI_CALL session_Subscribe(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                      const MI_Char *ns, const MI_Char *dialect, const MI_Char *query,
                                      const MI_SubscriptionDeliveryOptions *delivery, MI_OperationCallbacks *callbacks,
                                      MI_Operation *operation)
{
    FIXME("%p %s: subscriptions not supported\n", session, debugstr_w(query));
    op_fail(get_session(session), flags, callbacks, operation, MI_RESULT_NOT_SUPPORTED, OP_ENUMERATE);
}

static void MI_CALL session_GetClass(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                     const MI_Char *ns, const MI_Char *class_name, MI_OperationCallbacks *callbacks,
                                     MI_Operation *operation)
{
    struct mi_session *s = get_session(session);
    struct mi_operation *op;

    TRACE("%p %#x %p %s %s %p %p\n", session, flags, options, debugstr_w(ns), debugstr_w(class_name), callbacks, operation);
    if (!operation) return;
    if (!s || !class_name || !class_name[0] || !check_options(options))
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_INVALID_PARAMETER, OP_GET_CLASS);
        return;
    }
    if (!(op = op_create(s, flags, ns, callbacks, operation, OP_GET_CLASS))) return;
    op->class_name = mi_strdup(class_name);
    op_start(op);
}

static void MI_CALL session_EnumerateClasses(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
                                             const MI_Char *ns, const MI_Char *class_name, MI_Boolean names_only,
                                             MI_OperationCallbacks *callbacks, MI_Operation *operation)
{
    struct mi_session *s = get_session(session);
    struct mi_operation *op;

    TRACE("%p %#x %p %s %s %d %p %p\n", session, flags, options, debugstr_w(ns), debugstr_w(class_name), names_only,
          callbacks, operation);
    if (!operation) return;
    if (!s || !check_options(options))
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_INVALID_PARAMETER, OP_ENUMERATE_CLASSES);
        return;
    }
    if (!(op = op_create(s, flags, ns, callbacks, operation, OP_ENUMERATE_CLASSES))) return;
    if (class_name && class_name[0]) op->class_name = mi_strdup(class_name);
    op->keys_only = names_only;
    op_start(op);
}

static void MI_CALL session_TestConnection(MI_Session *session, MI_Uint32 flags, MI_OperationCallbacks *callbacks,
                                           MI_Operation *operation)
{
    struct mi_session *s = get_session(session);
    struct mi_operation *op;

    TRACE("%p %#x %p %p\n", session, flags, callbacks, operation);
    if (!operation) return;
    if (!s)
    {
        op_fail(s, flags, callbacks, operation, MI_RESULT_INVALID_PARAMETER, OP_TEST_CONNECTION);
        return;
    }
    if (!(op = op_create(s, flags, NULL, callbacks, operation, OP_TEST_CONNECTION))) return;
    op_start(op);
}

const MI_SessionFT mi_session_ft =
{
    session_Close,
    session_GetApplication,
    session_GetInstance,
    session_ModifyInstance,
    session_CreateInstance,
    session_DeleteInstance,
    session_Invoke,
    session_EnumerateInstances,
    session_QueryInstances,
    session_AssociatorInstances,
    session_ReferenceInstances,
    session_Subscribe,
    session_GetClass,
    session_EnumerateClasses,
    session_TestConnection,
};
