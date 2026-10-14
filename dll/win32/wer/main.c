/*
 * Copyright 2010 Louis Lenders
 * Copyright 2010 Detlef Riekenberg
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

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "werapi.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wer);

typedef struct {
    struct list entry;
    WER_REPORT_INFORMATION info;
    WER_REPORT_TYPE reporttype;
    BOOL submitted;
    WCHAR eventtype[1];
} report_t;


static CRITICAL_SECTION report_table_cs;
static CRITICAL_SECTION_DEBUG report_table_cs_debug =
{
    0, 0, &report_table_cs,
    { &report_table_cs_debug.ProcessLocksList, &report_table_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": report_table_cs") }
};
static CRITICAL_SECTION report_table_cs = { &report_table_cs_debug, -1, 0, 0, 0, 0 };

static struct list report_table = LIST_INIT(report_table);

/* Windows limits the event type to 63 characters and a parameter value to 259 */
#define WER_MAX_EVENT_TYPE_LEN  63
#define WER_MAX_PARAM_VALUE_LEN 259
#define WER_SUBMIT_KNOWN_FLAGS  0xffff

static const WCHAR regpath_exclude[] = L"Software\\Microsoft\\Windows\\Windows Error Reporting\\ExcludedApplications";

/* The length of a string, counted only up to one past the limit that matters */
static SIZE_T bounded_length(const WCHAR *text, SIZE_T limit)
{
    SIZE_T length = 0;

    while (length <= limit && text[length]) length++;
    return length;
}

static BOOL is_report(HREPORT hreport)
{
    report_t *cursor;
    BOOL found = FALSE;

    EnterCriticalSection(&report_table_cs);
    LIST_FOR_EACH_ENTRY(cursor, &report_table, report_t, entry)
    {
        if (cursor == (report_t *)hreport)
        {
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&report_table_cs);
    return found;
}

/***********************************************************************
 * WerAddExcludedApplication (wer.@)
 *
 * Add an application to the user specific or the system wide exclusion list
 *
 * PARAMS
 *  exeName  [i] The application name
 *  allUsers [i] for all users (TRUE) or for the current user (FALSE)
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 */
HRESULT WINAPI WerAddExcludedApplication(PCWSTR exeName, BOOL allUsers)
{
    HKEY hkey;
    DWORD value = 1;
    LPWSTR bs;

    TRACE("(%s, %d)\n",debugstr_w(exeName), allUsers);
    if (!exeName || !exeName[0])
        return E_INVALIDARG;

    bs = wcsrchr(exeName, '\\');
    if (bs) {
        bs++;   /* skip the backslash */
        if (!bs[0]) {
            return E_INVALIDARG;
        }
    } else
        bs = (LPWSTR) exeName;

    if (!RegCreateKeyW(allUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, regpath_exclude, &hkey)) {
        LONG lres = RegSetValueExW(hkey, bs, 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));
        RegCloseKey(hkey);
        return lres ? HRESULT_FROM_WIN32(lres) : S_OK;
    }
    return E_ACCESSDENIED;
}

/***********************************************************************
 * WerRemoveExcludedApplication (wer.@)
 *
 * remove an application from the exclusion list
 *
 * PARAMS
 *  exeName  [i] The application name
 *  allUsers [i] for all users (TRUE) or for the current user (FALSE)
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 */
HRESULT WINAPI WerRemoveExcludedApplication(PCWSTR exeName, BOOL allUsers)
{
    HKEY hkey;
    LPWSTR bs;
    LONG lres;

    TRACE("(%s, %d)\n",debugstr_w(exeName), allUsers);
    if (!exeName || !exeName[0])
        return E_INVALIDARG;

    bs = wcsrchr(exeName, '\\');
    if (bs) {
        bs++;   /* skip the backslash */
        if (!bs[0]) {
            return E_INVALIDARG;
        }
    } else
        bs = (LPWSTR) exeName;

    if (!RegCreateKeyW(allUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, regpath_exclude, &hkey)) {
        lres = RegDeleteValueW(hkey, bs);
        RegCloseKey(hkey);
        if (lres == ERROR_FILE_NOT_FOUND) return E_FAIL;
        return lres ? HRESULT_FROM_WIN32(lres) : S_OK;
    }
    return E_ACCESSDENIED;
}

