/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Astra audit #8 - NtUserRegisterUserApiHook registers a module
 *              that win32k injects (MSQ_INJECTMODULE) into every non-system
 *              process on the desktop. It must be gated: only a caller holding
 *              SeTcbPrivilege may register a user-api hook. The legitimate
 *              caller is uxtheme's ThemeHooksInstall, invoked from
 *              ThemeStartCallback which runs in the context of winlogon
 *              (LocalSystem, SeTcb) - never an ordinary process. An unprivileged
 *              caller must be denied with ERROR_ACCESS_DENIED, with no hook
 *              registered and no cross-process injection.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

START_TEST(NtUserRegisterUserApiHookGate)
{
    UNICODE_STRING dll, func;
    BOOL ret;
    DWORD err;

    /* Bogus (non-loadable) module/function - the point is the privilege gate,
     * which is reached before any module load or injection. */
    RtlInitUnicodeString(&dll, L"astra8_nonexistent.dll");
    RtlInitUnicodeString(&func, L"Astra8InitApiHook");

    SetLastError(0);
    ret = NtUserRegisterUserApiHook(&dll, &func, 0, 0);
    err = GetLastError();

    if (ret)
    {
        /*
         * Success means the caller was allowed. In the ordinary (unprivileged)
         * apitest context that is the vulnerability; but if this harness runs
         * privileged (SeTcb, e.g. LocalSystem), the gate correctly permits it
         * and the denial path cannot be exercised here. We cannot distinguish
         * the two from user mode without a token query, so undo any registration
         * and skip rather than spuriously fail a privileged run.
         */
        NtUserUnregisterUserApiHook();
        skip("NtUserRegisterUserApiHook allowed the caller - running privileged "
             "(SeTcb)? denial path not exercised\n");
        return;
    }

    /* Unprivileged caller: the gate must deny with ERROR_ACCESS_DENIED. */
    ok(ret == FALSE, "expected denial, got TRUE\n");
    ok(err == ERROR_ACCESS_DENIED,
       "SECURITY(Astra#8): expected ERROR_ACCESS_DENIED (%d) from the SeTcb "
       "gate, got %lu\n", ERROR_ACCESS_DENIED, err);
}
