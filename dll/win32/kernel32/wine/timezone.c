/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/wine/timezone.c
 * PURPOSE:         Time conversion functions
 * PROGRAMMER:      Ariadne
 *                  DOSDATE and DOSTIME structures from Onno Hovers
 * UPDATE HISTORY:
 *                  Created 19/01/99
 */

/* INCLUDES ******************************************************************/

#include <k32.h>
#include <ndk/sefuncs.h>

#define NDEBUG
#include <debug.h>

/* TYPES *********************************************************************/

#define TICKSPERMIN 600000000

#define LL2FILETIME( ll, pft )\
    (pft)->dwLowDateTime = (UINT)(ll); \
    (pft)->dwHighDateTime = (UINT)((ll) >> 32);
#define FILETIME2LL( pft, ll) \
    ll = (((LONGLONG)((pft)->dwHighDateTime))<<32) + (pft)-> dwLowDateTime ;

static const int MonthLengths[2][12] =
{
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

/* STATIC FUNCTIONS **********************************************************/

static inline int IsLeapYear(int Year)
{
    return Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0) ? 1 : 0;
}

/***********************************************************************
 *  TIME_DayLightCompareDate
 *
 * Compares two dates without looking at the year.
 *
 * PARAMS
 *   date        [in] The local time to compare.
 *   compareDate [in] The daylight savings begin or end date.
 *
 * RETURNS
 *
 *  -1 if date < compareDate
 *   0 if date == compareDate
 *   1 if date > compareDate
 *  -2 if an error occurs
 */
static int
TIME_DayLightCompareDate(const SYSTEMTIME *date, const SYSTEMTIME *compareDate)
{
    int limit_day, dayinsecs;

    if (date->wMonth < compareDate->wMonth)
        return -1; /* We are in a month before the date limit. */

    if (date->wMonth > compareDate->wMonth)
        return 1; /* We are in a month after the date limit. */

    /* if year is 0 then date is in day-of-week format, otherwise
     * it's absolute date.
     */
    if (compareDate->wYear == 0)
    {
        WORD First;
        /* compareDate->wDay is interpreted as number of the week in the month
         * 5 means: the last week in the month */
        int weekofmonth = compareDate->wDay;
          /* calculate the day of the first DayOfWeek in the month */
        First = ( 6 + compareDate->wDayOfWeek - date->wDayOfWeek + date->wDay 
               ) % 7 + 1;
        limit_day = First + 7 * (weekofmonth - 1);
        /* check needed for the 5th weekday of the month */
        if(limit_day > MonthLengths[date->wMonth==2 && IsLeapYear(date->wYear)]
                [date->wMonth - 1])
            limit_day -= 7;
    }
    else
    {
       limit_day = compareDate->wDay;
    }

    /* convert to seconds */
    limit_day = ((limit_day * 24  + compareDate->wHour) * 60 +
            compareDate->wMinute ) * 60 + compareDate->wSecond;
    dayinsecs = ((date->wDay * 24  + date->wHour) * 60 +
            date->wMinute ) * 60 + date->wSecond;
    /* and compare, down to the millisecond */
    if (dayinsecs != limit_day)
        return dayinsecs < limit_day ? -1 : 1;
    return date->wMilliseconds < compareDate->wMilliseconds ? -1 :
           date->wMilliseconds > compareDate->wMilliseconds ? 1 :
           0;   /* date is equal to the date limit. */
}

/***********************************************************************
 *  TIME_CompTimeZoneID
 *
 *  Computes the local time bias for a given time and time zone.
 *
 *  PARAMS
 *      pTZinfo     [in] The time zone data.
 *      lpFileTime  [in] The system or local time.
 *      islocal     [in] it is local time.
 *
 *  RETURNS
 *      TIME_ZONE_ID_INVALID    An error occurred
 *      TIME_ZONE_ID_UNKNOWN    There are no transition time known
 *      TIME_ZONE_ID_STANDARD   Current time is standard time
 *      TIME_ZONE_ID_DAYLIGHT   Current time is daylight savings time
 */
