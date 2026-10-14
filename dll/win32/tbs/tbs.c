/*
 * Trusted Platform Module Base Services
 *
 * Copyright 2021 Alex Henrie
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

#include "windef.h"
#include "winerror.h"
#include "tbs.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(tbs);

/*
 * There is no TPM driver behind this library, so no context can be created and the machine
 * reports no TPM. A function that takes a context therefore finds none, after the checks of
 * its other arguments that Windows makes first.
 */

TBS_RESULT WINAPI Tbsi_Context_Create(const TBS_CONTEXT_PARAMS *params, TBS_HCONTEXT *out)
{
    FIXME("(%p, %p) stub\n", params, out);

    if (!out) return TBS_E_INVALID_OUTPUT_POINTER;
    *out = NULL;
    if (!params) return TBS_E_BAD_PARAMETER;
    return TBS_E_TPM_NOT_FOUND;
}

TBS_RESULT WINAPI Tbsi_GetDeviceInfo(UINT32 size, void *info)
{
    FIXME("(%u, %p) stub\n", size, info);
    return TBS_E_TPM_NOT_FOUND;
}

TBS_RESULT WINAPI Tbsip_Context_Close(TBS_HCONTEXT context)
{
    FIXME("(%p) stub\n", context);

    /* closing nothing succeeds, and no other context exists */
    return context ? TBS_E_INVALID_CONTEXT : TBS_SUCCESS;
}

TBS_RESULT WINAPI Tbsip_Cancel_Commands(TBS_HCONTEXT context)
{
    FIXME("(%p) stub\n", context);
    return TBS_E_INVALID_CONTEXT;
}

TBS_RESULT WINAPI Tbsip_Submit_Command(TBS_HCONTEXT context, TBS_COMMAND_LOCALITY locality,
        TBS_COMMAND_PRIORITY priority, const BYTE *command, UINT32 command_size, BYTE *result, UINT32 *result_size)
{
    FIXME("(%p, %u, %u, %p, %u, %p, %p) stub\n", context, locality, priority, command, command_size, result, result_size);

    if (!command) return TBS_E_BAD_PARAMETER;
    if (!result || !result_size) return TBS_E_INVALID_OUTPUT_POINTER;
    return TBS_E_INVALID_CONTEXT;
}

TBS_RESULT WINAPI Tbsi_Get_TCG_Log(TBS_HCONTEXT context, BYTE *buffer, UINT32 *size)
{
    FIXME("(%p, %p, %p) stub\n", context, buffer, size);

    if (!buffer || !size) return TBS_E_INVALID_OUTPUT_POINTER;
    return TBS_E_INVALID_CONTEXT;
}

TBS_RESULT WINAPI Tbsi_Physical_Presence_Command(TBS_HCONTEXT context, const BYTE *input, UINT32 input_size,
        BYTE *output, UINT32 *output_size)
{
    FIXME("(%p, %p, %u, %p, %p) stub\n", context, input, input_size, output, output_size);

    if (!input) return TBS_E_BAD_PARAMETER;
    if (!output || !output_size) return TBS_E_INVALID_OUTPUT_POINTER;
    return TBS_E_INVALID_CONTEXT;
}

HRESULT WINAPI GetDeviceIDString(WCHAR *out, UINT32 size, UINT32 *used, BOOL *tpm)
{
    FIXME("(%p, %u, %p, %p) stub\n", out, size, used, tpm);
    return E_NOTIMPL;
}
