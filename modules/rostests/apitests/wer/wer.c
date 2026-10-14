/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for the argument checks and failure codes of wer.dll
 *
 * The expected values were measured on Windows Server 2008 R2 and Windows 10, x64 and WOW64.
 */

#include <apitest.h>

#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <winreg.h>
#include <werapi.h>

typedef HRESULT (WINAPI *PWerExcludedApplication)(PCWSTR, BOOL);
typedef HRESULT (WINAPI *PWerReportCreate)(PCWSTR, WER_REPORT_TYPE, PWER_REPORT_INFORMATION, HREPORT *);
typedef HRESULT (WINAPI *PWerReportCloseHandle)(HREPORT);
typedef HRESULT (WINAPI *PWerReportSetParameter)(HREPORT, DWORD, PCWSTR, PCWSTR);
typedef HRESULT (WINAPI *PWerReportAddFile)(HREPORT, PCWSTR, WER_FILE_TYPE, DWORD);
typedef HRESULT (WINAPI *PWerReportSetUIOption)(HREPORT, WER_REPORT_UI, PCWSTR);
typedef HRESULT (WINAPI *PWerReportAddDump)(HREPORT, HANDLE, HANDLE, WER_DUMP_TYPE, PWER_EXCEPTION_INFORMATION, PWER_DUMP_CUSTOM_OPTIONS, DWORD);
typedef HRESULT (WINAPI *PWerReportSubmit)(HREPORT, WER_CONSENT, DWORD, PWER_SUBMIT_RESULT);

static HMODULE hWer;
static PWerExcludedApplication pWerAddExcludedApplication, pWerRemoveExcludedApplication;
static PWerReportCreate pWerReportCreate;
static PWerReportCloseHandle pWerReportCloseHandle;
static PWerReportSetParameter pWerReportSetParameter;
static PWerReportAddFile pWerReportAddFile;
static PWerReportSetUIOption pWerReportSetUIOption;
static PWerReportAddDump pWerReportAddDump;
static PWerReportSubmit pWerReportSubmit;

static const char *ExportNames[] =
{
    "WerAddExcludedApplication",
    "WerRemoveExcludedApplication",
    "WerReportAddDump",
    "WerReportAddFile",
    "WerReportCloseHandle",
    "WerReportCreate",
    "WerReportSetParameter",
    "WerReportSetUIOption",
    "WerReportSubmit",
};

static const WCHAR TestApp[] = L"wer_apitest.exe";

#define ERR_NOT_IN_LIST_WIN7 E_FAIL
#define ERR_NOT_IN_LIST_WIN10 __HRESULT_FROM_WIN32(ERROR_ENVVAR_NOT_FOUND)
#define ok_not_in_list(hr) \
    ok((hr) == ERR_NOT_IN_LIST_WIN7 || (hr) == ERR_NOT_IN_LIST_WIN10, "got 0x%lx\n", (hr))

static void test_exports(void)
{
    UINT i;

    for (i = 0; i < ARRAYSIZE(ExportNames); i++)
        ok(GetProcAddress(hWer, ExportNames[i]) != NULL, "%s is not exported\n", ExportNames[i]);
}

