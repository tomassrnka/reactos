/*
 * PROJECT:     ReactOS Management Infrastructure client (mi.dll)
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     MI_Application, option containers, utilities and the client function table
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The MI client API of Windows (mi.h) implemented locally on top of the WMI COM
 * stack (IWbemLocator/IWbemServices from wbemprox). Only local sessions are
 * supported; serializers, hosted providers and subscriptions return
 * MI_RESULT_NOT_SUPPORTED.
 */

#include "mi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(mi);

#define MI_APP_MAGIC 0x50414d49 /* "IMAP" */

struct mi_application
{
    LONG magic;
    LONG closed;
    WCHAR *id;
};

static struct mi_application *get_application(const MI_Application *app)
{
    struct mi_application *a;
    if (!app || app->ft != &mi_application_ft) return NULL;
    a = (struct mi_application *)app->reserved2;
    if (!a || a->magic != MI_APP_MAGIC) return NULL;
    return a;
}

/* option containers */

static struct mi_options *options_alloc(void)
{
    struct mi_options *o = mi_alloc(sizeof(*o));
    if (o) o->magic = MI_OPTIONS_MAGIC;
    return o;
}

static void options_free(struct mi_options *o)
{
    UINT i;
    if (!o) return;
    for (i = 0; i < o->count; i++)
    {
        mi_free(o->items[i].name);
        free_value(&o->items[i].value, o->items[i].type);
    }
    mi_free(o->items);
    o->magic = 0;
    mi_free(o);
}

static struct mi_options *options_from(ptrdiff_t reserved2)
{
    struct mi_options *o = (struct mi_options *)reserved2;
    if (!o || o->magic != MI_OPTIONS_MAGIC) return NULL;
    return o;
}

static struct mi_option *options_lookup(struct mi_options *o, const WCHAR *name, UINT *index)
{
    UINT i;
    for (i = 0; i < o->count; i++)
    {
        if (_wcsicmp(o->items[i].name, name)) continue;
        if (index) *index = i;
        return &o->items[i];
    }
    return NULL;
}

static MI_Result options_set(struct mi_options *o, const WCHAR *name, MI_Type type, const MI_Value *value, MI_Uint32 flags)
{
    struct mi_option *opt;
    MI_Value copy;
    MI_Result res;

    if (!o || !name || !value) return MI_RESULT_INVALID_PARAMETER;
    if ((res = copy_value(&copy, value, type))) return res;
    if (!(opt = options_lookup(o, name, NULL)))
    {
        if (o->count == o->size)
        {
            UINT size = o->size ? o->size * 2 : 8;
            struct mi_option *tmp = mi_alloc(size * sizeof(*tmp));
            if (!tmp) { free_value(&copy, type); return MI_RESULT_SERVER_LIMITS_EXCEEDED; }
            if (o->items) memcpy(tmp, o->items, o->count * sizeof(*tmp));
            mi_free(o->items);
            o->items = tmp;
            o->size = size;
        }
        opt = &o->items[o->count];
        memset(opt, 0, sizeof(*opt));
        if (!(opt->name = mi_strdup(name))) { free_value(&copy, type); return MI_RESULT_SERVER_LIMITS_EXCEEDED; }
        o->count++;
    }
    else free_value(&opt->value, opt->type);
    opt->type = type;
    opt->flags = flags;
    opt->value = copy;
    return MI_RESULT_OK;
}

static struct mi_options *options_clone(const struct mi_options *src)
{
    struct mi_options *o = options_alloc();
    UINT i;
    if (!o) return NULL;
    for (i = 0; i < src->count; i++)
        options_set(o, src->items[i].name, src->items[i].type, &src->items[i].value, src->items[i].flags);
    o->credentials = src->credentials;
    return o;
}

static MI_Result options_get_typed(struct mi_options *o, const WCHAR *name, MI_Type type, MI_Value *value,
                                   MI_Uint32 *index, MI_Uint32 *flags)
{
    struct mi_option *opt;
    UINT i;
    if (!o || !name || !value) return MI_RESULT_INVALID_PARAMETER;
    if (!(opt = options_lookup(o, name, &i))) return MI_RESULT_NO_SUCH_PROPERTY;
    if (opt->type != type) return MI_RESULT_TYPE_MISMATCH;
    *value = opt->value;
    if (index) *index = i;
    if (flags) *flags = opt->flags;
    return MI_RESULT_OK;
}

static MI_Result options_get_at(struct mi_options *o, MI_Uint32 index, const MI_Char **name, MI_Value *value,
                                MI_Type *type, MI_Uint32 *flags)
{
    if (!o || !name || !value || !type) return MI_RESULT_INVALID_PARAMETER;
    if (index >= o->count) return MI_RESULT_NOT_FOUND;
    *name = o->items[index].name;
    *value = o->items[index].value;
    *type = o->items[index].type;
    if (flags) *flags = o->items[index].flags;
    return MI_RESULT_OK;
}

