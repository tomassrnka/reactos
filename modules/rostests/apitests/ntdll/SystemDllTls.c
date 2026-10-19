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

/* ReactOS system DLLs with a TLS directory but no thread-local data */
static const PCWSTR g_TlsAllowed[] =
{
    L"ucrtbase.dll", /* vcruntime frees its per-thread data from a TLS callback */
};

static const PCWSTR g_Patterns[] =
{
    L"*.dll", L"*.acm", L"*.ax", L"*.cpl", L"*.drv", L"*.ime", L"*.ocx", L"*.tsp",
};

typedef enum _TLS_KIND
{
    TlsUnreadable = -1,
    TlsNone = 0,
    TlsCallbacksOnly,   /* TLS callbacks and an empty template: no thread-local data */
    TlsOther,           /* thread-local data, or no callbacks */
} TLS_KIND;

static TLS_KIND FileTlsKind(PCWSTR Path)
{
    HANDLE hFile, hMapping;
    PVOID Base;
    PIMAGE_NT_HEADERS NtHeaders;
    PIMAGE_TLS_DIRECTORY Tls;
    PUCHAR Data;
    ULONG_PTR *Callback;
    ULONG Size = 0, i;
    ULONG_PTR Start, End;
    TLS_KIND Kind = TlsUnreadable;

    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return TlsUnreadable;
    hMapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hFile);
    if (!hMapping)
        return TlsUnreadable;
    Base = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMapping);
    if (!Base)
        return TlsUnreadable;

    NtHeaders = RtlImageNtHeader(Base);
    if (NtHeaders && NtHeaders->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR_MAGIC)
    {
        Tls = RtlImageDirectoryEntryToData(Base, FALSE, IMAGE_DIRECTORY_ENTRY_TLS, &Size);
        if (!Tls)
        {
            Kind = TlsNone;
        }
        else
        {
            /* The CRT template starts with a pointer-sized placeholder, so data makes it larger */
            BOOL Empty = !Tls->SizeOfZeroFill;
            Start = Tls->StartAddressOfRawData;
            End = Tls->EndAddressOfRawData;
            if (End > Start + sizeof(PVOID))
                Empty = FALSE;
            if (Empty && End > Start)
            {
                Data = RtlImageRvaToVa(NtHeaders, Base,
                                       (ULONG)(Start - NtHeaders->OptionalHeader.ImageBase), NULL);
                for (i = 0; Data && i < End - Start; i++)
                {
                    if (Data[i])
                        Empty = FALSE;
                }
            }
            Callback = NULL;
            if (Tls->AddressOfCallBacks)
            {
                Callback = RtlImageRvaToVa(NtHeaders, Base,
                                           (ULONG)(Tls->AddressOfCallBacks - NtHeaders->OptionalHeader.ImageBase), NULL);
            }
            Kind = (Empty && Callback && *Callback) ? TlsCallbacksOnly : TlsOther;
        }
    }
    UnmapViewOfFile(Base);
    return Kind;
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
    TLS_KIND Kind;
    ULONG i;

    for (i = 0; i < _countof(g_CoreDlls); i++)
    {
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", SystemDir, g_CoreDlls[i]);
        Kind = FileTlsKind(Path);
        if (Kind == TlsUnreadable)
        {
            skip("%ls is not readable\n", g_CoreDlls[i]);
            continue;
        }
        ok(Kind == TlsNone, "%ls has a TLS directory (%d)\n", g_CoreDlls[i], Kind);
    }
}

typedef DWORD (WINAPI *FN_GetFileVersionInfoSizeW)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *FN_GetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL (WINAPI *FN_VerQueryValueW)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

static FN_GetFileVersionInfoSizeW g_pGetFileVersionInfoSizeW;
static FN_GetFileVersionInfoW g_pGetFileVersionInfoW;
static FN_VerQueryValueW g_pVerQueryValueW;

/* The CompanyName of the file's version resource; FALSE if it has none */
static BOOL GetCompanyName(PCWSTR Path, PWSTR Company, SIZE_T cchCompany)
{
    DWORD Size, Handle;
    PVOID Data;
    WORD *Translation;
    PWSTR Value;
    UINT Length;
    WCHAR Query[64];
    BOOL Ret = FALSE;

    if (!g_pGetFileVersionInfoSizeW || !g_pGetFileVersionInfoW || !g_pVerQueryValueW)
        return FALSE;
    Size = g_pGetFileVersionInfoSizeW(Path, &Handle);
    if (!Size)
        return FALSE;
    Data = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Data)
        return FALSE;
    if (g_pGetFileVersionInfoW(Path, 0, Size, Data) &&
        g_pVerQueryValueW(Data, L"\\VarFileInfo\\Translation", (PVOID *)&Translation, &Length) &&
        Length >= 2 * sizeof(WORD))
    {
        StringCchPrintfW(Query, _countof(Query), L"\\StringFileInfo\\%04x%04x\\CompanyName",
                         Translation[0], Translation[1]);
        if (g_pVerQueryValueW(Data, Query, (PVOID *)&Value, &Length) && Length)
            Ret = SUCCEEDED(StringCchCopyW(Company, cchCompany, Value));
    }
    HeapFree(GetProcessHeap(), 0, Data);
    return Ret;
}