static BOOL is_excluded(PCWSTR Name, BOOL AllUsers)
{
    HKEY Key;
    DWORD Value = 0, Size = sizeof(Value), Type = 0;
    LONG Error;
    BOOL Found = FALSE;

    /* a 32-bit program on 64-bit Windows may find the list in either view of the registry */
    static const REGSAM Views[] = { 0, KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    UINT i;

    for (i = 0; i < ARRAYSIZE(Views) && !Found; i++)
    {
        Error = RegOpenKeyExW(AllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\Windows Error Reporting\\ExcludedApplications",
                              0, KEY_QUERY_VALUE | Views[i], &Key);
        if (Error != ERROR_SUCCESS)
            continue;
        Size = sizeof(Value);
        Error = RegQueryValueExW(Key, Name, NULL, &Type, (LPBYTE)&Value, &Size);
        if (Error == ERROR_SUCCESS && Type == REG_DWORD && Value == 1)
            Found = TRUE;
        RegCloseKey(Key);
    }
    return Found;
}

static void test_excluded_application(BOOL AllUsers)
{
    WCHAR Path[MAX_PATH];
    HRESULT hr;

    /* start clean, the list may hold the name from an aborted run */
    pWerRemoveExcludedApplication(TestApp, AllUsers);

    hr = pWerAddExcludedApplication(NULL, AllUsers);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerAddExcludedApplication(L"", AllUsers);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerRemoveExcludedApplication(NULL, AllUsers);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerRemoveExcludedApplication(L"", AllUsers);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);

    hr = pWerAddExcludedApplication(TestApp, AllUsers);
    if (hr == E_ACCESSDENIED)
    {
        skip("Not enough rights to change the exclusion list\n");
        return;
    }
    ok(hr == S_OK, "got 0x%lx\n", hr);
    /* the list is a registry key with one DWORD value of 1 per application */
    ok(is_excluded(TestApp, AllUsers), "the registry has no value for the application\n");
    /* a name that is in the list already is accepted */
    hr = pWerAddExcludedApplication(TestApp, AllUsers);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerRemoveExcludedApplication(TestApp, AllUsers);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    ok(!is_excluded(TestApp, AllUsers), "the registry still has the value\n");
    hr = pWerRemoveExcludedApplication(TestApp, AllUsers);
    ok_not_in_list(hr);

    /* only the last component of a path counts, and a trailing backslash leaves no name */
    lstrcpyW(Path, L"C:\\wer_apitest_dir\\");
    hr = pWerAddExcludedApplication(Path, AllUsers);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerRemoveExcludedApplication(Path, AllUsers);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    lstrcatW(Path, TestApp);
    hr = pWerAddExcludedApplication(Path, AllUsers);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerRemoveExcludedApplication(TestApp, AllUsers);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerAddExcludedApplication(TestApp, AllUsers);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerRemoveExcludedApplication(Path, AllUsers);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerRemoveExcludedApplication(Path, AllUsers);
    ok_not_in_list(hr);
}

static void test_report_handle(void)
{
    HREPORT Report, Again;
    HRESULT hr;
    UINT i;

    Report = (HREPORT)(ULONG_PTR)0xdeadbeef;
    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    ok(Report != NULL && Report != (HREPORT)(ULONG_PTR)0xdeadbeef, "got %p\n", Report);
    if (hr != S_OK || !Report)
    {
        skip("No report to work with\n");
        return;
    }

    hr = pWerReportCloseHandle(Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportCloseHandle(Report);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportCloseHandle(NULL);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportCloseHandle((HREPORT)(ULONG_PTR)0x1234);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);

    /* the output handle is needed, and it is written only after the name is known to be present */
    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, NULL);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    Report = (HREPORT)(ULONG_PTR)0xdeadbeef;
    hr = pWerReportCreate(NULL, WerReportCritical, NULL, &Report);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    ok(Report == (HREPORT)(ULONG_PTR)0xdeadbeef, "got %p\n", Report);
    Report = (HREPORT)(ULONG_PTR)0xdeadbeef;
    hr = pWerReportCreate(L"", WerReportCritical, NULL, &Report);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    ok(Report == NULL, "got %p\n", Report);

    /* every report type is accepted, also the ones outside the enumeration on Windows 7 */
    for (i = 0; i < WerReportInvalid; i++)
    {
        Report = NULL;
        hr = pWerReportCreate(L"APPCRASH", i, NULL, &Report);
        ok(hr == S_OK, "%u: got 0x%lx\n", i, hr);
        if (hr == S_OK)
            pWerReportCloseHandle(Report);
    }
    Report = NULL;
    hr = pWerReportCreate(L"APPCRASH", WerReportInvalid, NULL, &Report);
    ok(hr == S_OK || broken(hr == E_INVALIDARG), "got 0x%lx\n", hr);
    if (hr == S_OK)
        pWerReportCloseHandle(Report);

    /* reports are independent */
    Report = Again = NULL;
    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Again);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    ok(Report != Again, "got %p twice\n", Report);
    if (Report)
    {
        hr = pWerReportCloseHandle(Report);
        ok(hr == S_OK, "got 0x%lx\n", hr);
    }
    if (Again)
    {
        hr = pWerReportCloseHandle(Again);
        ok(hr == S_OK, "got 0x%lx\n", hr);
    }
}

