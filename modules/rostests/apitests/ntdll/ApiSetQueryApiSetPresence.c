/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ntdll!ApiSetQueryApiSetPresence and ApiSetQueryApiSetPresenceEx (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64), and the first table is the one of Wine's test for the
 * two functions. The first function answers for the family of a contract, whatever the extension and the minor
 * version are; the second one wants a name without an extension and knows the contract by its name or by a newer
 * revision of its family. Windows 8 answers FALSE to the first one for a revision it does not list and for a name
 * with an unknown extension, as Wine's test allows.
 *
 * On ReactOS the answers follow what its loader can resolve, which depends on the API set schema that the program
 * runs with (the manifest chooses it), so the expectations there are taken from the loader: a contract is present
 * when LoadLibrary finds it by that name, a family when one of its revisions is. The table above is the one of
 * Windows. A second program with a manifest for Windows 10 checks that nothing is reported that cannot be loaded.
 */

#include "precomp.h"

typedef NTSTATUS (WINAPI *PFN_Query)(const UNICODE_STRING *, BOOLEAN *);
typedef NTSTATUS (WINAPI *PFN_QueryEx)(const UNICODE_STRING *, BOOLEAN *, BOOLEAN *);

/* Does the loader of this program find the contract by that name (a name without an extension)? */
static BOOLEAN LoaderResolves(PCWSTR Name)
{
    WCHAR File[160];
    HMODULE Module;

    if (wcslen(Name) > 100)
        return FALSE;
    swprintf(File, RTL_NUMBER_OF(File), L"%ls.dll", Name);
    Module = LoadLibraryW(File);
    if (Module)
        FreeLibrary(Module);
    return Module != NULL;
}

/* Is a revision of the family of this contract (the name without its extension and its minor version) loadable? */
static BOOLEAN FamilyLoads(PCWSTR Name)
{
    WCHAR Buffer[160];
    SIZE_T Chars = wcslen(Name), Family;
    PCWSTR Dot = wcsrchr(Name, L'.');
    ULONG Minor;

    if (Dot)
        Chars = Dot - Name;
    if (Chars >= RTL_NUMBER_OF(Buffer) - 4)
        return FALSE;
    for (Family = Chars; Family > 0 && Name[Family - 1] != L'-'; Family--)
        ;
    if (Family == 0 || Family == Chars)
        return FALSE;
    for (Minor = 0; Minor <= 15; Minor++)
    {
        wcsncpy(Buffer, Name, Family);
        swprintf(Buffer + Family, RTL_NUMBER_OF(Buffer) - Family, L"%lu", Minor);
        if (LoaderResolves(Buffer))
            return TRUE;
    }
    return FALSE;
}

/* The program with the manifest of Windows 10 reports the contracts that it was asked about and cannot load */
static void TestWindows10Schema(void)
{
    WCHAR Path[MAX_PATH];
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process;
    DWORD Exit = 0xdead;
    PWSTR Slash;

    GetModuleFileNameW(NULL, Path, RTL_NUMBER_OF(Path));
    Slash = wcsrchr(Path, L'\\');
    if (!Slash)
        return;
    wcscpy(Slash + 1, L"apiset_win10.exe");
    if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES)
    {
        skip("%ls is not here\n", Path);
        return;
    }
    if (!CreateProcessW(Path, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Process))
    {
        skip("%ls did not start (%lu)\n", Path, GetLastError());
        return;
    }
    ok(WaitForSingleObject(Process.hProcess, 60000) == WAIT_OBJECT_0, "the program did not end\n");
    GetExitCodeProcess(Process.hProcess, &Exit);
    ok(Exit == 0, "%lu contracts are reported as present in the schema of Windows 10 and cannot be loaded\n", Exit);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
}

