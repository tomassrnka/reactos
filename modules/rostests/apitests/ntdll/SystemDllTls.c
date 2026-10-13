/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     TLS directories of system DLLs and CRT thread key destructors
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <winuser.h>

/* System DLLs that have no TLS directory on Windows Server 2008 R2 and Windows 10 */
static const PCWSTR g_CoreDlls[] =
{
    L"advapi32.dll", L"comctl32.dll", L"dnsapi.dll", L"gdi32.dll", L"iphlpapi.dll",
    L"kernel32.dll", L"netapi32.dll", L"ntdll.dll", L"rpcrt4.dll", L"secur32.dll",
    L"setupapi.dll", L"shlwapi.dll", L"user32.dll", L"userenv.dll", L"uxtheme.dll",
    L"version.dll", L"winhttp.dll", L"ws2_32.dll",
};

/* ReactOS system DLLs that have thread-local data or TLS callbacks of their own */
static const PCWSTR g_TlsAllowed[] =
{
    L"ucrtbase.dll", /* vcruntime frees its per-thread data from a TLS callback */
};

static const PCWSTR g_Patterns[] =
{
    L"*.dll", L"*.acm", L"*.ax", L"*.cpl", L"*.drv", L"*.ime", L"*.ocx", L"*.tsp",
};

/* Returns -1 if the file cannot be read as a PE image, else 1 with a TLS directory, 0 without */
static int FileHasTlsDirectory(PCWSTR Path)
{
    HANDLE hFile, hMapping;
    PVOID Base;
    ULONG Size = 0;
    int Result = -1;

    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return -1;
    hMapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hFile);
    if (!hMapping)
        return -1;
    Base = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMapping);
    if (!Base)
        return -1;
    if (RtlImageNtHeader(Base))
        Result = RtlImageDirectoryEntryToData(Base, FALSE, IMAGE_DIRECTORY_ENTRY_TLS, &Size) != NULL;
    UnmapViewOfFile(Base);
    return Result;
}

static BOOL IsTlsAllowed(PCWSTR Name)
{
    ULONG i;

    for (i = 0; i < _countof(g_TlsAllowed); i++)
    {
        if (!_wcsicmp(Name, g_TlsAllowed[i]))
            return TRUE;
    }
    return FALSE;
}

static void TestCoreDlls(PCWSTR SystemDir)
{
    WCHAR Path[MAX_PATH];
    ULONG i;
    int Has;

    for (i = 0; i < _countof(g_CoreDlls); i++)
    {
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", SystemDir, g_CoreDlls[i]);
        Has = FileHasTlsDirectory(Path);
        if (Has < 0)
        {
            skip("%ls is not readable\n", g_CoreDlls[i]);
            continue;
        }
        ok(Has == 0, "%ls has a TLS directory\n", g_CoreDlls[i]);
    }
}

static void TestAllSystemDlls(PCWSTR SystemDir)
{
    WCHAR Path[MAX_PATH];
    WIN32_FIND_DATAW Find;
    HANDLE hFind;
    ULONG i, Scanned = 0, WithTls = 0;
    BOOL Reactos = is_reactos();
    int Has;

    for (i = 0; i < _countof(g_Patterns); i++)
    {
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", SystemDir, g_Patterns[i]);
        hFind = FindFirstFileW(Path, &Find);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            if (Find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            StringCchPrintfW(Path, _countof(Path), L"%s\\%s", SystemDir, Find.cFileName);
            Has = FileHasTlsDirectory(Path);
            if (Has < 0)
                continue;
            Scanned++;
            if (!Has)
                continue;
            WithTls++;
            if (Reactos)
                ok(IsTlsAllowed(Find.cFileName), "%ls has a TLS directory\n", Find.cFileName);
            else
                trace("%ls has a TLS directory\n", Find.cFileName);
        } while (FindNextFileW(hFind, &Find));
        FindClose(hFind);
    }
    ok(Scanned > 50, "Only %lu system images found\n", Scanned);
    trace("%lu of %lu system images have a TLS directory\n", WithTls, Scanned);
}

