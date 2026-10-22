/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetFontRealizationInfo, GetFontFileInfo and GetFontFileData
 *              (undocumented gdi32 functions that DirectWrite uses to open the font of a device context)
 *
 * wine_test.ttf comes from the Wine project (LGPL-2.1-or-later).
 */

#include "precomp.h"

typedef struct
{
    DWORD size;
    DWORD flags;
    DWORD cache_num;
    DWORD instance_id;
    DWORD file_count;
    WORD  face_index;
    WORD  simulations;
} FRI;

typedef struct
{
    FILETIME writetime;
    LARGE_INTEGER size;
    WCHAR path[1];
} FFI;

static BOOL (WINAPI *pGetFontRealizationInfo)(HDC, void *);
static BOOL (WINAPI *pGetFontFileInfo)(DWORD, DWORD, void *, SIZE_T, SIZE_T *);
static BOOL (WINAPI *pGetFontFileData)(DWORD, DWORD, UINT64, void *, SIZE_T);

static const BYTE *g_data;
static DWORD g_datasize;

static BOOL LoadTestFont(void)
{
    HRSRC res = FindResourceA(NULL, "wine_test.ttf", (LPCSTR)RT_RCDATA);

    if (!res)
        return FALSE;
    g_data = LockResource(LoadResource(NULL, res));
    g_datasize = SizeofResource(NULL, res);
    return g_data && g_datasize;
}

static HFONT SelectTestFont(HDC hdc, LONG height, LONG weight, BYTE italic, HFONT *old)
{
    LOGFONTA lf;
    HFONT hf;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfItalic = italic;
    lstrcpyA(lf.lfFaceName, "wine_test");
    hf = CreateFontIndirectA(&lf);
    *old = SelectObject(hdc, hf);
    return hf;
}

static void TestRealizationInfo(HDC hdc, FRI *out)
{
    DWORD buf[16];
    FRI *fri = (FRI *)buf;
    DWORD size;
    BOOL ret;

    /* Only two layouts of the structure are accepted. A size below 16 is refused on a freshly started system and
       accepted after a first success, so it is not tested. */
    for (size = 16; size <= 40; size += 4)
    {
        FillMemory(buf, sizeof(buf), 0xCC);
        buf[0] = size;
        ret = pGetFontRealizationInfo(hdc, buf);
        if (size == 16 || size == 24)
        {
            ok(ret, "size %lu: failed, error %lu\n", size, GetLastError());
            ok(fri->size == size, "size %lu: got size %lu\n", size, fri->size);
            ok((fri->flags & 0xF) == 3, "size %lu: got flags %#lx\n", size, fri->flags);
            ok(fri->instance_id != 0 && fri->instance_id != 0xCCCCCCCC, "size %lu: got instance id %#lx\n", size, fri->instance_id);
            if (size == 24)
            {
                ok(fri->file_count == 1, "got %lu files\n", fri->file_count);
                ok(buf[6] == 0xCCCCCCCC, "structure longer than 6 dwords\n");
            }
        }
        else
        {
            ok(!ret, "size %lu: succeeded\n", size);
            ok(buf[1] == 0xCCCCCCCC, "size %lu: wrote flags %#lx\n", size, buf[1]);
        }
    }

    ZeroMemory(out, sizeof(*out));
    out->size = sizeof(*out);
    ret = pGetFontRealizationInfo(hdc, out);
    ok(ret, "failed, error %lu\n", GetLastError());
}

