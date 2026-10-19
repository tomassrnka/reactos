/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that removing a font resource removes the font and frees it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define LOOP_COUNT 100
#define SELECTED_COUNT 10
#define SystemPoolTagInformationClass 22
/* win32k's TAG_FONT ('ETNF': font entries, shared faces, memory font copies),
 * GDITAG_RFONT ('tnfG': FONTGDI) and GDITAG_PFF ('ffpG': font file names) */
#define WIN32K_TAG_FONT 0x45544E46
#define WIN32K_TAG_RFONT 0x746E6647
#define WIN32K_TAG_PFF 0x66667047

static const WCHAR s_FaceName[] = L"ReactOSTestTahoma";

typedef LONG (WINAPI *PNT_QUERY_SYSTEM_INFORMATION)(ULONG, PVOID, ULONG, PULONG);

typedef struct _POOL_TAG_ENTRY
{
    ULONG TagUlong;
    ULONG PagedAllocs;
    ULONG PagedFrees;
    SIZE_T PagedUsed;
    ULONG NonPagedAllocs;
    ULONG NonPagedFrees;
    SIZE_T NonPagedUsed;
} POOL_TAG_ENTRY;

/* Outstanding allocations of win32k's font tags */
static BOOL
QueryFontPool(LONG *Allocations)
{
    static PNT_QUERY_SYSTEM_INFORMATION pNtQuerySystemInformation;
    ULONG Size = 256 * 1024, Returned = 0, Count, i;
    PUCHAR Buffer;
    LONG Status;

    if (!pNtQuerySystemInformation)
        pNtQuerySystemInformation = (PNT_QUERY_SYSTEM_INFORMATION)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                                                 "NtQuerySystemInformation");
    if (!pNtQuerySystemInformation)
        return FALSE;

    for (;;)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Buffer)
            return FALSE;
        Status = pNtQuerySystemInformation(SystemPoolTagInformationClass, Buffer, Size, &Returned);
        if (Status != (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */ || Size >= 16 * 1024 * 1024)
            break;
        HeapFree(GetProcessHeap(), 0, Buffer);
        Size *= 2;
    }
    if (Status < 0)
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return FALSE;
    }

    Count = *(PULONG)Buffer;
    *Allocations = 0;
    for (i = 0; i < Count; i++)
    {
        POOL_TAG_ENTRY *Entry = (POOL_TAG_ENTRY *)(Buffer + sizeof(SIZE_T) + i * sizeof(POOL_TAG_ENTRY));
        if (Entry->TagUlong == WIN32K_TAG_FONT || Entry->TagUlong == WIN32K_TAG_RFONT ||
            Entry->TagUlong == WIN32K_TAG_PFF)
        {
            *Allocations += (LONG)(Entry->PagedAllocs - Entry->PagedFrees) +
                            (LONG)(Entry->NonPagedAllocs - Entry->NonPagedFrees);
        }
    }
    HeapFree(GetProcessHeap(), 0, Buffer);
    return TRUE;
}

static PVOID
GetFontResource(PDWORD Size)
{
    HRSRC hRsrc = FindResourceW(GetModuleHandleW(NULL), L"ReactOSTestTahoma.ttf", (LPCWSTR)RT_RCDATA);
    if (!hRsrc)
        return NULL;
    *Size = SizeofResource(GetModuleHandleW(NULL), hRsrc);
    return LockResource(LoadResource(GetModuleHandleW(NULL), hRsrc));
}