START_TEST(ApiSetQueryApiSetPresence)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_Query pQuery = (PFN_Query)GetProcAddress(Ntdll, "ApiSetQueryApiSetPresence");
    PFN_QueryEx pQueryEx = (PFN_QueryEx)GetProcAddress(Ntdll, "ApiSetQueryApiSetPresenceEx");
    static const struct
    {
        PCWSTR Name;
        BOOLEAN Present;
        NTSTATUS ExStatus;
        BOOLEAN ExPresent;
        BOOLEAN FalseOnWindows8;
    } Cases[] =
    {
        { L"api-ms-win-core-console-l1-1-0",      TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-core-console-l1-1-0.dll",  TRUE,  STATUS_INVALID_PARAMETER, FALSE },
        { L"api-ms-win-core-console-l1-1-9",      TRUE,  STATUS_SUCCESS,           FALSE, TRUE },
        { L"api-ms-win-core-console-l1-1-9.dll",  TRUE,  STATUS_INVALID_PARAMETER, FALSE, TRUE },
        { L"api-ms-win-core-console-l1-1",        FALSE, STATUS_SUCCESS,           FALSE },
        { L"api-ms-win-core-console-l1-1-0.fake", TRUE,  STATUS_INVALID_PARAMETER, FALSE, TRUE },
        { L"api-ms-win-foo-bar-l1-1-0",           FALSE, STATUS_SUCCESS,           FALSE },
        { L"api-ms-win-foo-bar-l1-1-0.dll",       FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"ext-ms-win-gdi-draw-l1-1-3",          TRUE,  STATUS_SUCCESS,           TRUE },
        { L"ext-ms-win-gdi-draw-l1-1-3.dll",      TRUE,  STATUS_INVALID_PARAMETER, FALSE },
        { L"foo",                                 FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"foo.dll",                             FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"",                                    FALSE, STATUS_INVALID_PARAMETER, FALSE },
        { L"api-ms-win-core-synch-l1-2-0",        TRUE,  STATUS_SUCCESS,           TRUE },
        { L"API-MS-WIN-CORE-SYNCH-L1-2-0",        TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-core-synch-l1-3-0",        FALSE, STATUS_SUCCESS,           FALSE },
        { L"api-ms-win-security-base-l1-1-0",     TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-eventing-provider-l1-1-0", TRUE,  STATUS_SUCCESS,           TRUE },
        { L"api-ms-win-core-eventing-provider-l1-1-0", FALSE, STATUS_SUCCESS,      FALSE },
        { L"kernel32",                            FALSE, STATUS_INVALID_PARAMETER, FALSE },
    };
    ULONG i;

    /* Windows 7 and older have none: they came with Windows 8; ReactOS has it from export level 0x601 */
    if (!pQuery)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "ApiSetQueryApiSetPresence is not exported\n");
        skip("ApiSetQueryApiSetPresence is not exported\n");
        return;
    }

    ok(pQueryEx != NULL || !(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
       "ApiSetQueryApiSetPresenceEx is not exported\n");

    for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
    {
        UNICODE_STRING Name;
        BOOLEAN Present = 0xff, InSchema = 0xff;
        NTSTATUS Status;

        BOOLEAN WantPresent = Cases[i].Present, WantExPresent = Cases[i].ExPresent;

        /* what the loader of this program resolves is what a contract is: the schema of the program decides it */
        if (is_reactos())
        {
            WantPresent = FamilyLoads(Cases[i].Name);
            WantExPresent = Cases[i].ExStatus == STATUS_SUCCESS ? LoaderResolves(Cases[i].Name) : FALSE;
        }

        RtlInitUnicodeString(&Name, Cases[i].Name);
        Status = pQuery(&Name, &Present);
        ok(Status == STATUS_SUCCESS, "%ls: status %#lx\n", Cases[i].Name, Status);
        ok(Present == WantPresent || (!is_reactos() && Cases[i].FalseOnWindows8 && !Present),
           "%ls: present %u, expected %u\n", Cases[i].Name, Present, WantPresent);

        if (!pQueryEx)
            continue;
        Present = InSchema = 0xff;
        Status = pQueryEx(&Name, &InSchema, &Present);
        ok(Status == Cases[i].ExStatus, "%ls: Ex status %#lx, expected %#lx\n", Cases[i].Name, Status, Cases[i].ExStatus);
        if (Status == STATUS_SUCCESS)
        {
            ok(InSchema == WantExPresent, "%ls: in schema %u, expected %u\n", Cases[i].Name, InSchema, WantExPresent);
            ok(Present == WantExPresent, "%ls: Ex present %u, expected %u\n", Cases[i].Name, Present, WantExPresent);
        }
        else
        {
            /* a refused name leaves the answers alone */
            ok(InSchema == 0xff && Present == 0xff, "%ls: the answers became %u and %u\n", Cases[i].Name, InSchema, Present);
        }
    }

    if (is_reactos() && pQueryEx)
    {
        UNICODE_STRING Anchor;
        BOOLEAN InSchema = 0xff, Present = 0xff;

        /* a contract that every schema of the table has by this exact name is present whatever the program's schema */
        RtlInitUnicodeString(&Anchor, L"api-ms-win-eventing-provider-l1-1-0");
        ok(pQueryEx(&Anchor, &InSchema, &Present) == STATUS_SUCCESS && Present, "the eventing provider contract is not present\n");
        TestWindows10Schema();
    }
}
