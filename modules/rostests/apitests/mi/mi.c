/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the MI client API (mi.dll)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Every expectation here was checked against Windows 10 22H2 (19045).
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>

#define mi_clientFT_V1 mi_clientFT_V1_import
#include <mi.h>
#undef mi_clientFT_V1

#ifndef MI_DLL_NAME
#define MI_DLL_NAME L"mi.dll"
#endif

typedef MI_Result (MI_MAIN_CALL *PFN_INIT)(MI_Uint32, const MI_Char *, MI_Instance **, MI_Application *);

static HMODULE hmi;
static PFN_INIT pMI_Application_InitializeV1;
static const MI_ClientFT_V1 **pmi_clientFT_V1;

/* pull every instance of an operation; returns the final result, the count and a clone of the first instance */
static MI_Result pull_all(MI_Operation *op, UINT *count, MI_Instance **first, const MI_Char **message_out)
{
    const MI_Instance *inst;
    MI_Boolean more = MI_TRUE;
    MI_Result result = MI_RESULT_FAILED, res;
    const MI_Char *message;
    const MI_Instance *details;
    UINT guard = 0;

    *count = 0;
    if (first) *first = NULL;
    while (more && guard++ < 100000)
    {
        inst = NULL;
        message = NULL;
        details = NULL;
        res = op->ft->GetInstance(op, &inst, &more, &result, &message, &details);
        ok(res == MI_RESULT_OK, "GetInstance returned %d\n", res);
        if (res != MI_RESULT_OK) break;
        if (inst)
        {
            if (!*count && first) inst->ft->Clone(inst, first);
            (*count)++;
        }
        if (!more && message_out) *message_out = message;
        if (!more && result != MI_RESULT_OK)
            ok(details != NULL, "no error details for result %d\n", result);
    }
    return result;
}

static void test_exports(void)
{
    ok(pMI_Application_InitializeV1 != NULL, "MI_Application_InitializeV1 not exported\n");
    ok(pmi_clientFT_V1 != NULL, "mi_clientFT_V1 not exported\n");
    if (pmi_clientFT_V1)
    {
        const MI_ClientFT_V1 *ft = *pmi_clientFT_V1;
        ok(ft != NULL, "mi_clientFT_V1 is NULL\n");
        if (ft)
        {
            ok(ft->applicationFT != NULL, "no applicationFT\n");
            ok(ft->sessionFT != NULL, "no sessionFT\n");
            ok(ft->operationFT != NULL, "no operationFT\n");
            ok(ft->destinationOptionsFT != NULL, "no destinationOptionsFT\n");
            ok(ft->operationOptionsFT != NULL, "no operationOptionsFT\n");
            ok(ft->utilitiesFT != NULL, "no utilitiesFT\n");
            ok(ft->serializerFT != NULL, "no serializerFT\n");
            ok(ft->deserializerFT != NULL, "no deserializerFT\n");
        }
    }
}

static void test_utilities(void)
{
    const MI_ClientFT_V1 *ft = *pmi_clientFT_V1;
    MI_Instance *err = NULL;
    MI_Value v;
    MI_Type type;
    MI_Result res;

    res = ft->utilitiesFT->CimErrorFromErrorCode(ERROR_FILE_NOT_FOUND, L"Win32", L"file is missing", &err);
    ok(res == MI_RESULT_OK, "CimErrorFromErrorCode returned %d\n", res);
    ok(err != NULL, "no error instance\n");
    if (!err) return;
    res = err->ft->GetElement(err, L"Message", &v, &type, NULL, NULL);
    ok(res == MI_RESULT_OK, "GetElement(Message) returned %d\n", res);
    ok(res || type == MI_STRING, "Message type %d\n", type);
    if (!res && type == MI_STRING) ok(v.string && wcsstr(v.string, L"file is missing"), "Message %ls\n", v.string);
    res = err->ft->GetElement(err, L"error_Code", &v, &type, NULL, NULL);
    ok(res == MI_RESULT_OK, "GetElement(error_Code) returned %d\n", res);
    ok(res || (type == MI_UINT32 && v.uint32 == ERROR_FILE_NOT_FOUND), "error_Code type %d value %u\n", type, v.uint32);
    err->ft->Delete(err);
}

