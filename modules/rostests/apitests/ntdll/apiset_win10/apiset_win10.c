/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A program that runs with the API set schema of Windows 10 (the manifest says so) and checks that
 *              every contract that ApiSetQueryApiSetPresenceEx reports can be loaded by that name
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The exit code is the number of contracts that are reported but not loadable. It is started by the test
 * ApiSetQueryApiSetPresence of ntdll_apitest.
 */

#include <stdio.h>
#include <wchar.h>
#include <windows.h>

typedef struct
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} NAME_STRING;

typedef LONG (WINAPI *PFN_QueryEx)(const NAME_STRING *, BOOLEAN *, BOOLEAN *);

int main(void)
{
    static const WCHAR *const Names[] =
    {
        L"api-ms-win-core-console-l1-1-0",
        L"api-ms-win-core-synch-l1-1-0",
        L"api-ms-win-core-synch-l1-2-0",
        L"api-ms-win-core-synch-l1-2-1",
        L"api-ms-win-core-processthreads-l1-1-0",
        L"api-ms-win-core-processthreads-l1-1-1",
        L"api-ms-win-core-processthreads-l1-1-2",
        L"api-ms-win-core-file-l1-1-0",
        L"api-ms-win-core-file-l1-2-0",
        L"api-ms-win-core-file-l1-2-1",
        L"api-ms-win-core-libraryloader-l1-1-0",
        L"api-ms-win-core-libraryloader-l1-2-0",
        L"api-ms-win-security-base-l1-1-0",
        L"api-ms-win-security-base-l1-2-0",
        L"api-ms-win-eventing-provider-l1-1-0",
        L"ext-ms-win-gdi-draw-l1-1-3",
        L"api-ms-win-core-synch-l1-3-0",
        L"api-ms-win-nonexistent-l1-1-0",
    };
    PFN_QueryEx pQueryEx = (PFN_QueryEx)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "ApiSetQueryApiSetPresenceEx");
    int Failures = 0;
    ULONG i;

    if (!pQueryEx)
    {
        printf("apiset_win10: ApiSetQueryApiSetPresenceEx is not exported\n");
        return 0;
    }

    for (i = 0; i < sizeof(Names) / sizeof(Names[0]); i++)
    {
        WCHAR File[160];
        NAME_STRING Name;
        BOOLEAN InSchema = 0xff, Present = 0xff;
        HMODULE Module;
        LONG Status;

        Name.Buffer = (PWSTR)Names[i];
        Name.Length = (USHORT)(wcslen(Names[i]) * sizeof(WCHAR));
        Name.MaximumLength = Name.Length + sizeof(WCHAR);
        Status = pQueryEx(&Name, &InSchema, &Present);
        if (Status != 0 || !Present)
            continue;

        swprintf(File, 160, L"%ls.dll", Names[i]);
        Module = LoadLibraryW(File);
        if (!Module)
        {
            printf("apiset_win10: %ls is reported as present but cannot be loaded (error %lu)\n", Names[i], GetLastError());
            Failures++;
        }
        else
        {
            FreeLibrary(Module);
        }
    }

    return Failures;
}
