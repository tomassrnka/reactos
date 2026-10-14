/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for the argument checks and failure codes of tbs.dll
 *
 * The expected values were measured on Windows Server 2008 R2 and Windows 10, x64 and WOW64,
 * which have no TPM. The tests skip when a TPM is present.
 */

#include <apitest.h>

#include <windef.h>
#include <winbase.h>
#include <winerror.h>

typedef UINT32 TBS_RESULT;
typedef PVOID TBS_HCONTEXT;

typedef TBS_RESULT (WINAPI *PTbsi_Context_Create)(const UINT32 *, TBS_HCONTEXT *);
typedef TBS_RESULT (WINAPI *PTbsip_Context_Close)(TBS_HCONTEXT);
typedef TBS_RESULT (WINAPI *PTbsip_Cancel_Commands)(TBS_HCONTEXT);
typedef TBS_RESULT (WINAPI *PTbsip_Submit_Command)(TBS_HCONTEXT, UINT32, UINT32, const BYTE *, UINT32, BYTE *, UINT32 *);
typedef TBS_RESULT (WINAPI *PTbsi_Get_TCG_Log)(TBS_HCONTEXT, BYTE *, UINT32 *);
typedef TBS_RESULT (WINAPI *PTbsi_Physical_Presence_Command)(TBS_HCONTEXT, const BYTE *, UINT32, BYTE *, UINT32 *);

static HMODULE hTbs;
static PTbsi_Context_Create pTbsi_Context_Create;
static PTbsip_Context_Close pTbsip_Context_Close;
static PTbsip_Cancel_Commands pTbsip_Cancel_Commands;
static PTbsip_Submit_Command pTbsip_Submit_Command;
static PTbsi_Get_TCG_Log pTbsi_Get_TCG_Log;
static PTbsi_Physical_Presence_Command pTbsi_Physical_Presence_Command;

static const char *ExportNames[] =
{
    "Tbsi_Context_Create",
    "Tbsi_Get_TCG_Log",
    "Tbsi_Physical_Presence_Command",
    "Tbsip_Cancel_Commands",
    "Tbsip_Context_Close",
    "Tbsip_Submit_Command",
};

#define TBS_SUCCESS 0

static void test_exports(void)
{
    UINT i;

    for (i = 0; i < ARRAYSIZE(ExportNames); i++)
        ok(GetProcAddress(hTbs, ExportNames[i]) != NULL, "%s is not exported\n", ExportNames[i]);
}

static BOOL has_tpm(void)
{
    /* version two with both kinds of TPM, then version one for a Windows that does not know version two */
    static const UINT32 Probes[][2] = { { 2, 6 }, { 1, 0 } };
    TBS_HCONTEXT Context;
    TBS_RESULT Result;
    UINT i;

    for (i = 0; i < ARRAYSIZE(Probes); i++)
    {
        Context = NULL;
        Result = pTbsi_Context_Create(Probes[i], &Context);
        if (Result == TBS_SUCCESS)
        {
            pTbsip_Context_Close(Context);
            return TRUE;
        }
        /* any failure but the one of no TPM may hide one that is there */
        if (Result != TBS_E_TPM_NOT_FOUND)
            return TRUE;
    }
    return FALSE;
}

static void test_context_create(void)
{
    UINT32 Params[2];
    TBS_HCONTEXT Context;
    TBS_RESULT Result;
    UINT i;
    static const UINT32 Versions[][2] =
    {
        { 1, 0 },                   /* version one */
        { 2, 0 }, { 2, 1 }, { 2, 2 }, { 2, 4 }, { 2, 6 },   /* version two with its request flags */
        { 0, 0 }, { 3, 0 }, { 0xffffffff, 0 },              /* versions that do not exist */
    };

    /* without a TPM no context is made, whatever the parameters say, and the output is cleared */
    for (i = 0; i < ARRAYSIZE(Versions); i++)
    {
        Params[0] = Versions[i][0];
        Params[1] = Versions[i][1];
        Context = (TBS_HCONTEXT)(ULONG_PTR)0x11111111;
        Result = pTbsi_Context_Create(Params, &Context);
        ok(Result == TBS_E_TPM_NOT_FOUND, "%u: got 0x%lx\n", i, Result);
        ok(Context == NULL || broken(Context == (TBS_HCONTEXT)(ULONG_PTR)0x11111111) /* Windows 10 */,
           "%u: got %p\n", i, Context);
    }

    /* the output pointer is checked first, and the output is cleared when only the parameters are missing */
    Params[0] = 1;
    Result = pTbsi_Context_Create(Params, NULL);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    Result = pTbsi_Context_Create(NULL, NULL);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER || broken(Result == TBS_E_BAD_PARAMETER) /* Windows 10 */,
       "got 0x%lx\n", Result);
    Context = (TBS_HCONTEXT)(ULONG_PTR)0x22222222;
    Result = pTbsi_Context_Create(NULL, &Context);
    ok(Result == TBS_E_BAD_PARAMETER, "got 0x%lx\n", Result);
    ok(Context == NULL || broken(Context == (TBS_HCONTEXT)(ULONG_PTR)0x22222222) /* Windows 10 */,
       "got %p\n", Context);
}