static void test_instance(MI_Application *app)
{
    MI_Instance *inst = NULL, *copy = NULL;
    const MI_Char *name;
    MI_Value v;
    MI_Type type;
    MI_Uint32 count, flags, index;
    MI_Result res;

    res = app->ft->NewInstance(app, L"MI_Test", NULL, &inst);
    ok(res == MI_RESULT_OK, "NewInstance returned %d\n", res);
    if (!inst) return;
    res = inst->ft->GetClassName(inst, &name);
    ok(res == MI_RESULT_OK && name && !wcscmp(name, L"MI_Test"), "GetClassName %d %ls\n", res, name);
    v.uint32 = 42;
    res = inst->ft->AddElement(inst, L"Answer", &v, MI_UINT32, 0);
    ok(res == MI_RESULT_OK, "AddElement returned %d\n", res);
    res = inst->ft->AddElement(inst, L"Answer", &v, MI_UINT32, 0);
    ok(res == MI_RESULT_ALREADY_EXISTS, "AddElement duplicate returned %d\n", res);
    v.string = L"hello";
    res = inst->ft->AddElement(inst, L"Text", &v, MI_STRING, 0);
    ok(res == MI_RESULT_OK, "AddElement string returned %d\n", res);
    res = inst->ft->AddElement(inst, L"Nothing", NULL, MI_STRING, 0);
    ok(res == MI_RESULT_OK, "AddElement NULL returned %d\n", res);
    res = inst->ft->GetElementCount(inst, &count);
    ok(res == MI_RESULT_OK && count == 3, "GetElementCount %d %u\n", res, count);

    memset(&v, 0, sizeof(v));
    res = inst->ft->GetElement(inst, L"answer", &v, &type, &flags, &index);
    ok(res == MI_RESULT_OK, "GetElement (case-insensitive) returned %d\n", res);
    ok(type == MI_UINT32 && v.uint32 == 42 && index == 0, "got type %d value %u index %u\n", type, v.uint32, index);
    res = inst->ft->GetElement(inst, L"Nothing", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && (flags & MI_FLAG_NULL), "NULL element: %d flags %#x\n", res, flags);
    res = inst->ft->GetElement(inst, L"Missing", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_NO_SUCH_PROPERTY, "GetElement missing returned %d\n", res);
    res = inst->ft->GetElementAt(inst, 7, &name, &v, &type, &flags);
    ok(res == MI_RESULT_INVALID_PARAMETER, "GetElementAt out of range returned %d\n", res);

    v.uint32 = 7;
    res = inst->ft->SetElement(inst, L"Answer", &v, MI_UINT32, 0);
    ok(res == MI_RESULT_OK, "SetElement returned %d\n", res);
    res = inst->ft->SetElement(inst, L"Missing", &v, MI_UINT32, 0);
    ok(res == MI_RESULT_NO_SUCH_PROPERTY, "SetElement missing returned %d\n", res);

    res = inst->ft->Clone(inst, &copy);
    ok(res == MI_RESULT_OK && copy, "Clone returned %d\n", res);
    if (copy)
    {
        res = copy->ft->GetElement(copy, L"Text", &v, &type, &flags, NULL);
        ok(res == MI_RESULT_OK && type == MI_STRING && v.string && !wcscmp(v.string, L"hello"), "clone Text %d\n", res);
        res = copy->ft->GetElement(copy, L"Answer", &v, &type, &flags, NULL);
        ok(res == MI_RESULT_OK && v.uint32 == 7, "clone Answer %d %u\n", res, v.uint32);
        copy->ft->Delete(copy);
    }
    res = inst->ft->ClearElement(inst, L"Text");
    ok(res == MI_RESULT_OK, "ClearElement returned %d\n", res);
    res = inst->ft->GetElement(inst, L"Text", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && (flags & MI_FLAG_NULL), "cleared element flags %#x\n", flags);
    inst->ft->Delete(inst);
}

static void test_options(MI_Application *app)
{
    MI_OperationOptions oo = MI_OPERATIONOPTIONS_NULL;
    MI_DestinationOptions dopt = MI_DESTINATIONOPTIONS_NULL;
    MI_Interval in, out;
    MI_Result res;

    res = app->ft->NewOperationOptions(app, MI_FALSE, &oo);
    ok(res == MI_RESULT_OK && oo.ft, "NewOperationOptions returned %d\n", res);
    if (oo.ft)
    {
        memset(&in, 0, sizeof(in));
        in.seconds = 30;
        res = oo.ft->SetInterval(&oo, L"__MI_OPERATIONOPTIONS_TIMEOUT", &in, 0);
        ok(res == MI_RESULT_OK, "SetInterval returned %d\n", res);
        memset(&out, 0xcc, sizeof(out));
        res = oo.ft->GetInterval(&oo, L"__MI_OPERATIONOPTIONS_TIMEOUT", &out, NULL, NULL);
        ok(res == MI_RESULT_OK && out.seconds == 30 && out.minutes == 0, "GetInterval %d %u\n", res, out.seconds);
        oo.ft->Delete(&oo);
    }
    res = app->ft->NewDestinationOptions(app, &dopt);
    ok(res == MI_RESULT_OK && dopt.ft, "NewDestinationOptions returned %d\n", res);
    if (dopt.ft)
    {
        const MI_Char *s = NULL;
        res = dopt.ft->SetString(&dopt, L"__MI_DESTINATIONOPTIONS_DATA_LOCALE", L"en-US", 0);
        ok(res == MI_RESULT_OK, "SetString returned %d\n", res);
        res = dopt.ft->GetString(&dopt, L"__MI_DESTINATIONOPTIONS_DATA_LOCALE", &s, NULL, NULL);
        ok(res == MI_RESULT_OK && s && !wcscmp(s, L"en-US"), "GetString %d %ls\n", res, s);
        dopt.ft->Delete(&dopt);
    }
}

