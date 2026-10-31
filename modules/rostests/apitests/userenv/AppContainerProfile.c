/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for userenv!CreateAppContainerProfile, DeleteAppContainerProfile, GetAppContainerFolderPath and
 *              GetAppContainerRegistryLocation (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The create and delete round trip changes the state of the user (registry keys and a folder under Packages, all
 * removed again). On ReactOS it always runs; on Windows it runs only when the environment variable
 * ROSAPITEST_APPCONTAINER_PROFILE is set.
 */

#include <apitest.h>
#include <winreg.h>
#include <sddl.h>
#include <objbase.h>

typedef HRESULT (WINAPI *PFN_Create)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID *);
typedef HRESULT (WINAPI *PFN_Delete)(PCWSTR);
typedef HRESULT (WINAPI *PFN_Derive)(PCWSTR, PSID *);
typedef HRESULT (WINAPI *PFN_FolderPath)(PCWSTR, PWSTR *);
typedef HRESULT (WINAPI *PFN_RegistryLocation)(REGSAM, PHKEY);

static PFN_Create pCreate;
static PFN_Delete pDelete;
static PFN_Derive pDerive;
static PFN_FolderPath pFolderPath;
static PFN_RegistryLocation pRegistryLocation;

#define APPCONTAINER_KEY L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\"
#define TEST_NAME L"ReactOS.ApiTest.AppContainerProfile"
#define TEST_DISPLAY L"ReactOS API test AppContainer profile"
#define TEST_DESCRIPTION L"Throwaway profile of the ReactOS API test"
#define ERROR_NOT_AN_APPCONTAINER_SID 4250

static BOOL RegistryKeyExists(PCWSTR Path)
{
    HKEY Key;
    LONG Error = RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_READ, &Key);

    if (Error == ERROR_SUCCESS)
        RegCloseKey(Key);
    return Error == ERROR_SUCCESS;
}

static BOOL ReadString(PCWSTR Path, PCWSTR Name, PWSTR Buffer, DWORD Chars)
{
    HKEY Key;
    DWORD Type, Size = Chars * sizeof(WCHAR);
    LONG Error = RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_READ, &Key);

    if (Error != ERROR_SUCCESS)
        return FALSE;
    Error = RegQueryValueExW(Key, Name, NULL, &Type, (PBYTE)Buffer, &Size);
    RegCloseKey(Key);
    return Error == ERROR_SUCCESS && Type == REG_SZ;
}

static BOOL IsDirectory(PCWSTR Path)
{
    DWORD Attributes = GetFileAttributesW(Path);

    return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* Everything that these functions do not need a profile for */
static void TestWithoutProfile(void)
{
    PWSTR Path = (PWSTR)(ULONG_PTR)0x33;
    HKEY Key = (HKEY)(ULONG_PTR)0x44;
    HRESULT hr;

    /* a well-formed AppContainer SID that is not registered */
    hr = pFolderPath(L"S-1-15-2-1861897761-1695161497-2927542615-642690995-327840285-2659745135-2630312742", &Path);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "unregistered SID: hr %#lx\n", hr);
    ok(Path == (PWSTR)(ULONG_PTR)0x33, "unregistered SID: the result was written\n");

    /* a SID that is not an AppContainer SID */
    Path = (PWSTR)(ULONG_PTR)0x33;
    hr = pFolderPath(L"S-1-5-32-544", &Path);
    ok(hr == HRESULT_FROM_WIN32(ERROR_NOT_AN_APPCONTAINER_SID), "Administrators SID: hr %#lx\n", hr);
    ok(Path == (PWSTR)(ULONG_PTR)0x33, "Administrators SID: the result was written\n");

    /* this process does not run in an AppContainer */
    hr = pRegistryLocation(KEY_READ, &Key);
    ok(hr == E_UNEXPECTED, "GetAppContainerRegistryLocation: hr %#lx\n", hr);
    ok(Key == (HKEY)(ULONG_PTR)0x44, "GetAppContainerRegistryLocation: the key was written\n");
}