static void test_device_info(void)
{
    typedef TBS_RESULT (WINAPI *PTbsi_GetDeviceInfo)(UINT32, PVOID);
    PTbsi_GetDeviceInfo pTbsi_GetDeviceInfo = (PVOID)GetProcAddress(hTbs, "Tbsi_GetDeviceInfo");
    static const UINT32 Sizes[] = { 0, 4, 8, 16, 20, 32 };
    BYTE Info[32];
    TBS_RESULT Result;
    UINT i;

    if (!pTbsi_GetDeviceInfo)
    {
        skip("Tbsi_GetDeviceInfo is not exported\n");
        return;
    }

    /* without a TPM there is no device, whatever the size, and nothing is written */
    for (i = 0; i < ARRAYSIZE(Sizes); i++)
    {
        memset(Info, 0xcc, sizeof(Info));
        Result = pTbsi_GetDeviceInfo(Sizes[i], Info);
        ok(Result == TBS_E_TPM_NOT_FOUND, "%lu: got 0x%lx\n", (ULONG)Sizes[i], Result);
        ok(Info[0] == 0xcc && Info[sizeof(Info) - 1] == 0xcc, "%lu: the buffer was written\n", (ULONG)Sizes[i]);
    }
    Result = pTbsi_GetDeviceInfo(16, NULL);
    ok(Result == TBS_E_TPM_NOT_FOUND, "got 0x%lx\n", Result);
}

#ifdef __i386__
/* GetDeviceIDString is stdcall on Windows 10: the callee removes its four arguments, so the stack pointer is the same after the call */
static void test_device_id_convention(void)
{
    typedef HRESULT (__stdcall *PGetDeviceIDString)(WCHAR *, UINT32, UINT32 *, BOOL *);
    PGetDeviceIDString pGetDeviceIDString = (PVOID)GetProcAddress(hTbs, "GetDeviceIDString");
    WCHAR Buffer[64];
    UINT32 Used = 0;
    BOOL Protected = FALSE;
    ULONG_PTR Before, After;

    if (!pGetDeviceIDString)
    {
        skip("GetDeviceIDString is not exported\n");
        return;
    }

#ifdef _MSC_VER
    __asm mov Before, esp
    pGetDeviceIDString(Buffer, ARRAYSIZE(Buffer), &Used, &Protected);
    __asm mov After, esp
#else
    __asm__ volatile ("movl %%esp, %0" : "=r"(Before) : : "memory");
    pGetDeviceIDString(Buffer, ARRAYSIZE(Buffer), &Used, &Protected);
    __asm__ volatile ("movl %%esp, %0" : "=r"(After) : : "memory");
#endif
    ok(Before == After, "the stack pointer moved by %ld\n", (LONG)(Before - After));
}
#endif