/* On ReactOS a system image may have TLS callbacks only together with thread-local data.
   Only images whose CompanyName names the system's vendor (the first word of ntdll.dll's,
   "ReactOS" also in the Wine-derived modules) are asserted: an empty template cannot be
   told apart from a small zero-initialised variable in another vendor's image. */
static void TestAllSystemDlls(PCWSTR SystemDir)
{
    WCHAR Path[MAX_PATH], Vendor[128], Company[128];
    WIN32_FIND_DATAW Find;
    HANDLE hFind;
    HMODULE hVersion;
    ULONG i, Scanned = 0, CallbacksOnly = 0, Other = 0, Asserted = 0;
    BOOL Reactos = is_reactos();
    TLS_KIND Kind;

    hVersion = LoadLibraryW(L"version.dll");
    if (hVersion)
    {
        g_pGetFileVersionInfoSizeW = (FN_GetFileVersionInfoSizeW)GetProcAddress(hVersion, "GetFileVersionInfoSizeW");
        g_pGetFileVersionInfoW = (FN_GetFileVersionInfoW)GetProcAddress(hVersion, "GetFileVersionInfoW");
        g_pVerQueryValueW = (FN_VerQueryValueW)GetProcAddress(hVersion, "VerQueryValueW");
    }
    StringCchPrintfW(Path, _countof(Path), L"%s\\ntdll.dll", SystemDir);
    Vendor[0] = L'\0';
    if (GetCompanyName(Path, Vendor, _countof(Vendor)))
        Vendor[wcscspn(Vendor, L" /,")] = L'\0';
    ok(Vendor[0] != L'\0', "ntdll.dll has no CompanyName\n");

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
            Kind = FileTlsKind(Path);
            if (Kind == TlsUnreadable)
                continue;
            Scanned++;
            if (Kind == TlsOther)
            {
                Other++;
                trace("%ls has a TLS directory with data or without callbacks\n", Find.cFileName);
            }
            else if (Kind == TlsCallbacksOnly)
            {
                CallbacksOnly++;
                if (Reactos && Vendor[0] && GetCompanyName(Path, Company, _countof(Company)) &&
                    wcsstr(Company, Vendor))
                {
                    Asserted++;
                    ok(IsTlsAllowed(Find.cFileName), "%ls has TLS callbacks without thread-local data\n", Find.cFileName);
                }
                else
                {
                    trace("%ls has TLS callbacks without thread-local data (not asserted)\n", Find.cFileName);
                }
            }
        } while (FindNextFileW(hFind, &Find));
        FindClose(hFind);
    }
    if (hVersion)
        FreeLibrary(hVersion);
    ok(Scanned > 50, "Only %lu system images found\n", Scanned);
    trace("%lu system images: %lu with TLS callbacks only (%lu of the system's vendor), %lu with another TLS directory\n",
          Scanned, CallbacksOnly, Asserted, Other);
}