/***********************************************************************
 * WerReportAddDump (wer.@)
 *
 * Add a dump of dumpType to hReportHandle.
 *
 * PARAMS
 *  hReportHandle      [i] error reporting handle to add the dump
 *  hProcess           [i] handle to the regarding process
 *  hThread            [o] handle to the regarding thread
 *  dumpType           [i] type of the dump
 *  pExceptionParam    [o] pointer to a WER_EXCEPTION_INFORMATION
 *  pDumpCustomOptions [o] pointer to a WER_DUMP_CUSTOM_OPTIONS
 *  dwFlags            [i] flag to control the heap dump
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 */
HRESULT WINAPI WerReportAddDump(HREPORT hReportHandle, HANDLE hProcess, HANDLE hThread,
                                WER_DUMP_TYPE dumpType, PWER_EXCEPTION_INFORMATION pExceptionParam,
                                PWER_DUMP_CUSTOM_OPTIONS pDumpCustomOptions, DWORD dwFlags)
{
    FIXME("(%p, %p, %p, %d, %p, %p, %lu) :stub\n", hReportHandle, hProcess, hThread, dumpType,
          pExceptionParam, pDumpCustomOptions, dwFlags);

    /* a micro dump is taken of a thread, so Windows needs one for it */
    if (!is_report(hReportHandle) || !hProcess || (dumpType == WerDumpTypeMicroDump && !hThread)) return E_INVALIDARG;
    if (dumpType < WerDumpTypeMicroDump || dumpType >= WerDumpTypeMax) return E_FAIL;

    /* the dump is added to a report that is never sent, so none is written */
    return S_OK;
}

/***********************************************************************
 * WerReportAddFile (wer.@)
 *
 * Add a file to an error report handle.
 *
 * PARAMS
 *  hreport [i] error reporting handle to add the file
 *  path    [i] path to the file to add
 *  type    [i] type of the file to add
 *  flags   [i] flags for the file
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 */
HRESULT WINAPI WerReportAddFile(HREPORT hreport, PCWSTR path, WER_FILE_TYPE type, DWORD flags)
{
    TRACE("(%p, %s, %d, 0x%lx)\n", hreport, debugstr_w(path), type, flags);

    if (!is_report(hreport) || !path) return E_INVALIDARG;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return __HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    return S_OK;
}

/***********************************************************************
 * WerReportCloseHandle (wer.@)
 *
 * Close an error reporting handle and free associated resources
 *
 * PARAMS
 *  hreport [i] error reporting handle to close
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 */
HRESULT WINAPI WerReportCloseHandle(HREPORT hreport)
{
    report_t * report = (report_t *) hreport;
    report_t * cursor;
    BOOL found = FALSE;

    TRACE("(%p)\n", hreport);
    EnterCriticalSection(&report_table_cs);
    if (report) {
        LIST_FOR_EACH_ENTRY(cursor, &report_table, report_t, entry)
        {
            if (cursor == report) {
                found = TRUE;
                list_remove(&report->entry);
                break;
            }
        }
    }
    LeaveCriticalSection(&report_table_cs);
    if (!found)
        return E_INVALIDARG;

    free(report);

    return S_OK;
}

/***********************************************************************
 * WerReportCreate (wer.@)
 *
 * Create an error report in memory and return a related HANDLE
 *
 * PARAMS
 *  eventtype  [i] a name for the event type
 *  reporttype [i] what type of report should be created
 *  reportinfo [i] NULL or a ptr to a struct with some detailed information
 *  phandle    [o] ptr, where the resulting handle should be saved
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 * NOTES
 *  The event type must be registered at microsoft. Predefined types are
 *  "APPCRASH" as the default on Windows, "Crash32" and "Crash64"
 *
 */
HRESULT WINAPI WerReportCreate(PCWSTR eventtype, WER_REPORT_TYPE reporttype, PWER_REPORT_INFORMATION reportinfo, HREPORT *phandle)
{
    report_t *report;
    SIZE_T length;

    TRACE("(%s, %d, %p, %p)\n", debugstr_w(eventtype), reporttype, reportinfo, phandle);
    if (reportinfo) {
        TRACE(".wzFriendlyEventName: %s\n", debugstr_w(reportinfo->wzFriendlyEventName));
        TRACE(".wzApplicationName: %s\n", debugstr_w(reportinfo->wzApplicationName));
    }

    if (!eventtype || !phandle) return E_INVALIDARG;
    *phandle = NULL;
    if (!eventtype[0]) return E_INVALIDARG;
    length = bounded_length(eventtype, WER_MAX_EVENT_TYPE_LEN + 1);
    if (length > WER_MAX_EVENT_TYPE_LEN)
        return length == WER_MAX_EVENT_TYPE_LEN + 1 && !eventtype[length] ? __HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : E_INVALIDARG;

    report = calloc(1, FIELD_OFFSET(report_t, eventtype[length + 1]));
    if (!report)
        return __HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY);

    memcpy(report->eventtype, eventtype, length * sizeof(WCHAR));
    report->reporttype = reporttype;

    if (reportinfo) {
        report->info = *reportinfo;
    } else {
        FIXME("build report information from scratch for %p\n", report);
    }

    EnterCriticalSection(&report_table_cs);
    list_add_head(&report_table, &report->entry);
    LeaveCriticalSection(&report_table_cs);

    *phandle = report;
    TRACE("=> %p\n", report);
    return S_OK;
}