static void test_context_functions(void)
{
    static const ULONG_PTR Handles[] = { 1, 0x1234, (ULONG_PTR)-1 };
    BYTE Command[12] = { 0x80, 1, 0, 0, 0, 0x0c, 0, 0, 1, 0x44, 0, 0 };
    BYTE Output[64];
    UINT32 OutputSize;
    TBS_RESULT Result;
    UINT i;

    /* Windows 8 and later close, cancel and check the arguments of a context in another order */
    if (GetNTVersion() >= _WIN32_WINNT_WIN8)
    {
        skip("Windows 8 and later check the arguments of the context functions differently\n");
        return;
    }

    /* closing no context closes nothing */
    Result = pTbsip_Context_Close(NULL);
    ok(Result == TBS_SUCCESS, "got 0x%lx\n", Result);

    /* every other handle is not a context, and so is no handle at all for the functions that use one */
    Result = pTbsip_Cancel_Commands(NULL);
    ok(Result == TBS_E_INVALID_CONTEXT, "got 0x%lx\n", Result);
    OutputSize = sizeof(Output);
    Result = pTbsi_Get_TCG_Log(NULL, Output, &OutputSize);
    ok(Result == TBS_E_INVALID_CONTEXT, "got 0x%lx\n", Result);
    OutputSize = sizeof(Output);
    Result = pTbsi_Physical_Presence_Command(NULL, Command, 4, Output, &OutputSize);
    ok(Result == TBS_E_INVALID_CONTEXT, "got 0x%lx\n", Result);
    OutputSize = sizeof(Output);
    Result = pTbsip_Submit_Command(NULL, 0, 200, Command, sizeof(Command), Output, &OutputSize);
    ok(Result == TBS_E_INVALID_CONTEXT, "got 0x%lx\n", Result);

    for (i = 0; i < ARRAYSIZE(Handles); i++)
    {
        TBS_HCONTEXT Handle = (TBS_HCONTEXT)Handles[i];

        Result = pTbsip_Context_Close(Handle);
        ok(Result == TBS_E_INVALID_CONTEXT, "%p: got 0x%lx\n", Handle, Result);
        Result = pTbsip_Cancel_Commands(Handle);
        ok(Result == TBS_E_INVALID_CONTEXT, "%p: got 0x%lx\n", Handle, Result);
        OutputSize = sizeof(Output);
        Result = pTbsip_Submit_Command(Handle, 0, 200, Command, sizeof(Command), Output, &OutputSize);
        ok(Result == TBS_E_INVALID_CONTEXT, "%p: got 0x%lx\n", Handle, Result);
        OutputSize = sizeof(Output);
        Result = pTbsi_Get_TCG_Log(Handle, Output, &OutputSize);
        ok(Result == TBS_E_INVALID_CONTEXT, "%p: got 0x%lx\n", Handle, Result);
        OutputSize = sizeof(Output);
        Result = pTbsi_Physical_Presence_Command(Handle, Command, 4, Output, &OutputSize);
        ok(Result == TBS_E_INVALID_CONTEXT, "%p: got 0x%lx\n", Handle, Result);
    }

    /* the pointers of a call are checked before its context: the command first, then the outputs */
    OutputSize = sizeof(Output);
    Result = pTbsip_Submit_Command((TBS_HCONTEXT)(ULONG_PTR)0x1234, 0, 200, NULL, 0, NULL, NULL);
    ok(Result == TBS_E_BAD_PARAMETER, "got 0x%lx\n", Result);
    Result = pTbsip_Submit_Command((TBS_HCONTEXT)(ULONG_PTR)0x1234, 0, 200, NULL, sizeof(Command), Output, &OutputSize);
    ok(Result == TBS_E_BAD_PARAMETER, "got 0x%lx\n", Result);
    Result = pTbsip_Submit_Command((TBS_HCONTEXT)(ULONG_PTR)0x1234, 0, 200, Command, sizeof(Command), NULL, &OutputSize);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    Result = pTbsip_Submit_Command((TBS_HCONTEXT)(ULONG_PTR)0x1234, 0, 200, Command, sizeof(Command), Output, NULL);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);

    /* the other values of a command are not looked at before the context */
    OutputSize = 0;
    Result = pTbsip_Submit_Command((TBS_HCONTEXT)(ULONG_PTR)0x1234, 5, 0xffffffff, Command, 0, Output, &OutputSize);
    ok(Result == TBS_E_INVALID_CONTEXT, "got 0x%lx\n", Result);

    OutputSize = sizeof(Output);
    Result = pTbsi_Get_TCG_Log(NULL, NULL, &OutputSize);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    Result = pTbsi_Get_TCG_Log(NULL, Output, NULL);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    OutputSize = 0;
    Result = pTbsi_Get_TCG_Log(NULL, Output, &OutputSize);
    ok(Result == TBS_E_INVALID_CONTEXT, "got 0x%lx\n", Result);

    OutputSize = sizeof(Output);
    Result = pTbsi_Physical_Presence_Command(NULL, NULL, 4, Output, &OutputSize);
    ok(Result == TBS_E_BAD_PARAMETER, "got 0x%lx\n", Result);
    Result = pTbsi_Physical_Presence_Command(NULL, Command, 4, NULL, &OutputSize);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    Result = pTbsi_Physical_Presence_Command(NULL, Command, 4, Output, NULL);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    Result = pTbsi_Physical_Presence_Command(NULL, Command, 0, Output, &OutputSize);
    ok(Result == TBS_E_INVALID_CONTEXT, "got 0x%lx\n", Result);
}

