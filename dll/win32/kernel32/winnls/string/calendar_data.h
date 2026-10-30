/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Calendar data of the locales
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#pragma once

#include <windef.h>

/* A value of a locale and calendar: key is (LANGID << 16) | (calendar << 8) | CALTYPE, Offset is that of
 * the NUL-terminated string in the string pool (several NUL-separated strings for the enumeration rows) */
typedef struct _NLS_CAL_ROW
{
    DWORD Key;
    DWORD Offset;
} NLS_CAL_ROW;

#define NLS_CAL_MAX 8
typedef struct _NLS_CAL_LIST
{
    WORD LangId;
    WORD Count;
    BYTE Calendars[NLS_CAL_MAX]; /* the default calendar first */
} NLS_CAL_LIST;

const WCHAR *NlsCalPoolBase(void);
const NLS_CAL_ROW *NlsCalRowTable(ULONG *Count);
const NLS_CAL_ROW *NlsCalEnumRowTable(ULONG *Count);
const NLS_CAL_LIST *NlsCalListTable(ULONG *Count);
