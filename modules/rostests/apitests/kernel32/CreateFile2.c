/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for CreateFile2
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/iofuncs.h>
#include <ndk/iotypes.h>

/* The test builds at NT 5.2 level, where the Windows 8 declarations can be hidden */
typedef struct
{
    DWORD dwSize;
    DWORD dwFileAttributes;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} TEST_CREATEFILE2_PARAMS;

typedef HANDLE (WINAPI *FN_CreateFile2)(LPCWSTR, DWORD, DWORD, DWORD, TEST_CREATEFILE2_PARAMS *);
static FN_CreateFile2 pCreateFile2;

static WCHAR TestFile[MAX_PATH];

static void RemoveTestFile(void)
{
    SetFileAttributesW(TestFile, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(TestFile);
}

static void MakeTestFile(DWORD Size)
{
    HANDLE hFile = CreateFileW(TestFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char Data[64] = "0123456789";
    DWORD Written;

    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    if (Size)
        WriteFile(hFile, Data, Size, &Written, NULL);
    CloseHandle(hFile);
}

/* Opens the file and describes it. Returns whether the open worked; Error is the last error right after the call. */
static BOOL OpenAndDescribe(LPCWSTR Name, DWORD Access, DWORD Share, DWORD Disposition,
                            TEST_CREATEFILE2_PARAMS *Params, PDWORD Error, PDWORD Size, PDWORD Attributes)
{
    BY_HANDLE_FILE_INFORMATION Info;
    HANDLE hFile;

    SetLastError(0xdeadbeef);
    hFile = pCreateFile2(Name, Access, Share, Disposition, Params);
    *Error = GetLastError();
    if (Size)
        *Size = 0;
    if (Attributes)
        *Attributes = 0;
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    ZeroMemory(&Info, sizeof(Info));
    ok(GetFileInformationByHandle(hFile, &Info), "GetFileInformationByHandle failed with %lu\n", GetLastError());
    if (Size)
        *Size = Info.nFileSizeLow;
    if (Attributes)
        *Attributes = Info.dwFileAttributes;
    CloseHandle(hFile);
    return TRUE;
}

static void TestDispositions(void)
{
    DWORD Size, Error;
    BOOL Opened;

    RemoveTestFile();

    /* Creating a file that is not there */
    Opened = OpenAndDescribe(TestFile, GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW, NULL, &Error, &Size, NULL);
    ok(Opened && Error == ERROR_SUCCESS && Size == 0, "CREATE_NEW on a missing file: %d, error %lu, size %lu\n", Opened, Error, Size);

    /* and one that is there */
    Opened = OpenAndDescribe(TestFile, GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW, NULL, &Error, &Size, NULL);
    ok(!Opened && Error == ERROR_FILE_EXISTS, "CREATE_NEW on an existing file: %d, error %lu\n", Opened, Error);

    MakeTestFile(10);
    Opened = OpenAndDescribe(TestFile, GENERIC_READ, 0, OPEN_EXISTING, NULL, &Error, &Size, NULL);
    ok(Opened && Error == ERROR_SUCCESS && Size == 10, "OPEN_EXISTING on an existing file: %d, error %lu, size %lu\n", Opened, Error, Size);

    Opened = OpenAndDescribe(TestFile, GENERIC_READ | GENERIC_WRITE, 0, OPEN_ALWAYS, NULL, &Error, &Size, NULL);
    ok(Opened && Error == ERROR_ALREADY_EXISTS && Size == 10, "OPEN_ALWAYS on an existing file: %d, error %lu, size %lu\n", Opened, Error, Size);

    Opened = OpenAndDescribe(TestFile, GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, NULL, &Error, &Size, NULL);
    ok(Opened && Error == ERROR_ALREADY_EXISTS && Size == 0, "CREATE_ALWAYS on an existing file: %d, error %lu, size %lu\n", Opened, Error, Size);

    MakeTestFile(10);
    Opened = OpenAndDescribe(TestFile, GENERIC_READ | GENERIC_WRITE, 0, TRUNCATE_EXISTING, NULL, &Error, &Size, NULL);
    ok(Opened && Error == ERROR_SUCCESS && Size == 0, "TRUNCATE_EXISTING on an existing file: %d, error %lu, size %lu\n", Opened, Error, Size);

    RemoveTestFile();
    Opened = OpenAndDescribe(TestFile, GENERIC_READ, 0, OPEN_EXISTING, NULL, &Error, &Size, NULL);
    ok(!Opened && Error == ERROR_FILE_NOT_FOUND, "OPEN_EXISTING on a missing file: %d, error %lu\n", Opened, Error);

    Opened = OpenAndDescribe(TestFile, GENERIC_READ | GENERIC_WRITE, 0, TRUNCATE_EXISTING, NULL, &Error, &Size, NULL);
    ok(!Opened && Error == ERROR_FILE_NOT_FOUND, "TRUNCATE_EXISTING on a missing file: %d, error %lu\n", Opened, Error);

    Opened = OpenAndDescribe(TestFile, GENERIC_READ | GENERIC_WRITE, 0, OPEN_ALWAYS, NULL, &Error, &Size, NULL);
    ok(Opened && Error == ERROR_SUCCESS && Size == 0, "OPEN_ALWAYS on a missing file: %d, error %lu, size %lu\n", Opened, Error, Size);
    RemoveTestFile();

    Opened = OpenAndDescribe(TestFile, GENERIC_READ, 0, 0, NULL, &Error, &Size, NULL);
    ok(!Opened && Error == ERROR_INVALID_PARAMETER, "Disposition 0: %d, error %lu\n", Opened, Error);
    Opened = OpenAndDescribe(TestFile, GENERIC_READ, 0, 6, NULL, &Error, &Size, NULL);
    ok(!Opened && Error == ERROR_INVALID_PARAMETER, "Disposition 6: %d, error %lu\n", Opened, Error);
    ok(GetFileAttributesW(TestFile) == INVALID_FILE_ATTRIBUTES, "A bad disposition created the file\n");
}

static void TestParameterSize(void)
{
    TEST_CREATEFILE2_PARAMS Params;
    DWORD Error, Size;
    DWORD Sizes[] = { 0, 1, sizeof(Params) - 1 };
    DWORD Good[] = { sizeof(Params), sizeof(Params) + 1, sizeof(Params) + 8 };
    BOOL Opened;
    int i;

    MakeTestFile(10);

    for (i = 0; i < _countof(Sizes); i++)
    {
        ZeroMemory(&Params, sizeof(Params));
        Params.dwSize = Sizes[i];
        Opened = OpenAndDescribe(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params, &Error, &Size, NULL);
        ok(!Opened && Error == ERROR_INVALID_PARAMETER, "dwSize %lu: %d, error %lu\n", Sizes[i], Opened, Error);
    }

    /* A structure of a later Windows is larger and accepted */
    for (i = 0; i < _countof(Good); i++)
    {
        ZeroMemory(&Params, sizeof(Params));
        Params.dwSize = Good[i];
        Opened = OpenAndDescribe(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params, &Error, &Size, NULL);
        ok(Opened && Error == ERROR_SUCCESS && Size == 10, "dwSize %lu: %d, error %lu, size %lu\n", Good[i], Opened, Error, Size);
    }

    RemoveTestFile();
}

static void TestAttributes(void)
{
    static const struct
    {
        DWORD Requested;
        DWORD Mask;
        DWORD Expected;
    } Cases[] =
    {
        { FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_HIDDEN },
        { FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_READONLY },
        { FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_SYSTEM },
        { FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM, 0 },
        /* Attributes that a new file cannot have are ignored */
        { FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED, 0 },
        { FILE_ATTRIBUTE_COMPRESSED, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED, 0 },
        { FILE_ATTRIBUTE_REPARSE_POINT, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT, 0 },
    };
    TEST_CREATEFILE2_PARAMS Params;
    DWORD Attributes, Error;
    BOOL Opened;
    int i;

    for (i = 0; i < _countof(Cases); i++)
    {
        RemoveTestFile();
        ZeroMemory(&Params, sizeof(Params));
        Params.dwSize = sizeof(Params);
        Params.dwFileAttributes = Cases[i].Requested;
        Opened = OpenAndDescribe(TestFile, GENERIC_WRITE, 0, CREATE_NEW, &Params, &Error, NULL, &Attributes);
        ok(Opened && Error == ERROR_SUCCESS, "Attributes %#lx: %d, error %lu\n", Cases[i].Requested, Opened, Error);
        ok((Attributes & Cases[i].Mask) == Cases[i].Expected,
           "Attributes %#lx gave %#lx\n", Cases[i].Requested, Attributes);
    }

    RemoveTestFile();
}

static DWORD QueryMode(HANDLE hFile)
{
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_MODE_INFORMATION Mode = { 0xdeadbeef };
    NTSTATUS Status = NtQueryInformationFile(hFile, &IoStatusBlock, &Mode, sizeof(Mode), FileModeInformation);

    ok(NT_SUCCESS(Status), "NtQueryInformationFile failed with %#lx\n", Status);
    return Mode.Mode;
}

static void TestFlags(void)
{
    TEST_CREATEFILE2_PARAMS Params;
    HANDLE hFile;
    OVERLAPPED Overlapped;
    char Buffer[4];
    DWORD Read, Error, Attributes, Mode;
    BOOL Ret;

    /* FILE_FLAG_DELETE_ON_CLOSE is taken from dwFileFlags */
    RemoveTestFile();
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwFileAttributes = FILE_ATTRIBUTE_HIDDEN;
    Params.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
    hFile = pCreateFile2(TestFile, GENERIC_WRITE, 0, CREATE_NEW, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        Attributes = GetFileAttributesW(TestFile);
        ok(Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_HIDDEN),
           "Attributes of the open file are %#lx\n", Attributes);
        CloseHandle(hFile);
    }
    ok(GetFileAttributesW(TestFile) == INVALID_FILE_ATTRIBUTES, "The file is still there after the close\n");

    /* A flag in dwFileAttributes is not a flag */
    MakeTestFile(10);
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwFileAttributes = FILE_FLAG_DELETE_ON_CLOSE;
    hFile = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    ok(GetFileAttributesW(TestFile) != INVALID_FILE_ATTRIBUTES, "A flag in dwFileAttributes deleted the file\n");

    /* and an attribute in dwFileFlags is not an attribute */
    RemoveTestFile();
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwFileFlags = FILE_ATTRIBUTE_HIDDEN;
    hFile = pCreateFile2(TestFile, GENERIC_WRITE, 0, CREATE_NEW, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    Attributes = GetFileAttributesW(TestFile);
    ok(Attributes != INVALID_FILE_ATTRIBUTES && !(Attributes & FILE_ATTRIBUTE_HIDDEN),
       "An attribute in dwFileFlags made the file %#lx\n", Attributes);
    MakeTestFile(10);

    /* An overlapped handle is not a synchronous one, and completes a read through its OVERLAPPED */
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwFileFlags = FILE_FLAG_OVERLAPPED;
    hFile = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        Mode = QueryMode(hFile);
        ok(!(Mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)), "The handle is synchronous: mode %#lx\n", Mode);
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        SetLastError(0xdeadbeef);
        Ret = ReadFile(hFile, Buffer, sizeof(Buffer), &Read, &Overlapped);
        Error = GetLastError();
        ok(Ret || Error == ERROR_IO_PENDING, "ReadFile failed with %lu\n", Error);
        Ret = GetOverlappedResult(hFile, &Overlapped, &Read, TRUE);
        ok(Ret && Read == sizeof(Buffer), "GetOverlappedResult: %d, %lu bytes\n", Ret, Read);
        CloseHandle(Overlapped.hEvent);
        CloseHandle(hFile);
    }

    /* and a handle without the flag is a synchronous one and reads synchronously */
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    hFile = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        Mode = QueryMode(hFile);
        ok(Mode & FILE_SYNCHRONOUS_IO_NONALERT, "The handle is not synchronous: mode %#lx\n", Mode);
        Ret = ReadFile(hFile, Buffer, sizeof(Buffer), &Read, NULL);
        ok(Ret && Read == sizeof(Buffer), "ReadFile: %d, %lu bytes\n", Ret, Read);
        CloseHandle(hFile);
    }

    RemoveTestFile();
}