static BOOL ExtractResource(PCWSTR Path, UINT Id)
{
    HRSRC hRsrc;
    HGLOBAL hGlobal;
    PVOID Data;
    DWORD Size, Written = 0;
    HANDLE hFile;
    BOOL Ret;

    hRsrc = FindResourceW(NULL, MAKEINTRESOURCEW(Id), (PCWSTR)RT_RCDATA);
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

/* Loads the DLL at run time, ends Threads threads that set the key; FALSE if a thread hangs */
static BOOL KeyDtorDynamicOnce(PCWSTR Name, PCWSTR DllPath, BOOL ExpectTlsDir, int Threads)
{
    HMODULE hDll;
    FN_KeyDtorInit pInit;
    FN_KeyDtorCalls pCalls;
    HANDLE hThread;
    ULONG Size = 0;
    BOOL Ended = TRUE;
    int i;

    hDll = LoadLibraryW(DllPath);
    ok(hDll != NULL, "%ls: LoadLibraryW failed with %lu\n", Name, GetLastError());
    if (!hDll)
        return TRUE;
    ok((RtlImageDirectoryEntryToData(hDll, TRUE, IMAGE_DIRECTORY_ENTRY_TLS, &Size) != NULL) == ExpectTlsDir,
       "%ls: TLS directory %s\n", Name, ExpectTlsDir ? "missing" : "present");
    pInit = (FN_KeyDtorInit)GetProcAddress(hDll, "KeyDtorInit");
    g_pKeyDtorSet = (FN_KeyDtorSet)GetProcAddress(hDll, "KeyDtorSet");
    pCalls = (FN_KeyDtorCalls)GetProcAddress(hDll, "KeyDtorCalls");
    if (!pInit || !g_pKeyDtorSet || !pCalls)
    {
        skip("%ls: exports missing\n", Name);
        FreeLibrary(hDll);
        return TRUE;
    }
    ok(pInit(), "%ls: KeyDtorInit failed\n", Name);
    for (i = 0; i < Threads && Ended; i++)
    {
        hThread = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
        ok(hThread != NULL, "%ls: CreateThread failed with %lu\n", Name, GetLastError());
        if (!hThread)
            break;
        Ended = WaitForSingleObject(hThread, 30000) == WAIT_OBJECT_0;
        ok(Ended, "%ls: thread did not end\n", Name);
        CloseHandle(hThread);
    }
    ok(pCalls() == Threads, "%ls: key destructor ran %ld times, expected %d\n", Name, pCalls(), Threads);
    /* A thread that is still running would call into the unloaded DLL */
    if (Ended)
        FreeLibrary(hDll);
    return Ended;
}

/* DLL_PROCESS_ATTACH sets the key and fails: the destructor runs, before DllMain's detach */
static void KeyDtorFailedAttach(PCWSTR Name, PCWSTR DllPath)
{
    WCHAR Trace[32] = L"";
    HMODULE hDll;

    SetEnvironmentVariableW(L"CRTKEYDTOR_TRACE", NULL);
    SetEnvironmentVariableW(L"CRTKEYDTOR_FAIL", L"1");
    hDll = LoadLibraryW(DllPath);
    SetEnvironmentVariableW(L"CRTKEYDTOR_FAIL", NULL);
    ok(hDll == NULL, "%ls: LoadLibraryW succeeded\n", Name);
    if (hDll)
        FreeLibrary(hDll);
    GetEnvironmentVariableW(L"CRTKEYDTOR_TRACE", Trace, _countof(Trace));
    SetEnvironmentVariableW(L"CRTKEYDTOR_TRACE", NULL);
    ok(!wcscmp(Trace, L"DM"), "%ls: failed attach trace '%ls', expected 'DM'\n", Name, Trace);
}

/* The DLL imported by an EXE, so it is loaded at process start */
static void KeyDtorStatic(PCWSTR Name, PCWSTR ExePath)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR CmdLine[MAX_PATH + 2];
    DWORD ExitCode = 0xdeadbeef;

    StringCchPrintfW(CmdLine, _countof(CmdLine), L"\"%s\"", ExePath);
    if (!CreateProcessW(ExePath, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(0, "%ls: CreateProcessW failed with %lu\n", Name, GetLastError());
        return;
    }
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "%ls: child did not exit\n", Name);
        TerminateProcess(pi.hProcess, 0xdead);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    GetExitCodeProcess(pi.hProcess, &ExitCode);
    ok(ExitCode == 1, "%ls: child exit code %lu, expected 1 (one destructor call)\n", Name, ExitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void TestKeyDtors(void)
{
    static const struct
    {
        PCWSTR Dll, Exe;
        UINT DllId, ExeId;
        BOOL TlsDir;
    } Helpers[] =
    {
        { L"crtkeydtor.dll", L"crtkeydtor_static.exe", 103, 104, FALSE },
        { L"crtkeydtor_tls.dll", L"crtkeydtor_tls_static.exe", 105, 106, TRUE },
    };
    WCHAR TempDir[MAX_PATH], Dir[MAX_PATH], DllPath[MAX_PATH], ExePath[MAX_PATH];
    BOOL Clean = TRUE;
    ULONG i;

    GetTempPathW(_countof(TempDir), TempDir);
    StringCchPrintfW(Dir, _countof(Dir), L"%ssystemdlltls_%lu", TempDir, GetCurrentProcessId());
    if (!CreateDirectoryW(Dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        skip("Cannot create %ls\n", Dir);
        return;
    }
    for (i = 0; i < _countof(Helpers); i++)
    {
        StringCchPrintfW(DllPath, _countof(DllPath), L"%s\\%s", Dir, Helpers[i].Dll);
        StringCchPrintfW(ExePath, _countof(ExePath), L"%s\\%s", Dir, Helpers[i].Exe);
        if (!ExtractResource(DllPath, Helpers[i].DllId) || !ExtractResource(ExePath, Helpers[i].ExeId))
        {
            skip("Cannot extract %ls\n", Helpers[i].Dll);
            continue;
        }
        /* Twice: the second load gets a fresh copy after the first one's process detach */
        if (Clean)
            Clean = KeyDtorDynamicOnce(Helpers[i].Dll, DllPath, Helpers[i].TlsDir, 3);
        if (Clean)
            Clean = KeyDtorDynamicOnce(Helpers[i].Dll, DllPath, Helpers[i].TlsDir, 1);
        if (Clean && !Helpers[i].TlsDir)
            KeyDtorFailedAttach(Helpers[i].Dll, DllPath);
        KeyDtorStatic(Helpers[i].Exe, ExePath);
        if (Clean)
            DeleteFileW(DllPath);
        DeleteFileW(ExePath);
    }
    if (Clean)
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
