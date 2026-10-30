/*
 * PROJECT:     ReactOS Management Infrastructure client (mi.dll)
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     MI_Value, MI_Instance, MI_Class, qualifier and parameter sets
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mi_private.h"
#include <wchar.h>
#include <wctype.h>

WINE_DEFAULT_DEBUG_CHANNEL(mi);

/* MI_Value deep copy / free */

static SIZE_T element_size(MI_Type type)
{
    switch (type & ~MI_ARRAY)
    {
    case MI_BOOLEAN: return sizeof(MI_Boolean);
    case MI_UINT8: case MI_SINT8: return 1;
    case MI_UINT16: case MI_SINT16: case MI_CHAR16: return 2;
    case MI_UINT32: case MI_SINT32: case MI_REAL32: return 4;
    case MI_UINT64: case MI_SINT64: case MI_REAL64: return 8;
    case MI_DATETIME: return sizeof(MI_Datetime);
    case MI_STRING: return sizeof(MI_Char *);
    case MI_REFERENCE: case MI_INSTANCE: return sizeof(MI_Instance *);
    }
    return 0;
}

static MI_Instance *clone_instance(const MI_Instance *src)
{
    MI_Instance *ret = NULL;
    if (!src || !src->ft || !src->ft->Clone) return NULL;
    if (src->ft->Clone(src, &ret) != MI_RESULT_OK) return NULL;
    return ret;
}