static void TestSharingAndSecurity(void)
{
    TEST_CREATEFILE2_PARAMS Params;
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, TRUE };
    HANDLE hFirst, hFile;
    DWORD Flags, Error;

    MakeTestFile(10);

    hFirst = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    ok(hFirst != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFirst != INVALID_HANDLE_VALUE)
    {
        SetLastError(0xdeadbeef);
        hFile = pCreateFile2(TestFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, NULL);
        Error = GetLastError();
        ok(hFile == INVALID_HANDLE_VALUE && Error == ERROR_SHARING_VIOLATION,
           "A write open next to a read-only sharer: %p, error %lu\n", hFile, Error);
        if (hFile != INVALID_HANDLE_VALUE)
            CloseHandle(hFile);

        hFile = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
        ok(hFile != INVALID_HANDLE_VALUE, "A second reader failed with %lu\n", GetLastError());
        if (hFile != INVALID_HANDLE_VALUE)
            CloseHandle(hFile);
        CloseHandle(hFirst);
    }

    /* The security attributes decide on inheritance */
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.lpSecurityAttributes = &Sa;
    hFile = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        Flags = 0;
        GetHandleInformation(hFile, &Flags);
        ok(Flags & HANDLE_FLAG_INHERIT, "The handle is not inheritable\n");
        CloseHandle(hFile);
    }

    Sa.bInheritHandle = FALSE;
    hFile = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        Flags = 0;
        GetHandleInformation(hFile, &Flags);
        ok(!(Flags & HANDLE_FLAG_INHERIT), "The handle is inheritable\n");
        CloseHandle(hFile);
    }

    /* Every quality of service value is accepted for a file */
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwSecurityQosFlags = SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION;
    hFile = pCreateFile2(TestFile, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 with a quality of service failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    /* A usable template gives the new file no attributes */
    {
        WCHAR Other[MAX_PATH];
        HANDLE hTemplate;

        StringCchPrintfW(Other, _countof(Other), L"%s.template", TestFile);
        SetFileAttributesW(Other, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(Other);
        MakeTestFile(10);
        SetFileAttributesW(TestFile, FILE_ATTRIBUTE_HIDDEN);
        hTemplate = CreateFileW(TestFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        ok(hTemplate != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
        ZeroMemory(&Params, sizeof(Params));
        Params.dwSize = sizeof(Params);
        Params.hTemplateFile = hTemplate;
        hFile = pCreateFile2(Other, GENERIC_WRITE, 0, CREATE_NEW, &Params);
        ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 with a template failed with %lu\n", GetLastError());
        if (hFile != INVALID_HANDLE_VALUE)
            CloseHandle(hFile);
        Flags = GetFileAttributesW(Other);
        ok(Flags != INVALID_FILE_ATTRIBUTES && !(Flags & FILE_ATTRIBUTE_HIDDEN), "The new file has the attributes %#lx\n", Flags);
        if (hTemplate != INVALID_HANDLE_VALUE)
            CloseHandle(hTemplate);
        SetFileAttributesW(Other, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(Other);
    }
    SetFileAttributesW(TestFile, FILE_ATTRIBUTE_NORMAL);

    /* The template handle is ignored when it is not usable */
    RemoveTestFile();
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.hTemplateFile = INVALID_HANDLE_VALUE;
    hFile = pCreateFile2(TestFile, GENERIC_WRITE, 0, CREATE_NEW, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFile2 with an invalid template failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    RemoveTestFile();
}

/* The impersonation level the server of a pipe gets for a client opened with the given quality of service; FALSE
   when the server cannot get a token, which also happens for the anonymous level */
static BOOL ImpersonationLevelOf(BOOL UseCreateFile2, DWORD Quality, PDWORD Level)
{
    static const WCHAR PipeName[] = L"\\\\.\\pipe\\CreateFile2-qos-test";
    TEST_CREATEFILE2_PARAMS Params;
    HANDLE hServer, hClient, hToken = NULL;
    DWORD Written, Read, Length = 0;
    char Byte;
    BOOL Ok = FALSE;

    *Level = 0xdeadbeef;
    hServer = CreateNamedPipeW(PipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    ok(hServer != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed with %lu\n", GetLastError());
    if (hServer == INVALID_HANDLE_VALUE)
        return FALSE;

    if (UseCreateFile2)
    {
        ZeroMemory(&Params, sizeof(Params));
        Params.dwSize = sizeof(Params);
        Params.dwSecurityQosFlags = SECURITY_SQOS_PRESENT | Quality;
        hClient = pCreateFile2(PipeName, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, &Params);
    }
    else
    {
        hClient = CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | Quality, NULL);
    }
    ok(hClient != INVALID_HANDLE_VALUE, "Opening the pipe failed with %lu\n", GetLastError());
    if (hClient != INVALID_HANDLE_VALUE)
    {
        WriteFile(hClient, "x", 1, &Written, NULL);
        ReadFile(hServer, &Byte, 1, &Read, NULL);
        if (ImpersonateNamedPipeClient(hServer))
        {
            if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hToken))
            {
                Ok = GetTokenInformation(hToken, TokenImpersonationLevel, Level, sizeof(*Level), &Length);
                CloseHandle(hToken);
            }
            RevertToSelf();
        }
        CloseHandle(hClient);
    }
    CloseHandle(hServer);
    return Ok;
}

static void TestSecurityQuality(void)
{
    static const struct
    {
        DWORD Quality;
        DWORD Expected;
    } Cases[] =
    {
        { SECURITY_IDENTIFICATION, SecurityIdentification },
        { SECURITY_IMPERSONATION, SecurityImpersonation },
        { SECURITY_DELEGATION, SecurityDelegation },
    };
    DWORD Level2, LevelW;
    BOOL Ok2, OkW;
    int i;

    for (i = 0; i < _countof(Cases); i++)
    {
        OkW = ImpersonationLevelOf(FALSE, Cases[i].Quality, &LevelW);
        Ok2 = ImpersonationLevelOf(TRUE, Cases[i].Quality, &Level2);
        if (!OkW)
        {
            skip("The server gets no token for quality %#lx\n", Cases[i].Quality);
            continue;
        }
        ok(Ok2, "No token for quality %#lx\n", Cases[i].Quality);
        ok(Level2 == LevelW, "Quality %#lx gave level %lu, CreateFileW gives %lu\n", Cases[i].Quality, Level2, LevelW);
        if (!is_reactos())
            ok(Level2 == Cases[i].Expected, "Quality %#lx gave level %lu\n", Cases[i].Quality, Level2);
    }
}

static void TestNamesAndDirectories(void)
{
    TEST_CREATEFILE2_PARAMS Params;
    WCHAR Dir[MAX_PATH], Missing[MAX_PATH];
    HANDLE hFile;
    DWORD Attributes;

    SetLastError(0xdeadbeef);
    hFile = pCreateFile2(NULL, GENERIC_READ, 0, OPEN_EXISTING, NULL);
    ok(hFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
       "A NULL name: %p, error %lu\n", hFile, GetLastError());

    SetLastError(0xdeadbeef);
    hFile = pCreateFile2(L"", GENERIC_READ, 0, OPEN_EXISTING, NULL);
    ok(hFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
       "An empty name: %p, error %lu\n", hFile, GetLastError());

    StringCchPrintfW(Missing, _countof(Missing), L"%s.missing\\file.txt", TestFile);
    SetLastError(0xdeadbeef);
    hFile = pCreateFile2(Missing, GENERIC_WRITE, 0, CREATE_NEW, NULL);
    ok(hFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
       "A missing directory: %p, error %lu\n", hFile, GetLastError());

    /* A directory needs backup semantics */
    StringCchPrintfW(Dir, _countof(Dir), L"%s.dir", TestFile);
    RemoveDirectoryW(Dir);
    ok(CreateDirectoryW(Dir, NULL), "CreateDirectoryW failed with %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    hFile = pCreateFile2(Dir, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    ok(hFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
       "A directory without backup semantics: %p, error %lu\n", hFile, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    /* CreateFileW reports the same, and a trailing backslash makes it a missing path */
    SetLastError(0xdeadbeef);
    hFile = CreateFileW(Dir, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
       "CreateFileW on a directory: %p, error %lu\n", hFile, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    StringCchCatW(Dir, _countof(Dir), L"\\");
    SetLastError(0xdeadbeef);
    hFile = CreateFileW(Dir, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
       "CreateFileW on a directory with a trailing backslash: %p, error %lu\n", hFile, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    Dir[lstrlenW(Dir) - 1] = UNICODE_NULL;

    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    Params.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS;
    hFile = pCreateFile2(Dir, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &Params);
    ok(hFile != INVALID_HANDLE_VALUE, "A directory with backup semantics failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        BY_HANDLE_FILE_INFORMATION Info;

        ZeroMemory(&Info, sizeof(Info));
        GetFileInformationByHandle(hFile, &Info);
        Attributes = Info.dwFileAttributes;
        ok(Attributes & FILE_ATTRIBUTE_DIRECTORY, "The handle is not of a directory: %#lx\n", Attributes);
        CloseHandle(hFile);
    }

    RemoveDirectoryW(Dir);
}

START_TEST(CreateFile2)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    WCHAR TempDir[MAX_PATH];

    pCreateFile2 = (FN_CreateFile2)GetProcAddress(hKernel32, "CreateFile2");
    if (!pCreateFile2)
    {
        ok(!is_reactos(), "CreateFile2 is missing\n");
        skip("CreateFile2 is not available\n");
        return;
    }

    GetTempPathW(_countof(TempDir), TempDir);
    StringCchPrintfW(TestFile, _countof(TestFile), L"%sCreateFile2-%lu.tmp", TempDir, GetCurrentProcessId());
    RemoveTestFile();

    TestDispositions();
    TestParameterSize();
    TestAttributes();
    TestFlags();
    TestSharingAndSecurity();
    TestSecurityQuality();
    TestNamesAndDirectories();

    RemoveTestFile();
}
