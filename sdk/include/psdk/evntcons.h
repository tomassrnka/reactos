/*
 * evntcons.h
 *
 * Event Tracing for Windows: event consumer definitions.
 *
 * This file is part of the ReactOS PSDK package.
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED
 *
 * This source code is offered for use in the public domain. You may
 * use, modify or distribute it freely.
 *
 * This code is distributed in the hope that it will be useful but
 * WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 * DISCLAIMED. This includes but is not limited to warranties of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#pragma once

#include <wmistr.h>
#include <evntrace.h>
#include <evntprov.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_HEADER_EXT_TYPE_RELATED_ACTIVITYID  0x0001
#define EVENT_HEADER_EXT_TYPE_SID                 0x0002
#define EVENT_HEADER_EXT_TYPE_TS_ID               0x0003
#define EVENT_HEADER_EXT_TYPE_INSTANCE_INFO       0x0004
#define EVENT_HEADER_EXT_TYPE_STACK_TRACE32       0x0005
#define EVENT_HEADER_EXT_TYPE_STACK_TRACE64       0x0006
#define EVENT_HEADER_EXT_TYPE_MAX                 0x0007

#define EVENT_HEADER_PROPERTY_XML                 0x0001
#define EVENT_HEADER_PROPERTY_FORWARDED_XML       0x0002
#define EVENT_HEADER_PROPERTY_LEGACY_EVENTLOG     0x0004

#define EVENT_HEADER_FLAG_EXTENDED_INFO           0x0001
#define EVENT_HEADER_FLAG_PRIVATE_SESSION         0x0002
#define EVENT_HEADER_FLAG_STRING_ONLY             0x0004
#define EVENT_HEADER_FLAG_TRACE_MESSAGE           0x0008
#define EVENT_HEADER_FLAG_NO_CPUTIME              0x0010
#define EVENT_HEADER_FLAG_32_BIT_HEADER           0x0020
#define EVENT_HEADER_FLAG_64_BIT_HEADER           0x0040
#define EVENT_HEADER_FLAG_CLASSIC_HEADER          0x0100

#define EVENT_ENABLE_PROPERTY_SID                 0x00000001
#define EVENT_ENABLE_PROPERTY_TS_ID               0x00000002
#define EVENT_ENABLE_PROPERTY_STACK_TRACE         0x00000004

#define PROCESS_TRACE_MODE_REAL_TIME              0x00000100
#define PROCESS_TRACE_MODE_RAW_TIMESTAMP          0x00001000
#define PROCESS_TRACE_MODE_EVENT_RECORD           0x10000000

typedef struct _EVENT_HEADER_EXTENDED_DATA_ITEM {
  USHORT Reserved1;
  USHORT ExtType;
  _ANONYMOUS_STRUCT struct {
    USHORT Linkage:1;
    USHORT Reserved2:15;
  } DUMMYSTRUCTNAME;
  USHORT DataSize;
  ULONGLONG DataPtr;
} EVENT_HEADER_EXTENDED_DATA_ITEM, *PEVENT_HEADER_EXTENDED_DATA_ITEM;

typedef struct _EVENT_EXTENDED_ITEM_INSTANCE {
  ULONG InstanceId;
  ULONG ParentInstanceId;
  GUID ParentGuid;
} EVENT_EXTENDED_ITEM_INSTANCE, *PEVENT_EXTENDED_ITEM_INSTANCE;

typedef struct _EVENT_EXTENDED_ITEM_RELATED_ACTIVITYID {
  GUID RelatedActivityId;
} EVENT_EXTENDED_ITEM_RELATED_ACTIVITYID, *PEVENT_EXTENDED_ITEM_RELATED_ACTIVITYID;

typedef struct _EVENT_EXTENDED_ITEM_TS_ID {
  ULONG SessionId;
} EVENT_EXTENDED_ITEM_TS_ID, *PEVENT_EXTENDED_ITEM_TS_ID;

typedef struct _EVENT_EXTENDED_ITEM_STACK_TRACE32 {
  ULONG64 MatchId;
  ULONG Address[ANYSIZE_ARRAY];
} EVENT_EXTENDED_ITEM_STACK_TRACE32, *PEVENT_EXTENDED_ITEM_STACK_TRACE32;

typedef struct _EVENT_EXTENDED_ITEM_STACK_TRACE64 {
  ULONG64 MatchId;
  ULONG64 Address[ANYSIZE_ARRAY];
} EVENT_EXTENDED_ITEM_STACK_TRACE64, *PEVENT_EXTENDED_ITEM_STACK_TRACE64;

typedef struct _EVENT_HEADER {
  USHORT Size;
  USHORT HeaderType;
  USHORT Flags;
  USHORT EventProperty;
  ULONG ThreadId;
  ULONG ProcessId;
  LARGE_INTEGER TimeStamp;
  GUID ProviderId;
  EVENT_DESCRIPTOR EventDescriptor;
  _ANONYMOUS_UNION union {
    _ANONYMOUS_STRUCT struct {
      ULONG KernelTime;
      ULONG UserTime;
    } DUMMYSTRUCTNAME;
    ULONG64 ProcessorTime;
  } DUMMYUNIONNAME;
  GUID ActivityId;
} EVENT_HEADER, *PEVENT_HEADER;

struct _EVENT_RECORD {
  EVENT_HEADER EventHeader;
  ETW_BUFFER_CONTEXT BufferContext;
  USHORT ExtendedDataCount;
  USHORT UserDataLength;
  PEVENT_HEADER_EXTENDED_DATA_ITEM ExtendedData;
  PVOID UserData;
  PVOID UserContext;
};

typedef const EVENT_RECORD *PCEVENT_RECORD;

#ifdef __cplusplus
}
#endif