/*
 * Names that cannot be a profile. The results of the first group are those of Windows 10 22H2, which fails without
 * creating anything. Windows creates a profile for the names of the second group ("a/b" makes nested folders, ".."
 * is the folder above Packages), so they are only tried on ReactOS, which rejects them (a profile name is a folder
 * name, and delete removes the folder).
 */
static void TestInvalidNames(void)
{
    static const struct
    {
        PCWSTR Name;
        HRESULT Result;
    } WindowsRejects[] =
    {
        { L"", E_INVALIDARG },
        { L"a*b", HRESULT_FROM_WIN32(ERROR_INVALID_NAME) },
        { L"a?b", HRESULT_FROM_WIN32(ERROR_INVALID_NAME) },
        { L"a\"b", HRESULT_FROM_WIN32(ERROR_INVALID_NAME) },
        { L"a<b", HRESULT_FROM_WIN32(ERROR_INVALID_NAME) },
        { L"a|b", HRESULT_FROM_WIN32(ERROR_INVALID_NAME) },
        { L"a\x01" L"b", HRESULT_FROM_WIN32(ERROR_INVALID_NAME) },
        { L"a:b", HRESULT_FROM_WIN32(ERROR_DIRECTORY) },
        { L"a\\b", HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) },
    };
    static const PCWSTR ReactOSRejects[] =
    {
        L".", L"..", L"a/b", L"../a", L"..\\..", L"a.", L"a ", L"C:x", L"..\\..\\Documents"
    };
    WCHAR Long[70], LocalAppData[MAX_PATH], Canary[MAX_PATH], Packages[MAX_PATH];
    PSID Sid;
    HRESULT hr;
    HANDLE File;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(WindowsRejects); i++)
    {
        Sid = (PSID)(ULONG_PTR)0x55;
        hr = pCreate(WindowsRejects[i].Name, L"invalid name", L"invalid name", NULL, 0, &Sid);
        ok(hr == WindowsRejects[i].Result, "create '%ls': hr %#lx, expected %#lx\n", WindowsRejects[i].Name, hr,
           WindowsRejects[i].Result);
        ok(Sid == (PSID)(ULONG_PTR)0x55, "create '%ls': the result was written\n", WindowsRejects[i].Name);
    }

    for (i = 0; i < 65; i++)
        Long[i] = L'a';
    Long[65] = UNICODE_NULL;
    Sid = (PSID)(ULONG_PTR)0x55;
    hr = pCreate(Long, L"invalid name", L"invalid name", NULL, 0, &Sid);
    ok(hr == E_INVALIDARG, "create with 65 characters: hr %#lx\n", hr);

    if (!is_reactos())
        return;

    for (i = 0; i < RTL_NUMBER_OF(ReactOSRejects); i++)
    {
        Sid = (PSID)(ULONG_PTR)0x55;
        hr = pCreate(ReactOSRejects[i], L"invalid name", L"invalid name", NULL, 0, &Sid);
        ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME) || hr == HRESULT_FROM_WIN32(ERROR_DIRECTORY) ||
           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "create '%ls': hr %#lx\n", ReactOSRejects[i], hr);
        ok(Sid == (PSID)(ULONG_PTR)0x55, "create '%ls': the result was written\n", ReactOSRejects[i]);
    }

    /* delete with such a name touches nothing: a canary file in LOCALAPPDATA and the Packages folder survive */
    ok(GetEnvironmentVariableW(L"LOCALAPPDATA", LocalAppData, MAX_PATH) != 0, "no LOCALAPPDATA\n");
    _snwprintf(Canary, MAX_PATH, L"%ls\\ReactOS-AppContainerProfile-canary.txt", LocalAppData);
    _snwprintf(Packages, MAX_PATH, L"%ls\\Packages", LocalAppData);
    CreateDirectoryW(Packages, NULL);
    File = CreateFileW(Canary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "cannot create the canary: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    CloseHandle(File);

    for (i = 0; i < RTL_NUMBER_OF(ReactOSRejects); i++)
    {
        hr = pDelete(ReactOSRejects[i]);
        ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME) || hr == HRESULT_FROM_WIN32(ERROR_DIRECTORY) ||
           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "delete '%ls': hr %#lx\n", ReactOSRejects[i], hr);
        ok(GetFileAttributesW(Canary) != INVALID_FILE_ATTRIBUTES, "delete '%ls' removed the canary\n", ReactOSRejects[i]);
        ok(IsDirectory(Packages), "delete '%ls' removed the Packages folder\n", ReactOSRejects[i]);
    }
    hr = pDelete(L"");
    ok(hr == E_INVALIDARG, "delete of the empty name: hr %#lx\n", hr);
    DeleteFileW(Canary);
}