/* Windows 8 and later export these; Windows 7 and Windows Server 2008 R2 do not */
static void test_windows8_exports(void)
{
    typedef TBS_RESULT (WINAPI *POwnerAuth)(TBS_HCONTEXT, UINT32, BYTE *, UINT32 *);
    typedef TBS_RESULT (WINAPI *PLogEx)(UINT32, BYTE *, UINT32 *);
    typedef TBS_RESULT (WINAPI *PRevoke)(void);
    typedef TBS_RESULT (WINAPI *PCreateKey)(UINT32);
    POwnerAuth pOwnerAuth = (PVOID)GetProcAddress(hTbs, "Tbsi_Get_OwnerAuth");
    PLogEx pLogEx = (PVOID)GetProcAddress(hTbs, "Tbsi_Get_TCG_Log_Ex");
    PRevoke pRevoke = (PVOID)GetProcAddress(hTbs, "Tbsi_Revoke_Attestation");
    PCreateKey pCreateKey = (PVOID)GetProcAddress(hTbs, "Tbsi_Create_Windows_Key");
    static const UINT32 Types[] = { 0, 1, 2, 3, 4, 12, 13, 0x7fff };
    static const ULONG_PTR Contexts[] = { 0, 0x1234 };
    BYTE Buffer[64];
    UINT32 Size;
    TBS_RESULT Result;
    UINT i, j;

    if (!pOwnerAuth || !pLogEx || !pRevoke || !pCreateKey)
    {
        skip("The exports of Windows 8 are not there\n");
        return;
    }

    /* the output pointers are looked at first; then no context and no type fits */
    Size = sizeof(Buffer);
    Result = pOwnerAuth(NULL, 1, NULL, &Size);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    Result = pOwnerAuth(NULL, 1, Buffer, NULL);
    ok(Result == TBS_E_INVALID_OUTPUT_POINTER, "got 0x%lx\n", Result);
    for (i = 0; i < ARRAYSIZE(Contexts); i++)
    {
        for (j = 0; j < ARRAYSIZE(Types); j++)
        {
            Size = sizeof(Buffer);
            Buffer[0] = 0xcc;
            Result = pOwnerAuth((TBS_HCONTEXT)Contexts[i], Types[j], Buffer, &Size);
            ok(Result == TBS_E_INVALID_CONTEXT_PARAM, "%Iu, %lu: got 0x%lx\n", Contexts[i], (ULONG)Types[j], Result);
            ok(Size == sizeof(Buffer) && Buffer[0] == 0xcc, "%Iu, %lu: the output was changed\n", Contexts[i], (ULONG)Types[j]);
        }
    }

    /* the log of a machine without a TPM: only the pointer to the size is checked before that */
    for (j = 0; j < 5; j++)
    {
        Size = sizeof(Buffer);
        Result = pLogEx(j, Buffer, &Size);
        ok(Result == TBS_E_TPM_NOT_FOUND, "%u: got 0x%lx\n", j, Result);
        ok(Size == sizeof(Buffer), "%u: the size is %lu\n", j, (ULONG)Size);
    }
    Result = pLogEx(1, Buffer, NULL);
    ok(Result == TBS_E_BAD_PARAMETER, "got 0x%lx\n", Result);
    Size = sizeof(Buffer);
    Result = pLogEx(1, NULL, &Size);
    ok(Result == TBS_E_TPM_NOT_FOUND, "got 0x%lx\n", Result);

    Result = pRevoke();
    ok(Result == TBS_E_TPM_NOT_FOUND, "got 0x%lx\n", Result);

    Result = pCreateKey(0);
    ok(Result == TBS_E_BAD_PARAMETER, "got 0x%lx\n", Result);
    Result = pCreateKey(1);
    ok(Result == TBS_E_BAD_PARAMETER, "got 0x%lx\n", Result);
}

START_TEST(tbs)
{
    hTbs = LoadLibraryW(L"tbs.dll");
    ok(hTbs != NULL, "LoadLibrary failed with %lu\n", GetLastError());
    if (!hTbs)
    {
        skip("tbs.dll is not available\n");
        return;
    }

    test_exports();

    pTbsi_Context_Create = (PVOID)GetProcAddress(hTbs, "Tbsi_Context_Create");
    pTbsip_Context_Close = (PVOID)GetProcAddress(hTbs, "Tbsip_Context_Close");
    pTbsip_Cancel_Commands = (PVOID)GetProcAddress(hTbs, "Tbsip_Cancel_Commands");
    pTbsip_Submit_Command = (PVOID)GetProcAddress(hTbs, "Tbsip_Submit_Command");
    pTbsi_Get_TCG_Log = (PVOID)GetProcAddress(hTbs, "Tbsi_Get_TCG_Log");
    pTbsi_Physical_Presence_Command = (PVOID)GetProcAddress(hTbs, "Tbsi_Physical_Presence_Command");
    if (!pTbsi_Context_Create || !pTbsip_Context_Close || !pTbsip_Cancel_Commands ||
        !pTbsip_Submit_Command || !pTbsi_Get_TCG_Log || !pTbsi_Physical_Presence_Command)
    {
        skip("Not all of the exports are available\n");
        FreeLibrary(hTbs);
        return;
    }

    if (has_tpm())
    {
        skip("A TPM is present\n");
        FreeLibrary(hTbs);
        return;
    }

    test_context_create();
    test_device_info();
    test_windows8_exports();
#ifdef __i386__
    test_device_id_convention();
#endif
    test_context_functions();

    FreeLibrary(hTbs);
}