/***********************************************************************
 * WerReportSetParameter (wer.@)
 *
 * Set one of 10 parameter / value pairs for a report handle
 *
 * PARAMS
 *  hreport [i] error reporting handle to add the parameter
 *  id      [i] parameter to set (WER_P0 up to WER_P9)
 *  name    [i] optional name of the parameter
 *  value   [i] value of the parameter
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 */
HRESULT WINAPI WerReportSetParameter(HREPORT hreport, DWORD id, PCWSTR name, PCWSTR value)
{
    TRACE("(%p, %ld, %s, %s)\n", hreport, id, debugstr_w(name), debugstr_w(value));

    if (!is_report(hreport) || id >= WER_MAX_PARAM_COUNT || !value) return E_INVALIDARG;
    if (bounded_length(value, WER_MAX_PARAM_VALUE_LEN) > WER_MAX_PARAM_VALUE_LEN) return __HRESULT_FROM_WIN32(ERROR_PARAMETER_QUOTA_EXCEEDED);

    return S_OK;
}

/***********************************************************************
 * WerReportSubmit (wer.@)
 *
 * Ask the user for permission and send the error report
 * then kill or restart the application, when requested
 *
 * PARAMS
 *  hreport [i] error reporting handle to send
 *  consent [i] current transmit permission
 *  flags   [i] flag to select dialog, transmission snd restart options
 *  presult [o] ptr, where the transmission result should be saved
 *
 * RETURNS
 *  Success: S_OK
 *  Failure: A HRESULT error code
 *
 */
HRESULT WINAPI WerReportSubmit(HREPORT hreport, WER_CONSENT consent, DWORD flags, PWER_SUBMIT_RESULT presult)
{
    report_t *report = NULL, *cursor;
    HRESULT hr;

    FIXME("(%p, %d, 0x%lx, %p) :stub\n", hreport, consent, flags, presult);

    EnterCriticalSection(&report_table_cs);
    LIST_FOR_EACH_ENTRY(cursor, &report_table, report_t, entry)
    {
        if (cursor == (report_t *)hreport)
        {
            report = cursor;
            break;
        }
    }

    if (!report)
    {
        hr = E_INVALIDARG;
    }
    else if (report->submitted)
    {
        /* a report is submitted once, and a second attempt is a failed one */
        if (presult) *presult = WerReportFailed;
        hr = __HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }
    else if (!presult)
    {
        hr = __HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }
    else if ((int)consent < WerConsentNotAsked || (int)consent >= WerConsentMax || (flags & ~WER_SUBMIT_KNOWN_FLAGS))
    {
        /* a consent or a flag that does not exist fails the submission; Windows reports that as a failed report */
        *presult = WerReportFailed;
        hr = (int)consent < WerConsentNotAsked ? E_INVALIDARG : __HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }
    else
    {
        /* no report can be sent from here, which Windows reports as disabled error reporting */
        report->submitted = TRUE;
        *presult = WerDisabled;
        hr = S_OK;
    }
    LeaveCriticalSection(&report_table_cs);
    return hr;
}

/***********************************************************************
 * WerReportSetUIOption (wer.@)
 */
HRESULT WINAPI WerReportSetUIOption(HREPORT hreport, WER_REPORT_UI uitype, PCWSTR value)
{
    TRACE("(%p, %d, %s)\n", hreport, uitype, debugstr_w(value));

    if (!is_report(hreport) || uitype < WerUIAdditionalDataDlgHeader || uitype >= WerUIMax || !value)
        return E_INVALIDARG;

    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        break;
    case DLL_PROCESS_DETACH:
        if (!reserved)
        {
            report_t *report, *next;

            LIST_FOR_EACH_ENTRY_SAFE(report, next, &report_table, report_t, entry)
            {
                list_remove(&report->entry);
                free(report);
            }
            DeleteCriticalSection(&report_table_cs);
        }
        break;
    }
    return TRUE;
}
