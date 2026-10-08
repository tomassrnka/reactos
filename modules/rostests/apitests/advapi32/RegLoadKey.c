/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for loading damaged hives with RegLoadKeyW
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define LOAD_TIMEOUT 30000

typedef struct _LOAD_CONTEXT
{
    WCHAR SubKey[64];
    WCHAR File[MAX_PATH];
    LONG Result;
} LOAD_CONTEXT, *PLOAD_CONTEXT;

static BOOL EnablePrivilege(PCWSTR Name)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Privileges;
    BOOL Success;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
        return FALSE;

    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Success = LookupPrivilegeValueW(NULL, Name, &Privileges.Privileges[0].Luid) &&
              AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(Token);
    return Success;
}

static DWORD WINAPI LoadThread(PVOID Parameter)
{
    PLOAD_CONTEXT Context = Parameter;

    Context->Result = RegLoadKeyW(HKEY_LOCAL_MACHINE, Context->SubKey, Context->File);
    return 0;
}

static void DeleteHiveFiles(PCWSTR File)
{
    static PCWSTR Suffixes[] = { L"", L".LOG", L".LOG1", L".LOG2" };
    WCHAR Name[MAX_PATH + 8];
    ULONG i;

    for (i = 0; i < _countof(Suffixes); i++)
    {
        StringCchPrintfW(Name, _countof(Name), L"%s%s", File, Suffixes[i]);
        DeleteFileW(Name);
    }
}

/* Saves a small key as a hive file and returns its contents */
static PBYTE SaveTestHive(PCWSTR File, PDWORD Size)
{
    HKEY Key;
    LONG Error;
    DWORD Data = 0x12345678, Read;
    HANDLE Handle;
    PBYTE Buffer;

    DeleteHiveFiles(File);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\ReactOSTestHiveSource");
    Error = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ReactOSTestHiveSource", 0, NULL, 0,
                            KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "RegCreateKeyExW failed: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return NULL;
    Error = RegSetValueExW(Key, L"Value", 0, REG_DWORD, (PBYTE)&Data, sizeof(Data));
    ok(Error == ERROR_SUCCESS, "RegSetValueExW failed: %ld\n", Error);
    Error = RegSaveKeyW(Key, File, NULL);
    ok(Error == ERROR_SUCCESS, "RegSaveKeyW failed: %ld\n", Error);
    RegCloseKey(Key);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\ReactOSTestHiveSource");
    if (Error != ERROR_SUCCESS)
        return NULL;

    Handle = CreateFileW(File, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return NULL;
    *Size = GetFileSize(Handle, NULL);
    Buffer = HeapAlloc(GetProcessHeap(), 0, *Size);
    if (Buffer && (!ReadFile(Handle, Buffer, *Size, &Read, NULL) || Read != *Size))
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        Buffer = NULL;
    }
    CloseHandle(Handle);
    ok(Buffer != NULL, "Reading the saved hive failed\n");
    return Buffer;
}

