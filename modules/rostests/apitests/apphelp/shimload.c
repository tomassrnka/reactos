/*
 * PROJECT:     apphelp_apitest
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     A program the shim engine loads a shim library into has to start
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <shlwapi.h>
#include <strsafe.h>

#include "wine/test.h"

#define CHILD_LOADED_SHIM       37
#define CHILD_NO_SHIM           38
#define CHILD_TIMEOUT           30000

/*
 * sysmain.sdb applies a shim to every program that has an msys-2.0.dll beside it (amd64 only). The
 * shim engine loads the shim library while the program's own libraries still wait for their
 * initialization, so a shim library whose imports initialize first (uxtheme and what it pulls in)
 * runs libraries that need the C runtime before the C runtime is initialized: the program hangs.
 * The child is a helper that imports the C runtime and nothing that shlwapi would add.
 */
static BOOL make_work_dir(PWSTR Dir, SIZE_T cchDir)
{
    WCHAR TempDir[MAX_PATH];
    DWORD Len, Try;

    Len = GetTempPathW(_countof(TempDir), TempDir);
    ok(Len > 0 && Len < _countof(TempDir), "GetTempPath returned %lu, error %lu\n", Len, GetLastError());
    if (Len == 0 || Len >= _countof(TempDir))
        return FALSE;

    for (Try = 0; Try < 100; Try++)
    {
        if (FAILED(StringCchPrintfW(Dir, cchDir, L"%sshimload_%lx_%lx", TempDir, GetCurrentProcessId(), Try)))
            break;
        if (CreateDirectoryW(Dir, NULL))
            return TRUE;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    ok(0, "Could not create a work directory below %ls, error %lu\n", TempDir, GetLastError());
    return FALSE;
}

static void delete_file_retry(PCWSTR Path)
{
    DWORD Try;

    for (Try = 0; Try < 50; Try++)
    {
        if (DeleteFileW(Path) || GetLastError() == ERROR_FILE_NOT_FOUND)
            return;
        Sleep(100);
    }
    ok(0, "Could not delete %ls, error %lu\n", Path, GetLastError());
}

static void run_child(PCWSTR Exe, PCWSTR Dir, DWORD ExpectedCode)
{
    WCHAR CmdLine[MAX_PATH + 32];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD Wait, ExitCode = 0;
    BOOL Ret;

    ok(SUCCEEDED(StringCchPrintfW(CmdLine, _countof(CmdLine), L"\"%ls\"", Exe)), "Path too long\n");
    Ret = CreateProcessW(NULL, CmdLine, NULL, NULL, FALSE, 0, NULL, Dir, &si, &pi);
    ok(Ret, "CreateProcess failed with %lu\n", GetLastError());
    if (!Ret)
        return;

    Wait = WaitForSingleObject(pi.hProcess, CHILD_TIMEOUT);
    ok(Wait == WAIT_OBJECT_0, "Wait returned %lu: the program did not exit in %u ms\n", Wait, CHILD_TIMEOUT);
    if (Wait != WAIT_OBJECT_0)
    {
        ok(TerminateProcess(pi.hProcess, 1), "TerminateProcess failed with %lu\n", GetLastError());
        Wait = WaitForSingleObject(pi.hProcess, 10000);
        ok(Wait == WAIT_OBJECT_0, "The program did not terminate, wait returned %lu\n", Wait);
    }
    else
    {
        Ret = GetExitCodeProcess(pi.hProcess, &ExitCode);
        ok(Ret, "GetExitCodeProcess failed with %lu\n", GetLastError());
        ok(ExitCode == ExpectedCode, "Exit code is %lu, expected %lu\n", ExitCode, ExpectedCode);
    }
    ok(CloseHandle(pi.hThread), "CloseHandle failed with %lu\n", GetLastError());
    ok(CloseHandle(pi.hProcess), "CloseHandle failed with %lu\n", GetLastError());
}

static BOOL make_paths(PCWSTR Dir, PWSTR Helper, PWSTR Source, PWSTR Exe, PWSTR Decoy)
{
    WCHAR Self[MAX_PATH], SystemDir[MAX_PATH];
    DWORD Len;
    PWSTR Name;

    Len = GetModuleFileNameW(NULL, Self, _countof(Self));
    ok(Len > 0 && Len < _countof(Self), "GetModuleFileName returned %lu, error %lu\n", Len, GetLastError());
    if (Len == 0 || Len >= _countof(Self))
        return FALSE;
    Name = StrRChrW(Self, NULL, L'\\');
    ok(Name != NULL, "No directory in %ls\n", Self);
    if (!Name)
        return FALSE;
    *Name = UNICODE_NULL;

    Len = GetSystemDirectoryW(SystemDir, _countof(SystemDir));
    ok(Len > 0 && Len < _countof(SystemDir), "GetSystemDirectory returned %lu, error %lu\n", Len, GetLastError());
    if (Len == 0 || Len >= _countof(SystemDir))
        return FALSE;

    if (FAILED(StringCchPrintfW(Helper, MAX_PATH, L"%ls\\apphelp_shimload_child.exe", Self)) ||
        FAILED(StringCchPrintfW(Source, MAX_PATH, L"%ls\\lz32.dll", SystemDir)) ||
        FAILED(StringCchPrintfW(Exe, MAX_PATH, L"%ls\\shimload_child.exe", Dir)) ||
        FAILED(StringCchPrintfW(Decoy, MAX_PATH, L"%ls\\msys-2.0.dll", Dir)))
    {
        ok(0, "A path is too long\n");
        return FALSE;
    }
    return TRUE;
}

static void test_msys_decoy_program(void)
{
    WCHAR Dir[MAX_PATH], Helper[MAX_PATH], Source[MAX_PATH], Exe[MAX_PATH], Decoy[MAX_PATH];
    BOOL CopiedExe = FALSE, CopiedDecoy = FALSE;

    if (!make_work_dir(Dir, _countof(Dir)))
        return;

    if (make_paths(Dir, Helper, Source, Exe, Decoy))
    {
        CopiedExe = CopyFileW(Helper, Exe, FALSE);
        ok(CopiedExe, "CopyFile %ls failed with %lu\n", Helper, GetLastError());
        if (CopiedExe)
        {
            CopiedDecoy = CopyFileW(Source, Decoy, FALSE);
            ok(CopiedDecoy, "CopyFile %ls failed with %lu\n", Source, GetLastError());
        }

        if (CopiedDecoy)
        {
#ifdef _M_AMD64
            run_child(Exe, Dir, CHILD_LOADED_SHIM);
#else
            run_child(Exe, Dir, CHILD_NO_SHIM);
#endif
        }

        if (CopiedDecoy)
            delete_file_retry(Decoy);
        if (CopiedExe)
            delete_file_retry(Exe);
    }

    ok(RemoveDirectoryW(Dir), "RemoveDirectory %ls failed with %lu\n", Dir, GetLastError());
}

START_TEST(shimload)
{
    if (!is_reactos())
    {
        skip("The msys-2.0.dll shim is a ReactOS shim\n");
        return;
    }

    test_msys_decoy_program();
}