static void check_os_instance(const MI_Instance *inst)
{
    const MI_Char *name = NULL, *ns = NULL;
    MI_Value v;
    MI_Type type;
    MI_Uint32 flags, count;
    MI_Result res;
    MI_Class *cls = NULL;

    res = inst->ft->GetClassName(inst, &name);
    ok(res == MI_RESULT_OK && name && !_wcsicmp(name, L"Win32_OperatingSystem"), "class name %ls\n", name);
    res = inst->ft->GetNameSpace(inst, &ns);
    ok(res == MI_RESULT_OK && ns && !_wcsicmp(ns, L"root/cimv2"), "namespace %ls\n", ns);
    res = inst->ft->GetElementCount(inst, &count);
    ok(res == MI_RESULT_OK && count >= 25, "element count %u\n", count);

    res = inst->ft->GetElement(inst, L"Caption", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_STRING && !(flags & MI_FLAG_NULL) && v.string && v.string[0],
       "Caption: res %d type %d flags %#x\n", res, type, flags);
    if (!res && type == MI_STRING && v.string) trace("Caption: %ls\n", v.string);

    res = inst->ft->GetElement(inst, L"BuildNumber", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_STRING && v.string && v.string[0] >= '0' && v.string[0] <= '9',
       "BuildNumber: res %d type %d\n", res, type);

    res = inst->ft->GetElement(inst, L"TotalVisibleMemorySize", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_UINT64 && v.uint64 > 65536, "TotalVisibleMemorySize: res %d type %d value %I64u\n",
       res, type, (res || type != MI_UINT64) ? 0 : v.uint64);

    res = inst->ft->GetElement(inst, L"NumberOfProcesses", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_UINT32 && v.uint32 > 3, "NumberOfProcesses: res %d type %d value %u\n",
       res, type, v.uint32);

    res = inst->ft->GetElement(inst, L"OSType", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_UINT16 && v.uint16 == 18, "OSType: res %d type %d value %u\n", res, type, v.uint16);

    res = inst->ft->GetElement(inst, L"Primary", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_BOOLEAN && v.boolean == MI_TRUE, "Primary: res %d type %d value %u\n",
       res, type, v.boolean);

    res = inst->ft->GetElement(inst, L"LastBootUpTime", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_DATETIME, "LastBootUpTime: res %d type %d\n", res, type);
    if (!res && type == MI_DATETIME)
        ok(v.datetime.isTimestamp && v.datetime.u.timestamp.year >= 2000 && v.datetime.u.timestamp.month >= 1 &&
           v.datetime.u.timestamp.month <= 12, "LastBootUpTime %u-%u\n",
           v.datetime.u.timestamp.year, v.datetime.u.timestamp.month);

    res = inst->ft->GetElement(inst, L"Name", &v, &type, &flags, NULL);
    ok(res == MI_RESULT_OK && type == MI_STRING && (flags & MI_FLAG_PROPERTY), "Name: res %d type %d flags %#x\n",
       res, type, flags);

    res = inst->ft->GetClass(inst, &cls);
    ok(res == MI_RESULT_OK && cls, "GetClass returned %d\n", res);
    if (cls)
    {
        const MI_Char *cname = NULL;
        res = cls->ft->GetClassName(cls, &cname);
        ok(res == MI_RESULT_OK && cname && !_wcsicmp(cname, L"Win32_OperatingSystem"), "class %ls\n", cname);
        cls->ft->Delete(cls);
    }
}

struct push_ctx
{
    HANDLE done;
    LONG instances;
    LONG finals;
    MI_Result result;
    BOOL bad_order;
};

static void MI_CALL push_instance(MI_Operation *op, void *context, const MI_Instance *inst, MI_Boolean more,
                                  MI_Result result, const MI_Char *message, const MI_Instance *details,
                                  MI_Result (MI_CALL *ack)(MI_Operation *))
{
    struct push_ctx *ctx = context;
    if (ctx->finals) ctx->bad_order = TRUE;
    if (inst) InterlockedIncrement(&ctx->instances);
    if (!more)
    {
        ctx->finals++;
        ctx->result = result;
    }
    if (ack) ack(op);
    if (!more) SetEvent(ctx->done);
}

