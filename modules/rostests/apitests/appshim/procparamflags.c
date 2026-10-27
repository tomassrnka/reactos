/*
 * PROJECT:     appshim_apitest
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the AddProcessParametersFlags shim: only allowlisted bits may reach Params->Flags
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <ndk/rtlfuncs.h>
#include <ndk/pstypes.h>
#include "wine/test.h"

#include "appshim_apitest.h"

#define SHIM_NOTIFY_ATTACH 1

typedef BOOL (WINAPI* tNOTIFYSHIMS)(DWORD fdwReason, PVOID ptr);

#define ALLOWED (RTL_USER_PROCESS_PARAMETERS_PROFILE_USER | RTL_USER_PROCESS_PARAMETERS_PROFILE_KERNEL | \
                 RTL_USER_PROCESS_PARAMETERS_PROFILE_SERVER | RTL_USER_PROCESS_PARAMETERS_CASE_SENSITIVE | \
                 RTL_USER_PROCESS_PARAMETERS_DISABLE_HEAP_CHECKS)

static tGETHOOKAPIS pGetHookAPIs;
static tNOTIFYSHIMS pNotifyShims;

static ULONG run_shim(PCSTR cmdline, ULONG initial)
{
    PRTL_USER_PROCESS_PARAMETERS Params = NtCurrentPeb()->ProcessParameters;
    DWORD num_hooks = 0;
    ULONG result;

    Params->Flags = initial;
    pGetHookAPIs(cmdline, L"AddProcessParametersFlags", &num_hooks);
    ok(num_hooks == 0, "Expected no hooks, got %lu\n", num_hooks);
    /* The flags are applied at init time, NotifyShims(ATTACH) is mapped to that */
    pNotifyShims(SHIM_NOTIFY_ATTACH, NULL);
    result = Params->Flags;
    return result;
}

START_TEST(procparamflags)
{
    PRTL_USER_PROCESS_PARAMETERS Params = NtCurrentPeb()->ProcessParameters;
    ULONG saved = Params->Flags, base = saved | RTL_USER_PROCESS_PARAMETERS_NORMALIZED, res;
    HMODULE module;

    if (!LoadShimDLL(L"acgenral.dll", &module, &pGetHookAPIs) || !pGetHookAPIs)
        return;
    pNotifyShims = (tNOTIFYSHIMS)GetProcAddress(module, "NotifyShims");
    if (!pNotifyShims)
    {
        skip("No NotifyShims found\n");
        return;
    }

    /* An allowed bit is applied, existing bits are kept */
    res = run_shim("0x80", base);
    ok(res == (base | 0x80), "Expected 0x%lx, got 0x%lx\n", base | 0x80, res);
    /* NotifyShims re-notifies all shims used so far on some builds, so the later cases only look at the bits
       that may never change */

    /* Decimal is understood too */
    res = run_shim("256", base);
    ok((res & 0x100) == 0x100, "Expected 0x100 to be set, got 0x%lx\n", res);

    /* Disallowed bits are dropped: layout (NORMALIZED), loader DLL path, NX, and high garbage */
    res = run_shim("0x1", 0);
    ok(res == 0, "NORMALIZED must not be set from the command line, got 0x%lx\n", res);
    res = run_shim("0x3000", base);
    ok((res & ~ALLOWED) == (base & ~ALLOWED), "DLL path bits must not be set, got 0x%lx\n", res);
    res = run_shim("0xFFFFFFFF", 0);
    ok((res & ~ALLOWED) == 0, "Only allowed bits may be set, got 0x%lx\n", res);
    ok((res & ALLOWED) == ALLOWED, "Allowed bits should have been set, got 0x%lx\n", res);
    res = run_shim("0x80000000", base);
    ok((res & ~ALLOWED) == (base & ~ALLOWED), "High bit must not be set, got 0x%lx\n", res);

    /* No / invalid command line changes nothing */
    res = run_shim("", base);
    ok((res & ~ALLOWED) == (base & ~ALLOWED), "Unexpected flags 0x%lx\n", res);
    res = run_shim("garbage", base);
    ok((res & ~ALLOWED) == (base & ~ALLOWED), "Unexpected flags 0x%lx\n", res);

    Params->Flags = saved;
}