static MI_Result options_get(struct mi_options *o, const MI_Char *name, MI_Value *value, MI_Type *type,
                             MI_Uint32 *index, MI_Uint32 *flags)
{
    struct mi_option *opt;
    UINT i;
    if (!o || !name || !value || !type) return MI_RESULT_INVALID_PARAMETER;
    if (!(opt = options_lookup(o, name, &i))) return MI_RESULT_NO_SUCH_PROPERTY;
    *value = opt->value;
    *type = opt->type;
    if (index) *index = i;
    if (flags) *flags = opt->flags;
    return MI_RESULT_OK;
}

static MI_Result interval_to_value(const MI_Interval *interval, MI_Value *value)
{
    if (!interval) return MI_RESULT_INVALID_PARAMETER;
    memset(value, 0, sizeof(*value));
    value->datetime.isTimestamp = MI_FALSE;
    value->datetime.u.interval = *interval;
    return MI_RESULT_OK;
}

const struct mi_option *options_find(const MI_OperationOptions *options, const WCHAR *name)
{
    struct mi_options *o;
    if (!options || options->ft != &mi_operation_options_ft) return NULL;
    if (!(o = options_from(options->reserved2))) return NULL;
    return options_lookup(o, name, NULL);
}

/* MI_DestinationOptions */

#define DEST(options) options_from((options) ? (options)->reserved2 : 0)

static void MI_CALL destopt_Delete(MI_DestinationOptions *options)
{
    struct mi_options *o = DEST(options);
    if (!o) return;
    options_free(o);
    options->reserved2 = 0;
    options->ft = NULL;
}

static MI_Result MI_CALL destopt_SetString(MI_DestinationOptions *options, const MI_Char *name, const MI_Char *value, MI_Uint32 flags)
{
    MI_Value v;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    v.string = (MI_Char *)value;
    return options_set(DEST(options), name, MI_STRING, &v, flags);
}

static MI_Result MI_CALL destopt_SetNumber(MI_DestinationOptions *options, const MI_Char *name, MI_Uint32 value, MI_Uint32 flags)
{
    MI_Value v;
    v.uint32 = value;
    return options_set(DEST(options), name, MI_UINT32, &v, flags);
}