static void test_session(MI_Application *app, const MI_Char *protocol)
{
    MI_Session session = MI_SESSION_NULL;
    MI_Operation op = MI_OPERATION_NULL;
    MI_Instance *ext = NULL, *first = NULL;
    MI_OperationCallbacks cb = MI_OPERATIONCALLBACKS_NULL;
    struct push_ctx ctx;
    const MI_Char *message;
    WCHAR query[128];
    UINT count, pull_count;
    MI_Result res;

    res = app->ft->NewSession(app, protocol, NULL, NULL, NULL, &ext, &session);
    ok(res == MI_RESULT_OK, "NewSession(%ls) returned %d\n", protocol ? protocol : L"NULL", res);
    if (res) return;
    ok(session.ft != NULL, "no session ft\n");

    /* pull: enumerate Win32_OperatingSystem */
    session.ft->EnumerateInstances(&session, 0, NULL, L"root\\cimv2", L"Win32_OperatingSystem", MI_FALSE, NULL, &op);
    ok(op.ft != NULL, "no operation ft\n");
    res = pull_all(&op, &count, &first, NULL);
    ok(res == MI_RESULT_OK, "enumerate result %d\n", res);
    ok(count == 1, "got %u Win32_OperatingSystem instances\n", count);
    op.ft->Close(&op);
    if (first)
    {
        check_os_instance(first);
        first->ft->Delete(first);
    }

    /* pull: WQL query for our own process */
    _snwprintf(query, ARRAYSIZE(query) - 1, L"SELECT * FROM Win32_Process WHERE ProcessId = %lu", GetCurrentProcessId());
    query[ARRAYSIZE(query) - 1] = 0;
    memset(&op, 0, sizeof(op));
    session.ft->QueryInstances(&session, 0, NULL, L"root/cimv2", L"WQL", query, NULL, &op);
    res = pull_all(&op, &count, &first, NULL);
    ok(res == MI_RESULT_OK, "query result %d\n", res);
    ok(count == 1, "got %u processes\n", count);
    op.ft->Close(&op);
    if (first)
    {
        MI_Value v;
        MI_Type type;
        res = first->ft->GetElement(first, L"ProcessId", &v, &type, NULL, NULL);
        ok(res == MI_RESULT_OK && type == MI_UINT32 && v.uint32 == GetCurrentProcessId(), "ProcessId %d %d %u\n",
           res, type, v.uint32);
        res = first->ft->GetElement(first, L"Name", &v, &type, NULL, NULL);
        ok(res == MI_RESULT_OK && type == MI_STRING && v.string && wcsstr(v.string, L".exe"), "Name %d\n", res);
        first->ft->Delete(first);
    }

    /* push: the same enumeration through callbacks */
    memset(&ctx, 0, sizeof(ctx));
    ctx.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx.result = MI_RESULT_FAILED;
    cb.callbackContext = &ctx;
    cb.instanceResult = push_instance;
    memset(&op, 0, sizeof(op));
    session.ft->EnumerateInstances(&session, 0, NULL, L"root/cimv2", L"Win32_Process", MI_FALSE, &cb, &op);
    ok(WaitForSingleObject(ctx.done, 60000) == WAIT_OBJECT_0, "push enumeration timed out\n");
    ok(ctx.result == MI_RESULT_OK, "push result %d\n", ctx.result);
    ok(ctx.finals == 1 && !ctx.bad_order, "finals %ld bad order %d\n", ctx.finals, ctx.bad_order);
    ok(ctx.instances > 3, "push got %ld processes\n", ctx.instances);
    op.ft->Close(&op);
    CloseHandle(ctx.done);

    /* pull: GetClass Win32_BIOS */
    memset(&op, 0, sizeof(op));
    session.ft->GetClass(&session, 0, NULL, L"root\\cimv2", L"Win32_BIOS", NULL, &op);
    {
        const MI_Class *cls = NULL;
        MI_Boolean more = MI_TRUE;
        MI_Result result = MI_RESULT_FAILED;
        const MI_Instance *details;
        MI_Uint32 n = 0;
        BOOL got = FALSE;

        while (more)
        {
            res = op.ft->GetClass(&op, &cls, &more, &result, &message, &details);
            ok(res == MI_RESULT_OK, "Operation GetClass returned %d\n", res);
            if (res) break;
            if (cls && !got)
            {
                const MI_Char *cname = NULL;
                MI_Type type;
                MI_Uint32 flags;
                MI_Value v;
                MI_Boolean exists;
                got = TRUE;
                ok(cls->ft->GetClassName(cls, &cname) == MI_RESULT_OK && cname && !_wcsicmp(cname, L"Win32_BIOS"),
                   "class %ls\n", cname);
                ok(cls->ft->GetElementCount(cls, &n) == MI_RESULT_OK && n >= 8, "Win32_BIOS has %u properties\n", n);
                res = cls->ft->GetElement(cls, L"Manufacturer", &v, &exists, &type, NULL, NULL, &flags, NULL);
                ok(res == MI_RESULT_OK && type == MI_STRING, "Manufacturer: %d type %d\n", res, type);
                res = cls->ft->GetElement(cls, L"NoSuchProperty", &v, &exists, &type, NULL, NULL, &flags, NULL);
                ok(res == MI_RESULT_NO_SUCH_PROPERTY, "missing class property returned %d\n", res);
            }
        }
        ok(got, "no class returned\n");
        ok(result == MI_RESULT_OK, "GetClass result %d\n", result);
    }
    op.ft->Close(&op);

    /* errors */
    memset(&op, 0, sizeof(op));
    session.ft->EnumerateInstances(&session, 0, NULL, L"root\\cimv2", L"Win32_NoSuchClassAtAll", MI_FALSE, NULL, &op);
    res = pull_all(&op, &count, NULL, NULL);
    ok(res == MI_RESULT_INVALID_CLASS, "unknown class result %d\n", res);
    ok(count == 0, "unknown class returned %u instances\n", count);
    op.ft->Close(&op);

    memset(&op, 0, sizeof(op));
    session.ft->EnumerateInstances(&session, 0, NULL, L"root\\nosuchnamespace", L"Win32_OperatingSystem", MI_FALSE, NULL, &op);
    res = pull_all(&op, &count, NULL, NULL);
    ok(res == MI_RESULT_INVALID_NAMESPACE, "unknown namespace result %d\n", res);
    op.ft->Close(&op);

    memset(&op, 0, sizeof(op));
    session.ft->QueryInstances(&session, 0, NULL, L"root\\cimv2", L"WQL", L"SELEC * FRO Win32_Process", NULL, &op);
    res = pull_all(&op, &count, NULL, NULL);
    ok(res == MI_RESULT_INVALID_QUERY, "bad query result %d\n", res);
    op.ft->Close(&op);

    memset(&op, 0, sizeof(op));
    session.ft->GetClass(&session, 0, NULL, L"root\\cimv2", L"Win32_NoSuchClassAtAll", NULL, &op);
    {
        const MI_Class *cls = NULL;
        MI_Boolean more = MI_TRUE;
        MI_Result result = MI_RESULT_OK;
        const MI_Instance *details;
        while (more && op.ft->GetClass(&op, &cls, &more, &result, &message, &details) == MI_RESULT_OK) ;
        ok(result == MI_RESULT_NOT_FOUND || result == MI_RESULT_INVALID_CLASS, "unknown GetClass result %d\n", result);
    }
    op.ft->Close(&op);

    pull_count = 0;
    (void)pull_count;
    res = session.ft->Close(&session, NULL, NULL);
    ok(res == MI_RESULT_OK, "Session Close returned %d\n", res);
}