static
DWORD
TIME_CompTimeZoneID( const TIME_ZONE_INFORMATION *pTZinfo, FILETIME *lpFileTime, BOOL islocal )
{
    int ret, year;
    BOOL beforeStandardDate, afterDaylightDate;
    DWORD retval = TIME_ZONE_ID_INVALID;
    LONGLONG llTime = 0; /* initialized to prevent gcc complaining */
    SYSTEMTIME SysTime;
    FILETIME ftTemp;

    if (pTZinfo->DaylightDate.wMonth != 0)
    {
        /* if year is 0 then date is in day-of-week format, otherwise
         * it's absolute date.
         */
        if (pTZinfo->StandardDate.wMonth == 0 ||
            (pTZinfo->StandardDate.wYear == 0 &&
            (pTZinfo->StandardDate.wDay<1 ||
            pTZinfo->StandardDate.wDay>5 ||
            pTZinfo->DaylightDate.wDay<1 ||
            pTZinfo->DaylightDate.wDay>5)))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return TIME_ZONE_ID_INVALID;
        }

        if (!islocal) {
            FILETIME2LL( lpFileTime, llTime );
            llTime -= pTZinfo->Bias * (LONGLONG)TICKSPERMIN;
            LL2FILETIME( llTime, &ftTemp)
            lpFileTime = &ftTemp;
        }

        FileTimeToSystemTime(lpFileTime, &SysTime);
        year = SysTime.wYear;

        if (!islocal) {
            llTime -= pTZinfo->DaylightBias * (LONGLONG)TICKSPERMIN;
            LL2FILETIME( llTime, &ftTemp)
            FileTimeToSystemTime(lpFileTime, &SysTime);
        }
        
        /* check for daylight savings */
        if(year == SysTime.wYear) {
            ret = TIME_DayLightCompareDate( &SysTime, &pTZinfo->StandardDate);
            if (ret == -2)
                return TIME_ZONE_ID_INVALID;

            beforeStandardDate = ret < 0;
        } else
            beforeStandardDate = SysTime.wYear < year;

        if (!islocal) {
            llTime -= ( pTZinfo->StandardBias - pTZinfo->DaylightBias )
                * (LONGLONG)TICKSPERMIN;
            LL2FILETIME( llTime, &ftTemp)
            FileTimeToSystemTime(lpFileTime, &SysTime);
        }

        if(year == SysTime.wYear) {
            ret = TIME_DayLightCompareDate( &SysTime, &pTZinfo->DaylightDate);
            if (ret == -2)
                return TIME_ZONE_ID_INVALID;

            afterDaylightDate = ret >= 0;
        } else
            afterDaylightDate = SysTime.wYear > year;

        retval = TIME_ZONE_ID_STANDARD;
        if( pTZinfo->DaylightDate.wMonth <  pTZinfo->StandardDate.wMonth ) {
            /* Northern hemisphere */
            if( beforeStandardDate && afterDaylightDate )
                retval = TIME_ZONE_ID_DAYLIGHT;
        } else    /* Down south */
            if( beforeStandardDate || afterDaylightDate )
            retval = TIME_ZONE_ID_DAYLIGHT;
    } else 
        /* No transition date */
        retval = TIME_ZONE_ID_UNKNOWN;
        
    return retval;
}

/***********************************************************************
 *  TIME_TimeZoneID
 *
 *  Calculates whether daylight savings is on now.
 *
 *  PARAMS
 *      pTzi [in] Timezone info.
 *
 *  RETURNS
 *      TIME_ZONE_ID_INVALID    An error occurred
 *      TIME_ZONE_ID_UNKNOWN    There are no transition time known
 *      TIME_ZONE_ID_STANDARD   Current time is standard time
 *      TIME_ZONE_ID_DAYLIGHT   Current time is daylight savings time
 */
static DWORD TIME_ZoneID( const TIME_ZONE_INFORMATION *pTzi )
{
    FILETIME ftTime;
    GetSystemTimeAsFileTime( &ftTime);
    return TIME_CompTimeZoneID( pTzi, &ftTime, FALSE);
}

/***********************************************************************
 *  TIME_GetTimezoneBias
 *
 *  Calculates the local time bias for a given time zone.
 *
 * PARAMS
 *  pTZinfo    [in]  The time zone data.
 *  lpFileTime [in]  The system or local time.
 *  islocal    [in]  It is local time.
 *  pBias      [out] The calculated bias in minutes.
 *
 * RETURNS
 *  TRUE when the time zone bias was calculated.
 */
