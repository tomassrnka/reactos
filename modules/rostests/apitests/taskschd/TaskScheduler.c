/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Task Scheduler 2.0: registered tasks run their actions when their triggers fire
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define COBJMACROS
#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <oleauto.h>
#include <initguid.h>
#include <taskschd.h>
#include <stdio.h>

#define FOLDER L"\\ReactOS ApiTest"
#define WAIT_SECONDS 200

static ITaskService *service;
static ITaskFolder *folder;
static WCHAR marker_dir[MAX_PATH];

static void marker_path(const WCHAR *name, WCHAR *path)
{
    _swprintf(path, L"%s\\%s.txt", marker_dir, name);
}

/* "YYYY-MM-DDTHH:MM:SS" for now + seconds, in local time, or in UTC with a 'Z' */
static void boundary(int seconds, BOOL utc, WCHAR *str)
{
    FILETIME ft, local;
    ULARGE_INTEGER t;
    SYSTEMTIME st;

    GetSystemTimeAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    t.QuadPart += (LONGLONG)seconds * 10000000;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    if (utc)
        FileTimeToSystemTime(&ft, &st);
    else
    {
        FileTimeToLocalFileTime(&ft, &local);
        FileTimeToSystemTime(&local, &st);
    }
    _swprintf(str, L"%04u-%02u-%02uT%02u:%02u:%02u%s", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, utc ? L"Z" : L"");
}

/* the arguments of cmd.exe that write the marker */
static void marker_arguments(const WCHAR *name, WCHAR *args)
{
    WCHAR path[MAX_PATH];

    marker_path(name, path);
    DeleteFileW(path);
    _swprintf(args, L"/c echo %s> \"%s\"", name, path);
}

static IRegisteredTask *register_definition(const WCHAR *name, ITaskDefinition *def, TASK_LOGON_TYPE logon)
{
    IRegisteredTask *task = NULL;
    VARIANT empty;
    BSTR bname = SysAllocString(name);
    HRESULT hr;

    VariantInit(&empty);
    hr = ITaskFolder_RegisterTaskDefinition(folder, bname, def, TASK_CREATE_OR_UPDATE, empty, empty, logon, empty, &task);
    ok(hr == S_OK, "RegisterTaskDefinition(%ls) failed: 0x%lx\n", name, hr);
    SysFreeString(bname);
    return task;
}

static ITaskDefinition *new_definition(const WCHAR *marker)
{
    ITaskDefinition *def = NULL;
    IActionCollection *actions;
    IAction *action;
    IExecAction *exec;
    WCHAR args[MAX_PATH * 2];
    HRESULT hr;

    hr = ITaskService_NewTask(service, 0, &def);
    ok(hr == S_OK, "NewTask failed: 0x%lx\n", hr);
    if (hr != S_OK) return NULL;

    marker_arguments(marker, args);
    hr = ITaskDefinition_get_Actions(def, &actions);
    ok(hr == S_OK, "get_Actions failed: 0x%lx\n", hr);
    hr = IActionCollection_Create(actions, TASK_ACTION_EXEC, &action);
    ok(hr == S_OK, "Create(TASK_ACTION_EXEC) failed: 0x%lx\n", hr);
    hr = IAction_QueryInterface(action, &IID_IExecAction, (void **)&exec);
    ok(hr == S_OK, "QueryInterface(IExecAction) failed: 0x%lx\n", hr);
    IExecAction_put_Path(exec, (BSTR)L"%SystemRoot%\\system32\\cmd.exe");
    IExecAction_put_Arguments(exec, args);
    IExecAction_Release(exec);
    IAction_Release(action);
    IActionCollection_Release(actions);
    return def;
}

static ITrigger *add_trigger(ITaskDefinition *def, TASK_TRIGGER_TYPE2 type)
{
    ITriggerCollection *triggers;
    ITrigger *trigger = NULL;
    HRESULT hr;

    hr = ITaskDefinition_get_Triggers(def, &triggers);
    ok(hr == S_OK, "get_Triggers failed: 0x%lx\n", hr);
    hr = ITriggerCollection_Create(triggers, type, &trigger);
    ok(hr == S_OK, "Create(%d) failed: 0x%lx\n", type, hr);
    ITriggerCollection_Release(triggers);
    return trigger;
}

/* a time trigger built through the object model, as Google Updater does it */
static IRegisteredTask *register_time_task(void)
{
    ITaskDefinition *def = new_definition(L"time");
    IRegisteredTask *task;
    ITrigger *trigger;
    WCHAR start[64];

    if (!def) return NULL;
    trigger = add_trigger(def, TASK_TRIGGER_TIME);
    boundary(60, FALSE, start);
    ITrigger_put_StartBoundary(trigger, start);
    ITrigger_Release(trigger);

    task = register_definition(L"Time", def, TASK_LOGON_INTERACTIVE_TOKEN);
    ITaskDefinition_Release(def);
    return task;
}

