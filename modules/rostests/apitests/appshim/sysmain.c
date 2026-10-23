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

/* The database matches a program by its file name as well: run a copy of the probe that is named firefox.exe */
static DWORD run_as_firefox(PCWSTR Name)
{
    WCHAR Source[MAX_PATH], Dir[MAX_PATH], Target[MAX_PATH];
    DWORD Code = 0xdeadbeef;

    if (!sibling_path(Name, Source, ARRAYSIZE(Source)) || !GetTempPathW(ARRAYSIZE(Dir), Dir))
        return Code;
    if (wcslen(Dir) + 32 >= ARRAYSIZE(Dir))
        return Code;
    swprintf(Dir + wcslen(Dir), 32, L"appshim_ff%lu", GetCurrentProcessId());
    swprintf(Target, ARRAYSIZE(Target), L"%ls\\firefox.exe", Dir);

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
    if (is_reactos())
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
        Code = run_as_firefox(L"appshim_sysmain_firefox158.exe");
        ok(Code == PROBE_ALL, "Firefox 158 sees 0x%lx of the checks, expected 0x%x\n", Code, PROBE_ALL);
        Code = run_as_firefox(L"appshim_sysmain_firefox115.exe");
        ok(Code == 0, "Firefox 115 sees 0x%lx of the checks, expected none\n", Code);
    }
    else
        skip("The real version shows through on Windows\n");
}
