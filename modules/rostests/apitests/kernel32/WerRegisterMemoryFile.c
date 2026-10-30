/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for WerRegisterMemoryBlock, WerUnregisterMemoryBlock, WerRegisterFile and WerUnregisterFile
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef HRESULT (WINAPI *FN_WerRegisterMemoryBlock)(PVOID, DWORD);
typedef HRESULT (WINAPI *FN_WerUnregisterMemoryBlock)(PVOID);
typedef HRESULT (WINAPI *FN_WerRegisterFile)(PCWSTR, INT, DWORD);
typedef HRESULT (WINAPI *FN_WerUnregisterFile)(PCWSTR);

#define WER_FILE_DELETE_WHEN_DONE 1
#define WER_FILE_ANONYMOUS_DATA   2
#define REG_FILE_USER_DOCUMENT    1
#define REG_FILE_OTHER            2
#define MAX_ENTRIES               512

#define E_NOT_FOUND HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#define E_FULL      HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)

static FN_WerRegisterMemoryBlock pRegisterMemoryBlock;
static FN_WerUnregisterMemoryBlock pUnregisterMemoryBlock;
static FN_WerRegisterFile pRegisterFile;
static FN_WerUnregisterFile pUnregisterFile;

static BYTE Block[0x20000];

static void test_MemoryBlock(void)
{
    HRESULT hr;
    int i, Registered;

    /* The address and a size of 1 to 0x10000 */
    hr = pRegisterMemoryBlock(NULL, 16);
    ok(hr == E_INVALIDARG, "NULL block returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block, 0);
    ok(hr == E_INVALIDARG, "Size 0 returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block, 0x10001);
    ok(hr == E_INVALIDARG, "Size 0x10001 returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block, 0xffffffff);
    ok(hr == E_INVALIDARG, "Size 0xffffffff returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block);
    ok(hr == E_NOT_FOUND, "Unregister of a rejected block returned 0x%lx\n", hr);

    hr = pRegisterMemoryBlock(Block, 1);
    ok(hr == S_OK, "Size 1 returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block);
    ok(hr == S_OK, "Unregister returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block, 0x10000);
    ok(hr == S_OK, "Size 0x10000 returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block);
    ok(hr == S_OK, "Unregister returned 0x%lx\n", hr);

    /* The block is keyed by its address: registering it again does not add an entry */
    hr = pRegisterMemoryBlock(Block, 16);
    ok(hr == S_OK, "Register returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block, 32);
    ok(hr == S_OK, "Second register returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block + 8, 16);
    ok(hr == S_OK, "Register of another address returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block + 1);
    ok(hr == E_NOT_FOUND, "Unregister of an inside address returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block);
    ok(hr == S_OK, "Unregister returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block);
    ok(hr == E_NOT_FOUND, "Second unregister returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block + 8);
    ok(hr == S_OK, "Unregister of the other address returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(NULL);
    ok(hr == E_INVALIDARG, "Unregister of NULL returned 0x%lx\n", hr);

    /* The address is not validated */
    hr = pRegisterMemoryBlock((PVOID)(ULONG_PTR)8, 16);
    ok(hr == S_OK, "Unmapped address returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock((PVOID)(ULONG_PTR)8);
    ok(hr == S_OK, "Unregister of the unmapped address returned 0x%lx\n", hr);

    /* There is room for MAX_ENTRIES entries; a full table is ERROR_INSUFFICIENT_BUFFER */
    Registered = 0;
    for (i = 0; i < MAX_ENTRIES + 8; i++)
    {
        hr = pRegisterMemoryBlock(Block + i * 16, 16);
        if (hr != S_OK)
            break;
        Registered++;
    }
    ok(Registered == MAX_ENTRIES, "Registered %d blocks\n", Registered);
    ok(hr == E_FULL, "A full table returned 0x%lx\n", hr);
    hr = pUnregisterMemoryBlock(Block + 5 * 16);
    ok(hr == S_OK, "Unregister returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block + 2000 * 16, 16);
    ok(hr == S_OK, "Register into the freed entry returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block + 2001 * 16, 16);
    ok(hr == E_FULL, "A full table returned 0x%lx\n", hr);
    for (i = 0; i < Registered; i++)
        pUnregisterMemoryBlock(Block + i * 16);
    pUnregisterMemoryBlock(Block + 2000 * 16);
}

static void test_File(void)
{
    static const WCHAR Path[] = L"C:\\nonexistent\\werfile.txt";
    static const WCHAR PathUpper[] = L"C:\\NONEXISTENT\\WERFILE.TXT";
    WCHAR Long[MAX_PATH + 8];
    WCHAR Name[64];
    HRESULT hr;
    int i, Registered;

    /* The type is a user document or other; the flags are delete-when-done and anonymous-data */
    hr = pRegisterFile(NULL, REG_FILE_OTHER, 0);
    ok(hr == E_INVALIDARG, "NULL path returned 0x%lx\n", hr);
    hr = pRegisterFile(Path, 0, 0);
    ok(hr == E_INVALIDARG, "Type 0 returned 0x%lx\n", hr);
    hr = pRegisterFile(Path, 3, 0);
    ok(hr == E_INVALIDARG, "Type 3 returned 0x%lx\n", hr);
    hr = pRegisterFile(Path, -1, 0);
    ok(hr == E_INVALIDARG, "Type -1 returned 0x%lx\n", hr);
    hr = pRegisterFile(Path, REG_FILE_OTHER, 4);
    ok(hr == E_INVALIDARG, "Flags 4 returned 0x%lx\n", hr);
    hr = pRegisterFile(Path, REG_FILE_USER_DOCUMENT, 0xffff);
    ok(hr == E_INVALIDARG, "Flags 0xffff returned 0x%lx\n", hr);
    hr = pUnregisterFile(Path);
    ok(hr == E_NOT_FOUND, "Unregister of a rejected file returned 0x%lx\n", hr);

    /* The file does not have to exist */
    hr = pRegisterFile(Path, REG_FILE_USER_DOCUMENT, 0);
    ok(hr == S_OK, "Register returned 0x%lx\n", hr);
    /* The path is the key: registering it again does not add an entry */
    hr = pRegisterFile(Path, REG_FILE_OTHER, WER_FILE_DELETE_WHEN_DONE | WER_FILE_ANONYMOUS_DATA);
    ok(hr == S_OK, "Second register returned 0x%lx\n", hr);
    /* but unregistering ignores the case */
    hr = pUnregisterFile(PathUpper);
    ok(hr == S_OK, "Unregister with another case returned 0x%lx\n", hr);
    hr = pUnregisterFile(Path);
    ok(hr == E_NOT_FOUND, "Second unregister returned 0x%lx\n", hr);
    hr = pUnregisterFile(NULL);
    ok(hr == E_INVALIDARG, "Unregister of NULL returned 0x%lx\n", hr);

    /* Paths up to MAX_PATH - 1 characters */
    for (i = 0; i < MAX_PATH + 4; i++)
        Long[i] = L'a';
    Long[MAX_PATH - 1] = 0;
    hr = pRegisterFile(Long, REG_FILE_OTHER, 0);
    ok(hr == S_OK, "A path of %d characters returned 0x%lx\n", MAX_PATH - 1, hr);
    hr = pUnregisterFile(Long);
    ok(hr == S_OK, "Unregister returned 0x%lx\n", hr);
    Long[MAX_PATH - 1] = L'a';
    Long[MAX_PATH] = 0;
    hr = pRegisterFile(Long, REG_FILE_OTHER, 0);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), "A path of %d characters returned 0x%lx\n", MAX_PATH, hr);

    /* Files and memory blocks share the table */
    Registered = 0;
    for (i = 0; i < MAX_ENTRIES + 8; i++)
    {
        _snwprintf(Name, ARRAYSIZE(Name), L"C:\\nonexistent\\werfile%d.txt", i);
        hr = pRegisterFile(Name, REG_FILE_OTHER, 0);
        if (hr != S_OK)
            break;
        Registered++;
    }
    ok(Registered == MAX_ENTRIES, "Registered %d files\n", Registered);
    ok(hr == E_FULL, "A full table returned 0x%lx\n", hr);
    hr = pRegisterMemoryBlock(Block, 16);
    ok(hr == E_FULL, "A memory block in a full table returned 0x%lx\n", hr);
    for (i = 0; i < Registered; i++)
    {
        _snwprintf(Name, ARRAYSIZE(Name), L"C:\\nonexistent\\werfile%d.txt", i);
        pUnregisterFile(Name);
    }
    hr = pRegisterMemoryBlock(Block, 16);
    ok(hr == S_OK, "A memory block in an empty table returned 0x%lx\n", hr);
    pUnregisterMemoryBlock(Block);
}

START_TEST(WerRegisterMemoryFile)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");

    pRegisterMemoryBlock = (FN_WerRegisterMemoryBlock)GetProcAddress(Kernel32, "WerRegisterMemoryBlock");
    pUnregisterMemoryBlock = (FN_WerUnregisterMemoryBlock)GetProcAddress(Kernel32, "WerUnregisterMemoryBlock");
    pRegisterFile = (FN_WerRegisterFile)GetProcAddress(Kernel32, "WerRegisterFile");
    pUnregisterFile = (FN_WerUnregisterFile)GetProcAddress(Kernel32, "WerUnregisterFile");
    if (!pRegisterMemoryBlock && is_reactos() && !GetProcAddress(Kernel32, "K32EnumProcesses"))
    {
        skip("No NT 6.x exports\n");
        return;
    }
    ok(pRegisterMemoryBlock && pUnregisterMemoryBlock && pRegisterFile && pUnregisterFile,
       "The Wer memory block and file functions are not exported\n");
    if (!pRegisterMemoryBlock || !pUnregisterMemoryBlock || !pRegisterFile || !pUnregisterFile)
        return;

    test_MemoryBlock();
    test_File();
}