/* a daily trigger that repeats every minute, started a day and a few seconds ago */
static IRegisteredTask *register_daily_task(void)
{
    ITaskDefinition *def = new_definition(L"daily");
    IRepetitionPattern *repetition;
    IDailyTrigger *daily;
    IRegisteredTask *task;
    ITrigger *trigger;
    WCHAR start[64];
    HRESULT hr;

    if (!def) return NULL;
    trigger = add_trigger(def, TASK_TRIGGER_DAILY);
    boundary(-86400 - 10, FALSE, start);
    ITrigger_put_StartBoundary(trigger, start);
    hr = ITrigger_QueryInterface(trigger, &IID_IDailyTrigger, (void **)&daily);
    ok(hr == S_OK, "QueryInterface(IDailyTrigger) failed: 0x%lx\n", hr);
    IDailyTrigger_put_DaysInterval(daily, 1);
    hr = IDailyTrigger_get_Repetition(daily, &repetition);
    ok(hr == S_OK, "get_Repetition failed: 0x%lx\n", hr);
    IRepetitionPattern_put_Interval(repetition, (BSTR)L"PT1M");
    IRepetitionPattern_put_Duration(repetition, (BSTR)L"P1D");
    IRepetitionPattern_put_StopAtDurationEnd(repetition, VARIANT_FALSE);
    IRepetitionPattern_Release(repetition);
    IDailyTrigger_Release(daily);
    ITrigger_Release(trigger);

    task = register_definition(L"Daily", def, TASK_LOGON_INTERACTIVE_TOKEN);
    ITaskDefinition_Release(def);
    return task;
}

/* a LocalSystem task registered as XML, with a UTC start */
static IRegisteredTask *register_system_task(void)
{
    static const WCHAR fmt[] =
        L"<?xml version=\"1.0\"?>\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        L"  <Triggers><TimeTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled></TimeTrigger></Triggers>\n"
        L"  <Principals><Principal id=\"Author\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n"
        L"  <Settings><Enabled>true</Enabled></Settings>\n"
        L"  <Actions Context=\"Author\"><Exec><Command>%%SystemRoot%%\\system32\\cmd.exe</Command><Arguments>%s</Arguments></Exec></Actions>\n"
        L"</Task>\n";
    WCHAR start[64], args[MAX_PATH * 2], escaped[MAX_PATH * 3], xml[4096];
    IRegisteredTask *task = NULL;
    const WCHAR *p;
    WCHAR *q;
    VARIANT empty;
    HRESULT hr;

    boundary(75, TRUE, start);
    marker_arguments(L"system", args);
    for (p = args, q = escaped; *p; p++)
    {
        if (*p == '"') { wcscpy(q, L"&quot;"); q += 6; }
        else if (*p == '>') { wcscpy(q, L"&gt;"); q += 4; }
        else *q++ = *p;
    }
    *q = 0;
    _swprintf(xml, fmt, start, escaped);

    VariantInit(&empty);
    hr = ITaskFolder_RegisterTask(folder, (BSTR)L"System", xml, TASK_CREATE_OR_UPDATE, empty, empty,
                                  TASK_LOGON_SERVICE_ACCOUNT, empty, &task);
    ok(hr == S_OK, "RegisterTask(System) failed: 0x%lx\n", hr);
    return task;
}

/* a task with a far trigger, started now with IRegisteredTask::Run */
static IRegisteredTask *register_run_task(void)
{
    ITaskDefinition *def = new_definition(L"run");
    IRegisteredTask *task;
    ITrigger *trigger;
    WCHAR start[64];

    if (!def) return NULL;
    trigger = add_trigger(def, TASK_TRIGGER_TIME);
    boundary(86400 * 30, FALSE, start);
    ITrigger_put_StartBoundary(trigger, start);
    ITrigger_Release(trigger);

    task = register_definition(L"Run", def, TASK_LOGON_INTERACTIVE_TOKEN);
    ITaskDefinition_Release(def);
    return task;
}