static BOOL ExtractResource(PCWSTR Path, PCWSTR Name)
{
    HRSRC hRsrc;
    HGLOBAL hGlobal;
    PVOID Data;
    DWORD Size, Written = 0;
    HANDLE hFile;
    BOOL Ret;

    hRsrc = FindResourceW(NULL, Name, (PCWSTR)RT_RCDATA);
    if (!hRsrc || !(Size = SizeofResource(NULL, hRsrc)) ||
        !(hGlobal = LoadResource(NULL, hRsrc)) || !(Data = LockResource(hGlobal)))
    {
        return FALSE;
    }
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(hFile, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(hFile);
    return Ret;
}

typedef BOOL (WINAPI *FN_KeyDtorInit)(void);
typedef VOID (WINAPI *FN_KeyDtorSet)(void);
typedef LONG (WINAPI *FN_KeyDtorCalls)(void);

static FN_KeyDtorSet g_pKeyDtorSet;

static DWORD WINAPI KeyThread(LPVOID Parameter)
{
    g_pKeyDtorSet();
    return 0;
}

/* A DLL with the default CRT startup, loaded at run time */
static void TestKeyDtorDynamic(PCWSTR DllPath)
{
    HMODULE hDll;
    FN_KeyDtorInit pInit;
    FN_KeyDtorCalls pCalls;
    HANDLE hThread;
    ULONG Size = 0;
    int i;

    hDll = LoadLibraryW(DllPath);
    ok(hDll != NULL, "LoadLibraryW failed with %lu\n", GetLastError());
    if (!hDll)
        return;
    ok(RtlImageDirectoryEntryToData(hDll, TRUE, IMAGE_DIRECTORY_ENTRY_TLS, &Size) == NULL,
       "crtkeydtor.dll has a TLS directory\n");
    pInit = (FN_KeyDtorInit)GetProcAddress(hDll, "KeyDtorInit");
    g_pKeyDtorSet = (FN_KeyDtorSet)GetProcAddress(hDll, "KeyDtorSet");
    pCalls = (FN_KeyDtorCalls)GetProcAddress(hDll, "KeyDtorCalls");
    if (!pInit || !g_pKeyDtorSet || !pCalls)
    {
        skip("crtkeydtor.dll exports missing\n");
        FreeLibrary(hDll);
        return;
    }
    ok(pInit(), "KeyDtorInit failed\n");
    for (i = 0; i < 3; i++)
    {
        hThread = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
        ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (!hThread)
            break;
        ok(WaitForSingleObject(hThread, 30000) == WAIT_OBJECT_0, "Thread did not end\n");
        CloseHandle(hThread);
    }
    ok(pCalls() == 3, "Key destructor ran %ld times, expected 3\n", pCalls());
    FreeLibrary(hDll);
}

/* The same DLL imported by an EXE, so it is loaded at process start */
static void TestKeyDtorStatic(PCWSTR ExePath)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR CmdLine[MAX_PATH + 2];
    DWORD ExitCode = 0xdeadbeef;

    StringCchPrintfW(CmdLine, _countof(CmdLine), L"\"%s\"", ExePath);
    if (!CreateProcessW(ExePath, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessW failed with %lu\n", GetLastError());
        return;
    }
    ok(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0, "Child did not exit\n");
    GetExitCodeProcess(pi.hProcess, &ExitCode);
    ok(ExitCode == 1, "Child exit code %lu, expected 1 (one destructor call)\n", ExitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void TestKeyDtors(void)
{
    WCHAR TempDir[MAX_PATH], Dir[MAX_PATH], DllPath[MAX_PATH], ExePath[MAX_PATH];

    GetTempPathW(_countof(TempDir), TempDir);
    StringCchPrintfW(Dir, _countof(Dir), L"%ssystemdlltls_%lu", TempDir, GetCurrentProcessId());
    if (!CreateDirectoryW(Dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        skip("Cannot create %ls\n", Dir);
        return;
    }
    StringCchPrintfW(DllPath, _countof(DllPath), L"%s\\crtkeydtor.dll", Dir);
    StringCchPrintfW(ExePath, _countof(ExePath), L"%s\\crtkeydtor_static.exe", Dir);
    if (!ExtractResource(DllPath, MAKEINTRESOURCEW(103)) ||
        !ExtractResource(ExePath, MAKEINTRESOURCEW(104)))
    {
        skip("Cannot extract the helper images\n");
    }
    else
    {
        TestKeyDtorDynamic(DllPath);
        TestKeyDtorStatic(ExePath);
    }
    DeleteFileW(ExePath);
    DeleteFileW(DllPath);
    RemoveDirectoryW(Dir);
}

START_TEST(SystemDllTls)
{
    WCHAR SystemDir[MAX_PATH];

    if (!GetSystemDirectoryW(SystemDir, _countof(SystemDir)))
    {
        skip("GetSystemDirectoryW failed\n");
        return;
    }
    TestCoreDlls(SystemDir);
    TestAllSystemDlls(SystemDir);
    TestKeyDtors();
}
