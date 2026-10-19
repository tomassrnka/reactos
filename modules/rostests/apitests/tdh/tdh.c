/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for the argument checks and failure codes of tdh.dll
 *
 * The expected values were measured on Windows Server 2008 R2 and Windows 10, x64 and WOW64.
 * The event records below name no provider that is registered anywhere, so no schema exists.
 */

#include <apitest.h>

#include <windef.h>
#include <winbase.h>
#include <tdh.h>

typedef ULONG (WINAPI *PTdhEnumerateProviderFieldInformation)(LPGUID, EVENT_FIELD_TYPE, PPROVIDER_FIELD_INFOARRAY, PULONG);
typedef ULONG (WINAPI *PTdhEnumerateProviderFilters)(LPGUID, ULONG, PTDH_CONTEXT, PULONG, PPROVIDER_FILTER_INFO *, PULONG);
typedef ULONG (WINAPI *PTdhFormatProperty)(PTRACE_EVENT_INFO, PEVENT_MAP_INFO, ULONG, USHORT, USHORT, USHORT, USHORT, PBYTE, PULONG, PWCHAR, PUSHORT);
typedef ULONG (WINAPI *PTdhGetEventInformation)(PEVENT_RECORD, ULONG, PTDH_CONTEXT, PTRACE_EVENT_INFO, PULONG);
typedef ULONG (WINAPI *PTdhGetEventMapInformation)(PEVENT_RECORD, LPWSTR, PEVENT_MAP_INFO, PULONG);
typedef ULONG (WINAPI *PTdhGetProperty)(PEVENT_RECORD, ULONG, PTDH_CONTEXT, ULONG, PPROPERTY_DATA_DESCRIPTOR, ULONG, PBYTE);
typedef ULONG (WINAPI *PTdhGetPropertySize)(PEVENT_RECORD, ULONG, PTDH_CONTEXT, ULONG, PPROPERTY_DATA_DESCRIPTOR, PULONG);
typedef ULONG (WINAPI *PTdhLoadManifest)(PWSTR);
typedef ULONG (WINAPI *PTdhQueryProviderFieldInformation)(LPGUID, ULONGLONG, EVENT_FIELD_TYPE, PPROVIDER_FIELD_INFOARRAY, PULONG);
typedef ULONG (WINAPI *PTdhUnloadManifest)(PWSTR);

static HMODULE hTdh;

static const char *ExportNames[] =
{
    "TdhEnumerateProviderFieldInformation",
    "TdhEnumerateProviderFilters",
    "TdhEnumerateProviders",
    "TdhEnumerateRemoteWBEMProviderFieldInformation",
    "TdhEnumerateRemoteWBEMProviders",
    "TdhFormatProperty",
    "TdhGetAllEventsInformation",
    "TdhGetEventInformation",
    "TdhGetEventMapInformation",
    "TdhGetProperty",
    "TdhGetPropertyOffsetAndSize",
    "TdhGetPropertySize",
    "TdhLoadManifest",
    "TdhQueryProviderFieldInformation",
    "TdhQueryRemoteWBEMProviderFieldInformation",
    "TdhUnloadManifest",
};

/* Windows 7 reports ERROR_INVALID_DATA, Windows 10 ERROR_NOT_FOUND, for a property of an event without schema */
#define ok_property_error(err) \
    ok((err) == ERROR_NOT_FOUND || (err) == ERROR_INVALID_DATA, "got %lu\n", (err))

static void test_exports(void)
{
    UINT i;

    for (i = 0; i < ARRAYSIZE(ExportNames); i++)
        ok(GetProcAddress(hTdh, ExportNames[i]) != NULL, "%s is not exported\n", ExportNames[i]);
}