/* ---- filtered enumeration, concurrency and callback lifetime (review fixes) ---- */

/* pull a WQL query on a session and check that every instance has property `prop` equal to `expect`.
 * Returns the final result; *bad counts instances that do not match (wrong rows). */
static MI_Result query_checked(MI_Session *session, const WCHAR *query, const WCHAR *prop, const WCHAR *expect,
                               UINT *count, UINT *bad)
{
    MI_Operation op = MI_OPERATION_NULL;
    MI_Boolean more = MI_TRUE;
    MI_Result result = MI_RESULT_FAILED, res;
    UINT guard = 0;

    *count = *bad = 0;
    session->ft->QueryInstances(session, 0, NULL, L"root/cimv2", L"WQL", query, NULL, &op);
    if (!op.ft) return MI_RESULT_FAILED;
    while (more && guard++ < 100000)
    {
        const MI_Instance *inst = NULL, *details = NULL;
        const MI_Char *message = NULL;

        res = op.ft->GetInstance(&op, &inst, &more, &result, &message, &details);
        if (res != MI_RESULT_OK) { result = res; break; }
        if (!inst) continue;
        (*count)++;
        {
            MI_Value v;
            MI_Type type;
            MI_Uint32 flags;
            if (inst->ft->GetElement(inst, prop, &v, &type, &flags, NULL) != MI_RESULT_OK || type != MI_STRING ||
                (flags & MI_FLAG_NULL) || !v.string || _wcsicmp(v.string, expect))
                (*bad)++;
        }
    }
    op.ft->Close(&op);
    return result;
}

static void own_exe_name(WCHAR *name, DWORD size)
{
    WCHAR path[MAX_PATH], *p;
    GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    p = wcsrchr(path, '\\');
    wcsncpy(name, p ? p + 1 : path, size - 1);
    name[size - 1] = 0;
}

struct verify_ctx
{
    HANDLE done;
    const WCHAR *prop, *expect;
    LONG count, bad, finals;
    MI_Result result;
};

static void MI_CALL verify_instance(MI_Operation *op, void *context, const MI_Instance *inst, MI_Boolean more,
                                    MI_Result result, const MI_Char *message, const MI_Instance *details,
                                    MI_Result (MI_CALL *ack)(MI_Operation *))
{
    struct verify_ctx *ctx = context;
    if (inst)
    {
        MI_Value v;
        MI_Type type;
        MI_Uint32 flags;
        InterlockedIncrement(&ctx->count);
        if (inst->ft->GetElement(inst, ctx->prop, &v, &type, &flags, NULL) != MI_RESULT_OK || type != MI_STRING ||
            (flags & MI_FLAG_NULL) || !v.string || _wcsicmp(v.string, ctx->expect))
            InterlockedIncrement(&ctx->bad);
    }
    if (!more)
    {
        ctx->result = result;
        ctx->finals++;
    }
    if (ack) ack(op);
    if (!more) SetEvent(ctx->done);
}

