/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests that EncryptFile and FileEncryptionStatus report the real encryption state
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* Windows on a file system without encryption support (FAT on Windows Server 2008 R2)
 * fails EncryptFile with ERROR_INVALID_PARAMETER or ERROR_ACCESS_DENIED; ReactOS uses
 * ERROR_NOT_SUPPORTED. */
static BOOL IsNoEfsError(DWORD Error)
{
    if (is_reactos())
        return Error == ERROR_NOT_SUPPORTED;
    return Error == ERROR_INVALID_PARAMETER || Error == ERROR_ACCESS_DENIED || Error == ERROR_NOT_SUPPORTED;
}

static BOOL IsEncrypted(LPCWSTR Path)
{
    DWORD Attributes = GetFileAttributesW(Path);
    ok(Attributes != INVALID_FILE_ATTRIBUTES, "GetFileAttributesW(%S) failed with %lu\n", Path, GetLastError());
    return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_ENCRYPTED);
}

static void MakeFile(LPCWSTR Path)
{
    HANDLE hFile;
    DWORD Written;

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed with %lu\n", Path, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    WriteFile(hFile, "plaintext\r\n", 11, &Written, NULL);
    CloseHandle(hFile);
    ok(!IsEncrypted(Path), "%S is encrypted before any encryption call\n", Path);
}

static void CheckStatus(LPCWSTR Path, BOOL Encrypted)
{
    DWORD Status = 0xdeadbeef;
    BOOL Ret;

    Ret = FileEncryptionStatusW(Path, &Status);
    ok(Ret, "FileEncryptionStatusW(%S) failed with %lu\n", Path, GetLastError());
    ok(Status <= FILE_DIR_DISALLOWED, "Status = %lu\n", Status);
    if (Encrypted)
        ok(Status == FILE_IS_ENCRYPTED, "Status = %lu for an encrypted file\n", Status);
    else
        ok(Status != FILE_IS_ENCRYPTED, "Status = FILE_IS_ENCRYPTED for a file that is not encrypted\n");
    if (!Encrypted && is_reactos())
        ok(Status == FILE_SYSTEM_NOT_SUPPORT, "Status = %lu, expected FILE_SYSTEM_NOT_SUPPORT\n", Status);
}

/* A successful EncryptFile must leave the file encrypted; a failed one must leave it plain.
 * Returns whether the file was encrypted. */
static BOOL CheckEncrypt(LPCWSTR Path, BOOL Ret, DWORD Error, BOOL VolumeEncrypts, BOOL MustWork, const char *Name)
{
    BOOL Encrypted = IsEncrypted(Path);

    if (Ret)
    {
        ok(Encrypted, "%s reported success, but the file is not encrypted\n", Name);
        ok(VolumeEncrypts, "%s reported success on a volume without FILE_SUPPORTS_ENCRYPTION\n", Name);
    }
    else
    {
        ok(!Encrypted, "%s failed with %lu, but the file is encrypted\n", Name, Error);
        ok(!MustWork, "%s failed with %lu where EncryptFileW works\n", Name, Error);
        if (!VolumeEncrypts)
            ok(IsNoEfsError(Error), "%s failed with %lu\n", Name, Error);
        else
            trace("%s failed with %lu on a volume with FILE_SUPPORTS_ENCRYPTION\n", Name, Error);
    }
    CheckStatus(Path, Encrypted);

    if (Encrypted)
    {
        ok(DecryptFileW(Path, 0), "DecryptFileW failed with %lu\n", GetLastError());
        ok(!IsEncrypted(Path), "The file is still encrypted after DecryptFileW\n");
    }
    return Encrypted;
}