static BOOL
WriteFontFile(PWSTR Path)
{
    WCHAR TempPath[MAX_PATH];
    HANDLE hFile;
    PVOID Data;
    DWORD Size, Written;
    BOOL Ret;

    Data = GetFontResource(&Size);
    if (!Data)
        return FALSE;
    GetTempPathW(_countof(TempPath), TempPath);
    if (!GetTempFileNameW(TempPath, L"rfr", 0, Path))
        return FALSE;
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(hFile, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(hFile);
    return Ret;
}

static HFONT
CreateTestFont(VOID)
{
    LOGFONTW lf;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -20;
    lf.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(lf.lfFaceName, _countof(lf.lfFaceName), s_FaceName);
    return CreateFontIndirectW(&lf);
}

/* Whether a new logical font of the test face name is realized to the test font */
static BOOL
IsTestFontAvailable(HDC hdc)
{
    WCHAR Face[LF_FACESIZE] = L"";
    HFONT hFont = CreateTestFont(), hOld;

    hOld = SelectObject(hdc, hFont);
    GetTextFaceW(hdc, _countof(Face), Face);
    SelectObject(hdc, hOld);
    DeleteObject(hFont);
    return lstrcmpiW(Face, s_FaceName) == 0;
}

/* Use a logical font realized to the test font, as a selected font that survives the removal */
static VOID
UseFont(HDC hdc, PCSTR Step)
{
    TEXTMETRICW tm;
    WCHAR Face[LF_FACESIZE] = L"";
    SIZE Extent = { 0, 0 };

    ok(GetTextMetricsW(hdc, &tm), "%s: GetTextMetrics failed\n", Step);
    ok(GetTextFaceW(hdc, _countof(Face), Face) > 0, "%s: GetTextFace failed\n", Step);
    ok(lstrcmpiW(Face, s_FaceName) == 0, "%s: face %ls\n", Step, Face);
    ok(GetTextExtentPoint32W(hdc, L"Font", 4, &Extent) && Extent.cx > 0, "%s: GetTextExtentPoint32 failed\n", Step);
    ok(TextOutW(hdc, 0, 0, L"Font", 4), "%s: TextOut failed\n", Step);
}

static VOID
CheckPool(LONG Before, PCSTR What)
{
    LONG After = 0;

    ok(QueryFontPool(&After), "%s: SystemPoolTagInformation failed\n", What);
    trace("%s: %ld font pool allocations left\n", What, After - Before);
    /* A leaked load keeps at least three allocations (entry or memory copy, shared face,
     * shared memory) and a leaked FONTGDI two (itself, file name); other processes may
     * allocate a few meanwhile */
    ok(After - Before < LOOP_COUNT / 10,
       "%s: %ld font pool allocations are left\n", What, After - Before);
}

static VOID
Test_FileFont(HDC hdc, PCWSTR Path, DWORD fl, PCSTR What)
{
    LONG Before = 0;
    BOOL HavePool = QueryFontPool(&Before);
    INT i, AddFailed = 0, RemoveFailed = 0, StillThere = 0;

    for (i = 0; i < LOOP_COUNT; i++)
    {
        if (AddFontResourceExW(Path, fl, NULL) != 1)
        {
            AddFailed++;
            continue;
        }
        if (!RemoveFontResourceExW(Path, fl, NULL))
            RemoveFailed++;
        else if (i == 0 && IsTestFontAvailable(hdc))
            StillThere++;
    }
    ok(AddFailed == 0, "%s: %d of %d AddFontResourceEx calls failed\n", What, AddFailed, LOOP_COUNT);
    ok(RemoveFailed == 0, "%s: %d of %d RemoveFontResourceEx calls failed\n", What, RemoveFailed, LOOP_COUNT);
    ok(StillThere == 0, "%s: the font is still there after RemoveFontResourceEx\n", What);
    ok(!RemoveFontResourceExW(Path, fl, NULL), "%s: a second RemoveFontResourceEx succeeded\n", What);
    if (HavePool)
        CheckPool(Before, What);
    else
        skip("SystemPoolTagInformation is not available\n");
}

static VOID
Test_MemoryFont(VOID)
{
    LONG Before = 0;
    BOOL HavePool = QueryFontPool(&Before);
    INT i, AddFailed = 0, RemoveFailed = 0;
    DWORD Size = 0, Count;
    PVOID Data = GetFontResource(&Size);
    HANDLE hMem;

    if (!Data)
    {
        skip("No font resource\n");
        return;
    }
    for (i = 0; i < LOOP_COUNT; i++)
    {
        Count = 0;
        hMem = AddFontMemResourceEx(Data, Size, NULL, &Count);
        if (!hMem || Count != 1)
        {
            AddFailed++;
            continue;
        }
        if (!RemoveFontMemResourceEx(hMem))
            RemoveFailed++;
    }
    ok(AddFailed == 0, "memory: %d of %d AddFontMemResourceEx calls failed\n", AddFailed, LOOP_COUNT);
    ok(RemoveFailed == 0, "memory: %d of %d RemoveFontMemResourceEx calls failed\n", RemoveFailed, LOOP_COUNT);
    if (HavePool)
        CheckPool(Before, "memory");
    else
        skip("SystemPoolTagInformation is not available\n");
}

/* A font selected into a DC stays usable after its resource is removed */
static VOID
Test_RemoveSelected(HDC hdc, PCWSTR Path)
{
    DWORD Size = 0, Count = 0;
    PVOID Data = GetFontResource(&Size);
    HANDLE hMem;
    HFONT hFont, hOld;
    TEXTMETRICW tm;
    LONG Before = 0;
    BOOL HavePool = QueryFontPool(&Before);
    INT i;

    /* Repeat it, so that a reference a logical font keeps exceeds the pool noise */
    for (i = 0; i < SELECTED_COUNT; i++)
    {
        ok(AddFontResourceExW(Path, FR_PRIVATE, NULL) == 1, "AddFontResourceEx failed\n");
        hFont = CreateTestFont();
        hOld = SelectObject(hdc, hFont);
        UseFont(hdc, "file, before removal");
        ok(RemoveFontResourceExW(Path, FR_PRIVATE, NULL), "RemoveFontResourceEx failed\n");
        UseFont(hdc, "file, after removal");
        ok(!IsTestFontAvailable(hdc), "file: a new logical font still finds the removed font\n");
        SelectObject(hdc, hOld);
        DeleteObject(hFont);

        if (!Data)
            continue;
        hMem = AddFontMemResourceEx(Data, Size, NULL, &Count);
        ok(hMem != NULL, "AddFontMemResourceEx failed\n");
        hFont = CreateTestFont();
        hOld = SelectObject(hdc, hFont);
        UseFont(hdc, "memory, before removal");
        ok(RemoveFontMemResourceEx(hMem), "RemoveFontMemResourceEx failed\n");
        UseFont(hdc, "memory, after removal");
        ok(!IsTestFontAvailable(hdc), "memory: a new logical font still finds the removed font\n");
        SelectObject(hdc, hOld);
        DeleteObject(hFont);

        /* Deleting the last logical font that used them frees the removed fonts; a text
         * call makes the DC drop its reference of the logical font it selected before */
        GetTextMetricsW(hdc, &tm);
    }
    if (HavePool)
        CheckPool(Before, "selected");
}

/* Each load of a file needs a removal of its own */
static VOID
Test_RepeatedLoad(HDC hdc, PCWSTR Path)
{
    ok(AddFontResourceExW(Path, FR_PRIVATE, NULL) == 1, "first AddFontResourceEx failed\n");
    ok(AddFontResourceExW(Path, FR_PRIVATE, NULL) == 1, "second AddFontResourceEx failed\n");
    ok(RemoveFontResourceExW(Path, FR_PRIVATE, NULL), "first RemoveFontResourceEx failed\n");
    ok(IsTestFontAvailable(hdc), "The font is gone after one of two removals\n");
    ok(RemoveFontResourceExW(Path, FR_PRIVATE, NULL), "second RemoveFontResourceEx failed\n");
    ok(!IsTestFontAvailable(hdc), "The font is still there after two removals\n");
    ok(!RemoveFontResourceExW(Path, FR_PRIVATE, NULL), "a third RemoveFontResourceEx succeeded\n");
}

START_TEST(RemoveFontResourceEx)
{
    WCHAR Path[MAX_PATH];
    HDC hdc;

    if (!WriteFontFile(Path))
    {
        skip("Cannot write the test font file\n");
        return;
    }
    hdc = CreateCompatibleDC(NULL);
    ok(hdc != NULL, "CreateCompatibleDC failed\n");

    ok(!IsTestFontAvailable(hdc), "The test font is already installed\n");

    /* The flags must match those the font was added with */
    ok(AddFontResourceExW(Path, FR_PRIVATE, NULL) == 1, "AddFontResourceEx failed\n");
    ok(!RemoveFontResourceExW(Path, 0, NULL), "RemoveFontResourceEx of a private font without FR_PRIVATE succeeded\n");
    ok(IsTestFontAvailable(hdc), "The private font is not available\n");
    ok(RemoveFontResourceExW(Path, FR_PRIVATE, NULL), "RemoveFontResourceEx failed\n");
    ok(!IsTestFontAvailable(hdc), "The private font is still there\n");

    Test_FileFont(hdc, Path, FR_PRIVATE, "private");
    Test_MemoryFont();
    Test_RemoveSelected(hdc, Path);
    Test_RepeatedLoad(hdc, Path);
    Test_FileFont(hdc, Path, 0, "public");

    ok(!IsTestFontAvailable(hdc), "The test font is still there at the end\n");
    DeleteDC(hdc);
    ok(DeleteFileW(Path), "DeleteFile failed: %lu\n", GetLastError());
}
