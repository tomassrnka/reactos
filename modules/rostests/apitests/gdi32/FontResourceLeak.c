/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that removed font resources free their FreeType data
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/exfuncs.h>

#define CYCLES 50

/* Live paged allocations of the FreeType glue ('FTYP'), or -1 when the tag is absent (Windows) */
static LONG
GetFreeTypeLiveAllocs(void)
{
    static BYTE Buffer[256 * 1024];
    PSYSTEM_POOLTAG_INFORMATION Info = (PSYSTEM_POOLTAG_INFORMATION)Buffer;
    ULONG i;

    if (!NT_SUCCESS(NtQuerySystemInformation(SystemPoolTagInformation, Buffer, sizeof(Buffer), NULL)))
        return -1;

    for (i = 0; i < Info->Count; i++)
    {
        if (!memcmp(Info->TagInfo[i].Tag, "FTYP", 4))
            return (LONG)(Info->TagInfo[i].PagedAllocs - Info->TagInfo[i].PagedFrees);
    }
    return -1;
}

static PVOID
LoadFontResource(PDWORD pdwSize)
{
    HMODULE hMod = GetModuleHandleW(NULL);
    HRSRC hRsrc = FindResourceA(hMod, "ExampleFont.ttf", MAKEINTRESOURCEA(RT_RCDATA));
    if (!hRsrc)
        return NULL;
    *pdwSize = SizeofResource(hMod, hRsrc);
    return LockResource(LoadResource(hMod, hRsrc));
}

static void
ChildMain(const char *pszFile)
{
    PVOID pFont;
    DWORD dwSize, cFonts;

    /* Private fonts of this process; freed when it ends */
    AddFontResourceExA(pszFile, FR_PRIVATE, NULL);
    pFont = LoadFontResource(&dwSize);
    if (pFont)
        AddFontMemResourceEx(pFont, dwSize, NULL, &cFonts);
}

START_TEST(FontResourceLeak)
{
    PVOID pFont;
    DWORD dwSize, cFonts, dwWritten;
    LONG lBefore, lAfter, i;
    HANDLE hFont, hFile;
    CHAR szTempPath[MAX_PATH], szTempFile[MAX_PATH], szCmd[3 * MAX_PATH];
    char **argv;
    int argc;
    BOOL bOk;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "child"))
    {
        ChildMain(argv[3]);
        return;
    }

    if (GetFreeTypeLiveAllocs() < 0)
    {
        skip("No 'FTYP' pool tag\n");
        return;
    }

    pFont = LoadFontResource(&dwSize);
    ok(pFont != NULL, "Font resource not found\n");
    if (!pFont)
        return;

    /* Memory fonts */
    hFont = AddFontMemResourceEx(pFont, dwSize, NULL, &cFonts);
    ok(hFont != NULL, "AddFontMemResourceEx failed\n");
    if (hFont)
        RemoveFontMemResourceEx(hFont);

    lBefore = GetFreeTypeLiveAllocs();
    for (i = 0; i < CYCLES; i++)
    {
        hFont = AddFontMemResourceEx(pFont, dwSize, NULL, &cFonts);
        if (!hFont)
            break;
        ok(RemoveFontMemResourceEx(hFont), "RemoveFontMemResourceEx failed\n");
    }
    lAfter = GetFreeTypeLiveAllocs();
    ok(i == CYCLES, "AddFontMemResourceEx failed in cycle %ld\n", i);
    ok(lAfter - lBefore < CYCLES, "Memory fonts: %ld FreeType allocations left after %ld cycles\n", lAfter - lBefore, i);

    /* Private file and memory fonts of processes that end */
    GetTempPathA(_countof(szTempPath), szTempPath);
    GetTempFileNameA(szTempPath, "ttf", 0, szTempFile);
    hFile = CreateFileA(szTempFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileA failed\n");
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    bOk = WriteFile(hFile, pFont, dwSize, &dwWritten, NULL);
    CloseHandle(hFile);
    ok(bOk && dwWritten == dwSize, "WriteFile failed\n");

    StringCbPrintfA(szCmd, sizeof(szCmd), "\"%s\" FontResourceLeak child \"%s\"", argv[0], szTempFile);
    lBefore = GetFreeTypeLiveAllocs();
    for (i = 0; i < CYCLES / 2; i++)
    {
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;

        if (!CreateProcessA(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            break;
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    lAfter = GetFreeTypeLiveAllocs();
    ok(i == CYCLES / 2, "CreateProcessA failed in cycle %ld\n", i);
    ok(lAfter - lBefore < CYCLES / 2, "Ended processes: %ld FreeType allocations left after %ld processes\n", lAfter - lBefore, i);

    DeleteFileA(szTempFile);
}
