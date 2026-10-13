/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the rename paths of SetFileInformationByHandle
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* The test builds at NT 5.2 level, where the Vista types are hidden */
enum
{
    TestFileStandardInfo = 1,
    TestFileRenameInfo = 3,
    TestFileDispositionInfo = 4,
    TestFileEndOfFileInfo = 6
};

typedef struct
{
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
} TEST_RENAME_INFO;

typedef struct
{
    LARGE_INTEGER EndOfFile;
} TEST_END_OF_FILE_INFO;

typedef struct
{
    BOOLEAN Delete;
} TEST_DISPOSITION_INFO;

typedef BOOL (WINAPI *FN_SetFileInformationByHandle)(HANDLE, INT, LPVOID, DWORD);
static FN_SetFileInformationByHandle pSetFileInformationByHandle;

static WCHAR TestDir[MAX_PATH];

static BOOL FileExists(PCWSTR Name)
{
    WCHAR Path[MAX_PATH];
    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", TestDir, Name);
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

static HANDLE OpenTestFile(PCWSTR Name, DWORD Disposition)
{
    WCHAR Path[MAX_PATH];
    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", TestDir, Name);
    return CreateFileW(Path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, Disposition, 0, NULL);
}

/* Renames the open file to Name, with the name length in bytes adjusted by Delta */
static BOOL RenameTo(HANDLE File, PCWSTR Name, BOOL Replace, INT Delta)
{
    struct
    {
        TEST_RENAME_INFO Info;
        WCHAR Buffer[MAX_PATH];
    } Rename;
    DWORD Length = (DWORD)wcslen(Name) * sizeof(WCHAR);

    ZeroMemory(&Rename, sizeof(Rename));
    Rename.Info.ReplaceIfExists = Replace;
    Rename.Info.RootDirectory = NULL;
    Rename.Info.FileNameLength = Length + Delta;
    CopyMemory(Rename.Info.FileName, Name, Length);
    return pSetFileInformationByHandle(File, TestFileRenameInfo, &Rename, sizeof(Rename));
}

static void Test_Rename(void)
{
    WCHAR FullName[MAX_PATH];
    HANDLE File;
    BOOL Ret;

    File = OpenTestFile(L"a.txt", CREATE_ALWAYS);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;

    /* A plain name renames within the directory */
    Ret = RenameTo(File, L"b.txt", FALSE, 0);
    ok(Ret, "Rename to a plain name failed: %lu\n", GetLastError());
    ok(!FileExists(L"a.txt") && FileExists(L"b.txt"), "Plain-name rename did not happen\n");

    /* A full DOS path */
    StringCchPrintfW(FullName, _countof(FullName), L"%s\\c.txt", TestDir);
    Ret = RenameTo(File, FullName, FALSE, 0);
    ok(Ret, "Rename to a full path failed: %lu\n", GetLastError());
    ok(!FileExists(L"b.txt") && FileExists(L"c.txt"), "Full-path rename did not happen\n");

    /* An odd byte count is invalid and must not rename to a shortened name */
    StringCchPrintfW(FullName, _countof(FullName), L"%s\\d.txtx", TestDir);
    SetLastError(0xdeadbeef);
    Ret = RenameTo(File, FullName, FALSE, -1);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER,
       "Odd full-path length: got %d, error %lu\n", Ret, GetLastError());
    ok(FileExists(L"c.txt") && !FileExists(L"d.txt") && !FileExists(L"d.txtx"),
       "Odd full-path length changed the file name\n");

    SetLastError(0xdeadbeef);
    Ret = RenameTo(File, L"e.txtx", FALSE, -1);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER,
       "Odd plain-name length: got %d, error %lu\n", Ret, GetLastError());
    ok(FileExists(L"c.txt") && !FileExists(L"e.txt") && !FileExists(L"e.txtx"),
       "Odd plain-name length changed the file name\n");

    /* An existing target needs ReplaceIfExists */
    CloseHandle(OpenTestFile(L"f.txt", CREATE_ALWAYS));
    SetLastError(0xdeadbeef);
    Ret = RenameTo(File, L"f.txt", FALSE, 0);
    ok(!Ret && GetLastError() == ERROR_ALREADY_EXISTS, "Got %d, error %lu\n", Ret, GetLastError());
    Ret = RenameTo(File, L"f.txt", TRUE, 0);
    ok(Ret, "Replacing rename failed: %lu\n", GetLastError());
    ok(!FileExists(L"c.txt") && FileExists(L"f.txt"), "Replacing rename did not happen\n");

    CloseHandle(File);
}

static void Test_DispositionAndEndOfFile(void)
{
    TEST_END_OF_FILE_INFO EndOfFile;
    TEST_DISPOSITION_INFO Disposition;
    LARGE_INTEGER Size;
    HANDLE File;
    BOOL Ret;

    File = OpenTestFile(L"g.txt", CREATE_ALWAYS);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;

    EndOfFile.EndOfFile.QuadPart = 12345;
    Ret = pSetFileInformationByHandle(File, TestFileEndOfFileInfo, &EndOfFile, sizeof(EndOfFile));
    ok(Ret, "FileEndOfFileInfo failed: %lu\n", GetLastError());
    ok(GetFileSizeEx(File, &Size) && Size.QuadPart == 12345, "Size is %I64d\n", Size.QuadPart);

    Disposition.Delete = TRUE;
    Ret = pSetFileInformationByHandle(File, TestFileDispositionInfo, &Disposition, sizeof(Disposition));
    ok(Ret, "FileDispositionInfo failed: %lu\n", GetLastError());
    CloseHandle(File);
    ok(!FileExists(L"g.txt"), "File was not deleted on close\n");

    SetLastError(0xdeadbeef);
    Ret = pSetFileInformationByHandle(INVALID_HANDLE_VALUE, TestFileStandardInfo, &Disposition, sizeof(Disposition));
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "Got %d, error %lu\n", Ret, GetLastError());
}

START_TEST(SetFileInformationByHandle)
{
    WCHAR TempPath[MAX_PATH], Path[MAX_PATH];
    static const PCWSTR Names[] = { L"a.txt", L"b.txt", L"c.txt", L"d.txt", L"d.txtx",
                                    L"e.txt", L"e.txtx", L"f.txt", L"g.txt" };
    ULONG i;

    pSetFileInformationByHandle = (FN_SetFileInformationByHandle)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetFileInformationByHandle");
    if (!pSetFileInformationByHandle)
    {
        skip("SetFileInformationByHandle is not available\n");
        return;
    }

    GetTempPathW(_countof(TempPath), TempPath);
    StringCchPrintfW(TestDir, _countof(TestDir), L"%sSetFileInfoByHandle%lu", TempPath, GetCurrentProcessId());
    ok(CreateDirectoryW(TestDir, NULL), "CreateDirectoryW failed: %lu\n", GetLastError());

    Test_Rename();
    Test_DispositionAndEndOfFile();

    for (i = 0; i < _countof(Names); i++)
    {
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", TestDir, Names[i]);
        DeleteFileW(Path);
    }
    RemoveDirectoryW(TestDir);
}
