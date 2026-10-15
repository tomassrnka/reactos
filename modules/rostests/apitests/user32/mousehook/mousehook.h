/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared records between the MouseHookTransport test and its hook module
 */

#ifndef MOUSEHOOK_H
#define MOUSEHOOK_H

#define MOUSEHOOK_MAPPING_NAME L"ReactOSMouseHookTransportShared"
#define MOUSEHOOK_MAX_RECORDS 256

typedef struct
{
    DWORD message;
    DWORD processId;
    DWORD threadId;
    DWORD size;
    BYTE data[64];
} MOUSEHOOK_RECORD;

typedef struct
{
    LONG count;
    MOUSEHOOK_RECORD record[MOUSEHOOK_MAX_RECORDS];
} MOUSEHOOK_SHARED;

#endif