static BOOL
TIME_GetTimezoneBias(const TIME_ZONE_INFORMATION *pTZinfo, FILETIME *lpFileTime, BOOL islocal, LONG *pBias)
{
    LONG bias = pTZinfo->Bias;
    DWORD tzid = TIME_CompTimeZoneID(pTZinfo, lpFileTime, islocal);

    if( tzid == TIME_ZONE_ID_INVALID)
        return FALSE;
    if (tzid == TIME_ZONE_ID_DAYLIGHT)
        bias += pTZinfo->DaylightBias;
    else if (tzid == TIME_ZONE_ID_STANDARD)
        bias += pTZinfo->StandardBias;
    *pBias = bias;
    return TRUE;
}


/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
DWORD
WINAPI
GetTimeZoneInformation(LPTIME_ZONE_INFORMATION lpTimeZoneInformation)
{
    RTL_TIME_ZONE_INFORMATION TimeZoneInformation;
    NTSTATUS Status;

    DPRINT("GetTimeZoneInformation()\n");

    Status = NtQuerySystemInformation(SystemCurrentTimeZoneInformation,
                                      &TimeZoneInformation,
                                      sizeof(RTL_TIME_ZONE_INFORMATION),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return TIME_ZONE_ID_INVALID;
    }

    lpTimeZoneInformation->Bias = TimeZoneInformation.Bias;

    wcsncpy(lpTimeZoneInformation->StandardName,
            TimeZoneInformation.StandardName,
            ARRAYSIZE(lpTimeZoneInformation->StandardName));
    lpTimeZoneInformation->StandardDate.wYear = TimeZoneInformation.StandardDate.Year;
    lpTimeZoneInformation->StandardDate.wMonth = TimeZoneInformation.StandardDate.Month;
    lpTimeZoneInformation->StandardDate.wDay = TimeZoneInformation.StandardDate.Day;
    lpTimeZoneInformation->StandardDate.wHour = TimeZoneInformation.StandardDate.Hour;
    lpTimeZoneInformation->StandardDate.wMinute = TimeZoneInformation.StandardDate.Minute;
    lpTimeZoneInformation->StandardDate.wSecond = TimeZoneInformation.StandardDate.Second;
    lpTimeZoneInformation->StandardDate.wMilliseconds = TimeZoneInformation.StandardDate.Milliseconds;
    lpTimeZoneInformation->StandardDate.wDayOfWeek = TimeZoneInformation.StandardDate.Weekday;
    lpTimeZoneInformation->StandardBias = TimeZoneInformation.StandardBias;

    wcsncpy(lpTimeZoneInformation->DaylightName,
            TimeZoneInformation.DaylightName,
            ARRAYSIZE(lpTimeZoneInformation->DaylightName));
    lpTimeZoneInformation->DaylightDate.wYear = TimeZoneInformation.DaylightDate.Year;
    lpTimeZoneInformation->DaylightDate.wMonth = TimeZoneInformation.DaylightDate.Month;
    lpTimeZoneInformation->DaylightDate.wDay = TimeZoneInformation.DaylightDate.Day;
    lpTimeZoneInformation->DaylightDate.wHour = TimeZoneInformation.DaylightDate.Hour;
    lpTimeZoneInformation->DaylightDate.wMinute = TimeZoneInformation.DaylightDate.Minute;
    lpTimeZoneInformation->DaylightDate.wSecond = TimeZoneInformation.DaylightDate.Second;
    lpTimeZoneInformation->DaylightDate.wMilliseconds = TimeZoneInformation.DaylightDate.Milliseconds;
    lpTimeZoneInformation->DaylightDate.wDayOfWeek = TimeZoneInformation.DaylightDate.Weekday;
    lpTimeZoneInformation->DaylightBias = TimeZoneInformation.DaylightBias;

    return TIME_ZoneID(lpTimeZoneInformation);
}


