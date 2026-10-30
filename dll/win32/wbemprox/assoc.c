/*
 * PROJECT:     ReactOS WMI provider (wbemprox)
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ASSOCIATORS OF and REFERENCES OF queries
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * An association class is a table whose reference properties hold the object paths of the
 * two instances it links (Win32_SystemProcesses links Win32_ComputerSystem and Win32_Process).
 * assoc_data.h lists, for every association class of the schema, its reference properties and
 * the class each one refers to.
 *
 * ASSOCIATORS OF {path} returns the instances at the other end of every association instance
 * that refers to the object, REFERENCES OF {path} the association instances themselves.
 * WHERE SchemaOnly on a class path answers from the schema alone: one class per association
 * class that names the class itself in a reference property, the way the PowerShell completer
 * walks a class and its superclasses. ClassDefsOnly returns the classes of the results.
 *
 * Only association classes with a provider are walked, and only their tables, the class of the
 * object and the classes of the results are filled; the file and directory providers are never
 * started for an association walk.
 */

#define COBJMACROS

#include <stdarg.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "oleauto.h"
#include "wbemcli.h"

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

struct assoc_ref
{
    const WCHAR *class;     /* association class */
    const WCHAR *role;      /* reference property */
    const WCHAR *refclass;  /* class the property refers to */
};

#include "assoc_data.h"

#define MAX_OBJECT_PATH 4096    /* longest object path accepted */
#define MAX_PATH_KEYS   16      /* most key properties in an object path */
#define MAX_RESULTS     65536   /* most instances an association query returns */

/* object paths */

struct objkey
{
    WCHAR *name;        /* NULL: Class="value", the one key of the class */
    WCHAR *value;
};

struct objpath
{
    WCHAR *class;
    UINT num_keys;
    struct objkey keys[MAX_PATH_KEYS];
    BOOL singleton;     /* Class=@ */
};

static void free_objpath( struct objpath *path )
{
    UINT i;

    heap_free( path->class );
    for (i = 0; i < path->num_keys; i++)
    {
        heap_free( path->keys[i].name );
        heap_free( path->keys[i].value );
    }
    memset( path, 0, sizeof(*path) );
}

static BOOL is_name_char( WCHAR c )
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static WCHAR *strndupW( const WCHAR *str, UINT len )
{
    WCHAR *ret;
    if (!(ret = heap_alloc( (len + 1) * sizeof(WCHAR) ))) return NULL;
    memcpy( ret, str, len * sizeof(WCHAR) );
    ret[len] = 0;
    return ret;
}