static void test_filtered_enumeration(MI_Application *app)
{
    MI_Session session = MI_SESSION_NULL;
    MI_Operation op = MI_OPERATION_NULL;
    MI_OperationCallbacks cb = MI_OPERATIONCALLBACKS_NULL;
    struct verify_ctx ctx;
    WCHAR exe[MAX_PATH], query[MAX_PATH + 64];
    UINT count, bad, total, i;
    MI_Result res;

    res = app->ft->NewSession(app, NULL, NULL, NULL, NULL, NULL, &session);
    ok(res == MI_RESULT_OK, "NewSession returned %d\n", res);
    if (res) return;

    /* a filtered enumeration must return exactly the rows that match the filter */
    res = query_checked(&session, L"SELECT * FROM Win32_Service WHERE State = 'Running'", L"State", L"Running", &count, &bad);
    ok(res == MI_RESULT_OK, "running services result %d\n", res);
    ok(count > 0, "no running service returned\n");
    ok(bad == 0, "%u of %u 'Running' services do not match the filter\n", bad, count);

    res = query_checked(&session, L"SELECT * FROM Win32_Service WHERE State = 'Stopped'", L"State", L"Stopped", &count, &bad);
    ok(res == MI_RESULT_OK, "stopped services result %d\n", res);
    ok(bad == 0, "%u of %u 'Stopped' services do not match the filter\n", bad, count);

    own_exe_name(exe, ARRAYSIZE(exe));
    _snwprintf(query, ARRAYSIZE(query) - 1, L"SELECT * FROM Win32_Process WHERE Name = '%s'", exe);
    query[ARRAYSIZE(query) - 1] = 0;
    res = query_checked(&session, query, L"Name", exe, &count, &bad);
    ok(res == MI_RESULT_OK, "process by name result %d\n", res);
    ok(count >= 1, "own process %ls not returned\n", exe);
    ok(bad == 0, "%u of %u processes do not match Name = '%ls'\n", bad, count, exe);

    /* unfiltered: every process must have a name, and the total is at least the filtered count */
    res = query_checked(&session, L"SELECT * FROM Win32_Process", L"Name", L"", &total, &bad);
    ok(res == MI_RESULT_OK, "all processes result %d\n", res);
    ok(total >= count && total > 3, "total %u filtered %u\n", total, count);

    /* the same filter through push callbacks */
    for (i = 0; i < 2; i++)
    {
        memset(&ctx, 0, sizeof(ctx));
        ctx.done = CreateEventW(NULL, TRUE, FALSE, NULL);
        ctx.prop = L"State";
        ctx.expect = i ? L"Stopped" : L"Running";
        ctx.result = MI_RESULT_FAILED;
        cb.callbackContext = &ctx;
        cb.instanceResult = verify_instance;
        memset(&op, 0, sizeof(op));
        _snwprintf(query, ARRAYSIZE(query) - 1, L"SELECT * FROM Win32_Service WHERE State = '%s'", ctx.expect);
        query[ARRAYSIZE(query) - 1] = 0;
        session.ft->QueryInstances(&session, 0, NULL, L"root/cimv2", L"WQL", query, &cb, &op);
        ok(WaitForSingleObject(ctx.done, 60000) == WAIT_OBJECT_0, "push query timed out\n");
        ok(ctx.result == MI_RESULT_OK && ctx.finals == 1, "push result %d finals %ld\n", ctx.result, ctx.finals);
        ok(ctx.bad == 0, "push: %ld of %ld services do not match State = %ls\n", ctx.bad, ctx.count, ctx.expect);
        if (op.ft) op.ft->Close(&op);
        CloseHandle(ctx.done);
    }
    session.ft->Close(&session, NULL, NULL);
}

struct thread_ctx
{
    MI_Application *app;
    int id;
    LONG failures;
    UINT enumerations;
};

#define CONCURRENT_ROUNDS 15

