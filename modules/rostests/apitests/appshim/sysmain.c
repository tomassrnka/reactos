/*
 * PROJECT:     appshim_apitest
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A program that the system database matches by file attributes sees the version its entry names
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "wine/test.h"
#include "sysmain_probe.h"

/* Windows 10 1809 or later: the probe's manifest names Windows 10, so the real version shows through there */
static BOOL real_windows_is_1809(void)
{
    typedef LONG (WINAPI *RTLGETVERSIONPROC)(OSVERSIONINFOW *);
    RTLGETVERSIONPROC pRtlGetVersion = (RTLGETVERSIONPROC)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW Info = { sizeof(Info) };

    return pRtlGetVersion && pRtlGetVersion(&Info) == 0 && (Info.dwMajorVersion > 10 || (Info.dwMajorVersion == 10 && Info.dwBuildNumber >= PROBE_BUILD));
}

static DWORD run_path(PCWSTR Path)
{
    WCHAR Command[MAX_PATH + 4];
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process;
    DWORD Code = 0xdeadbeef;

    swprintf(Command, ARRAYSIZE(Command), L"\"%ls\"", Path);

    if (!CreateProcessW(Path, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Process))
    {
        ok(0, "CreateProcess(%ls) failed: %lu\n", Path, GetLastError());
        return Code;
    }
    if (WaitForSingleObject(Process.hProcess, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "%ls did not exit\n", Path);
        TerminateProcess(Process.hProcess, 1);
        WaitForSingleObject(Process.hProcess, 10000);
    }
    else
        GetExitCodeProcess(Process.hProcess, &Code);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    return Code;
}

/* The path of a file next to this program */
static BOOL sibling_path(PCWSTR Name, PWSTR Path, SIZE_T Count)
{
    DWORD Length;
    PWCHAR Slash;

    Length = GetModuleFileNameW(NULL, Path, Count);
    if (!Length || Length >= Count)
        return FALSE;
    Slash = wcsrchr(Path, L'\\');
    if (!Slash || (size_t)(Slash + 1 - Path) + wcslen(Name) >= Count)
        return FALSE;
    wcscpy(Slash + 1, Name);
    return TRUE;
}

static DWORD run_probe(PCWSTR Name)
{
    WCHAR Path[MAX_PATH];

    if (!sibling_path(Name, Path, ARRAYSIZE(Path)))
        return 0xdeadbeef;
    return run_path(Path);
}

/* The database matches a program by its file name as well: run a copy of the probe that has the name of the program */
static DWORD run_as_name(PCWSTR Name, PCWSTR AsName)
{
    WCHAR Source[MAX_PATH], Dir[MAX_PATH], Target[MAX_PATH];
    DWORD Code = 0xdeadbeef;

    if (!sibling_path(Name, Source, ARRAYSIZE(Source)) || !GetTempPathW(ARRAYSIZE(Dir), Dir))
        return Code;
    if (wcslen(Dir) + 32 >= ARRAYSIZE(Dir))
        return Code;
    swprintf(Dir + wcslen(Dir), 32, L"appshim_as%lu", GetCurrentProcessId());
    swprintf(Target, ARRAYSIZE(Target), L"%ls\\%ls", Dir, AsName);

    CreateDirectoryW(Dir, NULL);
    if (!CopyFileW(Source, Target, FALSE))
    {
        ok(0, "CopyFile(%ls) failed: %lu\n", Target, GetLastError());
        RemoveDirectoryW(Dir);
        return Code;
    }
    Code = run_path(Target);
    DeleteFileW(Target);
    RemoveDirectoryW(Dir);
    return Code;
}