static void test_event_information(void)
{
    PTdhGetEventInformation pTdhGetEventInformation = (PVOID)GetProcAddress(hTdh, "TdhGetEventInformation");
    GUID Provider = { 0x6f1d4c50, 0x7a07, 0x4d38, { 0x9d, 0x4a, 0x14, 0x62, 0x77, 0x02, 0x4b, 0x3e } };
    EVENT_RECORD Zero, Record;
    TDH_CONTEXT Context;
    BYTE Buffer[256];
    ULONG Size;
    ULONG Error;

    if (!pTdhGetEventInformation)
    {
        skip("TdhGetEventInformation is not available\n");
        return;
    }

    memset(&Zero, 0, sizeof(Zero));
    memset(&Record, 0, sizeof(Record));
    Record.EventHeader.Size = sizeof(Record);
    Record.EventHeader.ProviderId = Provider;
    Record.EventHeader.EventDescriptor.Id = 1;

    memset(&Context, 0, sizeof(Context));
    Context.ParameterType = TDH_CONTEXT_POINTERSIZE;
    Context.ParameterValue = sizeof(PVOID);
    Context.ParameterSize = sizeof(ULONG);

    Size = 0;
    Error = pTdhGetEventInformation(NULL, 0, NULL, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetEventInformation(NULL, 0, NULL, NULL, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetEventInformation(&Record, 0, NULL, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);

    /* a context count without a context */
    Error = pTdhGetEventInformation(&Record, 1, NULL, NULL, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);

    /* no schema for the provider */
    Size = 0;
    Error = pTdhGetEventInformation(&Zero, 0, NULL, NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);
    Error = pTdhGetEventInformation(&Record, 0, NULL, NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);
    ok(Size == 0, "got %lu\n", Size);
    Error = pTdhGetEventInformation(&Record, 1, &Context, NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);

    Size = sizeof(Buffer);
    memset(Buffer, 0x5a, sizeof(Buffer));
    Error = pTdhGetEventInformation(&Record, 0, NULL, (PTRACE_EVENT_INFO)Buffer, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);
    ok(Size == sizeof(Buffer), "got %lu\n", Size);
    ok(Buffer[0] == 0x5a && Buffer[sizeof(Buffer) - 1] == 0x5a, "the buffer was written\n");

    /* a size without a buffer, and a buffer without a size */
    Size = 0x12345;
    Error = pTdhGetEventInformation(&Record, 0, NULL, NULL, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Size = 0;
    Error = pTdhGetEventInformation(&Record, 0, NULL, (PTRACE_EVENT_INFO)Buffer, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);

    Record.EventHeader.Flags = EVENT_HEADER_FLAG_CLASSIC_HEADER;
    Size = 0;
    Error = pTdhGetEventInformation(&Record, 0, NULL, NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);
}

static void test_event_map_information(void)
{
    PTdhGetEventMapInformation pTdhGetEventMapInformation = (PVOID)GetProcAddress(hTdh, "TdhGetEventMapInformation");
    EVENT_RECORD Zero, Record;
    BYTE Buffer[64];
    ULONG Size;
    ULONG Error;

    if (!pTdhGetEventMapInformation)
    {
        skip("TdhGetEventMapInformation is not available\n");
        return;
    }

    memset(&Zero, 0, sizeof(Zero));
    memset(&Record, 0, sizeof(Record));
    Record.EventHeader.Size = sizeof(Record);
    Record.EventHeader.EventDescriptor.Id = 1;

    Size = 0;
    Error = pTdhGetEventMapInformation(NULL, NULL, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetEventMapInformation(&Record, NULL, NULL, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetEventMapInformation(&Record, L"x", NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);
    Error = pTdhGetEventMapInformation(&Zero, L"x", NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);

    /* a buffer without a size */
    Error = pTdhGetEventMapInformation(&Record, L"x", (PEVENT_MAP_INFO)Buffer, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
}

static void test_property(void)
{
    PTdhGetPropertySize pTdhGetPropertySize = (PVOID)GetProcAddress(hTdh, "TdhGetPropertySize");
    PTdhGetProperty pTdhGetProperty = (PVOID)GetProcAddress(hTdh, "TdhGetProperty");
    PROPERTY_DATA_DESCRIPTOR Descriptor;
    EVENT_RECORD Zero, Record;
    BYTE Buffer[16];
    ULONG Size;
    ULONG Error;

    if (!pTdhGetPropertySize || !pTdhGetProperty)
    {
        skip("TdhGetPropertySize or TdhGetProperty is not available\n");
        return;
    }

    memset(&Zero, 0, sizeof(Zero));
    memset(&Record, 0, sizeof(Record));
    Record.EventHeader.Size = sizeof(Record);
    Record.EventHeader.EventDescriptor.Id = 1;
    memset(&Descriptor, 0, sizeof(Descriptor));
    Descriptor.PropertyName = (ULONGLONG)(ULONG_PTR)L"Name";

    Size = 0;
    Error = pTdhGetPropertySize(NULL, 0, NULL, 0, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetPropertySize(&Zero, 0, NULL, 0, NULL, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetPropertySize(&Record, 0, NULL, 0, &Descriptor, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetPropertySize(&Record, 0, NULL, 1, &Descriptor, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetPropertySize(NULL, 0, NULL, 1, &Descriptor, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetPropertySize(&Record, 0, NULL, 1, NULL, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetPropertySize(&Record, 1, NULL, 1, &Descriptor, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetPropertySize(&Record, 0, NULL, 1, &Descriptor, &Size);
    ok_property_error(Error);

    Error = pTdhGetProperty(NULL, 0, NULL, 0, NULL, 0, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetProperty(&Record, 0, NULL, 1, &Descriptor, 0, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetProperty(&Record, 0, NULL, 1, &Descriptor, 0, Buffer);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetProperty(&Record, 0, NULL, 0, &Descriptor, sizeof(Buffer), Buffer);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhGetProperty(&Record, 0, NULL, 1, &Descriptor, sizeof(Buffer), Buffer);
    ok_property_error(Error);
    Error = pTdhGetProperty(&Zero, 0, NULL, 1, &Descriptor, sizeof(Buffer), Buffer);
    ok_property_error(Error);
}

static void test_provider_fields(void)
{
    PTdhEnumerateProviderFieldInformation pTdhEnumerateProviderFieldInformation =
        (PVOID)GetProcAddress(hTdh, "TdhEnumerateProviderFieldInformation");
    PTdhQueryProviderFieldInformation pTdhQueryProviderFieldInformation =
        (PVOID)GetProcAddress(hTdh, "TdhQueryProviderFieldInformation");
    GUID Provider = { 0x6f1d4c50, 0x7a07, 0x4d38, { 0x9d, 0x4a, 0x14, 0x62, 0x77, 0x02, 0x4b, 0x3e } };
    ULONG Size = 0;
    ULONG Error;

    if (!pTdhEnumerateProviderFieldInformation || !pTdhQueryProviderFieldInformation)
    {
        skip("The provider field functions are not available\n");
        return;
    }

    Error = pTdhEnumerateProviderFieldInformation(NULL, EventKeywordInformation, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhEnumerateProviderFieldInformation(&Provider, EventKeywordInformation, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhEnumerateProviderFieldInformation(&Provider, EventKeywordInformation, NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);
    Error = pTdhEnumerateProviderFieldInformation(&Provider, (EVENT_FIELD_TYPE)9, NULL, &Size);
    ok(Error == ERROR_NOT_SUPPORTED, "got %lu\n", Error);
    Error = pTdhEnumerateProviderFieldInformation(NULL, EventKeywordInformation, NULL, &Size);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);

    Error = pTdhQueryProviderFieldInformation(NULL, 0, EventKeywordInformation, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhQueryProviderFieldInformation(&Provider, 0, EventKeywordInformation, NULL, &Size);
    ok(Error == ERROR_NOT_FOUND, "got %lu\n", Error);
    Error = pTdhQueryProviderFieldInformation(&Provider, 0, (EVENT_FIELD_TYPE)6, NULL, &Size);
    ok(Error == ERROR_NOT_SUPPORTED, "got %lu\n", Error);
}

static void test_other_functions(void)
{
    PTdhEnumerateProviderFilters pTdhEnumerateProviderFilters = (PVOID)GetProcAddress(hTdh, "TdhEnumerateProviderFilters");
    PTdhFormatProperty pTdhFormatProperty = (PVOID)GetProcAddress(hTdh, "TdhFormatProperty");
    GUID Provider = { 0x6f1d4c50, 0x7a07, 0x4d38, { 0x9d, 0x4a, 0x14, 0x62, 0x77, 0x02, 0x4b, 0x3e } };
    PPROVIDER_FILTER_INFO Filters = NULL;
    ULONG Size = 0, Count = 0;
    ULONG Error;

    if (pTdhEnumerateProviderFilters)
    {
        Error = pTdhEnumerateProviderFilters(NULL, 0, NULL, NULL, NULL, NULL);
        ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
        Error = pTdhEnumerateProviderFilters(&Provider, 0, NULL, NULL, NULL, &Size);
        ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
        Error = pTdhEnumerateProviderFilters(&Provider, 0, NULL, &Count, NULL, NULL);
        ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
        Error = pTdhEnumerateProviderFilters(NULL, 0, NULL, &Count, NULL, &Size);
        ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
        Size = 0;
        Error = pTdhEnumerateProviderFilters(&Provider, 0, NULL, &Count, &Filters, &Size);
        ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    }
    else
        skip("TdhEnumerateProviderFilters is not available\n");

    if (pTdhFormatProperty)
    {
        Error = pTdhFormatProperty(NULL, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
        ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    }
    else
        skip("TdhFormatProperty is not available\n");
}

static void test_manifest(void)
{
    PTdhLoadManifest pTdhLoadManifest = (PVOID)GetProcAddress(hTdh, "TdhLoadManifest");
    PTdhUnloadManifest pTdhUnloadManifest = (PVOID)GetProcAddress(hTdh, "TdhUnloadManifest");
    ULONG Error;

    if (!pTdhLoadManifest || !pTdhUnloadManifest)
    {
        skip("TdhLoadManifest or TdhUnloadManifest is not available\n");
        return;
    }

    Error = pTdhLoadManifest(NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);
    Error = pTdhUnloadManifest(NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "got %lu\n", Error);

    /* a relative name is looked up in the current directory */
    Error = pTdhLoadManifest((PWSTR)L"tdh_apitest_no_such_file.man");
    ok(Error == ERROR_FILE_NOT_FOUND, "got %lu\n", Error);
    Error = pTdhUnloadManifest((PWSTR)L"tdh_apitest_no_such_file.man");
    ok(Error == ERROR_FILE_NOT_FOUND, "got %lu\n", Error);
}

START_TEST(tdh)
{
    hTdh = LoadLibraryW(L"tdh.dll");
    ok(hTdh != NULL, "LoadLibrary failed with %lu\n", GetLastError());
    if (!hTdh)
    {
        skip("tdh.dll is not available\n");
        return;
    }

    test_exports();
    test_event_information();
    test_event_map_information();
    test_property();
    test_provider_fields();
    test_other_functions();
    test_manifest();

    FreeLibrary(hTdh);
}
