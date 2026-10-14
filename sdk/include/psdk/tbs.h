/*
 * Copyright (C) 2021 Alex Henrie
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

#ifndef _TBS_H_
#define _TBS_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef UINT32 TBS_RESULT;

#define TBS_SUCCESS                         ((TBS_RESULT)0)

#define TBS_CONTEXT_VERSION_ONE             1
#define TBS_CONTEXT_VERSION_TWO             2

#define TBS_COMMAND_PRIORITY_LOW            100
#define TBS_COMMAND_PRIORITY_NORMAL         200
#define TBS_COMMAND_PRIORITY_SYSTEM         400
#define TBS_COMMAND_PRIORITY_HIGH           300
#define TBS_COMMAND_PRIORITY_MAX            0x80000000

#define TBS_COMMAND_LOCALITY_ZERO           0
#define TBS_COMMAND_LOCALITY_ONE            1
#define TBS_COMMAND_LOCALITY_TWO            2
#define TBS_COMMAND_LOCALITY_THREE          3
#define TBS_COMMAND_LOCALITY_FOUR           4

#define TPM_VERSION_UNKNOWN                 0
#define TPM_VERSION_12                      1
#define TPM_VERSION_20                      2

#define TPM_IFTYPE_UNKNOWN                  0
#define TPM_IFTYPE_1                        1
#define TPM_IFTYPE_TRUSTZONE                2
#define TPM_IFTYPE_HW                       3
#define TPM_IFTYPE_EMULATOR                 4
#define TPM_IFTYPE_SPB                      5

typedef UINT32 TBS_COMMAND_PRIORITY;
typedef UINT32 TBS_COMMAND_LOCALITY;

typedef struct tdTBS_CONTEXT_PARAMS
{
    UINT32 version;
} TBS_CONTEXT_PARAMS, *PTBS_CONTEXT_PARAMS;
typedef const TBS_CONTEXT_PARAMS *PCTBS_CONTEXT_PARAMS;

typedef struct tdTBS_CONTEXT_PARAMS2
{
    UINT32 version;
    union
    {
        struct
        {
            UINT32 requestRaw : 1;
            UINT32 includeTpm12 : 1;
            UINT32 includeTpm20 : 1;
        };
        UINT32 asUINT32;
    };
} TBS_CONTEXT_PARAMS2, *PTBS_CONTEXT_PARAMS2;
typedef const TBS_CONTEXT_PARAMS2 *PCTBS_CONTEXT_PARAMS2;

typedef struct _TPM_DEVICE_INFO
{
    UINT32 structVersion;
    UINT32 tpmVersion;
    UINT32 tpmInterfaceType;
    UINT32 tpmImpRevision;
} TPM_DEVICE_INFO, *PTPM_DEVICE_INFO;
typedef const TPM_DEVICE_INFO *PCTPM_DEVICE_INFO;

typedef void *TBS_HCONTEXT, **PTBS_HCONTEXT;

#ifdef _TBS_SOURCE_
#define TBSIMP TBS_RESULT WINAPI
#else
#define TBSIMP DECLSPEC_IMPORT TBS_RESULT WINAPI
#endif

TBSIMP Tbsi_Context_Create(PCTBS_CONTEXT_PARAMS, PTBS_HCONTEXT);
TBSIMP Tbsi_GetDeviceInfo(UINT32, PVOID);
TBSIMP Tbsi_Get_TCG_Log(TBS_HCONTEXT, PBYTE, PUINT32);
TBSIMP Tbsi_Physical_Presence_Command(TBS_HCONTEXT, const BYTE *, UINT32, PBYTE, PUINT32);
TBSIMP Tbsip_Cancel_Commands(TBS_HCONTEXT);
TBSIMP Tbsip_Context_Close(TBS_HCONTEXT);
TBSIMP Tbsip_Submit_Command(TBS_HCONTEXT, TBS_COMMAND_LOCALITY, TBS_COMMAND_PRIORITY, const BYTE *, UINT32, PBYTE, PUINT32);

#ifdef __cplusplus
}
#endif

#endif /* _TBS_H_ */
