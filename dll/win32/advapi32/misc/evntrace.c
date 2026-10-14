/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     advapi32.dll Event tracing stubs
 * COPYRIGHT:   Copyright 2017 Mark Jansen (mark.jansen@reactos.org)
 */

#include <advapi32.h>
#include <wmistr.h>
#include <evntrace.h>

WINE_DEFAULT_DEBUG_CHANNEL(advapi);


TRACEHANDLE
WINAPI
OpenTraceA(IN PEVENT_TRACE_LOGFILEA Logfile)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_PROCESSTRACE_HANDLE;
}

TRACEHANDLE
WINAPI
OpenTraceW(IN PEVENT_TRACE_LOGFILEW Logfile)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_PROCESSTRACE_HANDLE;
}

ULONG
WINAPI
ProcessTrace(IN PTRACEHANDLE HandleArray,
             IN ULONG HandleCount,
             IN LPFILETIME StartTime,
             IN LPFILETIME EndTime)
{
    UNIMPLEMENTED;
    return ERROR_NOACCESS;
}


/*
 * ReactOS has no event tracing sessions. Windows 7 and Windows 10 (measured) refuse a null handle or provider
 * and a control code past capture-state with ERROR_INVALID_PARAMETER and report a session that does not exist
 * with ERROR_WMI_INSTANCE_NOT_FOUND; here no session exists, so every other request ends that way.
 */
ULONG
WINAPI
EnableTraceEx2(
    IN TRACEHANDLE TraceHandle,
    IN LPCGUID ProviderId,
    IN ULONG ControlCode,
    IN UCHAR Level,
    IN ULONGLONG MatchAnyKeyword,
    IN ULONGLONG MatchAllKeyword,
    IN ULONG Timeout,
    IN PENABLE_TRACE_PARAMETERS EnableParameters OPTIONAL)
{
    ULONG Status;

    TRACE("(%I64x, %p, %lu, %u)\n", TraceHandle, ProviderId, ControlCode, Level);

    if (!TraceHandle || !ProviderId || ControlCode > EVENT_CONTROL_CODE_CAPTURE_STATE)
        Status = ERROR_INVALID_PARAMETER;
    else
        Status = ERROR_WMI_INSTANCE_NOT_FOUND;

    /* Windows sets the last error as well (measured) */
    SetLastError(Status);
    return Status;
}