/* the registered definition reads back with its trigger and action */
static void test_definition(IRegisteredTask *task, TASK_TRIGGER_TYPE2 expect_type)
{
    ITaskDefinition *def;
    ITriggerCollection *triggers;
    IActionCollection *actions;
    ITrigger *trigger;
    IAction *action;
    IExecAction *exec;
    TASK_TRIGGER_TYPE2 type;
    BSTR path = NULL, start = NULL;
    LONG count = -1;
    HRESULT hr;

    hr = IRegisteredTask_get_Definition(task, &def);
    ok(hr == S_OK, "get_Definition failed: 0x%lx\n", hr);
    if (hr != S_OK) return;

    hr = ITaskDefinition_get_Triggers(def, &triggers);
    ok(hr == S_OK, "get_Triggers failed: 0x%lx\n", hr);
    hr = ITriggerCollection_get_Count(triggers, &count);
    ok(hr == S_OK && count == 1, "trigger count: hr 0x%lx, count %ld\n", hr, count);
    if (count == 1 && ITriggerCollection_get_Item(triggers, 1, &trigger) == S_OK)
    {
        hr = ITrigger_get_Type(trigger, &type);
        ok(hr == S_OK && type == expect_type, "trigger type: hr 0x%lx, type %d\n", hr, type);
        hr = ITrigger_get_StartBoundary(trigger, &start);
        ok(hr == S_OK && start && start[0], "StartBoundary: hr 0x%lx, %ls\n", hr, start);
        SysFreeString(start);
        ITrigger_Release(trigger);
    }
    ITriggerCollection_Release(triggers);

    count = -1;
    hr = ITaskDefinition_get_Actions(def, &actions);
    ok(hr == S_OK, "get_Actions failed: 0x%lx\n", hr);
    hr = IActionCollection_get_Count(actions, &count);
    ok(hr == S_OK && count == 1, "action count: hr 0x%lx, count %ld\n", hr, count);
    if (count == 1 && IActionCollection_get_Item(actions, 1, &action) == S_OK)
    {
        hr = IAction_QueryInterface(action, &IID_IExecAction, (void **)&exec);
        ok(hr == S_OK, "QueryInterface(IExecAction) failed: 0x%lx\n", hr);
        if (hr == S_OK)
        {
            hr = IExecAction_get_Path(exec, &path);
            ok(hr == S_OK && path && wcsstr(path, L"cmd.exe"), "Path: hr 0x%lx, %ls\n", hr, path);
            SysFreeString(path);
            IExecAction_Release(exec);
        }
        IAction_Release(action);
    }
    IActionCollection_Release(actions);
    ITaskDefinition_Release(def);
}

static BOOL file_exists(const WCHAR *path)
{
    BOOL (WINAPI *pWow64DisableWow64FsRedirection)(void **);
    BOOL (WINAPI *pWow64RevertWow64FsRedirection)(void *);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void *old = NULL;
    BOOL ret, disabled = FALSE;

    pWow64DisableWow64FsRedirection = (void *)GetProcAddress(kernel32, "Wow64DisableWow64FsRedirection");
    pWow64RevertWow64FsRedirection = (void *)GetProcAddress(kernel32, "Wow64RevertWow64FsRedirection");
    if (pWow64DisableWow64FsRedirection && pWow64RevertWow64FsRedirection)
        disabled = pWow64DisableWow64FsRedirection(&old);
    ret = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    if (disabled) pWow64RevertWow64FsRedirection(old);
    return ret;
}

