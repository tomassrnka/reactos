/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for SetDefaultDllDirectories, AddDllDirectory, RemoveDllDirectory and the
 *              LOAD_LIBRARY_SEARCH_* flags of LoadLibraryEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SEARCH_DLL_LOAD_DIR     0x100
#define SEARCH_APPLICATION_DIR  0x200
#define SEARCH_USER_DIRS        0x400
#define SEARCH_SYSTEM32         0x800
#define SEARCH_DEFAULT_DIRS     0x1000

typedef PVOID (WINAPI *FN_AddDllDirectory)(PCWSTR);
typedef BOOL (WINAPI *FN_RemoveDllDirectory)(PVOID);
typedef BOOL (WINAPI *FN_SetDefaultDllDirectories)(DWORD);

static FN_AddDllDirectory pAddDllDirectory;
static FN_RemoveDllDirectory pRemoveDllDirectory;
static FN_SetDefaultDllDirectories pSetDefaultDllDirectories;

/* A copy of a small library of the system with a name of its own, in the directories of the test */
#define TEST_DLL L"dlldirtest.dll"
#define SYSTEM_DLL L"lz32.dll"

static WCHAR AppDir[MAX_PATH], UserDir1[MAX_PATH], UserDir2[MAX_PATH], TempRoot[MAX_PATH];
static BOOL HaveAppCopy, HaveSystemCopy;
static WCHAR SystemDir[MAX_PATH], SourceDll[MAX_PATH];

/* Which directory a load found the library in: 1 application, 2 first added, 3 second added, 4 system, 0 failed */
static int
Where(DWORD Flags, DWORD *Error)
{
    WCHAR Name[MAX_PATH];
    HMODULE Module;
    int Result = 0;

    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(TEST_DLL, NULL, Flags);
    *Error = GetLastError();
    if (!Module)
        return 0;

    GetModuleFileNameW(Module, Name, _countof(Name));
    if (HaveAppCopy && _wcsnicmp(Name, AppDir, wcslen(AppDir)) == 0 && Name[wcslen(AppDir)] == L'\\' &&
        wcschr(Name + wcslen(AppDir) + 1, L'\\') == NULL)
        Result = 1;
    else if (_wcsnicmp(Name, UserDir1, wcslen(UserDir1)) == 0)
        Result = 2;
    else if (_wcsnicmp(Name, UserDir2, wcslen(UserDir2)) == 0)
        Result = 3;
    else if (_wcsnicmp(Name, SystemDir, wcslen(SystemDir)) == 0)
        Result = 4;
    else
        Result = -1;
    FreeLibrary(Module);
    return Result;
}

#define EXPECT_WHERE(Flags, Expected) \
    do { \
        DWORD Error_; \
        int Got_ = Where(Flags, &Error_); \
        ok(Got_ == (Expected), "Flags 0x%lx: found in %d, expected %d (error %lu)\n", (DWORD)(Flags), Got_, (int)(Expected), Error_); \
    } while (0)

#define EXPECT_DEFAULT(Flags, Result, Err) \
    do { \
        BOOL Ret_; \
        DWORD Error_; \
        SetLastError(0xdeadbeef); \
        Ret_ = pSetDefaultDllDirectories(Flags); \
        Error_ = GetLastError(); \
        ok(Ret_ == (Result), "SetDefaultDllDirectories(0x%lx) returned %d, expected %d\n", (DWORD)(Flags), Ret_, (BOOL)(Result)); \
        if (!(Result)) \
            ok(Error_ == (Err), "SetDefaultDllDirectories(0x%lx) error %lu, expected %lu\n", (DWORD)(Flags), Error_, (DWORD)(Err)); \
    } while (0)

static BOOL
MakeDirectory(PWCHAR Dir, PCWSTR Name)
{
    wcscpy(Dir, TempRoot);
    wcscat(Dir, Name);
    return CreateDirectoryW(Dir, NULL);
}

static BOOL
CopyTestDll(PCWSTR Dir, PCWSTR Source)
{
    WCHAR Destination[MAX_PATH];

    wcscpy(Destination, Dir);
    wcscat(Destination, L"\\" TEST_DLL);
    return CopyFileW(Source, Destination, FALSE);
}