static BOOL WriteTestHive(PCWSTR File, PBYTE Buffer, DWORD Size)
{
    HANDLE Handle;
    DWORD Written;
    BOOL Success;

    Handle = CreateFileW(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    Success = WriteFile(Handle, Buffer, Size, &Written, NULL) && Written == Size;
    CloseHandle(Handle);
    return Success;
}

/* The first bin follows the 4 KB base block: "hbin", file offset, size, then cells from offset 0x20 */
#define BIN_OFFSET 0x1000

static PLONG FindFreeCell(PBYTE Buffer, DWORD Size)
{
    PBYTE Bin = Buffer + BIN_OFFSET;
    DWORD BinSize, Offset;
    LONG CellSize;

    if (Size < BIN_OFFSET + 0x1000 || memcmp(Bin, "hbin", 4) != 0)
        return NULL;
    BinSize = *(PDWORD)(Bin + 8);
    if (BinSize < 0x1000 || BinSize > Size - BIN_OFFSET)
        return NULL;
    for (Offset = 0x20; Offset + sizeof(LONG) <= BinSize; )
    {
        CellSize = *(PLONG)(Bin + Offset);
        if (CellSize > 0)
            return (PLONG)(Bin + Offset);
        if (CellSize == 0 || (DWORD)-CellSize > BinSize - Offset)
            return NULL;
        Offset += -CellSize;
    }
    return NULL;
}

/*
 * Loads the hive in another thread, so that a load that does not return
 * is reported as a failure. The process may still not exit while that
 * thread is stuck in the kernel.
 */
static void LoadDamagedHive(PCWSTR SubKey, PCWSTR File)
{
    PLOAD_CONTEXT Context;
    HANDLE Thread;
    DWORD Wait, Type = 0, Data, Size;
    LONG Error;
    HKEY Key;

    /* On a timeout the thread keeps using the context, so it is never freed then */
    Context = HeapAlloc(GetProcessHeap(), 0, sizeof(*Context));
    ok(Context != NULL, "HeapAlloc failed\n");
    if (!Context)
        return;
    StringCchCopyW(Context->SubKey, _countof(Context->SubKey), SubKey);
    StringCchCopyW(Context->File, _countof(Context->File), File);
    Context->Result = -1;
    Thread = CreateThread(NULL, 0, LoadThread, Context, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
    {
        HeapFree(GetProcessHeap(), 0, Context);
        return;
    }
    Wait = WaitForSingleObject(Thread, LOAD_TIMEOUT);
    ok(Wait == WAIT_OBJECT_0, "RegLoadKeyW did not return within %u ms\n", LOAD_TIMEOUT);
    CloseHandle(Thread);
    if (Wait != WAIT_OBJECT_0)
        return;

    /* A damaged hive is either repaired and loaded, or rejected as corrupt */
    trace("RegLoadKeyW returned %ld\n", Context->Result);
    ok(Context->Result == ERROR_SUCCESS || Context->Result == ERROR_BADDB,
       "RegLoadKeyW returned %ld\n", Context->Result);
    if (Context->Result == ERROR_SUCCESS)
    {
        /* The cells before the damage still read back */
        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, SubKey, 0, KEY_QUERY_VALUE, &Key);
        ok(Error == ERROR_SUCCESS, "RegOpenKeyExW failed: %ld\n", Error);
        if (Error == ERROR_SUCCESS)
        {
            Data = 0;
            Size = sizeof(Data);
            Error = RegQueryValueExW(Key, L"Value", NULL, &Type, (PBYTE)&Data, &Size);
            ok(Error == ERROR_SUCCESS, "RegQueryValueExW failed: %ld\n", Error);
            ok(Type == REG_DWORD && Data == 0x12345678, "Type %lu, data 0x%lx\n", Type, Data);
            RegCloseKey(Key);
        }
        Error = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, SubKey);
        ok(Error == ERROR_SUCCESS, "RegUnLoadKeyW failed: %ld\n", Error);
    }
    HeapFree(GetProcessHeap(), 0, Context);
    DeleteHiveFiles(File);
}

static BOOL PrepareFile(PWSTR File, PCWSTR Name)
{
    DWORD Length;

    Length = GetTempPathW(MAX_PATH, File);
    if (Length == 0 || Length >= MAX_PATH)
        return FALSE;
    return SUCCEEDED(StringCchCatW(File, MAX_PATH, Name));
}

START_TEST(RegLoadKeyZeroCell)
{
    WCHAR File[MAX_PATH];
    PBYTE Buffer;
    DWORD Size;
    PLONG Cell;

    if (!EnablePrivilege(L"SeBackupPrivilege") || !EnablePrivilege(L"SeRestorePrivilege"))
    {
        skip("Cannot enable the backup and restore privileges\n");
        return;
    }
    if (!PrepareFile(File, L"zerocell.hiv"))
    {
        skip("GetTempPathW failed\n");
        return;
    }
    Buffer = SaveTestHive(File, &Size);
    if (!Buffer)
        return;

    /* A free cell whose size field is 0 */
    Cell = FindFreeCell(Buffer, Size);
    ok(Cell != NULL, "No free cell in the first bin of the saved hive\n");
    if (Cell)
    {
        *Cell = 0;
        ok(WriteTestHive(File, Buffer, Size), "Writing the damaged hive failed\n");
        LoadDamagedHive(L"ReactOSTestZeroCell", File);
    }
    HeapFree(GetProcessHeap(), 0, Buffer);
}

START_TEST(RegLoadKeyZeroBin)
{
    WCHAR File[MAX_PATH];
    PBYTE Buffer;
    DWORD Size;

    if (!EnablePrivilege(L"SeBackupPrivilege") || !EnablePrivilege(L"SeRestorePrivilege"))
    {
        skip("Cannot enable the backup and restore privileges\n");
        return;
    }
    if (!PrepareFile(File, L"zerobin.hiv"))
    {
        skip("GetTempPathW failed\n");
        return;
    }
    Buffer = SaveTestHive(File, &Size);
    if (!Buffer)
        return;

    /* A bin whose size field is 0 */
    ok(Size >= BIN_OFFSET + 0x1000 && memcmp(Buffer + BIN_OFFSET, "hbin", 4) == 0,
       "No bin after the base block of the saved hive\n");
    if (Size >= BIN_OFFSET + 0x1000 && memcmp(Buffer + BIN_OFFSET, "hbin", 4) == 0)
    {
        *(PDWORD)(Buffer + BIN_OFFSET + 8) = 0;
        ok(WriteTestHive(File, Buffer, Size), "Writing the damaged hive failed\n");
        LoadDamagedHive(L"ReactOSTestZeroBin", File);
    }
    HeapFree(GetProcessHeap(), 0, Buffer);
}
