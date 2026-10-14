/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The Universal CRT contracts resolve at every compatibility level
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#include <apitest.h>
#include "apisetsp.h"

static const WCHAR *g_Contracts[] =
{
    L"api-ms-win-crt-conio-l1-1-0.dll",
    L"api-ms-win-crt-convert-l1-1-0.dll",
    L"api-ms-win-crt-environment-l1-1-0.dll",
    L"api-ms-win-crt-filesystem-l1-1-0.dll",
    L"api-ms-win-crt-heap-l1-1-0.dll",
    L"api-ms-win-crt-locale-l1-1-0.dll",
    L"api-ms-win-crt-math-l1-1-0.dll",
    L"api-ms-win-crt-multibyte-l1-1-0.dll",
    L"api-ms-win-crt-private-l1-1-0.dll",
    L"api-ms-win-crt-process-l1-1-0.dll",
    L"api-ms-win-crt-runtime-l1-1-0.dll",
    L"api-ms-win-crt-stdio-l1-1-0.dll",
    L"api-ms-win-crt-string-l1-1-0.dll",
    L"api-ms-win-crt-time-l1-1-0.dll",
    L"api-ms-win-crt-utility-l1-1-0.dll",
};

static const struct
{
    const char *Name;
    DWORD Version;
} g_Versions[] =
{
    { "Windows 7", APISET_WIN7 },
    { "Windows 8", APISET_WIN8 },
    { "Windows 8.1", APISET_WIN81 },
    { "Windows 10", APISET_WIN10 },
};

START_TEST(ucrt)
{
    unsigned i, j;

    for (i = 0; i < _countof(g_Versions); i++)
    {
        for (j = 0; j < _countof(g_Contracts); j++)
        {
            UNICODE_STRING Name;
            UNICODE_STRING Result = {0};
            BOOLEAN Resolved = FALSE;
            NTSTATUS Status;

            RtlInitUnicodeString(&Name, g_Contracts[j]);
            Status = ApiSetResolveToHost(g_Versions[i].Version, &Name, &Resolved, &Result);
            ok(NT_SUCCESS(Status), "%s: %S: status %lx\n", g_Versions[i].Name, g_Contracts[j], Status);
            ok(Resolved, "%s: %S does not resolve\n", g_Versions[i].Name, g_Contracts[j]);
            if (Resolved)
            {
                ok(Result.Buffer && !_wcsicmp(Result.Buffer, L"ucrtbase.dll"), "%s: %S resolves to %S\n",
                   g_Versions[i].Name, g_Contracts[j], Result.Buffer ? Result.Buffer : L"(null)");
            }
        }
    }
}