/* SeTimeZonePrivilege must be enabled in the effective token; STATUS_SUCCESS when it cannot be determined */
static NTSTATUS
BasepCheckTimeZonePrivilege(VOID)
{
    PRIVILEGE_SET PrivilegeSet;
    HANDLE hToken;
    BOOLEAN Held = FALSE;
    NTSTATUS Status;

    Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, FALSE, &hToken);
    if (Status == STATUS_NO_TOKEN)
        Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &hToken);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    PrivilegeSet.PrivilegeCount = 1;
    PrivilegeSet.Control = PRIVILEGE_SET_ALL_NECESSARY;
    PrivilegeSet.Privilege[0].Luid.LowPart = SE_TIME_ZONE_PRIVILEGE;
    PrivilegeSet.Privilege[0].Luid.HighPart = 0;
    PrivilegeSet.Privilege[0].Attributes = 0;
    Status = NtPrivilegeCheck(hToken, &PrivilegeSet, &Held);
    NtClose(hToken);

    return (NT_SUCCESS(Status) && !Held) ? STATUS_PRIVILEGE_NOT_HELD : STATUS_SUCCESS;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetTimeZoneInformation(CONST TIME_ZONE_INFORMATION *lpTimeZoneInformation)
{
    RTL_TIME_ZONE_INFORMATION TimeZoneInformation, OldTimeZoneInformation;
    BOOLEAN HaveOldZone;
    NTSTATUS Status;

    DPRINT("SetTimeZoneInformation()\n");

    TimeZoneInformation.Bias = lpTimeZoneInformation->Bias;

    wcsncpy(TimeZoneInformation.StandardName,
            lpTimeZoneInformation->StandardName,
            ARRAYSIZE(TimeZoneInformation.StandardName));
    TimeZoneInformation.StandardDate.Year = lpTimeZoneInformation->StandardDate.wYear;
    TimeZoneInformation.StandardDate.Month = lpTimeZoneInformation->StandardDate.wMonth;
    TimeZoneInformation.StandardDate.Day = lpTimeZoneInformation->StandardDate.wDay;
    TimeZoneInformation.StandardDate.Hour = lpTimeZoneInformation->StandardDate.wHour;
    TimeZoneInformation.StandardDate.Minute = lpTimeZoneInformation->StandardDate.wMinute;
    TimeZoneInformation.StandardDate.Second = lpTimeZoneInformation->StandardDate.wSecond;
    TimeZoneInformation.StandardDate.Milliseconds = lpTimeZoneInformation->StandardDate.wMilliseconds;
    TimeZoneInformation.StandardDate.Weekday = lpTimeZoneInformation->StandardDate.wDayOfWeek;
    TimeZoneInformation.StandardBias = lpTimeZoneInformation->StandardBias;

    wcsncpy(TimeZoneInformation.DaylightName,
            lpTimeZoneInformation->DaylightName,
            ARRAYSIZE(TimeZoneInformation.DaylightName));
    TimeZoneInformation.DaylightDate.Year = lpTimeZoneInformation->DaylightDate.wYear;
    TimeZoneInformation.DaylightDate.Month = lpTimeZoneInformation->DaylightDate.wMonth;
    TimeZoneInformation.DaylightDate.Day = lpTimeZoneInformation->DaylightDate.wDay;
    TimeZoneInformation.DaylightDate.Hour = lpTimeZoneInformation->DaylightDate.wHour;
    TimeZoneInformation.DaylightDate.Minute = lpTimeZoneInformation->DaylightDate.wMinute;
    TimeZoneInformation.DaylightDate.Second = lpTimeZoneInformation->DaylightDate.wSecond;
    TimeZoneInformation.DaylightDate.Milliseconds = lpTimeZoneInformation->DaylightDate.wMilliseconds;
    TimeZoneInformation.DaylightDate.Weekday = lpTimeZoneInformation->DaylightDate.wDayOfWeek;
    TimeZoneInformation.DaylightBias = lpTimeZoneInformation->DaylightBias;

    /* Refuse before the registry is written; the kernel's own check stays authoritative */
    Status = BasepCheckTimeZonePrivilege();
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Values missing from the registry stay zero */
    RtlZeroMemory(&OldTimeZoneInformation, sizeof(OldTimeZoneInformation));
    HaveOldZone = NT_SUCCESS(RtlQueryTimeZoneInformation(&OldTimeZoneInformation));

    Status = RtlSetTimeZoneInformation(&TimeZoneInformation);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("RtlSetTimeZoneInformation() failed (Status %lx)\n", Status);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    Status = NtSetSystemInformation(SystemCurrentTimeZoneInformation,
                                    (PVOID)&TimeZoneInformation,
                                    sizeof(RTL_TIME_ZONE_INFORMATION));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtSetSystemInformation() failed (Status %lx)\n", Status);
        if (HaveOldZone)
            RtlSetTimeZoneInformation(&OldTimeZoneInformation);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SystemTimeToTzSpecificLocalTime(CONST TIME_ZONE_INFORMATION *lpTimeZoneInformation,
                                CONST SYSTEMTIME *lpUniversalTime,
                                LPSYSTEMTIME lpLocalTime)
{
    TIME_ZONE_INFORMATION TzInfo;
    FILETIME FileTime;
    LONGLONG llTime;
    LONG lBias;

    if (lpTimeZoneInformation != NULL)
    {
        TzInfo = *lpTimeZoneInformation;
    }
    else
    {
        if (GetTimeZoneInformation(&TzInfo) == TIME_ZONE_ID_INVALID)
            return FALSE;
    }

    if (!lpUniversalTime || !lpLocalTime)
        return FALSE;

    if (!SystemTimeToFileTime(lpUniversalTime, &FileTime))
        return FALSE;

    FILETIME2LL(&FileTime, llTime)

    if (!TIME_GetTimezoneBias(&TzInfo, &FileTime, FALSE, &lBias))
        return FALSE;

    /* convert minutes to 100-nanoseconds-ticks */
    llTime -= (LONGLONG)lBias * TICKSPERMIN;

    LL2FILETIME( llTime, &FileTime)

    return FileTimeToSystemTime(&FileTime, lpLocalTime);
}


/*
 * @implemented (Wine 13 sep 2008)
 */
BOOL
WINAPI
TzSpecificLocalTimeToSystemTime(CONST TIME_ZONE_INFORMATION *lpTimeZoneInformation,
                                CONST SYSTEMTIME *lpLocalTime,
                                LPSYSTEMTIME lpUniversalTime)
{
    FILETIME ft;
    LONG lBias;
    LONGLONG t;
    TIME_ZONE_INFORMATION tzinfo;

    if (lpTimeZoneInformation != NULL)
    {
        tzinfo = *lpTimeZoneInformation;
    }
    else
    {
        if (GetTimeZoneInformation(&tzinfo) == TIME_ZONE_ID_INVALID)
            return FALSE;
    }

    if (!SystemTimeToFileTime(lpLocalTime, &ft))
        return FALSE;
    FILETIME2LL( &ft, t)
    if (!TIME_GetTimezoneBias(&tzinfo, &ft, TRUE, &lBias))
        return FALSE;
    /* convert minutes to 100-nanoseconds-ticks */
    t += (LONGLONG)lBias * TICKSPERMIN;
    LL2FILETIME( t, &ft)
    return FileTimeToSystemTime(&ft, lpUniversalTime);
}

/* REG_TZI_FORMAT: the TZI value of a Time Zones entry */
typedef struct _TIME_TZI_FORMAT
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    SYSTEMTIME StandardDate;
    SYSTEMTIME DaylightDate;
} TIME_TZI_FORMAT;