static DWORD WINAPI concurrent_thread(void *arg)
{
    struct thread_ctx *t = arg;
    MI_Session session = MI_SESSION_NULL;
    WCHAR exe[MAX_PATH], query[MAX_PATH + 64];
    UINT i, count, bad;
    MI_Result res;

    if (t->app->ft->NewSession(t->app, NULL, NULL, NULL, NULL, NULL, &session) != MI_RESULT_OK)
    {
        t->failures++;
        return 0;
    }
    own_exe_name(exe, ARRAYSIZE(exe));
    for (i = 0; i < CONCURRENT_ROUNDS; i++)
    {
        /* every thread uses the same two classes, so they share wbemprox's tables */
        res = query_checked(&session, L"SELECT * FROM Win32_Service WHERE State = 'Running'", L"State", L"Running",
                            &count, &bad);
        if (res != MI_RESULT_OK || !count || bad) t->failures++;
        _snwprintf(query, ARRAYSIZE(query) - 1, L"SELECT * FROM Win32_Process WHERE Name = '%s'", exe);
        query[ARRAYSIZE(query) - 1] = 0;
        res = query_checked(&session, query, L"Name", exe, &count, &bad);
        if (res != MI_RESULT_OK || !count || bad) t->failures++;
        res = query_checked(&session, L"SELECT * FROM Win32_Process", L"Name", L"", &count, &bad);
        if (res != MI_RESULT_OK || count < 4) t->failures++;
        t->enumerations += 3;
    }
    session.ft->Close(&session, NULL, NULL);
    return 0;
}

static void test_concurrent_enumeration(MI_Application *app)
{
    struct thread_ctx ctx[4];
    HANDLE threads[4];
    int i;

    for (i = 0; i < 4; i++)
    {
        memset(&ctx[i], 0, sizeof(ctx[i]));
        ctx[i].app = app;
        ctx[i].id = i;
        threads[i] = CreateThread(NULL, 0, concurrent_thread, &ctx[i], 0, NULL);
        ok(threads[i] != NULL, "CreateThread failed\n");
    }
    for (i = 0; i < 4; i++)
    {
        if (!threads[i]) continue;
        ok(WaitForSingleObject(threads[i], 240000) == WAIT_OBJECT_0, "thread %d did not finish\n", i);
        CloseHandle(threads[i]);
        ok(ctx[i].failures == 0, "thread %d: %ld wrong or failed enumerations of %u\n", i, ctx[i].failures,
           ctx[i].enumerations);
    }
}

struct life_ctx
{
    HANDLE in_final, done;
    LONG final_returned;
    LONG callbacks;
    BOOL close_inside;
    MI_Result close_result;
    MI_Result (MI_CALL *first_ack)(MI_Operation *);
    MI_Operation *first_op;
    HANDLE first_seen;
    BOOL manual;
};

static void MI_CALL life_callback(MI_Operation *op, void *context, const MI_Instance *inst, MI_Boolean more,
                                  MI_Result result, const MI_Char *message, const MI_Instance *details,
                                  MI_Result (MI_CALL *ack)(MI_Operation *))
{
    struct life_ctx *ctx = context;
    LONG n = InterlockedIncrement(&ctx->callbacks);

    if (ctx->manual)
    {
        /* hold the first result un-acked; every later one is acked at once */
        if (n == 1)
        {
            ctx->first_ack = ack;
            ctx->first_op = op;
            SetEvent(ctx->first_seen);
            return;
        }
    }
    if (more) { if (ack) ack(op); return; }
    SetEvent(ctx->in_final);
    if (ctx->close_inside) ctx->close_result = op->ft->Close(op);
    else Sleep(400); /* the client is still using its context while the final callback runs */
    InterlockedExchange(&ctx->final_returned, 1);
    SetEvent(ctx->done);
}

static void init_life(struct life_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_final = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx->first_seen = CreateEventW(NULL, TRUE, FALSE, NULL);
}

static void free_life(struct life_ctx *ctx)
{
    CloseHandle(ctx->in_final);
    CloseHandle(ctx->done);
    CloseHandle(ctx->first_seen);
}

static BOOL running_on_reactos(void)
{
    WCHAR root[MAX_PATH];
    if (GetEnvironmentVariableW(L"MI_TEST_STRICT_CLOSE", root, ARRAYSIZE(root))) return TRUE;
    return GetEnvironmentVariableW(L"SystemRoot", root, ARRAYSIZE(root)) && wcsstr(root, L"ReactOS") != NULL;
}

