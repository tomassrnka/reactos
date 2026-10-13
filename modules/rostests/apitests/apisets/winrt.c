/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The WinRT apisets resolve to a module that has their functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>

typedef struct HSTRING__ *HSTRING;
typedef HRESULT (WINAPI *FN_WindowsCreateString)(LPCWSTR, UINT32, HSTRING *);
typedef LPCWSTR (WINAPI *FN_WindowsGetStringRawBuffer)(HSTRING, UINT32 *);
typedef HRESULT (WINAPI *FN_WindowsDeleteString)(HSTRING);
typedef HRESULT (WINAPI *FN_RoGetActivationFactory)(HSTRING, REFIID, void **);
typedef struct HSTRING_BUFFER__ *HSTRING_BUFFER;
typedef HRESULT (WINAPI *FN_WindowsPreallocateStringBuffer)(UINT32, WCHAR **, HSTRING_BUFFER *);
typedef HRESULT (WINAPI *FN_WindowsDeleteStringBuffer)(HSTRING_BUFFER);

START_TEST(winrt)
{
    static const WCHAR Text[] = L"ReactOS";
    FN_WindowsCreateString pWindowsCreateString;
    FN_WindowsGetStringRawBuffer pWindowsGetStringRawBuffer;
    FN_WindowsDeleteString pWindowsDeleteString;
    FN_RoGetActivationFactory pRoGetActivationFactory;
    FN_WindowsPreallocateStringBuffer pWindowsPreallocateStringBuffer;
    FN_WindowsDeleteStringBuffer pWindowsDeleteStringBuffer;
    HMODULE hString, hWinrt;
    HSTRING String = NULL;
    LPCWSTR Buffer;
    UINT32 Length = 0;
    HRESULT hr;

    hString = LoadLibraryW(L"api-ms-win-core-winrt-string-l1-1-0.dll");
    hWinrt = LoadLibraryW(L"api-ms-win-core-winrt-l1-1-0.dll");
    if (!hString && !is_reactos() && LOBYTE(LOWORD(GetVersion())) == 6 && HIBYTE(LOWORD(GetVersion())) < 2)
    {
        /* Windows 7 has no WinRT */
        skip("No WinRT apisets before Windows 8\n");
        return;
    }
    ok(hString != NULL, "api-ms-win-core-winrt-string-l1-1-0 did not load: %lu\n", GetLastError());
    ok(hWinrt != NULL, "api-ms-win-core-winrt-l1-1-0 did not load: %lu\n", GetLastError());
    if (!hString || !hWinrt)
        return;

    pWindowsCreateString = (FN_WindowsCreateString)GetProcAddress(hString, "WindowsCreateString");
    pWindowsGetStringRawBuffer = (FN_WindowsGetStringRawBuffer)GetProcAddress(hString, "WindowsGetStringRawBuffer");
    pWindowsDeleteString = (FN_WindowsDeleteString)GetProcAddress(hString, "WindowsDeleteString");
    pRoGetActivationFactory = (FN_RoGetActivationFactory)GetProcAddress(hWinrt, "RoGetActivationFactory");
    ok(pWindowsCreateString && pWindowsGetStringRawBuffer && pWindowsDeleteString,
       "The WinRT string apiset lacks its functions\n");
    ok(pRoGetActivationFactory != NULL, "The WinRT apiset lacks RoGetActivationFactory\n");
    if (pWindowsCreateString && pWindowsGetStringRawBuffer && pWindowsDeleteString)
    {
        hr = pWindowsCreateString(Text, ARRAYSIZE(Text) - 1, &String);
        ok(hr == S_OK, "WindowsCreateString returned 0x%lx\n", hr);
        Buffer = pWindowsGetStringRawBuffer(String, &Length);
        ok(Length == ARRAYSIZE(Text) - 1, "Length = %u\n", Length);
        ok(Buffer && !wcscmp(Buffer, Text), "Buffer = '%S'\n", Buffer);
        hr = pWindowsDeleteString(String);
        ok(hr == S_OK, "WindowsDeleteString returned 0x%lx\n", hr);
    }

    /* A buffer whose size does not fit 32 bits is refused, not truncated (as on Windows 10) */
    pWindowsPreallocateStringBuffer = (FN_WindowsPreallocateStringBuffer)GetProcAddress(hString, "WindowsPreallocateStringBuffer");
    pWindowsDeleteStringBuffer = (FN_WindowsDeleteStringBuffer)GetProcAddress(hString, "WindowsDeleteStringBuffer");
    ok(pWindowsPreallocateStringBuffer && pWindowsDeleteStringBuffer, "The WinRT string apiset lacks the buffer functions\n");
    if (pWindowsPreallocateStringBuffer && pWindowsDeleteStringBuffer)
    {
        WCHAR *Chars = (WCHAR *)(ULONG_PTR)0xdeadbeef;
        HSTRING_BUFFER StringBuffer = NULL;

        hr = pWindowsPreallocateStringBuffer(sizeof(void *) == 4 ? 0x80000000 : 0xFFFFFFFF, &Chars, &StringBuffer);
        ok(hr == MEM_E_INVALID_SIZE, "Oversized WindowsPreallocateStringBuffer returned 0x%lx\n", hr);
        if (SUCCEEDED(hr))
            pWindowsDeleteStringBuffer(StringBuffer);
    }

    FreeLibrary(hWinrt);
    FreeLibrary(hString);
}