/* Reads a REG_SZ value into a buffer of BufferLength characters, terminated; FALSE if absent, of another type or too long */
static
BOOL
TIME_QueryStringValue(
    _In_ HANDLE Key,
    _In_ PCWSTR ValueName,
    _Out_writes_(BufferLength) PWSTR Buffer,
    _In_ ULONG BufferLength)
{
    UNICODE_STRING Name;
    ULONG Length, DataLength;
    NTSTATUS Status;
    union
    {
        KEY_VALUE_PARTIAL_INFORMATION Partial;
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 128 * sizeof(WCHAR)];
    } Info;

    RtlInitUnicodeString(&Name, ValueName);
    Status = NtQueryValueKey(Key, &Name, KeyValuePartialInformation, &Info, sizeof(Info), &Length);
    if (!NT_SUCCESS(Status) || Info.Partial.Type != REG_SZ || (Info.Partial.DataLength % sizeof(WCHAR)))
        return FALSE;

    /* Drop the terminator(s) stored with the string */
    DataLength = Info.Partial.DataLength;
    while (DataLength >= sizeof(WCHAR) && !((PWCHAR)Info.Partial.Data)[DataLength / sizeof(WCHAR) - 1])
        DataLength -= sizeof(WCHAR);
    if (DataLength / sizeof(WCHAR) >= BufferLength)
        return FALSE;

    RtlCopyMemory(Buffer, Info.Partial.Data, DataLength);
    Buffer[DataLength / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

/* Reads a REG_DWORD value; FALSE if absent or not a four-byte REG_DWORD */
static
BOOL
TIME_QueryDwordValue(
    _In_ HANDLE Key,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value)
{
    UNICODE_STRING Name;
    ULONG Length;
    NTSTATUS Status;
    union
    {
        KEY_VALUE_PARTIAL_INFORMATION Partial;
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    } Info;

    RtlInitUnicodeString(&Name, ValueName);
    Status = NtQueryValueKey(Key, &Name, KeyValuePartialInformation, &Info, sizeof(Info), &Length);
    if (!NT_SUCCESS(Status) || Info.Partial.Type != REG_DWORD || Info.Partial.DataLength != sizeof(ULONG))
        return FALSE;

    *Value = *(PULONG)Info.Partial.Data;
    return TRUE;
}

/* Whether a Time Zones entry describes the zone in effect: same standard name, biases and transitions */
static
BOOL
TIME_ZoneKeyMatches(
    _In_ HANDLE ZoneKey,
    _In_ const TIME_ZONE_INFORMATION *TimeZoneInformation)
{
    UNICODE_STRING TziName = RTL_CONSTANT_STRING(L"TZI");
    WCHAR Std[ARRAYSIZE(TimeZoneInformation->StandardName)];
    TIME_TZI_FORMAT *Tzi;
    ULONG Length;
    NTSTATUS Status;
    union
    {
        KEY_VALUE_PARTIAL_INFORMATION Partial;
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(TIME_TZI_FORMAT)];
    } Info;

    if (!TIME_QueryStringValue(ZoneKey, L"Std", Std, ARRAYSIZE(Std)) ||
        _wcsicmp(Std, TimeZoneInformation->StandardName))
    {
        return FALSE;
    }

    Status = NtQueryValueKey(ZoneKey, &TziName, KeyValuePartialInformation, &Info, sizeof(Info), &Length);
    if (!NT_SUCCESS(Status) || Info.Partial.Type != REG_BINARY || Info.Partial.DataLength != sizeof(TIME_TZI_FORMAT))
        return FALSE;

    Tzi = (TIME_TZI_FORMAT *)Info.Partial.Data;
    return Tzi->Bias == TimeZoneInformation->Bias &&
           Tzi->StandardBias == TimeZoneInformation->StandardBias &&
           Tzi->DaylightBias == TimeZoneInformation->DaylightBias &&
           !memcmp(&Tzi->StandardDate, &TimeZoneInformation->StandardDate, sizeof(SYSTEMTIME)) &&
           !memcmp(&Tzi->DaylightDate, &TimeZoneInformation->DaylightDate, sizeof(SYSTEMTIME));
}

