/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for user32!GetUpdatedClipboardFormats
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64). LibreOffice (vclplug_winlo.dll) imports it.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>

typedef BOOL (WINAPI *PFN_GetUpdated)(PUINT, UINT, PUINT);

static BOOL Has(const UINT *Formats, UINT Count, UINT Format)
{
    UINT i;

    for (i = 0; i < Count; i++)
    {
        if (Formats[i] == Format)
            return TRUE;
    }
    return FALSE;
}

START_TEST(GetUpdatedClipboardFormats)
{
    PFN_GetUpdated pGet = (PFN_GetUpdated)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                         "GetUpdatedClipboardFormats");
    UINT Formats[32], Count;
    HWND Window;
    HGLOBAL Mem;
    BOOL Ret;
    DWORD Error;
    char *Text;

    ok(pGet != NULL, "GetUpdatedClipboardFormats is not exported by user32.dll\n");
    if (!pGet)
    {
        skip("GetUpdatedClipboardFormats is not exported\n");
        return;
    }

    /* The count is not optional */
    SetLastError(0xdeadbeef);
    Ret = pGet(Formats, _countof(Formats), NULL);
    Error = GetLastError();
    ok(!Ret, "No count succeeded\n");
    ok(Error == ERROR_NOACCESS, "Error is %lu\n", Error);

    Window = CreateWindowExW(0, L"STATIC", L"clip", 0, 0, 0, 10, 10, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (!Window || !OpenClipboard(Window))
    {
        skip("Cannot open the clipboard (error %lu)\n", GetLastError());
        if (Window)
            DestroyWindow(Window);
        return;
    }

    /* Empty clipboard */
    EmptyClipboard();
    CloseClipboard();
    Count = 77;
    SetLastError(0xdeadbeef);
    Ret = pGet(NULL, 0, &Count);
    ok(Ret, "An empty clipboard failed, error %lu\n", GetLastError());
    ok(Count == 0, "Count is %u\n", Count);

    /* Text, with the formats that come from it */
    ok(OpenClipboard(Window), "OpenClipboard failed\n");
    EmptyClipboard();
    Mem = GlobalAlloc(GMEM_MOVEABLE, 6);
    Text = GlobalLock(Mem);
    strcpy(Text, "hello");
    GlobalUnlock(Mem);
    ok(SetClipboardData(CF_TEXT, Mem) != NULL, "SetClipboardData failed\n");
    CloseClipboard();

    Count = 77;
    SetLastError(0xdeadbeef);
    Ret = pGet(NULL, 0, &Count);
    Error = GetLastError();
    ok(!Ret, "No buffer succeeded\n");
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "Error is %lu\n", Error);
    ok(Count == 4, "Count is %u\n", Count);

    /* A format buffer is needed unless the size is 0 */
    Count = 77;
    SetLastError(0xdeadbeef);
    Ret = pGet(NULL, _countof(Formats), &Count);
    Error = GetLastError();
    ok(!Ret, "No buffer but a size succeeded\n");
    ok(Error == ERROR_NOACCESS, "Error is %lu\n", Error);
    ok(Count == 4, "Count is %u\n", Count);

    Count = 77;
    SetLastError(0xdeadbeef);
    Ret = pGet(Formats, 1, &Count);
    Error = GetLastError();
    ok(!Ret, "A short buffer succeeded\n");
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "Error is %lu\n", Error);
    ok(Count == 4, "Count is %u\n", Count);

    Count = 77;
    SetLastError(0xdeadbeef);
    Ret = pGet(Formats, _countof(Formats), &Count);
    ok(Ret, "GetUpdatedClipboardFormats failed, error %lu\n", GetLastError());
    ok(Count == 4, "Count is %u\n", Count);
    if (Ret && Count == 4)
    {
        ok(Has(Formats, Count, CF_TEXT), "CF_TEXT is missing\n");
        ok(Has(Formats, Count, CF_OEMTEXT), "CF_OEMTEXT is missing\n");
        ok(Has(Formats, Count, CF_UNICODETEXT), "CF_UNICODETEXT is missing\n");
        ok(Has(Formats, Count, CF_LOCALE), "CF_LOCALE is missing\n");
        /* the order of the clipboard: the format that was set, then the ones synthesized from it */
        ok(Formats[0] == CF_TEXT && Formats[1] == CF_LOCALE && Formats[2] == CF_OEMTEXT && Formats[3] == CF_UNICODETEXT,
           "Order is %u %u %u %u\n", Formats[0], Formats[1], Formats[2], Formats[3]);
    }

    /* The formats are not consumed by asking */
    Count = 77;
    Ret = pGet(Formats, _countof(Formats), &Count);
    ok(Ret && Count == 4, "Second call: %d, %u formats\n", Ret, Count);

    if (OpenClipboard(Window))
    {
        EmptyClipboard();
        CloseClipboard();
    }
    DestroyWindow(Window);
}