static void TestFileData(DWORD id, UINT64 filesize, const BYTE *expected)
{
    BYTE *buf;
    BOOL ret;

    buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)filesize + 16);
    if (!buf)
        return;

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(id, 0, 0, buf, (SIZE_T)filesize);
    ok(ret, "whole file: failed, error %lu\n", GetLastError());
    if (ret && expected)
        ok(!memcmp(buf, expected, (SIZE_T)filesize), "whole file: content differs\n");

    ZeroMemory(buf, 16);
    ret = pGetFontFileData(id, 0, 2, buf, 16);
    ok(ret, "bytes 2 to 18: failed, error %lu\n", GetLastError());
    if (ret && expected)
        ok(!memcmp(buf, expected + 2, 16), "bytes 2 to 18: content differs\n");

    ret = pGetFontFileData(id, 0, filesize - 16, buf, 16);
    ok(ret, "last 16 bytes: failed, error %lu\n", GetLastError());
    if (ret && expected)
        ok(!memcmp(buf, expected + filesize - 16, 16), "last 16 bytes: content differs\n");

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(id, 0, filesize - 15, buf, 16);
    ok(!ret, "range ends one byte past the file: succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(id, 0, 0, buf, (SIZE_T)filesize + 1);
    ok(!ret, "range longer than the file: succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(0xabababab, 0, 0, buf, 16);
    ok(!ret, "unknown instance: succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(id, 1, 0, buf, 16);
    ok(!ret, "file index 1: succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "file index 1: got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(id, 0, 0, buf, 0);
    ok(!ret, "no bytes: succeeded\n");
    ok(GetLastError() == ERROR_NOACCESS, "no bytes: got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(id, 0, filesize + 1, buf, 0);
    ok(!ret, "offset past the end: succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "offset past the end: got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = pGetFontFileData(id, 0, 0, NULL, 16);
    ok(!ret, "no buffer: succeeded\n");
    ok(GetLastError() == ERROR_NOACCESS, "no buffer: got error %lu\n", GetLastError());

    HeapFree(GetProcessHeap(), 0, buf);
}

static BOOL SameFile(const WCHAR *path1, const WCHAR *path2)
{
    BY_HANDLE_FILE_INFORMATION i1, i2;
    HANDLE h1, h2;
    BOOL ret = FALSE;

    h1 = CreateFileW(path1, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    h2 = CreateFileW(path2, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h1 != INVALID_HANDLE_VALUE && h2 != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(h1, &i1) && GetFileInformationByHandle(h2, &i2))
    {
        ret = i1.dwVolumeSerialNumber == i2.dwVolumeSerialNumber &&
              i1.nFileIndexHigh == i2.nFileIndexHigh && i1.nFileIndexLow == i2.nFileIndexLow;
    }
    if (h1 != INVALID_HANDLE_VALUE) CloseHandle(h1);
    if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
    return ret;
}

static void TestMemoryFont(HDC hdc)
{
    DWORD fonts = 0;
    HANDLE res;
    HFONT hf, old;
    FRI fri;
    BYTE buf[512];
    FFI *info = (FFI *)buf;
    SIZE_T needed;
    BOOL ret;

    res = AddFontMemResourceEx((void *)g_data, g_datasize, NULL, &fonts);
    ok(res != NULL && fonts == 1, "AddFontMemResourceEx: %p, %lu fonts, error %lu\n", res, fonts, GetLastError());
    if (!res)
        return;

    hf = SelectTestFont(hdc, -12, FW_NORMAL, FALSE, &old);
    TestRealizationInfo(hdc, &fri);
    ok(fri.face_index == 0, "got face index %u\n", fri.face_index);
    ok(fri.simulations == 0, "got simulations %#x\n", fri.simulations);

    FillMemory(buf, sizeof(buf), 0xCC);
    needed = 0;
    SetLastError(0xdeadbeef);
    ret = pGetFontFileInfo(fri.instance_id, 0, info, sizeof(buf), &needed);
    ok(ret, "failed, error %lu\n", GetLastError());
    if (ret)
    {
        ok(needed == sizeof(FILETIME) + sizeof(LARGE_INTEGER) + sizeof(WCHAR), "got required size %lu\n", (ULONG)needed);
        ok(info->size.QuadPart == g_datasize, "got size %I64d, expected %lu\n", info->size.QuadPart, g_datasize);
        ok(info->path[0] == 0, "a font in memory has a path: %S\n", info->path);
        ok(info->writetime.dwLowDateTime == 0 && info->writetime.dwHighDateTime == 0,
           "got a write time %08lx%08lx\n", info->writetime.dwHighDateTime, info->writetime.dwLowDateTime);
    }

    needed = 0x1234;
    SetLastError(0xdeadbeef);
    ret = pGetFontFileInfo(0xabababab, 0, info, sizeof(buf), &needed);
    ok(!ret, "unknown instance: succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "unknown instance: got error %lu\n", GetLastError());
    ok(needed == 0x1234, "unknown instance: got required size %lu\n", (ULONG)needed);

    needed = 0x1234;
    SetLastError(0xdeadbeef);
    ret = pGetFontFileInfo(fri.instance_id, 1, info, sizeof(buf), &needed);
    ok(!ret, "file index 1: succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "file index 1: got error %lu\n", GetLastError());
    ok(needed == 0x1234, "file index 1: got required size %lu\n", (ULONG)needed);

    TestFileData(fri.instance_id, g_datasize, g_data);

    /* the same face on another device context has the same instance */
    {
        HDC hdc2 = CreateCompatibleDC(NULL);
        HFONT old2, hf2 = SelectTestFont(hdc2, -20, FW_NORMAL, FALSE, &old2);
        FRI fri2;

        ZeroMemory(&fri2, sizeof(fri2));
        fri2.size = sizeof(fri2);
        ret = pGetFontRealizationInfo(hdc2, &fri2);
        ok(ret, "failed, error %lu\n", GetLastError());
        ok(fri2.simulations == 0, "got simulations %#x\n", fri2.simulations);
        SelectObject(hdc2, old2);
        DeleteObject(hf2);
        DeleteDC(hdc2);
    }

    SelectObject(hdc, old);
    DeleteObject(hf);

    /* simulated styles of a regular face */
    hf = SelectTestFont(hdc, -12, FW_BOLD, TRUE, &old);
    ZeroMemory(&fri, sizeof(fri));
    fri.size = sizeof(fri);
    ret = pGetFontRealizationInfo(hdc, &fri);
    ok(ret, "failed, error %lu\n", GetLastError());
    ok(fri.simulations == 3, "bold italic: got simulations %#x\n", fri.simulations);
    SelectObject(hdc, old);
    DeleteObject(hf);

    hf = SelectTestFont(hdc, -12, FW_NORMAL, TRUE, &old);
    ZeroMemory(&fri, sizeof(fri));
    fri.size = sizeof(fri);
    ret = pGetFontRealizationInfo(hdc, &fri);
    ok(ret, "failed, error %lu\n", GetLastError());
    ok(fri.simulations == 2, "italic: got simulations %#x\n", fri.simulations);
    SelectObject(hdc, old);
    DeleteObject(hf);

    ret = RemoveFontMemResourceEx(res);
    ok(ret, "RemoveFontMemResourceEx failed, error %lu\n", GetLastError());
}

/* A realized font does not own the font it was realized from: removing the font must not leave the
   device context with a dangling pointer, and the instance must not stay reachable after that */
static void TestRemovedMemoryFont(HDC hdc)
{
    DWORD fonts = 0;
    HANDLE res;
    HFONT hf, old;
    FRI fri, fri2;
    BYTE buf[512];
    SIZE_T needed = 0;
    BOOL ret, ret2;
    DWORD error;

    res = AddFontMemResourceEx((void *)g_data, g_datasize, NULL, &fonts);
    ok(res != NULL && fonts == 1, "AddFontMemResourceEx: %p, %lu fonts, error %lu\n", res, fonts, GetLastError());
    if (!res)
        return;

    hf = SelectTestFont(hdc, -12, FW_NORMAL, FALSE, &old);
    TestRealizationInfo(hdc, &fri);

    ret = RemoveFontMemResourceEx(res);
    ok(ret, "RemoveFontMemResourceEx failed, error %lu\n", GetLastError());

    ZeroMemory(&fri2, sizeof(fri2));
    fri2.size = sizeof(fri2);
    SetLastError(0xdeadbeef);
    ret = pGetFontRealizationInfo(hdc, &fri2);
    error = GetLastError();
    trace("realization info after the removal: %d, error %lu, instance %lu (before %lu), flags %lx\n",
          ret, error, fri2.instance_id, fri.instance_id, fri2.flags);
    if (!ret)
        ok(error == ERROR_INVALID_HANDLE || error == ERROR_INVALID_PARAMETER, "got error %lu\n", error);

    FillMemory(buf, sizeof(buf), 0xCC);
    SetLastError(0xdeadbeef);
    ret2 = pGetFontFileInfo(fri.instance_id, 0, buf, sizeof(buf), &needed);
    trace("file info after the removal: %d, error %lu\n", ret2, GetLastError());
    if (!ret2)
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "file info: got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret2 = pGetFontFileData(fri.instance_id, 0, 0, buf, 16);
    trace("file data after the removal: %d, error %lu\n", ret2, GetLastError());
    if (!ret2)
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "file data: got error %lu\n", GetLastError());

    SelectObject(hdc, old);
    DeleteObject(hf);
}

static void TestPrivateFileFont(HDC hdc)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD written;
    HFONT hf, old;
    FRI fri;
    FFI *info;
    SIZE_T needed, size;
    FILETIME writetime;
    LARGE_INTEGER filesize;
    BOOL ret;
    int added;

    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"gfi_test.ttf");
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(file != INVALID_HANDLE_VALUE, "cannot create %S, error %lu\n", path, GetLastError());
    if (file == INVALID_HANDLE_VALUE)
        return;
    WriteFile(file, g_data, g_datasize, &written, NULL);
    GetFileTime(file, NULL, NULL, &writetime);
    GetFileSizeEx(file, &filesize);
    CloseHandle(file);

    added = AddFontResourceExW(path, FR_PRIVATE, 0);
    ok(added == 1, "AddFontResourceEx added %d fonts, error %lu\n", added, GetLastError());
    if (added != 1)
    {
        DeleteFileW(path);
        return;
    }

    hf = SelectTestFont(hdc, -12, FW_NORMAL, FALSE, &old);
    TestRealizationInfo(hdc, &fri);
    ok(fri.simulations == 0, "got simulations %#x\n", fri.simulations);

    needed = 0x1234;
    SetLastError(0xdeadbeef);
    ret = pGetFontFileInfo(fri.instance_id, 0, NULL, 0, &needed);
    ok(!ret, "no buffer: succeeded\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "no buffer: got error %lu\n", GetLastError());
    ok(needed > sizeof(FILETIME) + sizeof(LARGE_INTEGER) + sizeof(WCHAR), "got required size %lu\n", (ULONG)needed);

    size = needed;
    info = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    SetLastError(0xdeadbeef);
    ret = pGetFontFileInfo(fri.instance_id, 0, info, size - 1, &needed);
    ok(!ret, "buffer one byte short: succeeded\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "buffer one byte short: got error %lu\n", GetLastError());

    needed = 0;
    ret = pGetFontFileInfo(fri.instance_id, 0, info, size, &needed);
    ok(ret, "failed, error %lu\n", GetLastError());
    if (ret)
    {
        ok(needed == size, "got required size %lu, then %lu\n", (ULONG)size, (ULONG)needed);
        ok(info->size.QuadPart == filesize.QuadPart, "got size %I64d, expected %I64d\n", info->size.QuadPart, filesize.QuadPart);
        ok(!CompareFileTime(&info->writetime, &writetime), "write time %08lx%08lx, expected %08lx%08lx\n",
           info->writetime.dwHighDateTime, info->writetime.dwLowDateTime, writetime.dwHighDateTime, writetime.dwLowDateTime);
        ok(info->path[0] != 0, "no path\n");
        ok(SameFile(info->path, path), "path %S is not %S\n", info->path, path);
    }
    HeapFree(GetProcessHeap(), 0, info);

    TestFileData(fri.instance_id, g_datasize, g_data);

    SelectObject(hdc, old);
    DeleteObject(hf);

    /* The private font goes away with the process; a failing removal is not what this test is about */
    ret = RemoveFontResourceExW(path, FR_PRIVATE, 0);
    if (!ret)
        trace("RemoveFontResourceEx failed, error %lu\n", GetLastError());
    DeleteFileW(path);
}

static void TestSystemFont(HDC hdc)
{
    LOGFONTA lf;
    HFONT hf, old;
    FRI fri;
    BYTE buf[1024];
    FFI *info = (FFI *)buf;
    SIZE_T needed;
    BOOL ret;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = 20;
    lf.lfWeight = FW_BOLD;
    lf.lfItalic = TRUE;
    lstrcpyA(lf.lfFaceName, "Tahoma");
    hf = CreateFontIndirectA(&lf);
    old = SelectObject(hdc, hf);

    ZeroMemory(&fri, sizeof(fri));
    fri.size = sizeof(fri);
    ret = pGetFontRealizationInfo(hdc, &fri);
    ok(ret, "failed, error %lu\n", GetLastError());
    ok((fri.flags & 0xF) == 3, "got flags %#lx\n", fri.flags);
    ok(fri.simulations == 2, "got simulations %#x\n", fri.simulations);

    needed = 0;
    ret = pGetFontFileInfo(fri.instance_id, 0, info, sizeof(buf), &needed);
    ok(ret, "failed, error %lu\n", GetLastError());
    if (ret)
    {
        HANDLE h;
        FILETIME time;
        LARGE_INTEGER size;

        h = CreateFileW(info->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        ok(h != INVALID_HANDLE_VALUE, "cannot open %S, error %lu\n", info->path, GetLastError());
        if (h != INVALID_HANDLE_VALUE)
        {
            BYTE head[16], data[16];
            DWORD read;

            GetFileTime(h, NULL, NULL, &time);
            GetFileSizeEx(h, &size);
            ReadFile(h, head, sizeof(head), &read, NULL);
            CloseHandle(h);
            ok(!CompareFileTime(&info->writetime, &time), "write time differs\n");
            ok(info->size.QuadPart == size.QuadPart, "got size %I64d, expected %I64d\n", info->size.QuadPart, size.QuadPart);
            ret = pGetFontFileData(fri.instance_id, 0, 0, data, sizeof(data));
            ok(ret && !memcmp(data, head, sizeof(head)), "file data differs\n");
        }
    }

    SelectObject(hdc, old);
    DeleteObject(hf);
}

START_TEST(GetFontFileInfo)
{
    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    HDC hdc;

    pGetFontRealizationInfo = (void *)GetProcAddress(gdi32, "GetFontRealizationInfo");
    pGetFontFileInfo = (void *)GetProcAddress(gdi32, "GetFontFileInfo");
    pGetFontFileData = (void *)GetProcAddress(gdi32, "GetFontFileData");
    if (!pGetFontRealizationInfo || !pGetFontFileInfo || !pGetFontFileData)
    {
        skip("The font file functions are not available\n");
        return;
    }

    if (!LoadTestFont())
    {
        skip("Cannot load the test font\n");
        return;
    }

    hdc = CreateCompatibleDC(NULL);
    ok(hdc != NULL, "CreateCompatibleDC failed\n");
    if (!hdc)
        return;

    TestSystemFont(hdc);
    TestMemoryFont(hdc);
    TestRemovedMemoryFont(hdc);
    TestPrivateFileFont(hdc);

    DeleteDC(hdc);
}