MI_Result copy_value(MI_Value *dst, const MI_Value *src, MI_Type type)
{
    SIZE_T size;
    MI_Uint32 i;

    memset(dst, 0, sizeof(*dst));
    if (!src) return MI_RESULT_OK;

    if (!(type & MI_ARRAY))
    {
        switch (type)
        {
        case MI_STRING:
            if (src->string && !(dst->string = mi_strdup(src->string))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
            return MI_RESULT_OK;
        case MI_REFERENCE:
        case MI_INSTANCE:
            if (src->instance && !(dst->instance = clone_instance(src->instance))) return MI_RESULT_FAILED;
            return MI_RESULT_OK;
        default:
            *dst = *src;
            return MI_RESULT_OK;
        }
    }

    size = element_size(type);
    if (!size) return MI_RESULT_INVALID_PARAMETER;
    if (!src->array.size || !src->array.data) { dst->array.data = NULL; return MI_RESULT_OK; }
    /* size * count must fit: array.size is a caller-controlled 32-bit count */
    if (src->array.size > ((SIZE_T)-1) / size) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    dst->array.size = src->array.size;
    if (!(dst->array.data = mi_alloc(size * src->array.size)))
    {
        dst->array.size = 0;
        return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    }

    switch (type & ~MI_ARRAY)
    {
    case MI_STRING:
        for (i = 0; i < src->array.size; i++)
        {
            if (src->stringa.data[i] && !(dst->stringa.data[i] = mi_strdup(src->stringa.data[i])))
            {
                free_value(dst, type);
                return MI_RESULT_SERVER_LIMITS_EXCEEDED;
            }
        }
        break;
    case MI_REFERENCE:
    case MI_INSTANCE:
        for (i = 0; i < src->array.size; i++)
        {
            if (src->instancea.data[i] && !(dst->instancea.data[i] = clone_instance(src->instancea.data[i])))
            {
                free_value(dst, type);
                return MI_RESULT_FAILED;
            }
        }
        break;
    default:
        memcpy(dst->array.data, src->array.data, size * src->array.size);
        break;
    }
    return MI_RESULT_OK;
}

void free_value(MI_Value *value, MI_Type type)
{
    MI_Uint32 i;

    if (!(type & MI_ARRAY))
    {
        switch (type)
        {
        case MI_STRING:
            mi_free(value->string);
            break;
        case MI_REFERENCE:
        case MI_INSTANCE:
            if (value->instance && value->instance->ft) value->instance->ft->Delete(value->instance);
            break;
        default:
            break;
        }
        memset(value, 0, sizeof(*value));
        return;
    }
    if (value->array.data)
    {
        switch (type & ~MI_ARRAY)
        {
        case MI_STRING:
            for (i = 0; i < value->array.size; i++) mi_free(value->stringa.data[i]);
            break;
        case MI_REFERENCE:
        case MI_INSTANCE:
            for (i = 0; i < value->array.size; i++)
                if (value->instancea.data[i] && value->instancea.data[i]->ft)
                    value->instancea.data[i]->ft->Delete(value->instancea.data[i]);
            break;
        default:
            break;
        }
        mi_free(value->array.data);
    }
    memset(value, 0, sizeof(*value));
}

static BOOL valid_type(MI_Type type)
{
    return (type & ~MI_ARRAY) <= MI_INSTANCE && (type & ~(MI_ARRAY | 0x0f)) == 0;
}

/* class data */

void qualifier_set_free(struct mi_qualifier_set *set)
{
    UINT i;
    for (i = 0; i < set->count; i++)
    {
        mi_free(set->items[i].name);
        free_value(&set->items[i].value, set->items[i].type);
    }
    mi_free(set->items);
    set->items = NULL;
    set->count = 0;
}

static void property_free(struct mi_property *prop)
{
    mi_free(prop->name);
    mi_free(prop->refclass);
    if (prop->has_value) free_value(&prop->value, prop->type);
    qualifier_set_free(&prop->quals);
}

struct mi_class_data *class_data_create(const WCHAR *name, const WCHAR *nameSpace, const WCHAR *server)
{
    struct mi_class_data *cls;
    if (!(cls = mi_alloc(sizeof(*cls)))) return NULL;
    cls->magic = MI_CLASSDECL_MAGIC;
    cls->refs = 1;
    cls->name = mi_strdup(name);
    cls->nameSpace = mi_strdup(nameSpace);
    cls->server = mi_strdup(server);
    cls->decl.flags = MI_FLAG_CLASS;
    cls->decl.code = MI_CLASSDECL_MAGIC;
    cls->decl.name = cls->name;
    return cls;
}

/* build the MI_ClassDecl view (name, properties) once the properties are known */
void class_data_finish(struct mi_class_data *cls)
{
    UINT i;

    cls->decl.name = cls->name;
    cls->decl.superClass = cls->parent;
    cls->decl.superClassDecl = cls->parent_data ? &cls->parent_data->decl : NULL;
    cls->decl.numMethods = 0;
    if (!cls->count) return;
    cls->prop_decls = mi_alloc(cls->count * sizeof(*cls->prop_decls));
    cls->prop_decl_ptrs = mi_alloc(cls->count * sizeof(*cls->prop_decl_ptrs));
    if (!cls->prop_decls || !cls->prop_decl_ptrs) return;
    for (i = 0; i < cls->count; i++)
    {
        MI_PropertyDecl *pd = &cls->prop_decls[i];
        const WCHAR *n = cls->props[i].name;
        pd->flags = cls->props[i].flags;
        pd->code = n[0] ? ((MI_Uint32)towlower(n[0]) << 16) | ((MI_Uint32)towlower(n[wcslen(n) - 1]) << 8) | (MI_Uint32)wcslen(n) : 0;
        pd->name = n;
        pd->type = cls->props[i].type;
        pd->className = cls->props[i].refclass;
        pd->origin = cls->name;
        pd->propagator = cls->name;
        cls->prop_decl_ptrs[i] = pd;
    }
    cls->decl.properties = (const MI_PropertyDecl * const *)cls->prop_decl_ptrs;
    cls->decl.numProperties = cls->count;
}

void class_data_release(struct mi_class_data *cls)
{
    UINT i, j;

    if (!cls || InterlockedDecrement(&cls->refs)) return;
    for (i = 0; i < cls->count; i++) property_free(&cls->props[i]);
    mi_free(cls->props);
    for (i = 0; i < cls->method_count; i++)
    {
        struct mi_method *m = &cls->methods[i];
        mi_free(m->name);
        qualifier_set_free(&m->quals);
        qualifier_set_free(&m->return_quals);
        for (j = 0; j < m->count; j++) property_free(&m->params[j]);
        mi_free(m->params);
    }
    mi_free(cls->methods);
    qualifier_set_free(&cls->quals);
    mi_free(cls->prop_decls);
    mi_free(cls->prop_decl_ptrs);
    mi_free(cls->name);
    mi_free(cls->nameSpace);
    mi_free(cls->server);
    mi_free(cls->parent);
    class_data_release(cls->parent_data);
    mi_free(cls);
}

/* MI_QualifierSet over a struct mi_qualifier_set (reserved2) */

static const MI_QualifierSetFT mi_qualifier_set_ft;

static void fill_qualifier_set(MI_QualifierSet *out, const struct mi_qualifier_set *set)
{
    if (!out) return;
    out->reserved1 = 0;
    out->reserved2 = (ptrdiff_t)set;
    out->ft = &mi_qualifier_set_ft;
}

static const struct mi_qualifier_set *get_qualifier_set(const MI_QualifierSet *self)
{
    if (!self || self->ft != &mi_qualifier_set_ft) return NULL;
    return (const struct mi_qualifier_set *)self->reserved2;
}

static MI_Result MI_CALL qualset_GetQualifierCount(const MI_QualifierSet *self, MI_Uint32 *count)
{
    const struct mi_qualifier_set *set = get_qualifier_set(self);
    if (!set || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = set->count;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL qualset_GetQualifierAt(const MI_QualifierSet *self, MI_Uint32 index, const MI_Char **name,
                                                MI_Type *type, MI_Uint32 *flags, MI_Value *value)
{
    const struct mi_qualifier_set *set = get_qualifier_set(self);
    const struct mi_qualifier *q;
    if (!set) return MI_RESULT_INVALID_PARAMETER;
    if (index >= set->count) return MI_RESULT_NOT_FOUND;
    q = &set->items[index];
    if (name) *name = q->name;
    if (type) *type = q->type;
    if (flags) *flags = q->flags;
    if (value) *value = q->value;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL qualset_GetQualifier(const MI_QualifierSet *self, const MI_Char *name, MI_Type *type,
                                              MI_Uint32 *flags, MI_Value *value, MI_Uint32 *index)
{
    const struct mi_qualifier_set *set = get_qualifier_set(self);
    UINT i;
    if (!set || !name) return MI_RESULT_INVALID_PARAMETER;
    for (i = 0; i < set->count; i++)
    {
        if (_wcsicmp(set->items[i].name, name)) continue;
        if (type) *type = set->items[i].type;
        if (flags) *flags = set->items[i].flags;
        if (value) *value = set->items[i].value;
        if (index) *index = i;
        return MI_RESULT_OK;
    }
    return MI_RESULT_NOT_FOUND;
}

static const MI_QualifierSetFT mi_qualifier_set_ft =
{
    qualset_GetQualifierCount,
    qualset_GetQualifierAt,
    qualset_GetQualifier,
};

static const struct mi_qualifier_set empty_qualifier_set;

/* MI_ParameterSet over a struct mi_method (reserved2) */

static const MI_ParameterSetFT mi_parameter_set_ft;

static const struct mi_method *get_method(const MI_ParameterSet *self)
{
    if (!self || self->ft != &mi_parameter_set_ft) return NULL;
    return (const struct mi_method *)self->reserved2;
}

static MI_Result MI_CALL paramset_GetMethodReturnType(const MI_ParameterSet *self, MI_Type *type, MI_QualifierSet *quals)
{
    const struct mi_method *m = get_method(self);
    if (!m || !type) return MI_RESULT_INVALID_PARAMETER;
    *type = m->return_type;
    fill_qualifier_set(quals, &m->return_quals);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL paramset_GetParameterCount(const MI_ParameterSet *self, MI_Uint32 *count)
{
    const struct mi_method *m = get_method(self);
    if (!m || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = m->count;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL paramset_GetParameterAt(const MI_ParameterSet *self, MI_Uint32 index, const MI_Char **name,
                                                 MI_Type *type, MI_Char **refclass, MI_QualifierSet *quals)
{
    const struct mi_method *m = get_method(self);
    if (!m) return MI_RESULT_INVALID_PARAMETER;
    if (index >= m->count) return MI_RESULT_NOT_FOUND;
    if (name) *name = m->params[index].name;
    if (type) *type = m->params[index].type;
    if (refclass) *refclass = m->params[index].refclass;
    fill_qualifier_set(quals, &m->params[index].quals);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL paramset_GetParameter(const MI_ParameterSet *self, const MI_Char *name, MI_Type *type,
                                               MI_Char **refclass, MI_QualifierSet *quals, MI_Uint32 *index)
{
    const struct mi_method *m = get_method(self);
    UINT i;
    if (!m || !name) return MI_RESULT_INVALID_PARAMETER;
    for (i = 0; i < m->count; i++)
    {
        if (_wcsicmp(m->params[i].name, name)) continue;
        if (type) *type = m->params[i].type;
        if (refclass) *refclass = m->params[i].refclass;
        fill_qualifier_set(quals, &m->params[i].quals);
        if (index) *index = i;
        return MI_RESULT_OK;
    }
    return MI_RESULT_NOT_FOUND;
}

static const MI_ParameterSetFT mi_parameter_set_ft =
{
    paramset_GetMethodReturnType,
    paramset_GetParameterCount,
    paramset_GetParameterAt,
    paramset_GetParameter,
};

/* MI_Class */

static struct mi_class_data *get_class(const MI_Class *self)
{
    if (!self || self->ft != &mi_class_ft) return NULL;
    return ((const struct mi_class *)self)->data;
}

MI_Class *class_create(struct mi_class_data *data)
{
    struct mi_class *cls;
    if (!data || !(cls = mi_alloc(sizeof(*cls)))) return NULL;
    InterlockedIncrement(&data->refs);
    cls->data = data;
    cls->base.ft = &mi_class_ft;
    cls->base.classDecl = &data->decl;
    cls->base.namespaceName = data->nameSpace;
    cls->base.serverName = data->server;
    return &cls->base;
}

void class_delete(MI_Class *self)
{
    struct mi_class *cls = (struct mi_class *)self;
    if (!self) return;
    class_data_release(cls->data);
    mi_free(cls);
}

static MI_Result MI_CALL class_GetClassName(const MI_Class *self, const MI_Char **name)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !name) return MI_RESULT_INVALID_PARAMETER;
    *name = c->name;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_GetNameSpace(const MI_Class *self, const MI_Char **ns)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !ns) return MI_RESULT_INVALID_PARAMETER;
    *ns = c->nameSpace;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_GetServerName(const MI_Class *self, const MI_Char **server)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !server) return MI_RESULT_INVALID_PARAMETER;
    *server = c->server;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_GetElementCount(const MI_Class *self, MI_Uint32 *count)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = c->count;
    return MI_RESULT_OK;
}

static void return_property(const struct mi_property *p, MI_Value *value, MI_Boolean *exists, MI_Type *type,
                            MI_Char **refclass, MI_QualifierSet *quals, MI_Uint32 *flags)
{
    if (value)
    {
        if (p->has_value) *value = p->value;
        else memset(value, 0, sizeof(*value));
    }
    if (exists) *exists = p->has_value ? MI_TRUE : MI_FALSE;
    if (type) *type = p->type;
    if (refclass) *refclass = p->refclass;
    fill_qualifier_set(quals, &p->quals);
    if (flags) *flags = p->flags | (p->has_value ? 0 : MI_FLAG_NULL);
}

static MI_Result MI_CALL class_GetElement(const MI_Class *self, const MI_Char *name, MI_Value *value, MI_Boolean *exists,
                                          MI_Type *type, MI_Char **refclass, MI_QualifierSet *quals, MI_Uint32 *flags,
                                          MI_Uint32 *index)
{
    struct mi_class_data *c = get_class(self);
    UINT i;
    if (!c || !name) return MI_RESULT_INVALID_PARAMETER;
    for (i = 0; i < c->count; i++)
    {
        if (_wcsicmp(c->props[i].name, name)) continue;
        return_property(&c->props[i], value, exists, type, refclass, quals, flags);
        if (index) *index = i;
        return MI_RESULT_OK;
    }
    return MI_RESULT_NO_SUCH_PROPERTY;
}

static MI_Result MI_CALL class_GetElementAt(const MI_Class *self, MI_Uint32 index, const MI_Char **name, MI_Value *value,
                                            MI_Boolean *exists, MI_Type *type, MI_Char **refclass,
                                            MI_QualifierSet *quals, MI_Uint32 *flags)
{
    struct mi_class_data *c = get_class(self);
    /* every output is optional: MI.NET reads one attribute of a property at a time */
    if (!c) return MI_RESULT_INVALID_PARAMETER;
    if (index >= c->count) return MI_RESULT_NOT_FOUND;
    if (name) *name = c->props[index].name;
    return_property(&c->props[index], value, exists, type, refclass, quals, flags);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_GetClassQualifierSet(const MI_Class *self, MI_QualifierSet *quals)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !quals) return MI_RESULT_INVALID_PARAMETER;
    fill_qualifier_set(quals, &c->quals);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_GetMethodCount(const MI_Class *self, MI_Uint32 *count)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = c->method_count;
    return MI_RESULT_OK;
}

static void return_method(const struct mi_method *m, MI_QualifierSet *quals, MI_ParameterSet *params)
{
    fill_qualifier_set(quals, &m->quals);
    if (params)
    {
        params->reserved1 = 0;
        params->reserved2 = (ptrdiff_t)m;
        params->ft = &mi_parameter_set_ft;
    }
}

static MI_Result MI_CALL class_GetMethodAt(const MI_Class *self, MI_Uint32 index, const MI_Char **name,
                                           MI_QualifierSet *quals, MI_ParameterSet *params)
{
    struct mi_class_data *c = get_class(self);
    if (!c) return MI_RESULT_INVALID_PARAMETER;
    if (index >= c->method_count) return MI_RESULT_METHOD_NOT_FOUND;
    if (name) *name = c->methods[index].name;
    return_method(&c->methods[index], quals, params);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_GetMethod(const MI_Class *self, const MI_Char *name, MI_QualifierSet *quals,
                                         MI_ParameterSet *params, MI_Uint32 *index)
{
    struct mi_class_data *c = get_class(self);
    UINT i;
    if (!c || !name) return MI_RESULT_INVALID_PARAMETER;
    for (i = 0; i < c->method_count; i++)
    {
        if (_wcsicmp(c->methods[i].name, name)) continue;
        return_method(&c->methods[i], quals, params);
        if (index) *index = i;
        return MI_RESULT_OK;
    }
    return MI_RESULT_METHOD_NOT_FOUND;
}

static MI_Result MI_CALL class_GetParentClassName(const MI_Class *self, const MI_Char **name)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !name) return MI_RESULT_INVALID_PARAMETER;
    if (!c->parent) return MI_RESULT_INVALID_SUPERCLASS;
    *name = c->parent;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_GetParentClass(const MI_Class *self, MI_Class **parent)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !parent) return MI_RESULT_INVALID_PARAMETER;
    *parent = NULL;
    if (!c->parent) return MI_RESULT_INVALID_SUPERCLASS;
    /* the superclass is read with the class (session.c); a class whose superclass could not be read
     * reports the name only */
    if (!c->parent_data) return MI_RESULT_INVALID_SUPERCLASS;
    if (!(*parent = class_create(c->parent_data))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_Delete(MI_Class *self)
{
    if (!get_class(self)) return MI_RESULT_INVALID_PARAMETER;
    class_delete(self);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL class_Clone(const MI_Class *self, MI_Class **copy)
{
    struct mi_class_data *c = get_class(self);
    if (!c || !copy) return MI_RESULT_INVALID_PARAMETER;
    if (!(*copy = class_create(c))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    return MI_RESULT_OK;
}

const MI_ClassFT mi_class_ft =
{
    class_GetClassName,
    class_GetNameSpace,
    class_GetServerName,
    class_GetElementCount,
    class_GetElement,
    class_GetElementAt,
    class_GetClassQualifierSet,
    class_GetMethodCount,
    class_GetMethodAt,
    class_GetMethod,
    class_GetParentClassName,
    class_GetParentClass,
    class_Delete,
    class_Clone,
};

/* MI_Instance */

struct mi_instance *instance_from_mi(const MI_Instance *inst)
{
    if (!inst || inst->ft != &mi_instance_ft) return NULL;
    return (struct mi_instance *)inst;
}

static void instance_update_pointers(struct mi_instance *inst)
{
    inst->base.ft = &mi_instance_ft;
    inst->base.classDecl = inst->cls ? &inst->cls->decl : &inst->decl;
    inst->base.nameSpace = inst->nameSpace;
    inst->base.serverName = inst->server;
    inst->decl.flags = MI_FLAG_CLASS;
    inst->decl.name = inst->class_name;
}

struct mi_instance *instance_create(const WCHAR *class_name, const WCHAR *nameSpace, const WCHAR *server)
{
    struct mi_instance *inst;
    if (!(inst = mi_alloc(sizeof(*inst)))) return NULL;
    inst->class_name = mi_strdup(class_name);
    inst->nameSpace = mi_strdup(nameSpace);
    inst->server = mi_strdup(server);
    instance_update_pointers(inst);
    return inst;
}

void instance_set_class(struct mi_instance *inst, struct mi_class_data *cls)
{
    if (cls) InterlockedIncrement(&cls->refs);
    class_data_release(inst->cls);
    inst->cls = cls;
    instance_update_pointers(inst);
}

static void instance_clear(struct mi_instance *inst)
{
    UINT i;
    for (i = 0; i < inst->count; i++)
    {
        mi_free(inst->elems[i].name);
        free_value(&inst->elems[i].value, inst->elems[i].type);
    }
    mi_free(inst->elems);
    inst->elems = NULL;
    inst->count = inst->size = 0;
    mi_free(inst->class_name);
    mi_free(inst->nameSpace);
    mi_free(inst->server);
    inst->class_name = inst->nameSpace = inst->server = NULL;
    class_data_release(inst->cls);
    inst->cls = NULL;
    /* MI_Instance_Destruct leaves the block with the caller: nothing may point at the freed names */
    inst->decl.name = NULL;
    inst->decl.superClass = NULL;
    inst->base.classDecl = NULL;
    inst->base.nameSpace = NULL;
    inst->base.serverName = NULL;
}

void instance_delete(struct mi_instance *inst)
{
    if (!inst) return;
    instance_clear(inst);
    mi_free(inst);
}

struct mi_element *instance_find(const struct mi_instance *inst, const WCHAR *name, UINT *index)
{
    UINT i;
    if (!name) return NULL;
    for (i = 0; i < inst->count; i++)
    {
        if (_wcsicmp(inst->elems[i].name, name)) continue;
        if (index) *index = i;
        return &inst->elems[i];
    }
    return NULL;
}

static MI_Result store_value(struct mi_element *elem, const MI_Value *value, MI_Type type, MI_Uint32 flags)
{
    MI_Value fresh;
    MI_Result res;
    MI_Uint32 new_flags = flags & ~(MI_FLAG_BORROW | MI_FLAG_ADOPT | MI_FLAG_NULL);

    if (!value || (flags & MI_FLAG_NULL))
    {
        free_value(&elem->value, elem->type);
        elem->type = type;
        elem->flags = new_flags | MI_FLAG_NULL;
        return MI_RESULT_OK;
    }
    if (value == &elem->value)
    {
        /* storing the element's own value: nothing to copy or free, the new type must match */
        if (type != elem->type) return MI_RESULT_INVALID_PARAMETER;
        elem->flags = new_flags;
        return MI_RESULT_OK;
    }
    /* build the new value before the old one goes away: on failure the element is unchanged */
    if (flags & MI_FLAG_ADOPT) fresh = *value;
    else if ((res = copy_value(&fresh, value, type)) != MI_RESULT_OK) return res;
    free_value(&elem->value, elem->type);
    elem->value = fresh;
    elem->type = type;
    elem->flags = new_flags;
    return MI_RESULT_OK;
}

MI_Result instance_add(struct mi_instance *inst, const WCHAR *name, const MI_Value *value, MI_Type type, MI_Uint32 flags)
{
    struct mi_element *elem;
    MI_Result res;

    if (!name || !valid_type(type)) return MI_RESULT_INVALID_PARAMETER;
    if (instance_find(inst, name, NULL)) return MI_RESULT_ALREADY_EXISTS;
    if (inst->count == inst->size)
    {
        UINT size = inst->size ? inst->size * 2 : 16;
        struct mi_element *tmp = mi_alloc(size * sizeof(*tmp));
        if (!tmp) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
        if (inst->elems) memcpy(tmp, inst->elems, inst->count * sizeof(*tmp));
        mi_free(inst->elems);
        inst->elems = tmp;
        inst->size = size;
    }
    elem = &inst->elems[inst->count];
    memset(elem, 0, sizeof(*elem));
    if (!(elem->name = mi_strdup(name))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    if ((res = store_value(elem, value, type, flags)) != MI_RESULT_OK)
    {
        mi_free(elem->name);
        memset(elem, 0, sizeof(*elem));
        return res;
    }
    inst->count++;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_Clone(const MI_Instance *self, MI_Instance **copy)
{
    struct mi_instance *inst = instance_from_mi(self), *ret;
    MI_Result res;
    UINT i;

    if (!inst || !copy) return MI_RESULT_INVALID_PARAMETER;
    if (!(ret = instance_create(inst->class_name, inst->nameSpace, inst->server))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    instance_set_class(ret, inst->cls);
    for (i = 0; i < inst->count; i++)
    {
        const struct mi_element *e = &inst->elems[i];
        if ((res = instance_add(ret, e->name, (e->flags & MI_FLAG_NULL) ? NULL : &e->value, e->type, e->flags)))
        {
            instance_delete(ret);
            return res;
        }
    }
    *copy = &ret->base;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_Destruct(MI_Instance *self)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    instance_clear(inst);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_Delete(MI_Instance *self)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    instance_delete(inst);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_IsA(const MI_Instance *self, const MI_ClassDecl *decl, MI_Boolean *flag)
{
    struct mi_instance *inst = instance_from_mi(self);
    const MI_ClassDecl *d;
    if (!inst || !decl || !flag) return MI_RESULT_INVALID_PARAMETER;
    *flag = MI_FALSE;
    for (d = decl; d; d = d->superClassDecl)
    {
        if (d->name && inst->class_name && !_wcsicmp(d->name, inst->class_name)) { *flag = MI_TRUE; break; }
    }
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_GetClassName(const MI_Instance *self, const MI_Char **name)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst || !name) return MI_RESULT_INVALID_PARAMETER;
    *name = inst->class_name;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_SetNameSpace(MI_Instance *self, const MI_Char *ns)
{
    struct mi_instance *inst = instance_from_mi(self);
    WCHAR *copy = NULL;
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    if (ns && !(copy = mi_strdup(ns))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    mi_free(inst->nameSpace);
    inst->nameSpace = copy;
    instance_update_pointers(inst);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_GetNameSpace(const MI_Instance *self, const MI_Char **ns)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst || !ns) return MI_RESULT_INVALID_PARAMETER;
    *ns = inst->nameSpace;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_GetElementCount(const MI_Instance *self, MI_Uint32 *count)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = inst->count;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_AddElement(MI_Instance *self, const MI_Char *name, const MI_Value *value,
                                             MI_Type type, MI_Uint32 flags)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    return instance_add(inst, name, value, type, flags);
}

static MI_Result set_element(struct mi_instance *inst, struct mi_element *elem, const MI_Value *value,
                             MI_Type type, MI_Uint32 flags)
{
    if (!valid_type(type)) return MI_RESULT_INVALID_PARAMETER;
    if (type != elem->type) return MI_RESULT_TYPE_MISMATCH;
    return store_value(elem, value, type, flags | (elem->flags & MI_FLAG_KEY));
}

static MI_Result MI_CALL instance_SetElement(MI_Instance *self, const MI_Char *name, const MI_Value *value,
                                             MI_Type type, MI_Uint32 flags)
{
    struct mi_instance *inst = instance_from_mi(self);
    struct mi_element *elem;
    if (!inst || !name) return MI_RESULT_INVALID_PARAMETER;
    if (!(elem = instance_find(inst, name, NULL))) return MI_RESULT_NO_SUCH_PROPERTY;
    return set_element(inst, elem, value, type, flags);
}

static MI_Result MI_CALL instance_SetElementAt(MI_Instance *self, MI_Uint32 index, const MI_Value *value,
                                               MI_Type type, MI_Uint32 flags)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    if (index >= inst->count) return MI_RESULT_INVALID_PARAMETER;
    return set_element(inst, &inst->elems[index], value, type, flags);
}

static void return_element(const struct mi_element *e, MI_Value *value, MI_Type *type, MI_Uint32 *flags)
{
    if (value)
    {
        if (e->flags & MI_FLAG_NULL) memset(value, 0, sizeof(*value));
        else *value = e->value;
    }
    if (type) *type = e->type;
    if (flags) *flags = e->flags;
}

static MI_Result MI_CALL instance_GetElement(const MI_Instance *self, const MI_Char *name, MI_Value *value,
                                             MI_Type *type, MI_Uint32 *flags, MI_Uint32 *index)
{
    struct mi_instance *inst = instance_from_mi(self);
    struct mi_element *elem;
    UINT i;
    if (!inst || !name) return MI_RESULT_INVALID_PARAMETER;
    if (!(elem = instance_find(inst, name, &i))) return MI_RESULT_NO_SUCH_PROPERTY;
    return_element(elem, value, type, flags);
    if (index) *index = i;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_GetElementAt(const MI_Instance *self, MI_Uint32 index, const MI_Char **name,
                                               MI_Value *value, MI_Type *type, MI_Uint32 *flags)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    if (index >= inst->count) return MI_RESULT_INVALID_PARAMETER;
    if (name) *name = inst->elems[index].name;
    return_element(&inst->elems[index], value, type, flags);
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_ClearElement(MI_Instance *self, const MI_Char *name)
{
    struct mi_instance *inst = instance_from_mi(self);
    struct mi_element *elem;
    if (!inst || !name) return MI_RESULT_INVALID_PARAMETER;
    if (!(elem = instance_find(inst, name, NULL))) return MI_RESULT_NO_SUCH_PROPERTY;
    free_value(&elem->value, elem->type);
    elem->flags |= MI_FLAG_NULL;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_ClearElementAt(MI_Instance *self, MI_Uint32 index)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    if (index >= inst->count) return MI_RESULT_INVALID_PARAMETER;
    free_value(&inst->elems[index].value, inst->elems[index].type);
    inst->elems[index].flags |= MI_FLAG_NULL;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_GetServerName(const MI_Instance *self, const MI_Char **name)
{
    struct mi_instance *inst = instance_from_mi(self);
    if (!inst || !name) return MI_RESULT_INVALID_PARAMETER;
    *name = inst->server;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL instance_SetServerName(MI_Instance *self, const MI_Char *name)
{
    struct mi_instance *inst = instance_from_mi(self);
    WCHAR *copy = NULL;
    if (!inst) return MI_RESULT_INVALID_PARAMETER;
    if (name && !(copy = mi_strdup(name))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    mi_free(inst->server);
    inst->server = copy;
    instance_update_pointers(inst);
    return MI_RESULT_OK;
}

/* class of an instance: the schema fetched with the results, else one made from the elements */
static MI_Result MI_CALL instance_GetClass(const MI_Instance *self, MI_Class **cls)
{
    struct mi_instance *inst = instance_from_mi(self);
    struct mi_class_data *data;
    UINT i;

    if (!inst || !cls) return MI_RESULT_INVALID_PARAMETER;
    *cls = NULL;
    if (inst->cls)
    {
        if (!(*cls = class_create(inst->cls))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
        return MI_RESULT_OK;
    }
    if (!inst->class_name) return MI_RESULT_INVALID_CLASS;
    if (!(data = class_data_create(inst->class_name, inst->nameSpace, inst->server))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    if (inst->count && (data->props = mi_alloc(inst->count * sizeof(*data->props))))
    {
        for (i = 0; i < inst->count; i++)
        {
            data->props[i].name = mi_strdup(inst->elems[i].name);
            data->props[i].type = inst->elems[i].type;
            data->props[i].flags = MI_FLAG_PROPERTY | (inst->elems[i].flags & (MI_FLAG_KEY | MI_FLAG_READONLY));
        }
        data->count = inst->count;
    }
    class_data_finish(data);
    *cls = class_create(data);
    class_data_release(data);
    return *cls ? MI_RESULT_OK : MI_RESULT_SERVER_LIMITS_EXCEEDED;
}

const MI_InstanceFT mi_instance_ft =
{
    instance_Clone,
    instance_Destruct,
    instance_Delete,
    instance_IsA,
    instance_GetClassName,
    instance_SetNameSpace,
    instance_GetNameSpace,
    instance_GetElementCount,
    instance_AddElement,
    instance_SetElement,
    instance_SetElementAt,
    instance_GetElement,
    instance_GetElementAt,
    instance_ClearElement,
    instance_ClearElementAt,
    instance_GetServerName,
    instance_SetServerName,
    instance_GetClass,
};

/* error details: an MSFT_WmiError instance like the one Windows returns */

const WCHAR *result_message(MI_Result result)
{
    switch (result)
    {
    case MI_RESULT_OK: return L"";
    case MI_RESULT_ACCESS_DENIED: return L"Access denied ";
    case MI_RESULT_INVALID_NAMESPACE: return L"Invalid namespace ";
    case MI_RESULT_INVALID_PARAMETER: return L"Invalid parameter ";
    case MI_RESULT_INVALID_CLASS: return L"Invalid class ";
    case MI_RESULT_NOT_FOUND: return L"Not found ";
    case MI_RESULT_NOT_SUPPORTED: return L"Not supported ";
    case MI_RESULT_ALREADY_EXISTS: return L"Already exists ";
    case MI_RESULT_NO_SUCH_PROPERTY: return L"No such property ";
    case MI_RESULT_TYPE_MISMATCH: return L"Type mismatch ";
    case MI_RESULT_QUERY_LANGUAGE_NOT_SUPPORTED: return L"Invalid query language ";
    case MI_RESULT_INVALID_QUERY: return L"Invalid query ";
    case MI_RESULT_METHOD_NOT_AVAILABLE: return L"Method not available ";
    case MI_RESULT_METHOD_NOT_FOUND: return L"Invalid method ";
    case MI_RESULT_SERVER_LIMITS_EXCEEDED: return L"Out of memory ";
    default: return L"Generic failure ";
    }
}

static MI_Uint16 result_category(MI_Result result)
{
    switch (result)
    {
    case MI_RESULT_ACCESS_DENIED: return 18;              /* PermissionDenied */
    case MI_RESULT_INVALID_NAMESPACE:
    case MI_RESULT_INVALID_CLASS:
    case MI_RESULT_INVALID_QUERY:
    case MI_RESULT_INVALID_PARAMETER: return MI_ERRORCATEGORY_INVALID_ARGUMENT;
    case MI_RESULT_NOT_FOUND: return MI_ERRORCATEGORY_OBJECT_NOT_FOUND;
    case MI_RESULT_NOT_SUPPORTED:
    case MI_RESULT_QUERY_LANGUAGE_NOT_SUPPORTED: return MI_ERRORCATEGORY_NOT_IMPLEMENTED;
    case MI_RESULT_TYPE_MISMATCH: return MI_ERRORCATEGORY_INVALID_TYPE;
    case MI_RESULT_METHOD_NOT_FOUND:
    case MI_RESULT_METHOD_NOT_AVAILABLE: return MI_ERRORCATEGORY_METADATA_ERROR;
    default: return MI_ERRORCATEGORY_NOT_SPECIFIED;
    }
}

struct mi_instance *make_error_instance(MI_Result result, HRESULT hr, const WCHAR *message)
{
    struct mi_instance *inst;
    MI_Value v;
    WCHAR buf[32];

    if (!(inst = instance_create(L"MSFT_WmiError", L"root/cimv2", NULL))) return NULL;
    /* WMI reports a malformed object path as such, not as the MI result it maps to */
    if ((!message || !message[0]) && hr == WBEM_E_INVALID_OBJECT_PATH) message = L"Invalid object path ";
    if (!message || !message[0]) message = result_message(result);
    v.uint32 = result;
    instance_add(inst, L"CIMStatusCode", &v, MI_UINT32, 0);
    instance_add(inst, L"CIMStatusCodeDescription", NULL, MI_STRING, 0);
    v.uint16 = 0;
    instance_add(inst, L"ErrorType", &v, MI_UINT16, 0);
    v.string = (MI_Char *)message;
    instance_add(inst, L"Message", &v, MI_STRING, 0);
    /* MI clients (CimCmdlets) take this as the error id: "HRESULT 0x80041002" for a WMI failure */
    if (hr) _snwprintf(buf, ARRAYSIZE(buf) - 1, L"HRESULT 0x%08lx", (unsigned long)hr);
    else _snwprintf(buf, ARRAYSIZE(buf) - 1, L"%u", (unsigned)result);
    buf[ARRAYSIZE(buf) - 1] = 0;
    v.string = buf;
    instance_add(inst, L"MessageID", &v, MI_STRING, 0);
    v.string = (MI_Char *)L"ReactOS MI";
    instance_add(inst, L"OwningEntity", &v, MI_STRING, 0);
    v.uint16 = 2;
    instance_add(inst, L"PerceivedSeverity", &v, MI_UINT16, 0);
    v.uint16 = 0;
    instance_add(inst, L"ProbableCause", &v, MI_UINT16, 0);
    v.uint16 = result_category(result);
    instance_add(inst, L"error_Category", &v, MI_UINT16, 0);
    v.uint32 = hr ? (MI_Uint32)hr : (MI_Uint32)result;
    instance_add(inst, L"error_Code", &v, MI_UINT32, 0);
    v.string = (MI_Char *)(hr ? L"HRESULT" : L"MI");
    instance_add(inst, L"error_Type", &v, MI_STRING, 0);
    v.string = (MI_Char *)message;
    instance_add(inst, L"error_WindowsErrorMessage", &v, MI_STRING, 0);
    return inst;
}