static void
DeleteTestDll(PCWSTR Dir)
{
    WCHAR Destination[MAX_PATH];

    wcscpy(Destination, Dir);
    wcscat(Destination, L"\\" TEST_DLL);
    DeleteFileW(Destination);
}

static void
TestAddDllDirectory(PVOID *Cookie1, PVOID *Cookie2)
{
    PVOID Cookie;
    DWORD Error;

    /* Only a full path names a directory */
    SetLastError(0xdeadbeef);
    Cookie = pAddDllDirectory(L"relative\\directory");
    Error = GetLastError();
    ok(Cookie == NULL, "AddDllDirectory of a relative path returned %p\n", Cookie);
    ok(Error == ERROR_INVALID_PARAMETER, "error %lu\n", Error);

    SetLastError(0xdeadbeef);
    Cookie = pAddDllDirectory(L"");
    Error = GetLastError();
    ok(Cookie == NULL, "AddDllDirectory of an empty path returned %p\n", Cookie);
    ok(Error == ERROR_INVALID_PARAMETER, "error %lu\n", Error);

    *Cookie1 = pAddDllDirectory(UserDir1);
    ok(*Cookie1 != NULL, "AddDllDirectory failed %lu\n", GetLastError());
    *Cookie2 = pAddDllDirectory(UserDir2);
    ok(*Cookie2 != NULL, "AddDllDirectory failed %lu\n", GetLastError());
    ok(*Cookie1 != *Cookie2, "The same cookie twice\n");
}

static void
TestSearchFlags(void)
{
    HMODULE Module;
    DWORD Error;

    /* The directories of one call; the directories of AddDllDirectory are searched the last added first */
    EXPECT_WHERE(SEARCH_USER_DIRS, 3);
    if (HaveSystemCopy)
        EXPECT_WHERE(SEARCH_SYSTEM32, 4);
    if (HaveAppCopy)
    {
        EXPECT_WHERE(SEARCH_APPLICATION_DIR, 1);
        /* The order is the application, the directories of the process, the system */
        EXPECT_WHERE(SEARCH_APPLICATION_DIR | SEARCH_USER_DIRS, 1);
        EXPECT_WHERE(SEARCH_APPLICATION_DIR | SEARCH_SYSTEM32, 1);
        EXPECT_WHERE(SEARCH_APPLICATION_DIR | SEARCH_USER_DIRS | SEARCH_SYSTEM32, 1);
        EXPECT_WHERE(SEARCH_DEFAULT_DIRS, 1);
    }
    else
    {
        EXPECT_WHERE(SEARCH_DEFAULT_DIRS, 3);
    }
    EXPECT_WHERE(SEARCH_USER_DIRS | SEARCH_SYSTEM32, 3);

    /* The system directory is not searched without its flag: a library only there is not found */
    if (HaveSystemCopy)
    {
        EXPECT_WHERE(SEARCH_SYSTEM32, 4);
        DeleteTestDll(UserDir1);
        DeleteTestDll(UserDir2);
        EXPECT_WHERE(SEARCH_USER_DIRS, 0);
        if (HaveAppCopy)
            EXPECT_WHERE(SEARCH_APPLICATION_DIR | SEARCH_USER_DIRS, 1);
        EXPECT_WHERE(SEARCH_USER_DIRS | SEARCH_SYSTEM32, 4);
        CopyTestDll(UserDir1, SourceDll);
        CopyTestDll(UserDir2, SourceDll);
    }

    /* The directory of the library is the one of a full path; a name that is not one cannot ask for it */
    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(TEST_DLL, NULL, SEARCH_DLL_LOAD_DIR);
    Error = GetLastError();
    ok(Module == NULL, "A relative name with the library directory was loaded\n");
    ok(Error == ERROR_INVALID_PARAMETER, "error %lu\n", Error);
    if (Module) FreeLibrary(Module);
    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(TEST_DLL, NULL, SEARCH_DLL_LOAD_DIR | SEARCH_SYSTEM32);
    Error = GetLastError();
    ok(Module == NULL, "A relative name with the library directory was loaded\n");
    ok(Error == ERROR_INVALID_PARAMETER, "error %lu\n", Error);
    if (Module) FreeLibrary(Module);

    {
        WCHAR Full[MAX_PATH];

        wcscpy(Full, UserDir1);
        wcscat(Full, L"\\" TEST_DLL);
        Module = LoadLibraryExW(Full, NULL, SEARCH_DLL_LOAD_DIR);
        ok(Module != NULL, "A full path with the library directory failed, error %lu\n", GetLastError());
        if (Module) FreeLibrary(Module);
        /* A full path is a full path, whatever the directories to search are */
        Module = LoadLibraryExW(Full, NULL, SEARCH_SYSTEM32);
        ok(Module != NULL, "A full path with the system directory failed, error %lu\n", GetLastError());
        if (Module) FreeLibrary(Module);
    }

    /* The search flags and the altered search path are two orders */
    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(TEST_DLL, NULL, SEARCH_SYSTEM32 | LOAD_WITH_ALTERED_SEARCH_PATH);
    Error = GetLastError();
    ok(Module == NULL, "Search flags with the altered search path were taken\n");
    ok(Error == ERROR_INVALID_PARAMETER, "error %lu\n", Error);
    if (Module) FreeLibrary(Module);

    /* A library that is not there */
    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(L"nosuchdlldirtest.dll", NULL, SEARCH_DEFAULT_DIRS);
    Error = GetLastError();
    ok(Module == NULL, "A library that does not exist was loaded\n");
    ok(Error == ERROR_MOD_NOT_FOUND, "error %lu\n", Error);
}

