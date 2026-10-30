/*
 * PROJECT:     ReactOS crypt32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     RFC 3161 time stamp entry points
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * Retrieving a time stamp needs an HTTP POST to a time stamping authority and verifying one
 * needs a CMS TSTInfo parser, neither of which exists here yet. The entry points are exported
 * (LibreOffice imports them statically, so without them it cannot start) and fail cleanly.
 */

#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>
#include <wine/debug.h>
#include <crypt32_private.h>

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

BOOL WINAPI CryptRetrieveTimeStamp(LPCWSTR url, DWORD flags, DWORD timeout, LPCSTR hash_oid,
                                   const void *para, const BYTE *data, DWORD data_len,
                                   void **context, PCCERT_CONTEXT *signer, HCERTSTORE *store)
{
    FIXME("(%s, %08lx, %lu, %s, %p, %p, %lu, %p, %p, %p) stub\n", debugstr_w(url), flags, timeout,
          debugstr_a(hash_oid), para, data, data_len, context, signer, store);

    if (!url || !hash_oid || (data_len && !data) || !context)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *context = NULL;
    if (signer) *signer = NULL;
    if (store) *store = NULL;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI CryptVerifyTimeStampSignature(const BYTE *ts_info, DWORD ts_info_len, const BYTE *data,
                                          DWORD data_len, HCERTSTORE additional_store,
                                          void **context, PCCERT_CONTEXT *signer, HCERTSTORE *store)
{
    FIXME("(%p, %lu, %p, %lu, %p, %p, %p, %p) stub\n", ts_info, ts_info_len, data, data_len,
          additional_store, context, signer, store);

    if (!ts_info || !ts_info_len || !context)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *context = NULL;
    if (signer) *signer = NULL;
    if (store) *store = NULL;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
