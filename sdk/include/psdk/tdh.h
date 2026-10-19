/*
 * tdh.h
 *
 * Trace Data Helper API: decoding of Event Tracing for Windows events.
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

#include <evntprov.h>
#include <evntcons.h>

#ifndef TDHSTATUS
#define TDHSTATUS ULONG
#endif

#ifndef TDHAPI
#ifdef _TDH_SOURCE_
#define TDHAPI TDHSTATUS __stdcall
#else
#define TDHAPI DECLSPEC_IMPORT TDHSTATUS __stdcall
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef PVOID TDH_HANDLE, *PTDH_HANDLE;

typedef enum _EVENT_FIELD_TYPE {
  EventKeywordInformation = 0,
  EventLevelInformation = 1,
  EventChannelInformation = 2,
  EventTaskInformation = 3,
  EventOpcodeInformation = 4,
  EventInformationMax = 5
} EVENT_FIELD_TYPE;

typedef struct _EVENT_MAP_ENTRY {
  ULONG OutputOffset;
  _ANONYMOUS_UNION union {
    ULONG Value;
    ULONG InputOffset;
  } DUMMYUNIONNAME;
} EVENT_MAP_ENTRY, *PEVENT_MAP_ENTRY;

typedef enum _MAP_VALUETYPE {
  EVENTMAP_ENTRY_VALUETYPE_ULONG = 0,
  EVENTMAP_ENTRY_VALUETYPE_STRING = 1
} MAP_VALUETYPE;

typedef enum _MAP_FLAGS {
  EVENTMAP_INFO_FLAG_MANIFEST_VALUEMAP = 0x1,
  EVENTMAP_INFO_FLAG_MANIFEST_BITMAP = 0x2,
  EVENTMAP_INFO_FLAG_MANIFEST_PATTERNMAP = 0x4,
  EVENTMAP_INFO_FLAG_WBEM_VALUEMAP = 0x8,
  EVENTMAP_INFO_FLAG_WBEM_BITMAP = 0x10,
  EVENTMAP_INFO_FLAG_WBEM_FLAG = 0x20,
  EVENTMAP_INFO_FLAG_WBEM_NO_MAP = 0x40
} MAP_FLAGS;

typedef struct _EVENT_MAP_INFO {
  ULONG NameOffset;
  MAP_FLAGS Flag;
  ULONG EntryCount;
  _ANONYMOUS_UNION union {
    MAP_VALUETYPE MapEntryValueType;
    ULONG FormatStringOffset;
  } DUMMYUNIONNAME;
  EVENT_MAP_ENTRY MapEntryArray[ANYSIZE_ARRAY];
} EVENT_MAP_INFO, *PEVENT_MAP_INFO;

typedef enum _PROPERTY_FLAGS {
  PropertyStruct = 0x1,
  PropertyParamLength = 0x2,
  PropertyParamCount = 0x4,
  PropertyWBEMXmlFragment = 0x8,
  PropertyParamFixedLength = 0x10
} PROPERTY_FLAGS;

typedef struct _EVENT_PROPERTY_INFO {
  PROPERTY_FLAGS Flags;
  ULONG NameOffset;
  _ANONYMOUS_UNION union {
    struct {
      USHORT InType;
      USHORT OutType;
      ULONG MapNameOffset;
    } nonStructType;
    struct {
      USHORT StructStartIndex;
      USHORT NumOfStructMembers;
      ULONG padding;
    } structType;
  } DUMMYUNIONNAME;
  _ANONYMOUS_UNION union {
    USHORT count;
    USHORT countPropertyIndex;
  } DUMMYUNIONNAME2;
  _ANONYMOUS_UNION union {
    USHORT length;
    USHORT lengthPropertyIndex;
  } DUMMYUNIONNAME3;
  ULONG Reserved;
} EVENT_PROPERTY_INFO, *PEVENT_PROPERTY_INFO;

typedef enum _DECODING_SOURCE {
  DecodingSourceXMLFile = 0,
  DecodingSourceWbem = 1,
  DecodingSourceWPP = 2
} DECODING_SOURCE;

typedef enum _TDH_CONTEXT_TYPE {
  TDH_CONTEXT_WPP_TMFFILE = 0,
  TDH_CONTEXT_WPP_TMFSEARCHPATH = 1,
  TDH_CONTEXT_WPP_GMT = 2,
  TDH_CONTEXT_POINTERSIZE = 3,
  TDH_CONTEXT_MAXIMUM = 4
} TDH_CONTEXT_TYPE;

typedef enum _TEMPLATE_FLAGS {
  TEMPLATE_EVENT_DATA = 1,
  TEMPLATE_USER_DATA = 2
} TEMPLATE_FLAGS;

typedef struct _TRACE_EVENT_INFO {
  GUID ProviderGuid;
  GUID EventGuid;
  EVENT_DESCRIPTOR EventDescriptor;
  DECODING_SOURCE DecodingSource;
  ULONG ProviderNameOffset;
  ULONG LevelNameOffset;
  ULONG ChannelNameOffset;
  ULONG KeywordsNameOffset;
  ULONG TaskNameOffset;
  ULONG OpcodeNameOffset;
  ULONG EventMessageOffset;
  ULONG ProviderMessageOffset;
  ULONG BinaryXMLOffset;
  ULONG BinaryXMLSize;
  ULONG ActivityIDNameOffset;
  ULONG RelatedActivityIDNameOffset;
  ULONG PropertyCount;
  ULONG TopLevelPropertyCount;
  TEMPLATE_FLAGS Flags;
  EVENT_PROPERTY_INFO EventPropertyInfoArray[ANYSIZE_ARRAY];
} TRACE_EVENT_INFO, *PTRACE_EVENT_INFO;

typedef struct _PROPERTY_DATA_DESCRIPTOR {
  ULONGLONG PropertyName;
  ULONG ArrayIndex;
  ULONG Reserved;
} PROPERTY_DATA_DESCRIPTOR, *PPROPERTY_DATA_DESCRIPTOR;

typedef struct _TRACE_PROVIDER_INFO {
  GUID ProviderGuid;
  ULONG SchemaSource;
  ULONG ProviderNameOffset;
} TRACE_PROVIDER_INFO;

typedef struct _PROVIDER_ENUMERATION_INFO {
  ULONG NumberOfProviders;
  ULONG Reserved;
  TRACE_PROVIDER_INFO TraceProviderInfoArray[ANYSIZE_ARRAY];
} PROVIDER_ENUMERATION_INFO, *PPROVIDER_ENUMERATION_INFO;

typedef struct _PROVIDER_FIELD_INFO {
  ULONG NameOffset;
  ULONG DescriptionOffset;
  ULONGLONG Value;
} PROVIDER_FIELD_INFO;

typedef struct _PROVIDER_FIELD_INFOARRAY {
  ULONG NumberOfElements;
  EVENT_FIELD_TYPE FieldType;
  PROVIDER_FIELD_INFO FieldInfoArray[ANYSIZE_ARRAY];
} PROVIDER_FIELD_INFOARRAY, *PPROVIDER_FIELD_INFOARRAY;

typedef struct _PROVIDER_EVENT_INFO {
  ULONG NumberOfEvents;
  ULONG Reserved;
  EVENT_DESCRIPTOR EventDescriptorsArray[ANYSIZE_ARRAY];
} PROVIDER_EVENT_INFO, *PPROVIDER_EVENT_INFO;

typedef struct _PROVIDER_FILTER_INFO {
  UCHAR Id;
  UCHAR Version;
  ULONG MessageOffset;
  ULONG Reserved;
  ULONG PropertyCount;
  EVENT_PROPERTY_INFO EventPropertyInfoArray[ANYSIZE_ARRAY];
} PROVIDER_FILTER_INFO, *PPROVIDER_FILTER_INFO;

typedef struct _TDH_CONTEXT {
  ULONGLONG ParameterValue;
  TDH_CONTEXT_TYPE ParameterType;
  ULONG ParameterSize;
} TDH_CONTEXT, *PTDH_CONTEXT;

TDHAPI
TdhGetEventInformation(
  _In_ PEVENT_RECORD Event,
  _In_ ULONG TdhContextCount,
  _In_reads_opt_(TdhContextCount) PTDH_CONTEXT TdhContext,
  _Out_writes_bytes_opt_(*BufferSize) PTRACE_EVENT_INFO Buffer,
  _Inout_ PULONG BufferSize);

TDHAPI
TdhGetEventMapInformation(
  _In_ PEVENT_RECORD pEvent,
  _In_ LPWSTR pMapName,
  _Out_writes_bytes_opt_(*pBufferSize) PEVENT_MAP_INFO pBuffer,
  _Inout_ PULONG pBufferSize);

TDHAPI
TdhGetProperty(
  _In_ PEVENT_RECORD pEvent,
  _In_ ULONG TdhContextCount,
  _In_reads_opt_(TdhContextCount) PTDH_CONTEXT pTdhContext,
  _In_ ULONG PropertyDataCount,
  _In_reads_(PropertyDataCount) PPROPERTY_DATA_DESCRIPTOR pPropertyData,
  _In_ ULONG BufferSize,
  _Out_writes_bytes_(BufferSize) PBYTE pBuffer);

TDHAPI
TdhGetPropertySize(
  _In_ PEVENT_RECORD pEvent,
  _In_ ULONG TdhContextCount,
  _In_reads_opt_(TdhContextCount) PTDH_CONTEXT pTdhContext,
  _In_ ULONG PropertyDataCount,
  _In_reads_(PropertyDataCount) PPROPERTY_DATA_DESCRIPTOR pPropertyData,
  _Out_ PULONG pPropertySize);

TDHAPI
TdhEnumerateProviders(
  _Out_writes_bytes_opt_(*pBufferSize) PPROVIDER_ENUMERATION_INFO pBuffer,
  _Inout_ PULONG pBufferSize);

TDHAPI
TdhEnumerateProviderFieldInformation(
  _In_ LPGUID pGuid,
  _In_ EVENT_FIELD_TYPE EventFieldType,
  _Out_writes_bytes_opt_(*pBufferSize) PPROVIDER_FIELD_INFOARRAY pBuffer,
  _Inout_ PULONG pBufferSize);

TDHAPI
TdhQueryProviderFieldInformation(
  _In_ LPGUID pGuid,
  _In_ ULONGLONG EventFieldValue,
  _In_ EVENT_FIELD_TYPE EventFieldType,
  _Out_writes_bytes_opt_(*pBufferSize) PPROVIDER_FIELD_INFOARRAY pBuffer,
  _Inout_ PULONG pBufferSize);

TDHAPI
TdhEnumerateManifestProviderEvents(
  _In_ LPGUID ProviderGuid,
  _Out_writes_bytes_opt_(*BufferSize) PPROVIDER_EVENT_INFO Buffer,
  _Inout_ PULONG BufferSize);

TDHAPI
TdhGetManifestEventInformation(
  _In_ LPGUID ProviderGuid,
  _In_ PEVENT_DESCRIPTOR EventDescriptor,
  _Out_writes_bytes_opt_(*BufferSize) PTRACE_EVENT_INFO Buffer,
  _Inout_ PULONG BufferSize);

TDHAPI
TdhFormatProperty(
  _In_ PTRACE_EVENT_INFO EventInfo,
  _In_opt_ PEVENT_MAP_INFO MapInfo,
  _In_ ULONG PointerSize,
  _In_ USHORT PropertyInType,
  _In_ USHORT PropertyOutType,
  _In_ USHORT PropertyLength,
  _In_ USHORT UserDataLength,
  _In_reads_bytes_(UserDataLength) PBYTE UserData,
  _Inout_ PULONG BufferSize,
  _Out_writes_bytes_opt_(*BufferSize) PWCHAR Buffer,
  _Out_ PUSHORT UserDataConsumed);

TDHAPI
TdhOpenDecodingHandle(
  _Out_ PTDH_HANDLE Handle);

TDHAPI
TdhSetDecodingParameter(
  _In_ TDH_HANDLE Handle,
  _In_ PTDH_CONTEXT TdhContext);

TDHAPI
TdhGetDecodingParameter(
  _In_ TDH_HANDLE Handle,
  _Inout_ PTDH_CONTEXT TdhContext);

TDHAPI
TdhGetWppProperty(
  _In_ TDH_HANDLE Handle,
  _In_ PEVENT_RECORD EventRecord,
  _In_ PWSTR PropertyName,
  _Inout_ PULONG BufferSize,
  _Out_writes_bytes_opt_(*BufferSize) PBYTE Buffer);

TDHAPI
TdhGetWppMessage(
  _In_ TDH_HANDLE Handle,
  _In_ PEVENT_RECORD EventRecord,
  _Inout_ PULONG BufferSize,
  _Out_writes_bytes_opt_(*BufferSize) PBYTE Buffer);

TDHAPI
TdhCloseDecodingHandle(
  _In_ TDH_HANDLE Handle);

TDHAPI
TdhLoadManifest(
  _In_ PWSTR Manifest);

TDHAPI
TdhLoadManifestFromBinary(
  _In_ PWSTR BinaryPath);

TDHAPI
TdhUnloadManifest(
  _In_ PWSTR Manifest);

TDHAPI
TdhEnumerateProviderFilters(
  _In_ LPGUID Guid,
  _In_ ULONG TdhContextCount,
  _In_reads_opt_(TdhContextCount) PTDH_CONTEXT TdhContext,
  _Out_ PULONG FilterCount,
  _Out_writes_bytes_opt_(*BufferSize) PPROVIDER_FILTER_INFO *Buffer,
  _Inout_ PULONG BufferSize);

#ifdef __cplusplus
}
#endif