static void test_callback_lifetime(MI_Application *app)
{
    MI_Session session = MI_SESSION_NULL;
    MI_Operation op = MI_OPERATION_NULL;
    MI_OperationCallbacks cb = MI_OPERATIONCALLBACKS_NULL;
    struct life_ctx ctx;
    MI_Result res;

    res = app->ft->NewSession(app, NULL, NULL, NULL, NULL, NULL, &session);
    ok(res == MI_RESULT_OK, "NewSession returned %d\n", res);
    if (res) return;

    /* Close from another thread while the final callback is still running must wait for it. Windows'
     * mi.dll does not wait and then crashes in its own worker (checked on the 22H2 oracle), so this and the
     * next case only run on ReactOS. */
    if (running_on_reactos())
    {
        trace("callback lifetime: Close during the final callback\n");
        init_life(&ctx);
        cb.callbackContext = &ctx;
        cb.instanceResult = life_callback;
        session.ft->EnumerateInstances(&session, 0, NULL, L"root/cimv2", L"Win32_OperatingSystem", MI_FALSE, &cb, &op);
        ok(op.ft != NULL, "no operation\n");
        if (op.ft)
        {
            ok(WaitForSingleObject(ctx.in_final, 60000) == WAIT_OBJECT_0, "final callback never started\n");
            res = op.ft->Close(&op);
            ok(res == MI_RESULT_OK, "Close returned %d\n", res);
            ok(ctx.final_returned, "Close returned while the final callback was still running\n");
            WaitForSingleObject(ctx.done, 5000);
        }
        free_life(&ctx);
    }
    cb.callbackContext = &ctx;
    cb.instanceResult = life_callback;

    /* Close from inside the final callback must not deadlock. */
    if (running_on_reactos())
    {
        trace("callback lifetime: Close inside the final callback\n");
        init_life(&ctx);
        ctx.close_inside = TRUE;
        ctx.close_result = MI_RESULT_FAILED;
        memset(&op, 0, sizeof(op));
        session.ft->EnumerateInstances(&session, 0, NULL, L"root/cimv2", L"Win32_OperatingSystem", MI_FALSE, &cb, &op);
        ok(WaitForSingleObject(ctx.done, 20000) == WAIT_OBJECT_0, "Close inside the callback deadlocked\n");
        ok(ctx.close_result == MI_RESULT_OK, "Close inside the callback returned %d\n", ctx.close_result);
        free_life(&ctx);
    }

    /* manual-ack pacing: Cancel does not acknowledge the result the client still holds.
     * The 22H2 oracle behaves the same way up to the ack (no further callback after Cancel while the result
     * is un-acked), but its mi.dll then hangs in the session/application teardown, so only ReactOS runs it. */
    if (running_on_reactos())
    {
        init_life(&ctx);
        ctx.manual = TRUE;
        memset(&op, 0, sizeof(op));
        session.ft->EnumerateInstances(&session, MI_OPERATIONFLAGS_MANUAL_ACK_RESULTS, NULL, L"root/cimv2", L"Win32_Process",
                                       MI_FALSE, &cb, &op);
        if (op.ft && WaitForSingleObject(ctx.first_seen, 60000) == WAIT_OBJECT_0)
        {
            op.ft->Cancel(&op, MI_REASON_NONE);
            Sleep(400);
            ok(ctx.callbacks == 1, "%ld callbacks arrived after Cancel without an ack\n", ctx.callbacks);
            if (ctx.first_ack) ctx.first_ack(&op);
            ok(WaitForSingleObject(ctx.done, 30000) == WAIT_OBJECT_0, "no final callback after the ack\n");
        }
        else ok(0, "manual-ack enumeration did not start\n");
        if (op.ft) op.ft->Close(&op);
        free_life(&ctx);
    }

    session.ft->Close(&session, NULL, NULL);
}

static void test_remote(MI_Application *app)
{
    MI_Session session = MI_SESSION_NULL;
    MI_Instance *ext = NULL;
    MI_Result res;

    res = app->ft->NewSession(app, L"NoSuchProtocol", NULL, NULL, NULL, &ext, &session);
    ok(res != MI_RESULT_OK, "unknown protocol accepted\n");
    if (ext) ext->ft->Delete(ext);
    if (res == MI_RESULT_OK) session.ft->Close(&session, NULL, NULL);
}

START_TEST(mi)
{
    MI_Application app = MI_APPLICATION_NULL;
    MI_Instance *ext = NULL;
    MI_Result res;

    hmi = LoadLibraryW(MI_DLL_NAME);
    ok(hmi != NULL, "mi.dll not loaded, error %lu\n", GetLastError());
    if (!hmi) return;
    pMI_Application_InitializeV1 = (PFN_INIT)GetProcAddress(hmi, "MI_Application_InitializeV1");
    pmi_clientFT_V1 = (const MI_ClientFT_V1 **)GetProcAddress(hmi, "mi_clientFT_V1");
    test_exports();
    if (!pMI_Application_InitializeV1 || !pmi_clientFT_V1 || !*pmi_clientFT_V1) return;

    res = pMI_Application_InitializeV1(0, L"mi_apitest", &ext, &app);
    ok(res == MI_RESULT_OK, "MI_Application_InitializeV1 returned %d\n", res);
    ok(ext == NULL, "unexpected extended error\n");
    ok(app.ft != NULL, "no application ft\n");
    if (res || !app.ft) return;

    test_utilities();
    test_instance(&app);
    test_options(&app);
    test_session(&app, NULL);
    test_session(&app, L"WMIDCOM");
    test_filtered_enumeration(&app);
    test_concurrent_enumeration(&app);
    test_callback_lifetime(&app);
    test_remote(&app);

    res = app.ft->Close(&app);
    ok(res == MI_RESULT_OK, "Application Close returned %d\n", res);
    FreeLibrary(hmi);
}
