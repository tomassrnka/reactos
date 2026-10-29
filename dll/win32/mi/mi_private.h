/*
 * PROJECT:     ReactOS Management Infrastructure client (mi.dll)
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Private declarations
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _MI_PRIVATE_H_
#define _MI_PRIVATE_H_

#define COBJMACROS
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemcli.h>

/* mi.h declares the client function table as a dllimport; this DLL defines it (main.c) */
#define mi_clientFT_V1 mi_clientFT_V1_import
#include <mi.h>
#undef mi_clientFT_V1

#include <wine/debug.h>

/* memory */
static inline void *mi_alloc(SIZE_T size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static inline void mi_free(void *mem)
{
    if (mem) HeapFree(GetProcessHeap(), 0, mem);
}

static inline WCHAR *mi_strdup(const WCHAR *str)
{
    WCHAR *ret;
    SIZE_T len;
    if (!str) return NULL;
    len = (wcslen(str) + 1) * sizeof(WCHAR);
    if ((ret = HeapAlloc(GetProcessHeap(), 0, len))) memcpy(ret, str, len);
    return ret;
}

/* option store shared by destination, operation and subscription delivery options */
struct mi_option
{
    WCHAR *name;
    MI_Type type;
    MI_Uint32 flags;
    MI_Value value;
};

struct mi_options
{
    LONG magic;
    UINT count, size;
    struct mi_option *items;
    UINT credentials;
};

/* qualifiers, properties, methods of a class */
struct mi_qualifier
{
    WCHAR *name;
    MI_Type type;
    MI_Uint32 flags;
    MI_Value value;
};

struct mi_qualifier_set
{
    UINT count;
    struct mi_qualifier *items;
};

struct mi_property
{
    WCHAR *name;
    MI_Type type;
    MI_Uint32 flags;
    BOOL has_value;
    MI_Value value;
    WCHAR *refclass;
    struct mi_qualifier_set quals;
};

struct mi_method
{
    WCHAR *name;
    struct mi_qualifier_set quals;
    MI_Type return_type;
    struct mi_qualifier_set return_quals;
    UINT count;
    struct mi_property *params;
};

struct mi_class_data
{
    LONG magic;
    LONG refs;
    WCHAR *name;
    WCHAR *nameSpace;
    WCHAR *server;
    WCHAR *parent;
    struct mi_qualifier_set quals;
    UINT count;
    struct mi_property *props;
    UINT method_count;
    struct mi_method *methods;
    MI_ClassDecl decl;
    MI_PropertyDecl *prop_decls;
    MI_PropertyDecl **prop_decl_ptrs;
};

struct mi_class
{
    MI_Class base;
    struct mi_class_data *data;
};

struct mi_element
{
    WCHAR *name;
    MI_Type type;
    MI_Uint32 flags;
    MI_Value value;
};

struct mi_instance
{
    MI_Instance base;
    MI_ClassDecl decl;
    WCHAR *class_name;
    WCHAR *nameSpace;
    WCHAR *server;
    struct mi_class_data *cls;
    UINT count, size;
    struct mi_element *elems;
};

/* instance.c */
extern const MI_InstanceFT mi_instance_ft;
extern const MI_ClassFT mi_class_ft;
MI_Result copy_value(MI_Value *dst, const MI_Value *src, MI_Type type);
void free_value(MI_Value *value, MI_Type type);
struct mi_instance *instance_create(const WCHAR *class_name, const WCHAR *nameSpace, const WCHAR *server);
MI_Result instance_add(struct mi_instance *inst, const WCHAR *name, const MI_Value *value, MI_Type type, MI_Uint32 flags);
struct mi_element *instance_find(const struct mi_instance *inst, const WCHAR *name, UINT *index);
void instance_set_class(struct mi_instance *inst, struct mi_class_data *cls);
struct mi_instance *instance_from_mi(const MI_Instance *inst);
void instance_delete(struct mi_instance *inst);
struct mi_class_data *class_data_create(const WCHAR *name, const WCHAR *nameSpace, const WCHAR *server);
void class_data_finish(struct mi_class_data *cls);
void class_data_release(struct mi_class_data *cls);
void qualifier_set_free(struct mi_qualifier_set *set);
MI_Class *class_create(struct mi_class_data *data);
void class_delete(MI_Class *cls);
struct mi_instance *make_error_instance(MI_Result result, HRESULT hr, const WCHAR *message);
const WCHAR *result_message(MI_Result result);

/* main.c */
extern const MI_ApplicationFT mi_application_ft;
extern const MI_DestinationOptionsFT mi_destination_options_ft;
extern const MI_OperationOptionsFT mi_operation_options_ft;
const struct mi_option *options_find(const MI_OperationOptions *options, const WCHAR *name);

/* session.c */
extern const MI_SessionFT mi_session_ft;
extern const MI_OperationFT mi_operation_ft;
MI_Result session_new(MI_Application *app, const MI_Char *protocol, const MI_Char *destination,
                      MI_Instance **extendedError, MI_Session *session);
MI_Result hresult_to_mi(HRESULT hr);

#define MI_OPTIONS_MAGIC 0x4f494d21 /* "!MIO" */
#define MI_CLASSDECL_MAGIC 0x434d4921 /* "!IMC" */

#endif /* _MI_PRIVATE_H_ */