static void TestRoundTrip(void)
{
    static const WCHAR SubFolders[][12] = { L"INetCache", L"INetCookies", L"INetHistory", L"Temp" };
    SID_IDENTIFIER_AUTHORITY AppPackageAuthority = { { 0, 0, 0, 0, 0, 15 } };
    SID_AND_ATTRIBUTES Capability;
    PSID Derived = NULL, Created = NULL, Again = (PSID)(ULONG_PTR)0x66, CapabilitySid = NULL;
    LPWSTR SidString = NULL;
    WCHAR Mapping[400], Storage[200], Text[600], LocalAppData[MAX_PATH], Expected[MAX_PATH], Folder[MAX_PATH];
    PWSTR Path;
    HRESULT hr;
    ULONG i;

    ok(GetEnvironmentVariableW(L"LOCALAPPDATA", LocalAppData, MAX_PATH) != 0, "no LOCALAPPDATA\n");
    _snwprintf(Expected, MAX_PATH, L"%ls\\Packages\\%ls", LocalAppData, TEST_NAME);

    hr = pDerive(TEST_NAME, &Derived);
    ok(hr == S_OK, "Derive: hr %#lx\n", hr);
    if (!Derived)
        return;
    ok(ConvertSidToStringSidW(Derived, &SidString), "ConvertSidToStringSid failed with %lu\n", GetLastError());
    if (!SidString)
        goto Cleanup;
    _snwprintf(Mapping, 400, APPCONTAINER_KEY L"Mappings\\%ls", SidString);
    _snwprintf(Storage, 200, APPCONTAINER_KEY L"Storage\\%ls", TEST_NAME);

    /* a profile of an earlier run that did not finish */
    pDelete(TEST_NAME);

    ok(AllocateAndInitializeSid(&AppPackageAuthority, 2, 3, 1, 0, 0, 0, 0, 0, 0, &CapabilitySid),
       "AllocateAndInitializeSid failed with %lu\n", GetLastError());
    Capability.Sid = CapabilitySid;
    Capability.Attributes = SE_GROUP_ENABLED;

    ok(!RegistryKeyExists(Mapping), "the mapping exists before the create\n");
    ok(!IsDirectory(Expected), "the folder exists before the create\n");

    hr = pCreate(TEST_NAME, TEST_DISPLAY, TEST_DESCRIPTION, &Capability, 1, &Created);
    ok(hr == S_OK, "CreateAppContainerProfile: hr %#lx\n", hr);
    if (hr != S_OK)
        goto Cleanup;
    ok(EqualSid(Created, Derived), "the profile SID is not the derived SID\n");

    /* the registration: moniker as given, display name, and the keys of the AppContainer */
    ok(ReadString(Mapping, L"Moniker", Text, 600) && !wcscmp(Text, TEST_NAME), "Moniker is '%ls'\n", Text);
    ok(ReadString(Mapping, L"DisplayName", Text, 600) && !wcscmp(Text, TEST_DISPLAY), "DisplayName is '%ls'\n", Text);
    wcscat(Mapping, L"\\Children");
    ok(RegistryKeyExists(Mapping), "the Children key of the mapping does not exist\n");
    ok(RegistryKeyExists(Storage), "the Storage key does not exist\n");
    wcscat(Storage, L"\\Children");
    ok(RegistryKeyExists(Storage), "the Children key of the Storage key does not exist\n");

    /* the folders: Packages\<name>\AC and four folders below it */
    ok(IsDirectory(Expected), "%ls is not a folder\n", Expected);
    _snwprintf(Folder, MAX_PATH, L"%ls\\AC", Expected);
    ok(IsDirectory(Folder), "%ls is not a folder\n", Folder);
    for (i = 0; i < RTL_NUMBER_OF(SubFolders); i++)
    {
        DWORD Attributes;

        _snwprintf(Folder, MAX_PATH, L"%ls\\AC\\%ls", Expected, SubFolders[i]);
        Attributes = GetFileAttributesW(Folder);
        ok(Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY), "%ls is not a folder\n", Folder);
        if (i < 3)
            ok((Attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM),
               "%ls: attributes %#lx\n", Folder, Attributes);
    }

    /* the folder path is the AC folder; the caller frees it with CoTaskMemFree */
    Path = NULL;
    hr = pFolderPath(SidString, &Path);
    ok(hr == S_OK, "GetAppContainerFolderPath: hr %#lx\n", hr);
    if (hr == S_OK)
    {
        _snwprintf(Folder, MAX_PATH, L"%ls\\AC", Expected);
        ok(!wcsicmp(Path, Folder), "GetAppContainerFolderPath: '%ls', expected '%ls'\n", Path, Folder);
        CoTaskMemFree(Path);
    }

    /* the same name again: the result is left alone */
    hr = pCreate(TEST_NAME, TEST_DISPLAY, TEST_DESCRIPTION, &Capability, 1, &Again);
    ok(hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "second create: hr %#lx\n", hr);
    ok(Again == (PSID)(ULONG_PTR)0x66, "second create: the result was written\n");
    if (hr == S_OK)
        FreeSid(Again);

    /* delete removes everything, and deleting what is not there succeeds */
    hr = pDelete(TEST_NAME);
    ok(hr == S_OK, "DeleteAppContainerProfile: hr %#lx\n", hr);
    wcscpy(Mapping + wcslen(Mapping) - 9, L"");
    ok(!RegistryKeyExists(Mapping), "the mapping exists after the delete\n");
    ok(!RegistryKeyExists(Storage), "the Children key of the Storage key exists after the delete\n");
    _snwprintf(Storage, 200, APPCONTAINER_KEY L"Storage\\%ls", TEST_NAME);
    ok(!RegistryKeyExists(Storage), "the Storage key exists after the delete\n");
    ok(!IsDirectory(Expected), "%ls exists after the delete\n", Expected);
    Path = (PWSTR)(ULONG_PTR)0x33;
    hr = pFolderPath(SidString, &Path);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "GetAppContainerFolderPath after the delete: hr %#lx\n", hr);
    hr = pDelete(TEST_NAME);
    ok(hr == S_OK, "second delete: hr %#lx\n", hr);

