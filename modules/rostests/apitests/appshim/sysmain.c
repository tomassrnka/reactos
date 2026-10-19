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

static DWORD run_probe(PCWSTR Name)
{
    WCHAR Path[MAX_PATH], Command[MAX_PATH + 4];
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process;
    DWORD Length, Code = 0xdeadbeef;
    PWCHAR Slash;

    Length = GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    if (!Length || Length >= ARRAYSIZE(Path))
        return Code;
    Slash = wcsrchr(Path, L'\\');
    if (!Slash || (size_t)(Slash + 1 - Path) + wcslen(Name) >= ARRAYSIZE(Path))
        return Code;
    wcscpy(Slash + 1, Name);
    swprintf(Command, ARRAYSIZE(Command), L"\"%ls\"", Path);

    if (!CreateProcessW(Path, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Process))
    {
        ok(0, "CreateProcess(%ls) failed: %lu\n", Path, GetLastError());
        return Code;
    }
    if (WaitForSingleObject(Process.hProcess, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "%ls did not exit\n", Name);
        TerminateProcess(Process.hProcess, 1);
        WaitForSingleObject(Process.hProcess, 10000);
    }
    else
        GetExitCodeProcess(Process.hProcess, &Code);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
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
}