/*
 * A registry written before TimeZoneKeyName existed (no value at all): the
 * Time Zones entry that describes the zone in effect. Names
 * alone are ambiguous (two zones can share a localized standard name), so the
 * biases and transitions must match as well, and nothing is returned unless
 * exactly one entry matches.
 */
static
BOOL
TIME_FindTimeZoneKeyName(
    _In_ const TIME_ZONE_INFORMATION *TimeZoneInformation,
    _Out_writes_(KeyNameLength) PWSTR KeyName,
    _In_ ULONG KeyNameLength)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ZonesKey, ZoneKey;
    ULONG Index, Length, Matches = 0;
    NTSTATUS Status;
    union
    {
        KEY_BASIC_INFORMATION Basic;
        UCHAR Buffer[sizeof(KEY_BASIC_INFORMATION) + 128 * sizeof(WCHAR)];
    } KeyInfo;

    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(NtOpenKey(&ZonesKey, KEY_ENUMERATE_SUB_KEYS, &ObjectAttributes)))
        return FALSE;

    for (Index = 0; ; Index++)
    {
        UNICODE_STRING SubKey;

        Status = NtEnumerateKey(ZonesKey, Index, KeyBasicInformation, &KeyInfo, sizeof(KeyInfo), &Length);
        if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_BUFFER_TOO_SMALL)
        {
            /* A name too long for a time zone key name: not a candidate */
            continue;
        }
        if (!NT_SUCCESS(Status))
        {
            /* STATUS_NO_MORE_ENTRIES, or the key is gone (then the search is incomplete): stop */
            break;
        }
        if (KeyInfo.Basic.NameLength >= KeyNameLength * sizeof(WCHAR))
            continue;

        SubKey.Buffer = KeyInfo.Basic.Name;
        SubKey.Length = SubKey.MaximumLength = (USHORT)KeyInfo.Basic.NameLength;
        InitializeObjectAttributes(&ObjectAttributes, &SubKey, OBJ_CASE_INSENSITIVE, ZonesKey, NULL);
        if (!NT_SUCCESS(NtOpenKey(&ZoneKey, KEY_QUERY_VALUE, &ObjectAttributes)))
            continue;

        if (TIME_ZoneKeyMatches(ZoneKey, TimeZoneInformation))
        {
            if (++Matches == 1)
            {
                RtlCopyMemory(KeyName, KeyInfo.Basic.Name, KeyInfo.Basic.NameLength);
                KeyName[KeyInfo.Basic.NameLength / sizeof(WCHAR)] = UNICODE_NULL;
            }
        }
        NtClose(ZoneKey);
    }

    NtClose(ZonesKey);

    /* Unique only if every entry was seen */
    if (Matches != 1 || Status != STATUS_NO_MORE_ENTRIES)
    {
        RtlZeroMemory(KeyName, KeyNameLength * sizeof(WCHAR));
        return FALSE;
    }
    return TRUE;
}