/* a key value: "quoted" or 'quoted' with backslash escapes, or a bare number or boolean */
static HRESULT parse_key_value( const WCHAR **str, WCHAR **ret )
{
    const WCHAR *p = *str;
    WCHAR *out;
    UINT len;

    if (*p == '"' || *p == '\'')
    {
        WCHAR quote = *p++;
        const WCHAR *start = p;

        for (len = 0; *p && *p != quote; p++, len++) if (*p == '\\' && p[1]) p++;
        if (*p != quote) return WBEM_E_INVALID_OBJECT_PATH;
        if (!(out = heap_alloc( (len + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
        for (p = start, len = 0; *p != quote; p++)
        {
            if (*p == '\\' && p[1]) p++;
            out[len++] = *p;
        }
        out[len] = 0;
        *str = p + 1;
        *ret = out;
        return S_OK;
    }
    for (len = 0; p[len] && p[len] != ','; len++)
        if (!is_name_char( p[len] ) && p[len] != '-' && p[len] != '.' && p[len] != '+') return WBEM_E_INVALID_OBJECT_PATH;
    if (!len) return WBEM_E_INVALID_OBJECT_PATH;
    if (!(*ret = strndupW( p, len ))) return E_OUTOFMEMORY;
    *str = p + len;
    return S_OK;
}

static BOOL is_local_server( const WCHAR *name, UINT len )
{
    WCHAR server[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = ARRAY_SIZE(server);

    if (len == 1 && name[0] == '.') return TRUE;
    if (len == 9 && !_wcsnicmp( name, L"localhost", 9 )) return TRUE;
    return GetComputerNameW( server, &size ) && size == len && !_wcsnicmp( name, server, len );
}

/* "root\cimv2" -> the namespace */
static BOOL parse_path_namespace( const WCHAR *str, UINT len, enum wbm_namespace *ns )
{
    WCHAR buf[64];

    if (len < 4 || len >= ARRAY_SIZE(buf) || _wcsnicmp( str, L"root", 4 )) return FALSE;
    if (len > 4 && str[4] != '\\' && str[4] != '/') return FALSE;
    str += 4;
    len -= 4;
    if (len)
    {
        str++;
        len--;
    }
    memcpy( buf, str, len * sizeof(WCHAR) );
    buf[len] = 0;
    return parse_namespace( buf, ns );
}

/* [\\server\namespace:]Class[.Key=value[,Key=value...] | ="value" | =@] */
static HRESULT parse_objpath( const WCHAR *str, enum wbm_namespace ns, struct objpath *path )
{
    const WCHAR *p = str, *q;
    HRESULT hr;
    UINT len;

    memset( path, 0, sizeof(*path) );
    if (!str || wcsnlen( str, MAX_OBJECT_PATH + 1 ) > MAX_OBJECT_PATH) return WBEM_E_INVALID_OBJECT_PATH;
    while (*p == ' ' || *p == '\t') p++;

    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/'))
    {
        p += 2;
        for (q = p; *q && *q != '\\' && *q != '/' && *q != ':'; q++) {}
        if (*q != '\\' && *q != '/') return WBEM_E_INVALID_OBJECT_PATH;
        if (!is_local_server( p, q - p )) return WBEM_E_NOT_SUPPORTED;
        p = q + 1;
    }
    for (q = p; *q && *q != ':' && *q != '.' && *q != '=' && *q != '"'; q++) {}
    if (*q == ':')
    {
        enum wbm_namespace path_ns;

        if (!parse_path_namespace( p, q - p, &path_ns )) return WBEM_E_INVALID_NAMESPACE;
        /* the provider's views of a namespace: an object of another one is not here */
        if (path_ns != ns && !((path_ns == WBEMPROX_NAMESPACE_CIMV2 || path_ns == WBEMPROX_NAMESPACE_DEFAULT) &&
                               (ns == WBEMPROX_NAMESPACE_CIMV2 || ns == WBEMPROX_NAMESPACE_DEFAULT)))
            return WBEM_E_INVALID_NAMESPACE;
        p = q + 1;
    }

    for (len = 0; is_name_char( p[len] ); len++) {}
    if (!len) return WBEM_E_INVALID_OBJECT_PATH;
    if (!(path->class = strndupW( p, len ))) return E_OUTOFMEMORY;
    p += len;

    if (*p == '=')
    {
        p++;
        if (*p == '@')
        {
            path->singleton = TRUE;
            p++;
        }
        else
        {
            if ((hr = parse_key_value( &p, &path->keys[0].value )) != S_OK) goto error;
            path->num_keys = 1;
        }
    }
    else if (*p == '.')
    {
        p++;
        for (;;)
        {
            struct objkey *key;
            UINT i;

            if (path->num_keys == MAX_PATH_KEYS) goto invalid;
            for (len = 0; is_name_char( p[len] ); len++) {}
            if (!len || p[len] != '=') goto invalid;
            for (i = 0; i < path->num_keys; i++)
                if (wcslen( path->keys[i].name ) == len && !_wcsnicmp( path->keys[i].name, p, len )) goto invalid;
            key = &path->keys[path->num_keys];
            if (!(key->name = strndupW( p, len ))) goto oom;
            path->num_keys++;
            p += len + 1;
            if ((hr = parse_key_value( &p, &key->value )) != S_OK) goto error;
            if (*p != ',') break;
            p++;
        }
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) goto invalid;
    return S_OK;

invalid:
    hr = WBEM_E_INVALID_OBJECT_PATH;
    goto error;
oom:
    hr = E_OUTOFMEMORY;
error:
    free_objpath( path );
    return hr;
}

static BOOL is_instance_path( const struct objpath *path )
{
    return path->singleton || path->num_keys;
}

static UINT count_keys( const struct table *table )
{
    UINT i, count = 0;
    for (i = 0; i < table->num_cols; i++) if (table->columns[i].type & COL_FLAG_KEY) count++;
    return count;
}

/* do the keys of the path name the keys of the class */
static BOOL check_path_keys( const struct table *table, const struct objpath *path )
{
    UINT i, column, num_keys = count_keys( table );

    /* a class without keys has one instance, every path names it; an abstract one has none */
    if (!num_keys) return table->fill || table->num_rows;
    if (path->singleton) return FALSE;
    if (path->num_keys == 1 && !path->keys[0].name) return num_keys == 1;
    if (path->num_keys != num_keys) return FALSE;
    for (i = 0; i < path->num_keys; i++)
    {
        if (get_column_index( table, path->keys[i].name, &column ) != S_OK) return FALSE;
        if (!(table->columns[column].type & COL_FLAG_KEY) || is_method( table, column )) return FALSE;
    }
    return TRUE;
}

/* the value of a key column the way an object path writes it, unquoted */
static WCHAR *get_key_string( const struct table *table, UINT row, UINT column )
{
    WCHAR buf[32];
    LONGLONG val;

    if (table->columns[column].type & CIM_FLAG_ARRAY) return NULL;
    if (get_value( table, row, column, &val ) != S_OK) return NULL;
    switch (table->columns[column].type & CIM_TYPE_MASK)
    {
    case CIM_STRING:
    case CIM_DATETIME:
    case CIM_REFERENCE:
        return heap_strdupW( val ? (const WCHAR *)(INT_PTR)val : L"" ); /* a key without a value: empty */
    case CIM_BOOLEAN:
        return heap_strdupW( val ? L"TRUE" : L"FALSE" );
    case CIM_SINT8:
    case CIM_SINT16:
    case CIM_SINT32:
    case CIM_SINT64:
        swprintf( buf, L"%I64d", val );
        return heap_strdupW( buf );
    case CIM_UINT8:
    case CIM_UINT16:
    case CIM_UINT32:
    case CIM_UINT64:
        swprintf( buf, L"%I64u", val );
        return heap_strdupW( buf );
    default:
        return NULL;
    }
}

static const struct objkey *find_key( const struct objpath *path, const WCHAR *name )
{
    UINT i;
    if (path->num_keys == 1 && !path->keys[0].name) return &path->keys[0];
    for (i = 0; i < path->num_keys; i++) if (!wcsicmp( path->keys[i].name, name )) return &path->keys[i];
    return NULL;
}

/* does the row hold the instance the path names; string keys compare as WMI does, ignoring case */
static BOOL match_row_path( const struct table *table, UINT row, const struct objpath *path )
{
    UINT i;

    for (i = 0; i < table->num_cols; i++)
    {
        const struct objkey *key;
        WCHAR *value;
        BOOL match;

        if (!(table->columns[i].type & COL_FLAG_KEY) || is_method( table, i )) continue;
        if (!(key = find_key( path, table->columns[i].name ))) return FALSE;
        if (!(value = get_key_string( table, row, i ))) return FALSE;
        match = !wcsicmp( value, key->value );
        heap_free( value );
        if (!match) return FALSE;
    }
    return TRUE;
}

static UINT escaped_len( const WCHAR *str )
{
    UINT len = 0;
    for (; *str; str++) len += (*str == '"' || *str == '\\') ? 2 : 1;
    return len;
}

static WCHAR *append_escaped( WCHAR *p, const WCHAR *str )
{
    *p++ = '"';
    for (; *str; str++)
    {
        if (*str == '"' || *str == '\\') *p++ = '\\';
        *p++ = *str;
    }
    *p++ = '"';
    return p;
}

/* \\SERVER\root\cimv2: */
static UINT path_prefix( WCHAR *buf, UINT size )
{
    WCHAR server[MAX_COMPUTERNAME_LENGTH + 1], *p;
    DWORD len = ARRAY_SIZE(server);

    if (!GetComputerNameW( server, &len ) || len + 15 > size) return 0;
    for (p = server; *p; p++) *p = towupper( *p );
    wcscpy( buf, L"\\\\" );
    wcscat( buf, server );
    wcscat( buf, L"\\root\\cimv2:" );
    return wcslen( buf );
}

static WCHAR *append_str( WCHAR *p, const WCHAR *str )
{
    UINT len = wcslen( str );
    memcpy( p, str, len * sizeof(WCHAR) );
    return p + len;
}

/* \\SERVER\root\cimv2:Class.Key="value" as WMI writes the reference properties of an association */
WCHAR *build_reference_path( const WCHAR *class, const WCHAR *key, const WCHAR *value )
{
    WCHAR prefix[MAX_COMPUTERNAME_LENGTH + 32], *ret, *p;
    UINT len;

    if (!(len = path_prefix( prefix, ARRAY_SIZE(prefix) ))) return NULL;
    len += wcslen( class ) + 3;                                 /* "=@" or ".", and the terminator */
    if (key) len += wcslen( key ) + escaped_len( value ) + 3;   /* "=" and the quotes */
    if (!(ret = heap_alloc( len * sizeof(WCHAR) ))) return NULL;
    p = append_str( ret, prefix );
    p = append_str( p, class );
    if (key)
    {
        *p++ = '.';
        p = append_str( p, key );
        *p++ = '=';
        p = append_escaped( p, value );
    }
    else
    {
        *p++ = '=';
        *p++ = '@';
    }
    *p = 0;
    return ret;
}

/* the object path of the instance in a row, keys in column order */
WCHAR *build_row_reference_path( const struct table *table, UINT row )
{
    WCHAR prefix[MAX_COMPUTERNAME_LENGTH + 32], **values, *ret = NULL, *p;
    UINT i, len, num_keys = count_keys( table ), k = 0;

    if (!(len = path_prefix( prefix, ARRAY_SIZE(prefix) ))) return NULL;
    if (!num_keys) return build_reference_path( table->name, NULL, NULL );
    if (!(values = heap_alloc_zero( num_keys * sizeof(*values) ))) return NULL;

    len += wcslen( table->name ) + 1;
    for (i = 0; i < table->num_cols && k < num_keys; i++)
    {
        UINT type = table->columns[i].type & CIM_TYPE_MASK;
        if (!(table->columns[i].type & COL_FLAG_KEY) || is_method( table, i )) continue;
        if (!(values[k] = get_key_string( table, row, i ))) goto done;
        len += wcslen( table->columns[i].name ) + 2;            /* "." or ",", and "=" */
        if (type == CIM_STRING || type == CIM_DATETIME || type == CIM_REFERENCE) len += escaped_len( values[k] ) + 2;
        else len += wcslen( values[k] );
        k++;
    }
    if (!(ret = heap_alloc( len * sizeof(WCHAR) ))) goto done;
    p = append_str( ret, prefix );
    p = append_str( p, table->name );
    for (i = 0, k = 0; i < table->num_cols && k < num_keys; i++)
    {
        UINT type = table->columns[i].type & CIM_TYPE_MASK;
        if (!(table->columns[i].type & COL_FLAG_KEY) || is_method( table, i )) continue;
        *p++ = k ? ',' : '.';
        p = append_str( p, table->columns[i].name );
        *p++ = '=';
        if (type == CIM_STRING || type == CIM_DATETIME || type == CIM_REFERENCE) p = append_escaped( p, values[k] );
        else p = append_str( p, values[k] );
        k++;
    }
    *p = 0;

done:
    for (i = 0; i < num_keys; i++) heap_free( values[i] );
    heap_free( values );
    return ret;
}

/* two object paths of this provider name the same instance */
static BOOL same_object( const struct objpath *a, const struct objpath *b )
{
    UINT i;

    if (wcsicmp( a->class, b->class )) return FALSE;
    if (a->num_keys != b->num_keys) return FALSE;
    for (i = 0; i < a->num_keys; i++)
    {
        const struct objkey *key = find_key( b, a->keys[i].name ? a->keys[i].name : L"" );
        if (!key || wcsicmp( key->value, a->keys[i].value )) return FALSE;
    }
    return TRUE;
}

/* the query */

struct assoc_query
{
    BOOL references;
    const WCHAR *assoc_class;
    const WCHAR *result_class;
    const WCHAR *role;
    const WCHAR *result_role;
    const WCHAR *required_qualifier;
    const WCHAR *required_assoc_qualifier;
    BOOL class_defs_only;
    BOOL schema_only;
};

static HRESULT parse_keywords( const struct keyword *keyword, BOOL references, struct assoc_query *aq )
{
    static const struct
    {
        const WCHAR *name;
        BOOL value;
        BOOL references;    /* valid in REFERENCES OF too */
        SIZE_T offset;
    }
    keywords[] =
    {
        { L"AssocClass", TRUE, FALSE, FIELD_OFFSET(struct assoc_query, assoc_class) },
        { L"ResultClass", TRUE, TRUE, FIELD_OFFSET(struct assoc_query, result_class) },
        { L"Role", TRUE, TRUE, FIELD_OFFSET(struct assoc_query, role) },
        { L"ResultRole", TRUE, FALSE, FIELD_OFFSET(struct assoc_query, result_role) },
        { L"RequiredQualifier", TRUE, TRUE, FIELD_OFFSET(struct assoc_query, required_qualifier) },
        { L"RequiredAssocQualifier", TRUE, FALSE, FIELD_OFFSET(struct assoc_query, required_assoc_qualifier) },
        { L"ClassDefsOnly", FALSE, TRUE, FIELD_OFFSET(struct assoc_query, class_defs_only) },
        { L"SchemaOnly", FALSE, TRUE, FIELD_OFFSET(struct assoc_query, schema_only) },
    };
    UINT seen = 0, i, count = 0;

    memset( aq, 0, sizeof(*aq) );
    aq->references = references;
    for (; keyword; keyword = keyword->next)
    {
        /* the parser builds the list from the query text; it cannot be longer than that */
        if (++count > 64) return WBEM_E_INVALID_QUERY;
        for (i = 0; i < ARRAY_SIZE(keywords); i++) if (!wcsicmp( keyword->name, keywords[i].name )) break;
        if (i == ARRAY_SIZE(keywords) || (references && !keywords[i].references)) return WBEM_E_INVALID_QUERY;
        if (seen & (1 << i)) return WBEM_E_INVALID_QUERY;
        seen |= 1 << i;
        if (keywords[i].value)
        {
            if (!keyword->value || !keyword->value[0]) return WBEM_E_INVALID_QUERY;
            *(const WCHAR **)((char *)aq + keywords[i].offset) = keyword->value;
        }
        else
        {
            if (keyword->value) return WBEM_E_INVALID_QUERY;
            *(BOOL *)((char *)aq + keywords[i].offset) = TRUE;
        }
    }
    if (aq->schema_only && aq->class_defs_only) return WBEM_E_INVALID_QUERY;
    /* WMI takes the class definitions of every result, not of some */
    if (aq->class_defs_only && aq->result_class) return WBEM_E_INVALID_QUERY;
    return S_OK;
}

/* class is name or derives from it */
static BOOL is_class_or_subclass( const WCHAR *class, const WCHAR *name )
{
    return !wcsicmp( class, name ) || is_subclass( class, name );
}

static struct table *grab_visible_table( const WCHAR *name, enum wbm_namespace ns )
{
    struct table *table = grab_table( name );
    if (table && !is_table_visible( table->name, ns ))
    {
        release_table( table );
        table = NULL;
    }
    return table;
}

/* a table with a provider or with static rows: an abstract class has neither */
static BOOL has_instances( const struct table *table )
{
    return table->fill || table->num_rows;
}

/* the qualifiers this provider can tell: every association class has Association, every class
 * with instances Dynamic and Provider */
static BOOL has_qualifier( const WCHAR *class, const WCHAR *qualifier, BOOL association )
{
    struct table *table;
    BOOL ret = FALSE;

    if (association && !wcsicmp( qualifier, L"Association" )) return TRUE;
    if (wcsicmp( qualifier, L"Dynamic" ) && wcsicmp( qualifier, L"Provider" )) return FALSE;
    if ((table = grab_table( class )))
    {
        ret = table->fill != NULL;
        release_table( table );
    }
    return ret;
}

/* the reference properties of the association class at assoc_refs[first] */
static UINT get_assoc_roles( UINT first )
{
    UINT count = 1;
    while (first + count < ARRAY_SIZE(assoc_refs) && !wcsicmp( assoc_refs[first + count].class, assoc_refs[first].class ))
        count++;
    return count;
}

/* growing lists */

struct name_list
{
    const WCHAR **names;
    UINT count;
    UINT size;
};

static HRESULT add_name( struct name_list *list, const WCHAR *name )
{
    UINT i;

    for (i = 0; i < list->count; i++) if (!wcsicmp( list->names[i], name )) return S_OK;
    if (list->count == list->size)
    {
        UINT size = list->size ? list->size * 2 : 16;
        const WCHAR **tmp;
        if (!(tmp = heap_realloc( list->names, size * sizeof(*tmp) ))) return E_OUTOFMEMORY;
        list->names = tmp;
        list->size = size;
    }
    list->names[list->count++] = name;
    return S_OK;
}

/* the class objects of a list of classes: SchemaOnly and ClassDefsOnly results */
static HRESULT create_class_objects( struct view *view, const WCHAR *const *names, UINT count )
{
    IWbemClassObject **objs = NULL;
    UINT i, done = 0;
    HRESULT hr = S_OK;

    if (count && !(objs = heap_alloc_zero( count * sizeof(*objs) ))) return E_OUTOFMEMORY;
    for (i = 0; i < count; i++)
        if (create_class_view_object( view->ns, names[i], &objs[done] ) == S_OK) done++;
    hr = EnumWbemClassObjectArray_create( objs, done, &view->objects );
    if (hr != S_OK) for (i = 0; i < done; i++) IWbemClassObject_Release( objs[i] );
    heap_free( objs );
    return hr;
}

/* WHERE SchemaOnly: the association classes that name the class in a reference property */
static HRESULT exec_schema_query( struct view *view, const struct assoc_query *aq, const WCHAR *class )
{
    struct name_list results = { NULL, 0, 0 };
    struct table *table;
    UINT first, roles, i, j;
    HRESULT hr = S_OK;

    if (!(table = grab_visible_table( class, view->ns ))) return WBEM_E_NOT_FOUND;
    release_table( table );

    for (first = 0; first < ARRAY_SIZE(assoc_refs) && hr == S_OK; first += roles)
    {
        const WCHAR *assoc = assoc_refs[first].class, *result = NULL;

        roles = get_assoc_roles( first );
        if (aq->assoc_class && !is_class_or_subclass( assoc, aq->assoc_class )) continue;
        if (aq->required_assoc_qualifier && !has_qualifier( assoc, aq->required_assoc_qualifier, TRUE )) continue;
        if (aq->references && aq->result_class && !is_class_or_subclass( assoc, aq->result_class )) continue;
        if (aq->references && aq->required_qualifier && !has_qualifier( assoc, aq->required_qualifier, TRUE )) continue;
        if (!(table = grab_visible_table( assoc, view->ns ))) continue;
        release_table( table );

        for (i = first; i < first + roles && !result; i++)
        {
            if (wcsicmp( assoc_refs[i].refclass, class )) continue;
            if (aq->role && wcsicmp( assoc_refs[i].role, aq->role )) continue;
            if (aq->references)
            {
                result = assoc;
                break;
            }
            for (j = first; j < first + roles; j++)
            {
                const WCHAR *other = assoc_refs[j].refclass;
                if (j == i) continue;
                if (aq->result_role && wcsicmp( assoc_refs[j].role, aq->result_role )) continue;
                if (aq->result_class && !is_class_or_subclass( other, aq->result_class )) continue;
                if (aq->required_qualifier && !has_qualifier( other, aq->required_qualifier, FALSE )) continue;
                if (!(table = grab_visible_table( other, view->ns ))) continue;
                release_table( table );
                result = other;
                break;
            }
        }
        if (!result) continue;
        /* one result per association class: a class associated twice is listed twice */
        if (results.count == results.size)
        {
            UINT size = results.size ? results.size * 2 : 16;
            const WCHAR **tmp;
            if (!(tmp = heap_realloc( results.names, size * sizeof(*tmp) )))
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            results.names = tmp;
            results.size = size;
        }
        results.names[results.count++] = result;
    }
    if (hr == S_OK) hr = create_class_objects( view, results.names, results.count );
    heap_free( results.names );
    return hr;
}

/* instance results */

struct assoc_results
{
    UINT count;
    UINT size;
    UINT *rows;
    UINT *tables;       /* index into view->table */
};

static HRESULT add_view_table( struct view *view, struct table *table, UINT *index )
{
    struct table **tmp;
    UINT i;

    for (i = 0; i < view->table_count; i++)
    {
        if (view->table[i] == table)
        {
            *index = i;
            return S_OK;
        }
    }
    if (!(tmp = heap_realloc( view->table, (view->table_count + 1) * sizeof(*tmp) ))) return E_OUTOFMEMORY;
    view->table = tmp;
    view->table[view->table_count] = addref_table( table );
    *index = view->table_count++;
    return S_OK;
}

static HRESULT add_result( struct assoc_results *results, UINT table, UINT row )
{
    UINT i;

    for (i = 0; i < results->count; i++)
        if (results->tables[i] == table && results->rows[i] == row) return S_OK;
    if (results->count >= MAX_RESULTS) return S_FALSE;
    if (results->count == results->size)
    {
        UINT size = results->size ? results->size * 2 : 16, *rows, *tables;
        if (!(rows = heap_realloc( results->rows, size * sizeof(*rows) ))) return E_OUTOFMEMORY;
        results->rows = rows;
        if (!(tables = heap_realloc( results->tables, size * sizeof(*tables) ))) return E_OUTOFMEMORY;
        results->tables = tables;
        results->size = size;
    }
    results->rows[results->count] = row;
    results->tables[results->count] = table;
    results->count++;
    return S_OK;
}

/* the instances at the other end of the associations */
struct endpoint
{
    struct objpath path;
};

struct endpoint_list
{
    struct endpoint *items;
    UINT count;
    UINT size;
};

static HRESULT add_endpoint( struct endpoint_list *list, const WCHAR *str, enum wbm_namespace ns )
{
    struct objpath path;
    UINT i;

    /* a reference this provider cannot read names nothing it could return */
    if (parse_objpath( str, ns, &path ) != S_OK) return S_OK;
    for (i = 0; i < list->count; i++)
    {
        if (same_object( &list->items[i].path, &path ))
        {
            free_objpath( &path );
            return S_OK;
        }
    }
    if (list->count >= MAX_RESULTS)
    {
        free_objpath( &path );
        return S_FALSE;
    }
    if (list->count == list->size)
    {
        UINT size = list->size ? list->size * 2 : 16;
        struct endpoint *tmp;
        if (!(tmp = heap_realloc( list->items, size * sizeof(*tmp) )))
        {
            free_objpath( &path );
            return E_OUTOFMEMORY;
        }
        list->items = tmp;
        list->size = size;
    }
    list->items[list->count++].path = path;
    return S_OK;
}

static void free_endpoints( struct endpoint_list *list )
{
    UINT i;
    for (i = 0; i < list->count; i++) free_objpath( &list->items[i].path );
    heap_free( list->items );
}

/* fill a table once for the walk; a failed provider has no instances */
static BOOL fill_for_walk( struct table *table )
{
    if (!table->fill) return TRUE;
    clear_table( table );
    return table->fill( table, NULL ) != FILL_STATUS_FAILED;
}

/* find the instance a path names: in its class, or for an abstract class in a subclass. Returns
 * its canonical path, the one association instances use. */
static HRESULT resolve_object( struct view *view, struct table *class, const struct objpath *path,
                               struct objpath *found )
{
    struct table *table;
    WCHAR *str = NULL;
    HRESULT hr;
    UINT row;

    LIST_FOR_EACH_ENTRY( table, table_list, struct table, entry )
    {
        if (table != class && !is_subclass( table->name, class->name )) continue;
        if (!is_table_visible( table->name, view->ns ) || !has_instances( table )) continue;
        /* the file and directory providers walk every fixed drive when nothing narrows them down */
        if (is_disk_walk_table( table )) continue;
        if (!check_path_keys( table, path )) continue;
        if (!fill_for_walk( table )) continue;
        for (row = 0; row < table->num_rows; row++)
        {
            if (!match_row_path( table, row, path )) continue;
            if (!(str = build_row_reference_path( table, row ))) return E_OUTOFMEMORY;
            break;
        }
        if (str) break;
    }
    if (!str) return WBEM_E_NOT_FOUND;
    hr = parse_objpath( str, WBEMPROX_NAMESPACE_CIMV2, found );
    heap_free( str );
    return hr;
}

/* walk one association class: the instances referring to the object */
static HRESULT walk_association( struct view *view, const struct assoc_query *aq, UINT first, UINT roles,
                                 const struct objpath *object, struct assoc_results *results,
                                 struct endpoint_list *endpoints, struct name_list *classes )
{
    const WCHAR *assoc = assoc_refs[first].class;
    UINT columns[MAX_PATH_KEYS], i, j, row, index = 0;
    BOOL match[MAX_PATH_KEYS], any = FALSE;
    struct table *table;
    HRESULT hr = S_OK;

    if (roles > MAX_PATH_KEYS) return S_OK;
    if (aq->assoc_class && !is_class_or_subclass( assoc, aq->assoc_class )) return S_OK;
    if (aq->required_assoc_qualifier && !has_qualifier( assoc, aq->required_assoc_qualifier, TRUE )) return S_OK;
    if (aq->references)
    {
        if (aq->result_class && !is_class_or_subclass( assoc, aq->result_class )) return S_OK;
        if (aq->required_qualifier && !has_qualifier( assoc, aq->required_qualifier, TRUE )) return S_OK;
    }
    for (i = 0; i < roles; i++)
    {
        const struct assoc_ref *ref = &assoc_refs[first + i];
        match[i] = is_class_or_subclass( object->class, ref->refclass ) && (!aq->role || !wcsicmp( ref->role, aq->role ));
        any |= match[i];
    }
    if (!any) return S_OK;
    if (!(table = grab_visible_table( assoc, view->ns ))) return S_OK;
    if (!has_instances( table ) || is_disk_walk_table( table )) goto done;
    for (i = 0; i < roles; i++)
    {
        if (get_column_index( table, assoc_refs[first + i].role, &columns[i] ) != S_OK) goto done;
        if ((table->columns[columns[i]].type & CIM_TYPE_MASK) != CIM_REFERENCE) goto done;
    }
    if (!fill_for_walk( table )) goto done;
    if (aq->references && !aq->class_defs_only && (hr = add_view_table( view, table, &index )) != S_OK) goto done;

    for (row = 0; row < table->num_rows && hr == S_OK; row++)
    {
        for (i = 0; i < roles && hr == S_OK; i++)
        {
            struct objpath ref;
            LONGLONG val;
            BOOL same;

            if (!match[i]) continue;
            if (get_value( table, row, columns[i], &val ) != S_OK || !val) continue;
            if (parse_objpath( (const WCHAR *)(INT_PTR)val, view->ns, &ref ) != S_OK) continue;
            same = same_object( &ref, object );
            free_objpath( &ref );
            if (!same) continue;

            if (aq->references)
            {
                if (aq->class_defs_only) hr = add_name( classes, table->name );
                else hr = add_result( results, index, row );
                break;
            }
            for (j = 0; j < roles && hr == S_OK; j++)
            {
                if (j == i) continue;
                if (aq->result_role && wcsicmp( assoc_refs[first + j].role, aq->result_role )) continue;
                if (get_value( table, row, columns[j], &val ) != S_OK || !val) continue;
                hr = add_endpoint( endpoints, (const WCHAR *)(INT_PTR)val, view->ns );
            }
        }
    }
    if (hr == S_FALSE)
    {
        WARN( "association walk capped at %u results\n", MAX_RESULTS );
        hr = S_OK;
    }

done:
    release_table( table );
    return hr;
}

/* the instances of the endpoints, one fill per class */
static HRESULT resolve_endpoints( struct view *view, const struct assoc_query *aq, struct endpoint_list *endpoints,
                                  struct assoc_results *results, struct name_list *classes )
{
    UINT i, j, row, index;
    HRESULT hr = S_OK;
    BOOL *done;

    if (!endpoints->count) return S_OK;
    if (!(done = heap_alloc_zero( endpoints->count * sizeof(*done) ))) return E_OUTOFMEMORY;
    for (i = 0; i < endpoints->count && hr == S_OK; i++)
    {
        const WCHAR *class = endpoints->items[i].path.class;
        struct table *table;

        if (done[i]) continue;
        for (j = i; j < endpoints->count; j++)
            if (!wcsicmp( endpoints->items[j].path.class, class )) done[j] = TRUE;
        if (aq->result_class && !is_class_or_subclass( class, aq->result_class )) continue;
        if (aq->required_qualifier && !has_qualifier( class, aq->required_qualifier, FALSE )) continue;
        if (!(table = grab_visible_table( class, view->ns ))) continue;
        if (aq->class_defs_only)
        {
            hr = add_name( classes, table->name );
            release_table( table );
            continue;
        }
        if (!has_instances( table ) || is_disk_walk_table( table ) || !fill_for_walk( table ) ||
            (hr = add_view_table( view, table, &index )) != S_OK)
        {
            release_table( table );
            continue;
        }
        for (j = i; j < endpoints->count && hr == S_OK; j++)
        {
            const struct objpath *path = &endpoints->items[j].path;
            if (wcsicmp( path->class, class ) || !check_path_keys( table, path )) continue;
            for (row = 0; row < table->num_rows; row++)
            {
                if (!match_row_path( table, row, path )) continue;
                hr = add_result( results, index, row );
                break;
            }
        }
        release_table( table );
    }
    heap_free( done );
    if (hr == S_FALSE) hr = S_OK;
    return hr;
}

static HRESULT exec_instance_query( struct view *view, const struct assoc_query *aq, struct table *class,
                                    const struct objpath *path )
{
    struct assoc_results results = { 0, 0, NULL, NULL };
    struct endpoint_list endpoints = { NULL, 0, 0 };
    struct name_list classes = { NULL, 0, 0 };
    struct objpath object;
    UINT first, roles;
    HRESULT hr;

    /* nothing this provider associates walks the file system */
    if (is_disk_walk_table( class )) return S_OK;

    lock_tables();
    if ((hr = resolve_object( view, class, path, &object )) != S_OK) goto done;

    for (first = 0; first < ARRAY_SIZE(assoc_refs) && hr == S_OK; first += roles)
    {
        roles = get_assoc_roles( first );
        hr = walk_association( view, aq, first, roles, &object, &results, &endpoints, &classes );
    }
    if (hr == S_OK && !aq->references) hr = resolve_endpoints( view, aq, &endpoints, &results, &classes );
    free_objpath( &object );

    if (hr == S_OK && aq->class_defs_only) hr = create_class_objects( view, classes.names, classes.count );
    else if (hr == S_OK)
    {
        view->result = results.rows;
        view->result_table = results.tables;
        view->result_count = results.count;
        results.rows = results.tables = NULL;
    }

done:
    unlock_tables();
    heap_free( results.rows );
    heap_free( results.tables );
    heap_free( classes.names );
    free_endpoints( &endpoints );
    return hr;
}

HRESULT exec_assoc_query( struct view *view )
{
    struct assoc_query aq;
    struct objpath path;
    HRESULT hr;

    if ((hr = parse_keywords( view->keywordlist, view->references, &aq )) != S_OK) return hr;
    if ((hr = parse_objpath( view->path, view->ns, &path )) != S_OK) return hr;

    if (!is_instance_path( &path ))
    {
        /* a class has no associated instances; its schema does */
        if (aq.class_defs_only) hr = WBEM_E_INVALID_QUERY;
        else if (aq.schema_only) hr = exec_schema_query( view, &aq, path.class );
    }
    else
    {
        struct table *class;

        if (!(class = grab_visible_table( path.class, view->ns ))) hr = WBEM_E_INVALID_CLASS;
        else
        {
            if (!check_path_keys( class, &path )) hr = WBEM_E_INVALID_OBJECT_PATH;
            else if (aq.schema_only) hr = WBEM_E_INVALID_QUERY;
            else hr = exec_instance_query( view, &aq, class, &path );
            release_table( class );
        }
    }

    free_objpath( &path );
    return hr;
}