Cleanup:
    pDelete(TEST_NAME);
    if (Created)
        FreeSid(Created);
    if (CapabilitySid)
        FreeSid(CapabilitySid);
    if (SidString)
        LocalFree(SidString);
    FreeSid(Derived);
}

START_TEST(AppContainerProfile)
{
    HMODULE Userenv = LoadLibraryW(L"userenv.dll");

    pCreate = (PFN_Create)GetProcAddress(Userenv, "CreateAppContainerProfile");
    pDelete = (PFN_Delete)GetProcAddress(Userenv, "DeleteAppContainerProfile");
    pDerive = (PFN_Derive)GetProcAddress(Userenv, "DeriveAppContainerSidFromAppContainerName");
    pFolderPath = (PFN_FolderPath)GetProcAddress(Userenv, "GetAppContainerFolderPath");
    pRegistryLocation = (PFN_RegistryLocation)GetProcAddress(Userenv, "GetAppContainerRegistryLocation");

    if (!pCreate || !pDelete || !pDerive || !pFolderPath || !pRegistryLocation)
    {
        /* Windows 7 has none of them; ReactOS exports them from export level 0x601 on */
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "the AppContainer profile functions are not all exported\n");
        skip("the AppContainer profile functions are not available\n");
        return;
    }

    TestWithoutProfile();
    TestInvalidNames();

    if (is_reactos() || GetEnvironmentVariableW(L"ROSAPITEST_APPCONTAINER_PROFILE", NULL, 0))
        TestRoundTrip();
    else
        skip("ROSAPITEST_APPCONTAINER_PROFILE is not set, not creating a profile\n");
}