static BOOL marker_exists(const WCHAR *name)
{
    WCHAR path[MAX_PATH];

    marker_path(name, path);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void delete_task(const WCHAR *name)
{
    HRESULT hr = ITaskFolder_DeleteTask(folder, (BSTR)name, 0);
    ok(hr == S_OK, "DeleteTask(%ls) failed: 0x%lx\n", name, hr);
}

START_TEST(TaskScheduler)
{
    static const WCHAR *markers[] = { L"time", L"daily", L"system", L"run" };
    IRegisteredTask *time_task, *daily_task, *system_task, *run_task;
    ITaskFolder *root = NULL;
    IRunningTask *running = NULL;
    VARIANT empty;
    WCHAR tasks_dir[MAX_PATH], file[MAX_PATH];
    BOOL seen[4] = { 0 };
    DWORD start_tick, i, left;
    HRESULT hr;

    GetTempPathW(ARRAYSIZE(marker_dir), marker_dir);
    i = wcslen(marker_dir);
    if (i && marker_dir[i - 1] == '\\') marker_dir[i - 1] = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService, (void **)&service);
    ok(hr == S_OK, "CoCreateInstance(CLSID_TaskScheduler) failed: 0x%lx\n", hr);
    if (hr != S_OK)
    {
        skip("no Task Scheduler\n");
        return;
    }

    VariantInit(&empty);
    hr = ITaskService_Connect(service, empty, empty, empty, empty);
    ok(hr == S_OK, "Connect failed: 0x%lx\n", hr);
    if (hr != S_OK)
    {
        ITaskService_Release(service);
        return;
    }

    hr = ITaskService_GetFolder(service, (BSTR)L"\\", &root);
    ok(hr == S_OK, "GetFolder failed: 0x%lx\n", hr);
    if (hr != S_OK)
    {
        ITaskService_Release(service);
        return;
    }
    hr = ITaskFolder_CreateFolder(root, (BSTR)FOLDER, empty, &folder);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        hr = ITaskService_GetFolder(service, (BSTR)FOLDER, &folder);
    ok(hr == S_OK, "CreateFolder failed: 0x%lx\n", hr);
    ITaskFolder_Release(root);
    if (hr != S_OK)
    {
        ITaskService_Release(service);
        return;
    }

    /* a task name cannot leave the task store */
    {
        static const WCHAR xml[] =
            L"<Task xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
            L"<Actions><Exec><Command>cmd.exe</Command></Exec></Actions></Task>";
        IRegisteredTask *escaped = NULL;
        WCHAR outside[MAX_PATH];

        hr = ITaskFolder_RegisterTask(folder, (BSTR)L"..\\..\\taskschd_apitest_escape", (BSTR)xml, TASK_CREATE,
                                      empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &escaped);
        ok(FAILED(hr), "RegisterTask outside the task store succeeded\n");
        if (escaped) IRegisteredTask_Release(escaped);
        GetSystemDirectoryW(outside, ARRAYSIZE(outside));
        wcscat(outside, L"\\taskschd_apitest_escape");
        ok(!file_exists(outside), "%ls was created\n", outside);
        DeleteFileW(outside);
    }

    time_task = register_time_task();
    daily_task = register_daily_task();
    system_task = register_system_task();
    run_task = register_run_task();
    start_tick = GetTickCount();

    if (time_task) test_definition(time_task, TASK_TRIGGER_TIME);
    if (daily_task) test_definition(daily_task, TASK_TRIGGER_DAILY);
    if (system_task) test_definition(system_task, TASK_TRIGGER_TIME);

    /* the task store (System32 itself, not SysWOW64, for a 32-bit test under WoW64) */
    GetSystemDirectoryW(tasks_dir, ARRAYSIZE(tasks_dir));
    _swprintf(file, L"%s\\Tasks%s\\Time", tasks_dir, FOLDER);
    ok(file_exists(file), "%ls does not exist\n", file);

    if (run_task)
    {
        hr = IRegisteredTask_Run(run_task, empty, &running);
        ok(hr == S_OK, "Run failed: 0x%lx\n", hr);
        if (running) IRunningTask_Release(running);
    }

    /* the time tasks fire about a minute after registration, none of them before */
    ok(!marker_exists(L"time"), "the time task ran early\n");
    ok(!marker_exists(L"system"), "the system task ran early\n");
    for (;;)
    {
        left = 0;
        for (i = 0; i < ARRAYSIZE(markers); i++)
        {
            if (!seen[i] && marker_exists(markers[i]))
            {
                seen[i] = TRUE;
                trace("%ls ran after %lu s\n", markers[i], (GetTickCount() - start_tick) / 1000);
            }
            if (!seen[i]) left++;
        }
        if (!left || GetTickCount() - start_tick > WAIT_SECONDS * 1000) break;
        Sleep(1000);
    }
    for (i = 0; i < ARRAYSIZE(markers); i++)
        ok(seen[i], "the %ls task did not run within %u s\n", markers[i], WAIT_SECONDS);

    if (time_task) IRegisteredTask_Release(time_task);
    if (daily_task) IRegisteredTask_Release(daily_task);
    if (system_task) IRegisteredTask_Release(system_task);
    if (run_task) IRegisteredTask_Release(run_task);

    delete_task(L"Time");
    delete_task(L"Daily");
    delete_task(L"System");
    delete_task(L"Run");
    ok(!file_exists(file), "%ls still exists\n", file);

    ITaskFolder_Release(folder);
    hr = ITaskService_GetFolder(service, (BSTR)L"\\", &root);
    if (hr == S_OK)
    {
        hr = ITaskFolder_DeleteFolder(root, (BSTR)FOLDER, 0);
        ok(hr == S_OK, "DeleteFolder failed: 0x%lx\n", hr);
        ITaskFolder_Release(root);
    }
    ITaskService_Release(service);

    for (i = 0; i < ARRAYSIZE(markers); i++)
    {
        marker_path(markers[i], file);
        DeleteFileW(file);
    }
    CoUninitialize();
}