static MI_Result MI_CALL destopt_AddCredentials(MI_DestinationOptions *options, const MI_Char *name,
                                                const MI_UserCredentials *credentials, MI_Uint32 flags)
{
    struct mi_options *o = DEST(options);
    if (!o || !name || !credentials) return MI_RESULT_INVALID_PARAMETER;
    /* credentials only matter for remote sessions, which are not supported */
    o->credentials++;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL destopt_GetString(MI_DestinationOptions *options, const MI_Char *name, const MI_Char **value,
                                           MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if (!(res = options_get_typed(DEST(options), name, MI_STRING, &v, index, flags))) *value = v.string;
    return res;
}

static MI_Result MI_CALL destopt_GetNumber(MI_DestinationOptions *options, const MI_Char *name, MI_Uint32 *value,
                                           MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if (!(res = options_get_typed(DEST(options), name, MI_UINT32, &v, index, flags))) *value = v.uint32;
    return res;
}

static MI_Result MI_CALL destopt_GetOptionCount(MI_DestinationOptions *options, MI_Uint32 *count)
{
    struct mi_options *o = DEST(options);
    if (!o || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = o->count;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL destopt_GetOptionAt(MI_DestinationOptions *options, MI_Uint32 index, const MI_Char **name,
                                             MI_Value *value, MI_Type *type, MI_Uint32 *flags)
{
    return options_get_at(DEST(options), index, name, value, type, flags);
}

static MI_Result MI_CALL destopt_GetOption(MI_DestinationOptions *options, const MI_Char *name, MI_Value *value,
                                           MI_Type *type, MI_Uint32 *index, MI_Uint32 *flags)
{
    return options_get(DEST(options), name, value, type, index, flags);
}

static MI_Result MI_CALL destopt_GetCredentialsCount(MI_DestinationOptions *options, MI_Uint32 *count)
{
    struct mi_options *o = DEST(options);
    if (!o || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = o->credentials;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL destopt_GetCredentialsAt(MI_DestinationOptions *options, MI_Uint32 index, const MI_Char **name,
                                                  MI_UserCredentials *credentials, MI_Uint32 *flags)
{
    return DEST(options) ? MI_RESULT_NOT_SUPPORTED : MI_RESULT_INVALID_PARAMETER;
}

static MI_Result MI_CALL destopt_GetCredentialsPasswordAt(MI_DestinationOptions *options, MI_Uint32 index, const MI_Char **name,
                                                          MI_Char *password, MI_Uint32 length, MI_Uint32 *needed, MI_Uint32 *flags)
{
    return DEST(options) ? MI_RESULT_NOT_SUPPORTED : MI_RESULT_INVALID_PARAMETER;
}

static MI_Result MI_CALL destopt_Clone(const MI_DestinationOptions *self, MI_DestinationOptions *copy)
{
    struct mi_options *o = DEST(self), *c;
    if (!o || !copy) return MI_RESULT_INVALID_PARAMETER;
    if (!(c = options_clone(o))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    copy->reserved1 = 0;
    copy->reserved2 = (ptrdiff_t)c;
    copy->ft = &mi_destination_options_ft;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL destopt_SetInterval(MI_DestinationOptions *options, const MI_Char *name, const MI_Interval *value, MI_Uint32 flags)
{
    MI_Value v;
    MI_Result res;
    if ((res = interval_to_value(value, &v))) return res;
    return options_set(DEST(options), name, MI_DATETIME, &v, flags);
}

static MI_Result MI_CALL destopt_GetInterval(MI_DestinationOptions *options, const MI_Char *name, MI_Interval *value,
                                             MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if ((res = options_get_typed(DEST(options), name, MI_DATETIME, &v, index, flags))) return res;
    if (v.datetime.isTimestamp) return MI_RESULT_TYPE_MISMATCH;
    *value = v.datetime.u.interval;
    return MI_RESULT_OK;
}

const MI_DestinationOptionsFT mi_destination_options_ft =
{
    destopt_Delete,
    destopt_SetString,
    destopt_SetNumber,
    destopt_AddCredentials,
    destopt_GetString,
    destopt_GetNumber,
    destopt_GetOptionCount,
    destopt_GetOptionAt,
    destopt_GetOption,
    destopt_GetCredentialsCount,
    destopt_GetCredentialsAt,
    destopt_GetCredentialsPasswordAt,
    destopt_Clone,
    destopt_SetInterval,
    destopt_GetInterval,
};

/* MI_OperationOptions */

#define OPOPT(options) options_from((options) ? (options)->reserved2 : 0)

static void MI_CALL opopt_Delete(MI_OperationOptions *options)
{
    struct mi_options *o = OPOPT(options);
    if (!o) return;
    options_free(o);
    options->reserved2 = 0;
    options->ft = NULL;
}

static MI_Result MI_CALL opopt_SetString(MI_OperationOptions *options, const MI_Char *name, const MI_Char *value, MI_Uint32 flags)
{
    MI_Value v;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    v.string = (MI_Char *)value;
    return options_set(OPOPT(options), name, MI_STRING, &v, flags);
}

static MI_Result MI_CALL opopt_SetNumber(MI_OperationOptions *options, const MI_Char *name, MI_Uint32 value, MI_Uint32 flags)
{
    MI_Value v;
    v.uint32 = value;
    return options_set(OPOPT(options), name, MI_UINT32, &v, flags);
}

static MI_Result MI_CALL opopt_SetCustomOption(MI_OperationOptions *options, const MI_Char *name, MI_Type type,
                                               const MI_Value *value, MI_Boolean mustComply, MI_Uint32 flags)
{
    return options_set(OPOPT(options), name, type, value, flags);
}

static MI_Result MI_CALL opopt_GetString(MI_OperationOptions *options, const MI_Char *name, const MI_Char **value,
                                         MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if (!(res = options_get_typed(OPOPT(options), name, MI_STRING, &v, index, flags))) *value = v.string;
    return res;
}

static MI_Result MI_CALL opopt_GetNumber(MI_OperationOptions *options, const MI_Char *name, MI_Uint32 *value,
                                         MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if (!(res = options_get_typed(OPOPT(options), name, MI_UINT32, &v, index, flags))) *value = v.uint32;
    return res;
}

static MI_Result MI_CALL opopt_GetOptionCount(MI_OperationOptions *options, MI_Uint32 *count)
{
    struct mi_options *o = OPOPT(options);
    if (!o || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = o->count;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL opopt_GetOptionAt(MI_OperationOptions *options, MI_Uint32 index, const MI_Char **name,
                                           MI_Value *value, MI_Type *type, MI_Uint32 *flags)
{
    return options_get_at(OPOPT(options), index, name, value, type, flags);
}

static MI_Result MI_CALL opopt_GetOption(MI_OperationOptions *options, const MI_Char *name, MI_Value *value,
                                         MI_Type *type, MI_Uint32 *index, MI_Uint32 *flags)
{
    return options_get(OPOPT(options), name, value, type, index, flags);
}

static MI_Result MI_CALL opopt_GetEnabledChannels(MI_OperationOptions *options, const MI_Char *name, MI_Uint32 *channels,
                                                  MI_Uint32 length, MI_Uint32 *count, MI_Uint32 *flags)
{
    struct mi_options *o = OPOPT(options);
    if (!o || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = 0;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL opopt_Clone(const MI_OperationOptions *self, MI_OperationOptions *copy)
{
    struct mi_options *o = OPOPT(self), *c;
    if (!o || !copy) return MI_RESULT_INVALID_PARAMETER;
    if (!(c = options_clone(o))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    copy->reserved1 = 0;
    copy->reserved2 = (ptrdiff_t)c;
    copy->ft = &mi_operation_options_ft;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL opopt_SetInterval(MI_OperationOptions *options, const MI_Char *name, const MI_Interval *value, MI_Uint32 flags)
{
    MI_Value v;
    MI_Result res;
    if ((res = interval_to_value(value, &v))) return res;
    return options_set(OPOPT(options), name, MI_DATETIME, &v, flags);
}

static MI_Result MI_CALL opopt_GetInterval(MI_OperationOptions *options, const MI_Char *name, MI_Interval *value,
                                           MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if ((res = options_get_typed(OPOPT(options), name, MI_DATETIME, &v, index, flags))) return res;
    if (v.datetime.isTimestamp) return MI_RESULT_TYPE_MISMATCH;
    *value = v.datetime.u.interval;
    return MI_RESULT_OK;
}

const MI_OperationOptionsFT mi_operation_options_ft =
{
    opopt_Delete,
    opopt_SetString,
    opopt_SetNumber,
    opopt_SetCustomOption,
    opopt_GetString,
    opopt_GetNumber,
    opopt_GetOptionCount,
    opopt_GetOptionAt,
    opopt_GetOption,
    opopt_GetEnabledChannels,
    opopt_Clone,
    opopt_SetInterval,
    opopt_GetInterval,
};

/* MI_SubscriptionDeliveryOptions: kept as a container, subscriptions are not supported */

static const MI_SubscriptionDeliveryOptionsFT mi_subscription_options_ft;
#define SUBOPT(options) options_from((options) ? (options)->reserved2 : 0)

static MI_Result MI_CALL subopt_SetString(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, const MI_Char *value, MI_Uint32 flags)
{
    MI_Value v;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    v.string = (MI_Char *)value;
    return options_set(SUBOPT(options), name, MI_STRING, &v, flags);
}

static MI_Result MI_CALL subopt_SetNumber(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, MI_Uint32 value, MI_Uint32 flags)
{
    MI_Value v;
    v.uint32 = value;
    return options_set(SUBOPT(options), name, MI_UINT32, &v, flags);
}

static MI_Result MI_CALL subopt_SetDateTime(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, const MI_Datetime *value, MI_Uint32 flags)
{
    MI_Value v;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    memset(&v, 0, sizeof(v));
    v.datetime = *value;
    return options_set(SUBOPT(options), name, MI_DATETIME, &v, flags);
}

static MI_Result MI_CALL subopt_SetInterval(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, const MI_Interval *value, MI_Uint32 flags)
{
    MI_Value v;
    MI_Result res;
    if ((res = interval_to_value(value, &v))) return res;
    return options_set(SUBOPT(options), name, MI_DATETIME, &v, flags);
}

static MI_Result MI_CALL subopt_AddCredentials(MI_SubscriptionDeliveryOptions *options, const MI_Char *name,
                                               const MI_UserCredentials *credentials, MI_Uint32 flags)
{
    struct mi_options *o = SUBOPT(options);
    if (!o || !name || !credentials) return MI_RESULT_INVALID_PARAMETER;
    o->credentials++;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL subopt_Delete(MI_SubscriptionDeliveryOptions *options)
{
    struct mi_options *o = SUBOPT(options);
    if (!o) return MI_RESULT_INVALID_PARAMETER;
    options_free(o);
    options->reserved2 = 0;
    options->ft = NULL;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL subopt_GetString(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, const MI_Char **value,
                                          MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if (!(res = options_get_typed(SUBOPT(options), name, MI_STRING, &v, index, flags))) *value = v.string;
    return res;
}

static MI_Result MI_CALL subopt_GetNumber(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, MI_Uint32 *value,
                                          MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if (!(res = options_get_typed(SUBOPT(options), name, MI_UINT32, &v, index, flags))) *value = v.uint32;
    return res;
}

static MI_Result MI_CALL subopt_GetDateTime(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, MI_Datetime *value,
                                            MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if (!(res = options_get_typed(SUBOPT(options), name, MI_DATETIME, &v, index, flags))) *value = v.datetime;
    return res;
}

static MI_Result MI_CALL subopt_GetInterval(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, MI_Interval *value,
                                            MI_Uint32 *index, MI_Uint32 *flags)
{
    MI_Value v;
    MI_Result res;
    if (!value) return MI_RESULT_INVALID_PARAMETER;
    if ((res = options_get_typed(SUBOPT(options), name, MI_DATETIME, &v, index, flags))) return res;
    if (v.datetime.isTimestamp) return MI_RESULT_TYPE_MISMATCH;
    *value = v.datetime.u.interval;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL subopt_GetOptionCount(MI_SubscriptionDeliveryOptions *options, MI_Uint32 *count)
{
    struct mi_options *o = SUBOPT(options);
    if (!o || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = o->count;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL subopt_GetOptionAt(MI_SubscriptionDeliveryOptions *options, MI_Uint32 index, const MI_Char **name,
                                            MI_Value *value, MI_Type *type, MI_Uint32 *flags)
{
    return options_get_at(SUBOPT(options), index, name, value, type, flags);
}

static MI_Result MI_CALL subopt_GetOption(MI_SubscriptionDeliveryOptions *options, const MI_Char *name, MI_Value *value,
                                          MI_Type *type, MI_Uint32 *index, MI_Uint32 *flags)
{
    return options_get(SUBOPT(options), name, value, type, index, flags);
}

static MI_Result MI_CALL subopt_GetCredentialsCount(MI_SubscriptionDeliveryOptions *options, MI_Uint32 *count)
{
    struct mi_options *o = SUBOPT(options);
    if (!o || !count) return MI_RESULT_INVALID_PARAMETER;
    *count = o->credentials;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL subopt_GetCredentialsAt(MI_SubscriptionDeliveryOptions *options, MI_Uint32 index, const MI_Char **name,
                                                 MI_UserCredentials *credentials, MI_Uint32 *flags)
{
    return SUBOPT(options) ? MI_RESULT_NOT_SUPPORTED : MI_RESULT_INVALID_PARAMETER;
}

static MI_Result MI_CALL subopt_GetCredentialsPasswordAt(MI_SubscriptionDeliveryOptions *options, MI_Uint32 index, const MI_Char **name,
                                                         MI_Char *password, MI_Uint32 length, MI_Uint32 *needed, MI_Uint32 *flags)
{
    return SUBOPT(options) ? MI_RESULT_NOT_SUPPORTED : MI_RESULT_INVALID_PARAMETER;
}

static MI_Result MI_CALL subopt_Clone(const MI_SubscriptionDeliveryOptions *self, MI_SubscriptionDeliveryOptions *copy)
{
    struct mi_options *o = SUBOPT(self), *c;
    if (!o || !copy) return MI_RESULT_INVALID_PARAMETER;
    if (!(c = options_clone(o))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    copy->reserved1 = 0;
    copy->reserved2 = (ptrdiff_t)c;
    copy->ft = &mi_subscription_options_ft;
    return MI_RESULT_OK;
}

static const MI_SubscriptionDeliveryOptionsFT mi_subscription_options_ft =
{
    subopt_SetString,
    subopt_SetNumber,
    subopt_SetDateTime,
    subopt_SetInterval,
    subopt_AddCredentials,
    subopt_Delete,
    subopt_GetString,
    subopt_GetNumber,
    subopt_GetDateTime,
    subopt_GetInterval,
    subopt_GetOptionCount,
    subopt_GetOptionAt,
    subopt_GetOption,
    subopt_GetCredentialsCount,
    subopt_GetCredentialsAt,
    subopt_GetCredentialsPasswordAt,
    subopt_Clone,
};

/* MI_Application */

static MI_Result MI_CALL app_Close(MI_Application *application)
{
    struct mi_application *a = get_application(application);

    TRACE("%p\n", application);
    if (!a) return MI_RESULT_INVALID_PARAMETER;
    if (InterlockedExchange(&a->closed, 1)) return MI_RESULT_INVALID_PARAMETER;
    /* sessions keep their own state; the application block itself stays valid
     * (never freed) so that late calls through stale handles fail cleanly */
    application->ft = NULL;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL app_NewSession(MI_Application *application, const MI_Char *protocol, const MI_Char *destination,
                                        MI_DestinationOptions *options, MI_SessionCallbacks *callbacks,
                                        MI_Instance **extendedError, MI_Session *session)
{
    TRACE("%p %s %s %p %p %p %p\n", application, debugstr_w(protocol), debugstr_w(destination), options,
          callbacks, extendedError, session);
    if (extendedError) *extendedError = NULL;
    if (!get_application(application) || !session) return MI_RESULT_INVALID_PARAMETER;
    return session_new(application, protocol, destination, extendedError, session);
}

static MI_Result MI_CALL app_NewHostedProvider(MI_Application *application, const MI_Char *ns, const MI_Char *name,
                                               MI_MainFunction main, MI_Instance **extendedError, MI_HostedProvider *provider)
{
    FIXME("%p %s %s: hosted providers are not supported\n", application, debugstr_w(ns), debugstr_w(name));
    if (extendedError) *extendedError = NULL;
    if (!get_application(application) || !provider) return MI_RESULT_INVALID_PARAMETER;
    return MI_RESULT_NOT_SUPPORTED;
}

static MI_Result new_instance(const MI_Char *class_name, struct mi_class_data *cls, const MI_ClassDecl *decl, MI_Instance **instance)
{
    struct mi_instance *inst;
    MI_Uint32 i;

    if (!class_name || !instance) return MI_RESULT_INVALID_PARAMETER;
    if (!(inst = instance_create(class_name, cls ? cls->nameSpace : NULL, cls ? cls->server : NULL)))
        return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    if (cls)
    {
        instance_set_class(inst, cls);
        for (i = 0; i < cls->count; i++)
        {
            const struct mi_property *p = &cls->props[i];
            instance_add(inst, p->name, p->has_value ? &p->value : NULL, p->type,
                         p->flags & (MI_FLAG_KEY | MI_FLAG_READONLY));
        }
    }
    else if (decl && decl->properties)
    {
        for (i = 0; i < decl->numProperties; i++)
        {
            const MI_PropertyDecl *p = decl->properties[i];
            if (p && p->name) instance_add(inst, p->name, NULL, (MI_Type)p->type, p->flags & (MI_FLAG_KEY | MI_FLAG_READONLY));
        }
    }
    *instance = &inst->base;
    return MI_RESULT_OK;
}

static struct mi_class_data *class_data_from_decl(const MI_ClassDecl *decl)
{
    struct mi_class_data *data;
    /* a class declaration handed out by this DLL is embedded in a struct mi_class_data */
    if (!decl || decl->code != MI_CLASSDECL_MAGIC) return NULL;
    data = CONTAINING_RECORD(decl, struct mi_class_data, decl);
    return data->magic == MI_CLASSDECL_MAGIC ? data : NULL;
}

static MI_Result MI_CALL app_NewInstance(MI_Application *application, const MI_Char *class_name,
                                         const MI_ClassDecl *decl, MI_Instance **instance)
{
    TRACE("%p %s %p %p\n", application, debugstr_w(class_name), decl, instance);
    if (instance) *instance = NULL;
    if (!get_application(application)) return MI_RESULT_INVALID_PARAMETER;
    return new_instance(class_name, class_data_from_decl(decl), decl, instance);
}

static MI_Result MI_CALL app_NewDestinationOptions(MI_Application *application, MI_DestinationOptions *options)
{
    struct mi_options *o;
    if (!get_application(application) || !options) return MI_RESULT_INVALID_PARAMETER;
    if (!(o = options_alloc())) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    options->reserved1 = 0;
    options->reserved2 = (ptrdiff_t)o;
    options->ft = &mi_destination_options_ft;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL app_NewOperationOptions(MI_Application *application, MI_Boolean mustUnderstand, MI_OperationOptions *options)
{
    struct mi_options *o;
    if (!get_application(application) || !options) return MI_RESULT_INVALID_PARAMETER;
    if (!(o = options_alloc())) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    options->reserved1 = 0;
    options->reserved2 = (ptrdiff_t)o;
    options->ft = &mi_operation_options_ft;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL app_NewSubscriptionDeliveryOptions(MI_Application *application, MI_SubscriptionDeliveryType type,
                                                            MI_SubscriptionDeliveryOptions *options)
{
    struct mi_options *o;
    if (!get_application(application) || !options) return MI_RESULT_INVALID_PARAMETER;
    if (type != MI_SubscriptionDeliveryType_Pull && type != MI_SubscriptionDeliveryType_Push) return MI_RESULT_INVALID_PARAMETER;
    if (!(o = options_alloc())) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    options->reserved1 = 0;
    options->reserved2 = (ptrdiff_t)o;
    options->ft = &mi_subscription_options_ft;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL app_NewSerializer(MI_Application *application, MI_Uint32 flags, MI_Char *format, MI_Serializer *serializer)
{
    FIXME("%p %#x %s: serializers are not supported\n", application, flags, debugstr_w(format));
    if (!get_application(application) || !serializer) return MI_RESULT_INVALID_PARAMETER;
    return MI_RESULT_NOT_SUPPORTED;
}

static MI_Result MI_CALL app_NewDeserializer(MI_Application *application, MI_Uint32 flags, MI_Char *format, MI_Deserializer *deserializer)
{
    FIXME("%p %#x %s: deserializers are not supported\n", application, flags, debugstr_w(format));
    if (!get_application(application) || !deserializer) return MI_RESULT_INVALID_PARAMETER;
    return MI_RESULT_NOT_SUPPORTED;
}

static MI_Result MI_CALL app_NewInstanceFromClass(MI_Application *application, const MI_Char *class_name,
                                                  const MI_Class *cls, MI_Instance **instance)
{
    struct mi_class_data *data = NULL;
    TRACE("%p %s %p %p\n", application, debugstr_w(class_name), cls, instance);
    if (instance) *instance = NULL;
    if (!get_application(application)) return MI_RESULT_INVALID_PARAMETER;
    if (cls && cls->ft == &mi_class_ft) data = ((const struct mi_class *)cls)->data;
    return new_instance(class_name, data, cls ? cls->classDecl : NULL, instance);
}

static MI_Result MI_CALL app_NewClass(MI_Application *application, const MI_ClassDecl *decl, const MI_Char *ns,
                                      const MI_Char *server, MI_Class **cls)
{
    FIXME("%p %p %s %s: not supported\n", application, decl, debugstr_w(ns), debugstr_w(server));
    if (cls) *cls = NULL;
    if (!get_application(application)) return MI_RESULT_INVALID_PARAMETER;
    return MI_RESULT_NOT_SUPPORTED;
}

const MI_ApplicationFT mi_application_ft =
{
    app_Close,
    app_NewSession,
    app_NewHostedProvider,
    app_NewInstance,
    app_NewDestinationOptions,
    app_NewOperationOptions,
    app_NewSubscriptionDeliveryOptions,
    app_NewSerializer,
    app_NewDeserializer,
    app_NewInstanceFromClass,
    app_NewClass,
};

/* serializer, deserializer and hosted provider tables: nothing is ever handed out */

static MI_Result MI_CALL ser_Close(MI_Serializer *s) { return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL ser_SerializeClass(MI_Serializer *s, MI_Uint32 f, const MI_Class *c, MI_Uint8 *b, MI_Uint32 l, MI_Uint32 *n)
{ return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL ser_SerializeInstance(MI_Serializer *s, MI_Uint32 f, const MI_Instance *i, MI_Uint8 *b, MI_Uint32 l, MI_Uint32 *n)
{ return MI_RESULT_NOT_SUPPORTED; }

static const MI_SerializerFT mi_serializer_ft = { ser_Close, ser_SerializeClass, ser_SerializeInstance };

static MI_Result MI_CALL deser_Close(MI_Deserializer *d) { return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL deser_DeserializeClass(MI_Deserializer *d, MI_Uint32 f, MI_Uint8 *b, MI_Uint32 l, MI_Class *p,
                                                const MI_Char *s, const MI_Char *n, MI_Deserializer_ClassObjectNeeded cb,
                                                void *ctx, MI_Uint32 *r, MI_Class **c, MI_Instance **e)
{ if (e) *e = NULL; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL deser_Class_GetClassName(MI_Deserializer *d, MI_Uint8 *b, MI_Uint32 l, MI_Char *c, MI_Uint32 *cl, MI_Instance **e)
{ if (e) *e = NULL; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL deser_Class_GetParentClassName(MI_Deserializer *d, MI_Uint8 *b, MI_Uint32 l, MI_Char *c, MI_Uint32 *cl, MI_Instance **e)
{ if (e) *e = NULL; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL deser_DeserializeInstance(MI_Deserializer *d, MI_Uint32 f, MI_Uint8 *b, MI_Uint32 l, MI_Class **cs,
                                                   MI_Uint32 nc, MI_Deserializer_ClassObjectNeeded cb, void *ctx,
                                                   MI_Uint32 *r, MI_Instance **i, MI_Instance **e)
{ if (e) *e = NULL; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL deser_Instance_GetClassName(MI_Deserializer *d, MI_Uint8 *b, MI_Uint32 l, MI_Char *c, MI_Uint32 *cl, MI_Instance **e)
{ if (e) *e = NULL; return MI_RESULT_NOT_SUPPORTED; }

static const MI_DeserializerFT mi_deserializer_ft =
{
    deser_Close,
    deser_DeserializeClass,
    deser_Class_GetClassName,
    deser_Class_GetParentClassName,
    deser_DeserializeInstance,
    deser_Instance_GetClassName,
};

static MI_Result MI_CALL hosted_Close(MI_HostedProvider *p) { return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL hosted_GetApplication(MI_HostedProvider *p, MI_Application *a) { return MI_RESULT_NOT_SUPPORTED; }

static const MI_HostedProviderFT mi_hosted_provider_ft = { hosted_Close, hosted_GetApplication };

/* MI_Utilities */

static MI_ErrorCategory MI_CALL util_MapErrorToMiErrorCategory(MI_Char *type, MI_Uint32 error)
{
    if (!type) return MI_ERRORCATEGORY_NOT_SPECIFIED;
    if (!_wcsicmp(type, L"MI"))
    {
        switch (error)
        {
        case MI_RESULT_ACCESS_DENIED: return MI_ERRORCATEGORY_ACCESS_DENIED;
        case MI_RESULT_INVALID_NAMESPACE:
        case MI_RESULT_INVALID_CLASS:
        case MI_RESULT_INVALID_PARAMETER:
        case MI_RESULT_INVALID_QUERY: return MI_ERRORCATEGORY_INVALID_ARGUMENT;
        case MI_RESULT_NOT_FOUND: return MI_ERRORCATEGORY_OBJECT_NOT_FOUND;
        case MI_RESULT_NOT_SUPPORTED: return MI_ERRORCATEGORY_NOT_IMPLEMENTED;
        case MI_RESULT_TYPE_MISMATCH: return MI_ERRORCATEGORY_INVALID_TYPE;
        case MI_RESULT_ALREADY_EXISTS: return MI_ERRORCATEGORY_RESOURCE_EXISTS;
        default: return MI_ERRORCATEGORY_NOT_SPECIFIED;
        }
    }
    if (!_wcsicmp(type, L"Win32"))
    {
        switch (error)
        {
        case ERROR_ACCESS_DENIED: return MI_ERRORCATEGORY_ACCESS_DENIED;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return MI_ERRORCATEGORY_OBJECT_NOT_FOUND;
        case ERROR_INVALID_PARAMETER: return MI_ERRORCATEGORY_INVALID_ARGUMENT;
        case ERROR_NOT_SUPPORTED: return MI_ERRORCATEGORY_NOT_IMPLEMENTED;
        case ERROR_TIMEOUT: return MI_ERRORCATEGORY_OPERATION_TIMEOUT;
        default: return MI_ERRORCATEGORY_NOT_SPECIFIED;
        }
    }
    if (!_wcsicmp(type, L"HRESULT"))
    {
        switch (error)
        {
        case E_ACCESSDENIED: return MI_ERRORCATEGORY_ACCESS_DENIED;
        case E_INVALIDARG: return MI_ERRORCATEGORY_INVALID_ARGUMENT;
        case E_NOTIMPL: return MI_ERRORCATEGORY_NOT_IMPLEMENTED;
        default: return MI_ERRORCATEGORY_NOT_SPECIFIED;
        }
    }
    return MI_ERRORCATEGORY_NOT_SPECIFIED;
}

static MI_Result MI_CALL util_CimErrorFromErrorCode(MI_Uint32 error, const MI_Char *type, const MI_Char *message, MI_Instance **cim_error)
{
    struct mi_instance *inst;
    MI_Value v;
    WCHAR *text = NULL;
    MI_Result result = MI_RESULT_FAILED;

    TRACE("%u %s %s %p\n", error, debugstr_w(type), debugstr_w(message), cim_error);
    if (!type || !cim_error) return MI_RESULT_INVALID_PARAMETER;
    *cim_error = NULL;

    if (!_wcsicmp(type, L"MI")) result = (MI_Result)error;
    if (!message || !message[0])
    {
        if (!_wcsicmp(type, L"Win32") || !_wcsicmp(type, L"HRESULT"))
        {
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, error, 0, (WCHAR *)&text, 0, NULL);
        }
        message = text ? text : result_message(result);
    }
    inst = make_error_instance(result, 0, message);
    if (text) LocalFree(text);
    if (!inst) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    v.uint32 = error;
    inst->base.ft->SetElement(&inst->base, L"error_Code", &v, MI_UINT32, 0);
    v.string = (MI_Char *)type;
    inst->base.ft->SetElement(&inst->base, L"error_Type", &v, MI_STRING, 0);
    v.uint16 = (MI_Uint16)util_MapErrorToMiErrorCategory((MI_Char *)type, error);
    inst->base.ft->SetElement(&inst->base, L"error_Category", &v, MI_UINT16, 0);
    *cim_error = &inst->base;
    return MI_RESULT_OK;
}

static const MI_UtilitiesFT mi_utilities_ft =
{
    util_MapErrorToMiErrorCategory,
    util_CimErrorFromErrorCode,
};

static const MI_ClientFT_V1 client_ft =
{
    &mi_application_ft,
    &mi_session_ft,
    &mi_operation_ft,
    &mi_hosted_provider_ft,
    &mi_serializer_ft,
    &mi_deserializer_ft,
    &mi_subscription_options_ft,
    &mi_destination_options_ft,
    &mi_operation_options_ft,
    &mi_utilities_ft,
};

/* exported data: a pointer to the client function table, as in Windows */
const MI_ClientFT_V1 *mi_clientFT_V1 = &client_ft;

MI_Result MI_MAIN_CALL MI_Application_InitializeV1(MI_Uint32 flags, const MI_Char *applicationID,
                                                   MI_Instance **extendedError, MI_Application *application)
{
    struct mi_application *a;

    TRACE("%#x %s %p %p\n", flags, debugstr_w(applicationID), extendedError, application);
    if (extendedError) *extendedError = NULL;
    if (!application) return MI_RESULT_INVALID_PARAMETER;
    memset(application, 0, sizeof(*application));
    if (flags) return MI_RESULT_INVALID_PARAMETER;
    if (!(a = mi_alloc(sizeof(*a)))) return MI_RESULT_SERVER_LIMITS_EXCEEDED;
    a->magic = MI_APP_MAGIC;
    a->id = mi_strdup(applicationID);
    application->reserved1 = 1;
    application->reserved2 = (ptrdiff_t)a;
    application->ft = &mi_application_ft;
    return MI_RESULT_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
