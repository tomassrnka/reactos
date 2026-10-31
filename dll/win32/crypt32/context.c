/*
 * Copyright 2006 Juan Lang
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
#include <assert.h>
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "wine/debug.h"
#include "crypt32_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(context);

context_t *Context_CreateDataContext(size_t contextSize, const context_vtbl_t *vtbl, WINECRYPT_CERTSTORE *store)
{
    context_t *context;

    context = CryptMemAlloc(sizeof(context_t) + contextSize);
    if (!context)
        return NULL;

    context->properties = ContextPropertyList_Create();
    if (!context->properties)
    {
        CryptMemFree(context);
        return NULL;
    }

    context->vtbl = vtbl;
    context->ref = 1;
    context->linked = NULL;

    store->vtbl->addref(store);
    context->store = store;

    TRACE("returning %p\n", context);
    return context;
}

context_t *Context_CreateLinkContext(unsigned int contextSize, context_t *linked, WINECRYPT_CERTSTORE *store)
{
    context_t *context;

    TRACE("(%d, %p)\n", contextSize, linked);

    context = CryptMemAlloc(sizeof(context_t) + contextSize);
    if (!context)
        return NULL;

    memcpy(context_ptr(context), context_ptr(linked), contextSize);
    context->vtbl = linked->vtbl;
    context->ref = 1;
    context->linked = linked;
    context->properties = linked->properties;
    Context_AddRef(linked);

    store->vtbl->addref(store);
    context->store = store;

    TRACE("returning %p\n", context);
    return context;
}

void Context_AddRef(context_t *context)
{
    LONG ref = InterlockedIncrement(&context->ref);

    TRACE("(%p) ref=%ld\n", context, context->ref);

    if(ref == 1) {
        /* This is the first external (non-store) reference. Increase store ref cnt. */
        context->store->vtbl->addref(context->store);
    }
}

void Context_Free(context_t *context)
{
    TRACE("(%p)\n", context);

    assert(!context->ref);

    if (!context->linked) {
        CRYPT_DATA_BLOB blob;

        if (ContextPropertyList_FindProperty(context->properties, CERT_KEY_CONTEXT_PROP_ID, &blob))
            CRYPT_ReleaseNCryptKeyContext(&blob);
        ContextPropertyList_Free(context->properties);
        context->vtbl->free(context);
    }else {
        Context_Release(context->linked);
    }

    CryptMemFree(context);
}

void Context_Release(context_t *context)
{
    LONG ref = InterlockedDecrement(&context->ref);

    TRACE("(%p) ref=%ld\n", context, ref);
    assert(ref >= 0);

    if (!ref) {
        WINECRYPT_CERTSTORE *store = context->store;

        /* This is the last reference, but the context still may be in a store.
         * We release our store reference, but leave it up to store to free or keep the context. */
        store->vtbl->releaseContext(store, context);
        store->vtbl->release(store, 0);
    }
}

void Context_CopyProperties(const void *to, const void *from)
{
    CONTEXT_PROPERTY_LIST *toProperties, *fromProperties;
    CRYPT_DATA_BLOB blob;
    BYTE *saved = NULL;
    DWORD saved_size = 0;
    BOOL ncrypt_from, ncrypt_to, copies_key_context;

    toProperties = context_from_ptr(to)->properties;
    fromProperties = context_from_ptr(from)->properties;
    assert(toProperties && fromProperties);

    /* An NCrypt key stays with the context that owns it: a key context of the source that holds one
     * is not copied, and the destination's own NCrypt key is kept. A CryptoAPI key context of the
     * source replaces the destination's NCrypt key, which is released with it. */
    copies_key_context = ContextPropertyList_FindProperty(fromProperties, CERT_KEY_CONTEXT_PROP_ID, &blob);
    ncrypt_from = copies_key_context && CRYPT_IsNCryptKeyContext(&blob);
    ncrypt_to = FALSE;
    if (copies_key_context && ContextPropertyList_FindProperty(toProperties, CERT_KEY_CONTEXT_PROP_ID, &blob) &&
     (saved = CryptMemAlloc(blob.cbData)))
    {
        memcpy(saved, blob.pbData, blob.cbData);
        saved_size = blob.cbData;
        ncrypt_to = CRYPT_IsNCryptKeyContext(&blob);
    }
    ContextPropertyList_Copy(toProperties, fromProperties);
    if (ncrypt_from)
    {
        if (saved)
            ContextPropertyList_SetProperty(toProperties, CERT_KEY_CONTEXT_PROP_ID, saved, saved_size);
        else
            ContextPropertyList_RemoveProperty(toProperties, CERT_KEY_CONTEXT_PROP_ID);
    }
    else if (ncrypt_to)
    {
        blob.pbData = saved;
        blob.cbData = saved_size;
        CRYPT_ReleaseNCryptKeyContext(&blob);
    }
    CryptMemFree(saved);
}
