/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetUserDefaultGeoName (Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * After GetUserDefaultGeoName of Wine's kernelbase (dlls/kernelbase/locale.c). The name is the one that the user
 * set (the value Name under Control Panel\International\Geo, an ISO 3166 code or a United Nations region such as
 * "150"); without it, the ISO code of the nation of the user, or "001" for the world, with the terminator counted
 * (the length and the size rules measured on Windows 10).
 */

#include "k32_vista.h"
#include <ndk/cmfuncs.h>

#define NDEBUG
#include <debug.h>

#ifndef REG_SZ
#define REG_SZ 1
#endif
#ifndef GEOCLASS_NATION
#define GEOCLASS_NATION 16
#endif
#ifndef GEO_ISO2
#define GEO_ISO2 4
#endif

#define GEOID_WORLD 39070
#define GEO_NAME_CHARS 32

static
BOOL
QueryUserGeoName(
    _Out_writes_(GEO_NAME_CHARS) PWCHAR Name)
{
    static const WCHAR KeyPath[] = L"Control Panel\\International\\Geo";
    UNICODE_STRING KeyName, ValueName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + GEO_NAME_CHARS * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    HANDLE Root, Key;
    ULONG Length, Chars;
    NTSTATUS Status;

    Status = RtlOpenCurrentUser(KEY_READ, &Root);
    if (!NT_SUCCESS(Status))
        return FALSE;

    RtlInitUnicodeString(&KeyName, KeyPath);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName, OBJ_CASE_INSENSITIVE, Root, NULL);
    Status = NtOpenKey(&Key, KEY_QUERY_VALUE, &ObjectAttributes);
    NtClose(Root);
    if (!NT_SUCCESS(Status))
        return FALSE;

    RtlInitUnicodeString(&ValueName, L"Name");
    Status = NtQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Buffer, sizeof(Buffer), &Length);
    NtClose(Key);
    if (!NT_SUCCESS(Status) || Info->Type != REG_SZ || Info->DataLength < sizeof(WCHAR))
        return FALSE;

    Chars = Info->DataLength / sizeof(WCHAR);
    RtlCopyMemory(Name, Info->Data, Chars * sizeof(WCHAR));
    Name[min(Chars, GEO_NAME_CHARS - 1)] = UNICODE_NULL;
    return Name[0] != UNICODE_NULL;
}

int
WINAPI
GetUserDefaultGeoName(
    _Out_writes_to_opt_(count, return) LPWSTR geoName,
    _In_ int count)
{
    WCHAR Name[GEO_NAME_CHARS];
    GEOID Nation;
    int Length;

    if (count < 0 || (count && !geoName))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!QueryUserGeoName(Name))
    {
        /* the lookup that follows can fail, and its error is not the one of this call */
        DWORD Error = GetLastError();

        Nation = GetUserGeoID(GEOCLASS_NATION);
        if (Nation == GEOID_WORLD || GetGeoInfoW(Nation, GEO_ISO2, Name, RTL_NUMBER_OF(Name), 0) <= 0)
            wcscpy(Name, L"001");
        SetLastError(Error);
    }

    Length = (int)wcslen(Name) + 1;

    /* A count of zero asks for the size */
    if (count == 0)
        return Length;

    if (count < Length)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    RtlCopyMemory(geoName, Name, Length * sizeof(WCHAR));
    return Length;
}
