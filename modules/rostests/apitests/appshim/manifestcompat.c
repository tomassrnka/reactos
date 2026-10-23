/*
 * PROJECT:     appshim_apitest
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     A process that declares a Windows version in its manifest sees that
 *              version from GetVersion()/RtlGetVersion(); one that does not keeps the
 *              real version. Guards the per-app manifest version lie (T94).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <stdio.h>
#include "wine/test.h"

#define VER_5_2 0x05020502  /* GetVersion 5.2 / RtlGetVersion 5.2 (real ReactOS base) */
#define VER_6_0 0x06000600  /* GetVersion 6.0 / RtlGetVersion 6.0 (Vista, from manifest) */

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

static DWORD run_probe(PCWSTR Name)
{
    WCHAR Path[MAX_PATH];
    DWORD Length;
    PWCHAR Slash;

    Length = GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    if (!Length || Length >= ARRAYSIZE(Path))
        return 0xdeadbeef;
    Slash = wcsrchr(Path, L'\\');
    if (!Slash || (size_t)(Slash + 1 - Path) + wcslen(Name) >= ARRAYSIZE(Path))
        return 0xdeadbeef;
    wcscpy(Slash + 1, Name);
    return run_path(Path);
}

START_TEST(manifestcompat)
{
    DWORD None, Vista;

    None = run_probe(L"appshim_manifestver_none.exe");
    Vista = run_probe(L"appshim_manifestver_vista.exe");
    trace("none=0x%08lx vista=0x%08lx\n", None, Vista);

    if (is_reactos())
    {
        /* Negative control: no manifest compatibility GUID -> the real NT 5.2 shows
         * through BOTH GetVersion() and RtlGetVersion(). Proves the fix is per-app
         * and did NOT globally bump the OS version. */
        ok(None == VER_5_2, "no-manifest process reported 0x%08lx, expected 0x%08lx (5.2/5.2)\n",
           None, (DWORD)VER_5_2);
        /* Positive control: a Vista supportedOS manifest -> GetVersion() and
         * RtlGetVersion() both report 6.0, so Crashpad-style GetVersion()>=6 gates pass. */
        ok(Vista == VER_6_0, "vista-manifest process reported 0x%08lx, expected 0x%08lx (6.0/6.0)\n",
           Vista, (DWORD)VER_6_0);
        /* And the manifest must raise, never lower, the reported version. */
        ok(Vista > None, "vista-manifest (0x%08lx) must report a higher version than none (0x%08lx)\n",
           Vista, None);
    }
    else
    {
        skip("Windows reports its own version through the manifest; ReactOS-specific contract\n");
    }
}
