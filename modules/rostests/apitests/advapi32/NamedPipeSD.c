/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Contract tests for CreateNamedPipeW with a security descriptor
 *              (T90: a mandatory-label SACL needs no SeSecurityPrivilege;
 *               an audit SACL still does)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <sddl.h>

#ifndef SE_SECURITY_PRIVILEGE
#define SE_SECURITY_PRIVILEGE (8L)
#endif

static int g_Uid = 0;

static
void
CheckPipe(PCWSTR Label, PCWSTR Sddl, BOOL ExpectOk, DWORD ExpectErr)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    SECURITY_ATTRIBUTES Sa, *pSa = NULL;
    WCHAR Name[128];
    HANDLE h;
    DWORD Err;

    if (Sddl != NULL)
    {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &Sd, NULL))
        {
            ok(0, "[%ls] ConvertStringSD failed %lu\n", Label, GetLastError());
            return;
        }
        ZeroMemory(&Sa, sizeof(Sa));
        Sa.nLength = sizeof(Sa);
        Sa.lpSecurityDescriptor = Sd;
        Sa.bInheritHandle = TRUE;
        pSa = &Sa;
    }

    StringCchPrintfW(Name, 128, L"\\\\.\\pipe\\t90_%lu_%d", GetCurrentProcessId(), g_Uid++);
    SetLastError(0xDEADBEEF);
    h = CreateNamedPipeW(Name,
                         PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                         255, 512, 512, 0, pSa);
    Err = GetLastError();

    if (ExpectOk)
    {
        ok(h != INVALID_HANDLE_VALUE && h != NULL,
           "[%ls] CreateNamedPipeW failed, err=%lu\n", Label, Err);
    }
    else
    {
        ok(h == INVALID_HANDLE_VALUE || h == NULL,
           "[%ls] CreateNamedPipeW unexpectedly succeeded\n", Label);
        ok(Err == ExpectErr, "[%ls] err = %lu (expected %lu)\n", Label, Err, ExpectErr);
    }

    if (h != INVALID_HANDLE_VALUE && h != NULL)
        CloseHandle(h);
    if (Sd != NULL)
        LocalFree(Sd);
}

START_TEST(NamedPipeSD)
{
    BOOLEAN WasEnabled;
    NTSTATUS Status;

    /* Make the audit-ACE negative case deterministic: drop SeSecurityPrivilege */
    RtlAdjustPrivilege(SE_SECURITY_PRIVILEGE, FALSE, FALSE, &WasEnabled);

    /* No security attributes: always allowed */
    CheckPipe(L"NULL-SA", NULL, TRUE, 0);

    /* DACL only (no SACL): always allowed */
    CheckPipe(L"DACL-only", L"D:(A;;GA;;;WD)", TRUE, 0);

    /* SACL with a mandatory label only: allowed WITHOUT SeSecurityPrivilege (T90) */
    CheckPipe(L"Label-only", L"S:(ML;;;;;S-1-16-0)", TRUE, 0);

    /* Crashpad-shaped descriptor (DACL + mandatory label): allowed */
    CheckPipe(L"Crashpad", L"D:(A;;GA;;;SY)(A;;GWGR;;;S-1-15-2-1)S:(ML;;;;;S-1-16-0)", TRUE, 0);

    /* SACL with an audit ACE: STILL needs SeSecurityPrivilege -> denied here */
    CheckPipe(L"Audit-ACE", L"S:(AU;;FA;;;WD)", FALSE, ERROR_PRIVILEGE_NOT_HELD);

    /* With the privilege, the same audit SACL is accepted: proves the gate is real */
    Status = RtlAdjustPrivilege(SE_SECURITY_PRIVILEGE, TRUE, FALSE, &WasEnabled);
    if (NT_SUCCESS(Status))
        CheckPipe(L"Audit-privileged", L"S:(AU;;FA;;;WD)", TRUE, 0);
    else
        skip("SeSecurityPrivilege not available (Status 0x%lx)\n", Status);

    RtlAdjustPrivilege(SE_SECURITY_PRIVILEGE, FALSE, FALSE, &WasEnabled);
}