START_TEST(sysmain)
{
    DWORD Code;

    /* The probe carries the file attributes of Sysinternals DebugView 5.02 (company, product, version) */
    Code = run_probe(L"appshim_sysmain_probe.exe");
    if (is_reactos() || real_windows_is_1809())
        ok(Code == PROBE_ALL, "A program with the DebugView attributes sees 0x%lx of the checks, expected 0x%x\n", Code, PROBE_ALL);
    else
        skip("The real version is older than Windows 10 1809\n");

    /* Same code and manifest, other product name: no entry matches, so the version stays the real one */
    Code = run_probe(L"appshim_sysmain_other.exe");
    if (is_reactos() && real_windows_is_1809())
        /* With the Windows 10 identity as the system default no entry is needed: the real version is the answer */
        ok(Code == PROBE_ALL, "A program with another product name sees 0x%lx of the checks, expected 0x%x (the real version)\n", Code, PROBE_ALL);
    else if (is_reactos())
        ok(Code == 0, "A program with another product name sees 0x%lx of the checks, expected none\n", Code);
    else
        skip("The real version shows through on Windows\n");

    /*
     * Firefox 116 and later need Windows 10, so the entry for them gives a program with these attributes (named
     * firefox.exe) the Windows 10 answer without any layer chosen; Firefox 115 and before get Windows 7 SP1, so they
     * must not see Windows 10
     */
    if (is_reactos())
    {
        Code = run_as_name(L"appshim_sysmain_firefox158.exe", L"firefox.exe");
        ok(Code == PROBE_ALL, "Firefox 158 sees 0x%lx of the checks, expected 0x%x\n", Code, PROBE_ALL);
        Code = run_as_name(L"appshim_sysmain_firefox115.exe", L"firefox.exe");
        ok(Code == 0, "Firefox 115 sees 0x%lx of the checks, expected none\n", Code);
    }
    else
        skip("The real version shows through on Windows\n");

    /*
     * The Microsoft .NET Framework 4.5 to 4.8 installers need Windows 7 SP1 to take their normal path (on the Windows 10
     * answer they treat the Framework as part of the system), so the entries for them give these programs the Windows 7
     * SP1 answer without any layer chosen: the outer installer, and the Setup.exe and SetupUtility.exe it starts (the
     * entries for these two name the file as well, so the probes run as copies with these names). Programs that only
     * look like them must keep the real answer: the Framework's own tools (same company and product), the same strings
     * from another company, the 4.0 installer, and the Setup.exe attributes under another file name.
     */
    if (is_reactos() && real_windows_is_1809())
    {
        Code = run_probe(L"appshim_sysmain_netfx48.exe");
        ok(Code == 0, "The .NET Framework 4.8 installer sees 0x%lx of the checks, expected none (Windows 7 SP1)\n", Code);
        Code = run_as_name(L"appshim_sysmain_netfx48_setup.exe", L"Setup.exe");
        ok(Code == 0, "The Setup.exe of the .NET Framework 4.8 installer sees 0x%lx of the checks, expected none\n", Code);
        Code = run_as_name(L"appshim_sysmain_netfx48_util.exe", L"SetupUtility.exe");
        ok(Code == 0, "The SetupUtility.exe of the .NET Framework 4.8 installer sees 0x%lx of the checks, expected none\n", Code);
        Code = run_probe(L"appshim_sysmain_netfx48_setup.exe");
        ok(Code == PROBE_ALL, "The Setup.exe attributes under another file name see 0x%lx of the checks, expected 0x%x (the real version)\n", Code, PROBE_ALL);
        Code = run_probe(L"appshim_sysmain_netfx40.exe");
        ok(Code == PROBE_ALL, "The .NET Framework 4.0 installer sees 0x%lx of the checks, expected 0x%x (the real version)\n", Code, PROBE_ALL);
        Code = run_probe(L"appshim_sysmain_netfx_tool.exe");
        ok(Code == PROBE_ALL, "csc.exe of the .NET Framework sees 0x%lx of the checks, expected 0x%x (the real version)\n", Code, PROBE_ALL);
        Code = run_probe(L"appshim_sysmain_netfx_other.exe");
        ok(Code == PROBE_ALL, "The installer strings with another company see 0x%lx of the checks, expected 0x%x (the real version)\n", Code, PROBE_ALL);
    }
    else
        skip("The .NET Framework installer entries are checked where the system answer is Windows 10\n");
}