static void
TestDefaultDirectories(void)
{
    DWORD Error;
    HMODULE Module;
    static const DWORD Refused[] = { 0, SEARCH_DLL_LOAD_DIR, 0x300, 0x1, 0x2, 0x8, 0x8000, 0x1f00 };
    static const DWORD Taken[] = { SEARCH_APPLICATION_DIR, SEARCH_USER_DIRS, SEARCH_SYSTEM32, SEARCH_DEFAULT_DIRS,
                                   0xe00, 0x1e00, 0x2000, 0x4000 };
    unsigned i;

    for (i = 0; i < _countof(Refused); i++)
        EXPECT_DEFAULT(Refused[i], FALSE, ERROR_INVALID_PARAMETER);
    for (i = 0; i < _countof(Taken); i++)
        EXPECT_DEFAULT(Taken[i], TRUE, 0);

    /* A load without flags takes the default directories */
    EXPECT_DEFAULT(SEARCH_USER_DIRS, TRUE, 0);
    EXPECT_WHERE(0, 3);
    Module = LoadLibraryW(TEST_DLL);
    ok(Module != NULL, "LoadLibrary with the default of the user directories failed %lu\n", GetLastError());
    if (Module)
    {
        WCHAR Name[MAX_PATH];

        GetModuleFileNameW(Module, Name, _countof(Name));
        ok(_wcsnicmp(Name, UserDir2, wcslen(UserDir2)) == 0, "Loaded %ls\n", Name);
        FreeLibrary(Module);
    }
    if (HaveSystemCopy)
    {
        EXPECT_DEFAULT(SEARCH_SYSTEM32, TRUE, 0);
        EXPECT_WHERE(0, 4);
    }
    if (HaveAppCopy)
    {
        EXPECT_DEFAULT(SEARCH_APPLICATION_DIR, TRUE, 0);
        EXPECT_WHERE(0, 1);
        EXPECT_DEFAULT(SEARCH_DEFAULT_DIRS, TRUE, 0);
        EXPECT_WHERE(0, 1);
    }
    EXPECT_DEFAULT(SEARCH_USER_DIRS | SEARCH_SYSTEM32, TRUE, 0);
    EXPECT_WHERE(0, 3);

    /* The flags of the call are the ones that count */
    EXPECT_DEFAULT(SEARCH_USER_DIRS, TRUE, 0);
    EXPECT_WHERE(SEARCH_USER_DIRS | SEARCH_APPLICATION_DIR, HaveAppCopy ? 1 : 3);
    if (HaveSystemCopy)
        EXPECT_WHERE(SEARCH_SYSTEM32, 4);

    /* The altered search path is for a full path beside the default directories, a name that is not one is refused */
    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(TEST_DLL, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    Error = GetLastError();
    ok(Module == NULL, "The altered search path of a relative name was taken beside the default directories\n");
    ok(Error == ERROR_INVALID_PARAMETER, "error %lu\n", Error);
    if (Module) FreeLibrary(Module);
    {
        WCHAR Full[MAX_PATH];

        wcscpy(Full, UserDir1);
        wcscat(Full, L"\\" TEST_DLL);
        Module = LoadLibraryExW(Full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        ok(Module != NULL, "The altered search path of a full path failed beside the default directories, error %lu\n", GetLastError());
        if (Module) FreeLibrary(Module);
    }

    /* The directory of SetDllDirectory is one of the directories of the process, after those of AddDllDirectory */
    ok(SetDllDirectoryW(UserDir1), "SetDllDirectory failed %lu\n", GetLastError());
    EXPECT_WHERE(0, 3);
    ok(SetDllDirectoryW(NULL), "SetDllDirectory failed %lu\n", GetLastError());
}

static void
TestRemoveDllDirectory(PVOID Cookie1, PVOID Cookie2)
{
    ok(pRemoveDllDirectory(Cookie1), "RemoveDllDirectory failed %lu\n", GetLastError());
    EXPECT_WHERE(SEARCH_USER_DIRS, 3);
    ok(pRemoveDllDirectory(Cookie2), "RemoveDllDirectory failed %lu\n", GetLastError());
    EXPECT_WHERE(SEARCH_USER_DIRS, 0);
}

START_TEST(DefaultDllDirectories)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    WCHAR *Source = SourceDll, Temp[MAX_PATH], *Slash;
    PVOID Cookie1 = NULL, Cookie2 = NULL;

    pAddDllDirectory = (FN_AddDllDirectory)GetProcAddress(Kernel32, "AddDllDirectory");
    pRemoveDllDirectory = (FN_RemoveDllDirectory)GetProcAddress(Kernel32, "RemoveDllDirectory");
    pSetDefaultDllDirectories = (FN_SetDefaultDllDirectories)GetProcAddress(Kernel32, "SetDefaultDllDirectories");
    if (!pAddDllDirectory || !pRemoveDllDirectory || !pSetDefaultDllDirectories)
    {
        skip("AddDllDirectory, RemoveDllDirectory or SetDefaultDllDirectories is not available\n");
        return;
    }

    GetModuleFileNameW(NULL, AppDir, _countof(AppDir));
    Slash = wcsrchr(AppDir, L'\\');
    *Slash = UNICODE_NULL;

    GetSystemDirectoryW(SystemDir, _countof(SystemDir));
    wcscpy(Source, SystemDir);
    wcscat(Source, L"\\" SYSTEM_DLL);

    GetTempPathW(_countof(Temp), Temp);
    /* The temporary path ends with a backslash */
    _snwprintf(TempRoot, _countof(TempRoot), L"%lsdlldirtest%lu", Temp, GetCurrentProcessId());
    if (!CreateDirectoryW(TempRoot, NULL) ||
        !MakeDirectory(UserDir1, L"\\one") || !MakeDirectory(UserDir2, L"\\two"))
    {
        skip("The test directories cannot be made, error %lu\n", GetLastError());
        return;
    }

    if (!CopyTestDll(UserDir1, Source) || !CopyTestDll(UserDir2, Source))
    {
        skip("The test library cannot be copied, error %lu\n", GetLastError());
        goto Cleanup;
    }
    HaveAppCopy = CopyTestDll(AppDir, Source);
    if (!HaveAppCopy)
        skip("The directory of the test is read only, the application directory is not tested\n");

    /* The system directory is written to only by an administrator, what is tested of it depends on that */
    HaveSystemCopy = CopyTestDll(SystemDir, Source);
    if (!HaveSystemCopy)
        skip("The system directory is read only, its search is not tested\n");

    TestAddDllDirectory(&Cookie1, &Cookie2);
    if (Cookie1 && Cookie2)
    {
        TestSearchFlags();
        TestDefaultDirectories();
        TestRemoveDllDirectory(Cookie1, Cookie2);
    }

Cleanup:
    if (HaveAppCopy)
        DeleteTestDll(AppDir);
    if (HaveSystemCopy)
        DeleteTestDll(SystemDir);
    DeleteTestDll(UserDir1);
    DeleteTestDll(UserDir2);
    RemoveDirectoryW(UserDir1);
    RemoveDirectoryW(UserDir2);
    RemoveDirectoryW(TempRoot);
}