static void TestInDirectory(LPCWSTR Dir)
{
    WCHAR File[MAX_PATH], Missing[MAX_PATH], Volume[MAX_PATH], FsName[32], Check[MAX_PATH];
    CHAR FileA[MAX_PATH];
    DWORD Flags = 0, Status, Error;
    BOOL Ret, VolumeEncrypts, EfsWorks, Lossy;
    HANDLE hFile;

    if (!GetVolumePathNameW(Dir, Volume, _countof(Volume)) ||
        !GetVolumeInformationW(Volume, NULL, 0, NULL, NULL, &Flags, FsName, _countof(FsName)))
    {
        skip("Cannot query the volume of %S: %lu\n", Dir, GetLastError());
        return;
    }
    VolumeEncrypts = (Flags & FILE_SUPPORTS_ENCRYPTION) != 0;
    trace("Testing in %S (%S, file system flags 0x%lx)\n", Dir, FsName, Flags);

    /* EncryptFileW */
    StringCbPrintfW(File, sizeof(File), L"%s\\plain.txt", Dir);
    MakeFile(File);
    SetLastError(0xdeadbeef);
    Ret = EncryptFileW(File);
    Error = GetLastError();
    EfsWorks = CheckEncrypt(File, Ret, Error, VolumeEncrypts, FALSE, "EncryptFileW");

    /* EncryptFileA, if the path survives the conversion to the ANSI code page */
    StringCbPrintfW(File, sizeof(File), L"%s\\plainA.txt", Dir);
    MakeFile(File);
    Lossy = FALSE;
    if (WideCharToMultiByte(CP_ACP, 0, File, -1, FileA, sizeof(FileA), NULL, &Lossy) &&
        !Lossy &&
        MultiByteToWideChar(CP_ACP, 0, FileA, -1, Check, _countof(Check)) &&
        !wcscmp(Check, File))
    {
        SetLastError(0xdeadbeef);
        Ret = EncryptFileA(FileA);
        Error = GetLastError();
        CheckEncrypt(File, Ret, Error, VolumeEncrypts, EfsWorks, "EncryptFileA");
    }
    else
    {
        skip("%S has no ANSI form\n", File);
    }

    /* A plain file: DecryptFile has nothing to do and succeeds */
    StringCbPrintfW(File, sizeof(File), L"%s\\plain.txt", Dir);
    ok(DecryptFileW(File, 0), "DecryptFileW on a plain file failed with %lu\n", GetLastError());
    CheckStatus(File, FALSE);

    /* A read-only file */
    StringCbPrintfW(File, sizeof(File), L"%s\\readonly.txt", Dir);
    MakeFile(File);
    ok(SetFileAttributesW(File, FILE_ATTRIBUTE_READONLY), "SetFileAttributesW failed with %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!EncryptFileW(File), "EncryptFileW on a read-only file succeeded\n");
    ok_err(ERROR_FILE_READ_ONLY);
    ok(!IsEncrypted(File), "The read-only file is encrypted\n");
    SetFileAttributesW(File, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(File);

    /* A missing file */
    StringCbPrintfW(Missing, sizeof(Missing), L"%s\\missing.txt", Dir);
    SetLastError(0xdeadbeef);
    Ret = EncryptFileW(Missing);
    Error = GetLastError();
    ok(!Ret, "EncryptFileW on a missing file succeeded\n");
    /* Windows 10 gives ERROR_ACCESS_DENIED */
    ok(Error == ERROR_FILE_NOT_FOUND || Error == ERROR_ACCESS_DENIED, "EncryptFileW on a missing file: error %lu\n", Error);
    SetLastError(0xdeadbeef);
    ok(!DecryptFileW(Missing, 0), "DecryptFileW on a missing file succeeded\n");
    ok_err(ERROR_FILE_NOT_FOUND);
    Status = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    ok(!FileEncryptionStatusW(Missing, &Status), "FileEncryptionStatusW on a missing file succeeded, status %lu\n", Status);
    ok_err(ERROR_FILE_NOT_FOUND);

    /* CreateFile with FILE_ATTRIBUTE_ENCRYPTED encrypts where EncryptFile does; elsewhere it is ignored */
    StringCbPrintfW(File, sizeof(File), L"%s\\created.txt", Dir);
    hFile = CreateFileW(File, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_ENCRYPTED, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW with FILE_ATTRIBUTE_ENCRYPTED failed with %lu\n", GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        BOOL Encrypted;

        CloseHandle(hFile);
        Encrypted = IsEncrypted(File);
        ok(Encrypted || !EfsWorks, "EncryptFileW works here, but the created file is not encrypted\n");
        ok(!Encrypted || VolumeEncrypts, "The created file is encrypted on a volume without FILE_SUPPORTS_ENCRYPTION\n");
        CheckStatus(File, Encrypted);
        if (Encrypted)
            DecryptFileW(File, 0);
        DeleteFileW(File);
    }

    StringCbPrintfW(File, sizeof(File), L"%s\\plain.txt", Dir);
    DeleteFileW(File);
    StringCbPrintfW(File, sizeof(File), L"%s\\plainA.txt", Dir);
    DeleteFileW(File);
}

START_TEST(EncryptFile)
{
    WCHAR TempPath[MAX_PATH], Dir[MAX_PATH];
    DWORD Length, Attributes;

    Length = GetTempPathW(_countof(TempPath), TempPath);
    if (Length == 0 || Length >= _countof(TempPath) ||
        FAILED(StringCbPrintfW(Dir, sizeof(Dir), L"%sEncryptFile-%lu-%lu", TempPath, GetCurrentProcessId(), GetTickCount())) ||
        wcslen(Dir) + 16 >= _countof(Dir))
    {
        skip("No usable temporary directory\n");
        return;
    }
    if (!CreateDirectoryW(Dir, NULL))
    {
        skip("Cannot create %S: %lu\n", Dir, GetLastError());
        return;
    }

    /* Files in an encrypted directory start encrypted; the checks need plain files */
    Attributes = GetFileAttributesW(Dir);
    if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_ENCRYPTED))
        skip("%S is encrypted\n", Dir);
    else
        TestInDirectory(Dir);

    ok(RemoveDirectoryW(Dir), "RemoveDirectoryW failed with %lu\n", GetLastError());
}