static void test_event_type_length(void)
{
    WCHAR Name[256];
    HREPORT Report;
    HRESULT hr;
    UINT Length;

    for (Length = 0; Length < ARRAYSIZE(Name) - 1; Length++)
        Name[Length] = L'e';
    Name[Length] = UNICODE_NULL;

    /* 63 characters fit; 64 give an unusual error, and more are an invalid argument */
    Name[63] = UNICODE_NULL;
    Report = NULL;
    hr = pWerReportCreate(Name, WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (hr == S_OK)
        pWerReportCloseHandle(Report);
    Name[63] = L'e';

    Name[64] = UNICODE_NULL;
    Report = NULL;
    hr = pWerReportCreate(Name, WerReportCritical, NULL, &Report);
    ok(hr == __HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || broken(hr == E_INVALIDARG), "got 0x%lx\n", hr);
    ok(Report == NULL, "got %p\n", Report);
    if (hr == S_OK)
        pWerReportCloseHandle(Report);
    Name[64] = L'e';

    Name[65] = UNICODE_NULL;
    Report = NULL;
    hr = pWerReportCreate(Name, WerReportCritical, NULL, &Report);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    ok(Report == NULL, "got %p\n", Report);
    Name[65] = L'e';

    Report = NULL;
    hr = pWerReportCreate(Name, WerReportCritical, NULL, &Report);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
}

static void test_set_parameter(void)
{
    WCHAR Value[300];
    HREPORT Report = NULL;
    HRESULT hr;
    DWORD Id;

    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (!Report)
    {
        skip("No report to work with\n");
        return;
    }

    for (Id = WER_P0; Id <= WER_P9; Id++)
    {
        hr = pWerReportSetParameter(Report, Id, L"name", L"value");
        ok(hr == S_OK, "%lu: got 0x%lx\n", Id, hr);
    }
    hr = pWerReportSetParameter(Report, WER_MAX_PARAM_COUNT, L"name", L"value");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportSetParameter(Report, 0xffffffff, L"name", L"value");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);

    /* the name is optional, the value is not */
    hr = pWerReportSetParameter(Report, 0, NULL, L"value");
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportSetParameter(Report, 0, L"", L"value");
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportSetParameter(Report, 0, L"name", NULL);
    ok(hr == E_INVALIDARG || broken(hr == S_OK) /* Windows 10 */, "got 0x%lx\n", hr);
    hr = pWerReportSetParameter(Report, 0, L"name", L"");
    ok(hr == S_OK, "got 0x%lx\n", hr);

    /* a value may have 259 characters */
    for (Id = 0; Id < ARRAYSIZE(Value) - 1; Id++)
        Value[Id] = L'v';
    Value[Id] = UNICODE_NULL;
    Value[259] = UNICODE_NULL;
    hr = pWerReportSetParameter(Report, 1, L"name", Value);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    Value[259] = L'v';
    Value[260] = UNICODE_NULL;
    hr = pWerReportSetParameter(Report, 1, L"name", Value);
    ok(hr == __HRESULT_FROM_WIN32(ERROR_PARAMETER_QUOTA_EXCEEDED), "got 0x%lx\n", hr);

    hr = pWerReportCloseHandle(Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportSetParameter(Report, 0, L"name", L"value");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportSetParameter(NULL, 0, L"name", L"value");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportSetParameter((HREPORT)(ULONG_PTR)0x1234, 0, L"name", L"value");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
}

static void test_add_file(void)
{
    WCHAR Path[MAX_PATH];
    HREPORT Report = NULL;
    HRESULT hr;
    UINT Type;

    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (!Report)
    {
        skip("No report to work with\n");
        return;
    }

    GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    for (Type = 0; Type <= 4; Type++)
    {
        hr = pWerReportAddFile(Report, Path, Type, 0);
        ok(hr == S_OK, "%u: got 0x%lx\n", Type, hr);
    }
    hr = pWerReportAddFile(Report, Path, WerFileTypeOther, 0x1fff);
    ok(hr == S_OK, "got 0x%lx\n", hr);

    hr = pWerReportAddFile(Report, L"C:\\wer_apitest_no_such_dir\\no_such_file.txt", WerFileTypeOther, 0);
    ok(hr == __HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got 0x%lx\n", hr);
    hr = pWerReportAddFile(Report, L"", WerFileTypeOther, 0);
    ok(hr == __HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
       broken(hr == __HRESULT_FROM_WIN32(ERROR_INVALID_NAME)) /* Windows 10 */, "got 0x%lx\n", hr);
    hr = pWerReportAddFile(Report, NULL, WerFileTypeOther, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);

    hr = pWerReportCloseHandle(Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportAddFile(Report, Path, WerFileTypeOther, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportAddFile(NULL, Path, WerFileTypeOther, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
}

static void test_ui_option(void)
{
    HREPORT Report = NULL;
    HRESULT hr;
    UINT Option;

    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (!Report)
    {
        skip("No report to work with\n");
        return;
    }

    hr = pWerReportSetUIOption(Report, 0, L"text");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    /* the options go up to eleven, and up to ten on Windows 8 and later */
    for (Option = WerUIAdditionalDataDlgHeader; Option <= WerUICustomActionButtonText; Option++)
    {
        hr = pWerReportSetUIOption(Report, Option, L"text");
        ok(hr == S_OK || broken(Option == WerUICustomActionButtonText && hr == E_INVALIDARG), "%u: got 0x%lx\n", Option, hr);
    }
    hr = pWerReportSetUIOption(Report, WerUIMax, L"text");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportSetUIOption(Report, WerUIAdditionalDataDlgHeader, NULL);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportSetUIOption(Report, WerUIAdditionalDataDlgHeader, L"");
    ok(hr == S_OK, "got 0x%lx\n", hr);

    hr = pWerReportCloseHandle(Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportSetUIOption(Report, WerUIAdditionalDataDlgHeader, L"text");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportSetUIOption(NULL, WerUIAdditionalDataDlgHeader, L"text");
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
}

static void test_add_dump(void)
{
    HREPORT Report = NULL;
    HRESULT hr;

    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (!Report)
    {
        skip("No report to work with\n");
        return;
    }

    /* a micro dump needs a thread, and no dump is taken of no process */
    hr = pWerReportAddDump(Report, GetCurrentProcess(), NULL, WerDumpTypeMicroDump, NULL, NULL, 0);
    ok(hr == E_INVALIDARG || broken(hr == S_OK) /* Windows 10 */, "got 0x%lx\n", hr);
    hr = pWerReportAddDump(Report, NULL, NULL, WerDumpTypeMicroDump, NULL, NULL, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportAddDump(Report, GetCurrentProcess(), GetCurrentThread(), 99, NULL, NULL, 0);
    ok(hr == E_FAIL, "got 0x%lx\n", hr);
    hr = pWerReportAddDump(Report, GetCurrentProcess(), GetCurrentThread(), 0, NULL, NULL, 0);
    ok(hr == E_FAIL, "got 0x%lx\n", hr);
    hr = pWerReportAddDump(Report, GetCurrentProcess(), GetCurrentThread(), WerDumpTypeMax, NULL, NULL, 0);
    ok(hr == E_FAIL || broken(hr == S_OK) /* Windows 10 */, "got 0x%lx\n", hr);

    /* a dump of this process, as a crash handler adds it, is taken */
    hr = pWerReportAddDump(Report, GetCurrentProcess(), GetCurrentThread(), WerDumpTypeMicroDump, NULL, NULL, 0);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportAddDump(Report, GetCurrentProcess(), GetCurrentThread(), WerDumpTypeMiniDump, NULL, NULL, 0);
    ok(hr == S_OK, "got 0x%lx\n", hr);

    hr = pWerReportCloseHandle(Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportAddDump(Report, GetCurrentProcess(), GetCurrentThread(), WerDumpTypeMiniDump, NULL, NULL, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    hr = pWerReportAddDump(NULL, GetCurrentProcess(), GetCurrentThread(), WerDumpTypeMiniDump, NULL, NULL, 0);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
}

static void test_submit(void)
{
    WER_SUBMIT_RESULT Result;
    HREPORT Report = NULL, Other = NULL;
    DWORD Flags = WER_SUBMIT_NO_QUEUE | WER_SUBMIT_NO_ARCHIVE | WER_SUBMIT_NO_CLOSE_UI;
    HRESULT hr;

    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    hr = pWerReportCreate(L"APPCRASH", WerReportCritical, NULL, &Other);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    if (!Report || !Other)
    {
        skip("No report to work with\n");
        return;
    }

    /* a consent that does not exist fails the submission, and the result says so */
    Result = (WER_SUBMIT_RESULT)0x77;
    hr = pWerReportSubmit(Other, 0, Flags, &Result);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    ok(Result == WerReportFailed, "got %d\n", Result);
    hr = pWerReportCloseHandle(Other);
    ok(hr == S_OK, "got 0x%lx\n", hr);

    /* a denied consent sends nothing and shows nothing, so the result is reproducible */
    Result = (WER_SUBMIT_RESULT)0x77;
    hr = pWerReportSubmit(Report, WerConsentDenied, Flags, &Result);
    ok(hr == S_OK, "got 0x%lx\n", hr);
    ok(Result == WerDisabled, "got %d\n", Result);

    /* a report is submitted once */
    Result = (WER_SUBMIT_RESULT)0x77;
    hr = pWerReportSubmit(Report, WerConsentDenied, Flags, &Result);
    ok(hr == __HRESULT_FROM_WIN32(ERROR_INVALID_STATE), "got 0x%lx\n", hr);
    ok(Result == WerReportFailed, "got %d\n", Result);
    hr = pWerReportSubmit(Report, WerConsentDenied, Flags, NULL);
    ok(hr == __HRESULT_FROM_WIN32(ERROR_INVALID_STATE), "got 0x%lx\n", hr);

    /* the report stays usable after a submission */
    hr = pWerReportSetParameter(Report, 0, L"name", L"value");
    ok(hr == S_OK, "got 0x%lx\n", hr);

    hr = pWerReportCloseHandle(Report);
    ok(hr == S_OK, "got 0x%lx\n", hr);

    Result = (WER_SUBMIT_RESULT)0x77;
    hr = pWerReportSubmit(Report, WerConsentDenied, Flags, &Result);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    ok(Result == 0x77, "got %d\n", Result);
    hr = pWerReportSubmit(NULL, WerConsentDenied, Flags, &Result);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
    ok(Result == 0x77, "got %d\n", Result);
    hr = pWerReportSubmit((HREPORT)(ULONG_PTR)0x1234, WerConsentDenied, Flags, &Result);
    ok(hr == E_INVALIDARG, "got 0x%lx\n", hr);
}

START_TEST(wer)
{
    hWer = LoadLibraryW(L"wer.dll");
    ok(hWer != NULL, "LoadLibrary failed with %lu\n", GetLastError());
    if (!hWer)
    {
        skip("wer.dll is not available\n");
        return;
    }

    test_exports();

    pWerAddExcludedApplication = (PVOID)GetProcAddress(hWer, "WerAddExcludedApplication");
    pWerRemoveExcludedApplication = (PVOID)GetProcAddress(hWer, "WerRemoveExcludedApplication");
    pWerReportCreate = (PVOID)GetProcAddress(hWer, "WerReportCreate");
    pWerReportCloseHandle = (PVOID)GetProcAddress(hWer, "WerReportCloseHandle");
    pWerReportSetParameter = (PVOID)GetProcAddress(hWer, "WerReportSetParameter");
    pWerReportAddFile = (PVOID)GetProcAddress(hWer, "WerReportAddFile");
    pWerReportSetUIOption = (PVOID)GetProcAddress(hWer, "WerReportSetUIOption");
    pWerReportAddDump = (PVOID)GetProcAddress(hWer, "WerReportAddDump");
    pWerReportSubmit = (PVOID)GetProcAddress(hWer, "WerReportSubmit");
    if (!pWerAddExcludedApplication || !pWerRemoveExcludedApplication || !pWerReportCreate ||
        !pWerReportCloseHandle || !pWerReportSetParameter || !pWerReportAddFile ||
        !pWerReportSetUIOption || !pWerReportAddDump || !pWerReportSubmit)
    {
        skip("Not all of the exports are available\n");
        FreeLibrary(hWer);
        return;
    }

    test_excluded_application(FALSE);
    test_excluded_application(TRUE);
    test_report_handle();
    test_event_type_length();
    test_set_parameter();
    test_add_file();
    test_ui_option();
    test_add_dump();
    test_submit();

    FreeLibrary(hWer);
}