/*
 * @implemented
 */
DWORD
WINAPI
GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION pTimeZoneInformation)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\TimeZoneInformation");
    TIME_ZONE_INFORMATION TimeZoneInformation;
    OBJECT_ATTRIBUTES ObjectAttributes;
    WCHAR KeyName[ARRAYSIZE(pTimeZoneInformation->TimeZoneKeyName)];
    ULONG DynamicDisabled = 0, AutoDisabled = 0;
    BOOL HaveKeyName = FALSE;
    HANDLE Key;
    DWORD Result;

    Result = GetTimeZoneInformation(&TimeZoneInformation);
    if (Result == TIME_ZONE_ID_INVALID)
        return TIME_ZONE_ID_INVALID;

    RtlZeroMemory(KeyName, sizeof(KeyName));
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(NtOpenKey(&Key, KEY_QUERY_VALUE, &ObjectAttributes)))
    {
        /*
         * Values that are missing or malformed leave their defaults. An empty
         * key name is a zone without one (on Windows SetTimeZoneInformation
         * stores that); a missing one is a registry from before it existed.
         */
        HaveKeyName = TIME_QueryStringValue(Key, L"TimeZoneKeyName", KeyName, ARRAYSIZE(KeyName));
        if (!HaveKeyName)
            KeyName[0] = UNICODE_NULL;
        TIME_QueryDwordValue(Key, L"DynamicDaylightTimeDisabled", &DynamicDisabled);
        TIME_QueryDwordValue(Key, L"DisableAutoDaylightTimeSet", &AutoDisabled);
        NtClose(Key);
    }

    if (!HaveKeyName)
        TIME_FindTimeZoneKeyName(&TimeZoneInformation, KeyName, ARRAYSIZE(KeyName));

    RtlZeroMemory(pTimeZoneInformation, sizeof(*pTimeZoneInformation));
    RtlCopyMemory(pTimeZoneInformation, &TimeZoneInformation, sizeof(TimeZoneInformation));
    RtlCopyMemory(pTimeZoneInformation->TimeZoneKeyName, KeyName, sizeof(KeyName));
    pTimeZoneInformation->DynamicDaylightTimeDisabled = (DynamicDisabled || AutoDisabled);

    /*
     * The flag says only that the transition dates are fixed, not that there
     * is no daylight saving time: as on Windows, the zone in effect is the one
     * GetTimeZoneInformation reports.
     */
    return Result;
}

/* EOF */
