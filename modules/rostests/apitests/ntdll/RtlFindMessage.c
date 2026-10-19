/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the hard error texts in the NTDLL message table
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifdef _WIN64
#define CODE_ADDRESS    ((ULONG_PTR)0x7FFB6006F19ULL)
#define CODE_HEX        L"7FFB6006F19"
#define DATA_ADDRESS    ((ULONG_PTR)0xFFFFF80012345678ULL)
#define DATA_HEX        L"FFFFF80012345678"
#else
#define CODE_ADDRESS    ((ULONG_PTR)0x86006F19)
#define CODE_HEX        L"86006F19"
#define DATA_ADDRESS    ((ULONG_PTR)0x12345678)
#define DATA_HEX        L"12345678"
#endif

/*
 * Formats a message of the NTDLL message table the way the hard error dialog
 * does: the text is a printf format and the parameters are pointer-sized.
 */
static BOOL
FormatStatusMessage(
    _In_ NTSTATUS Code,
    _Out_writes_(cchOut) PWSTR pszOut,
    _In_ SIZE_T cchOut,
    _In_ ULONG_PTR Param0,
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2)
{
    PMESSAGE_RESOURCE_ENTRY Entry;
    NTSTATUS Status;
    ANSI_STRING AnsiText;
    UNICODE_STRING Format;
    WCHAR szFormat[512];
    PWSTR p;

    Status = RtlFindMessage(GetModuleHandleW(L"ntdll.dll"),
                            11, // RT_MESSAGETABLE
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                            Code,
                            &Entry);
    ok(Status == STATUS_SUCCESS, "RtlFindMessage(0x%08lx) returned 0x%08lx\n", Code, Status);
    if (!NT_SUCCESS(Status))
        return FALSE;

    if (Entry->Flags & MESSAGE_RESOURCE_UNICODE)
        StringCchCopyW(szFormat, _countof(szFormat), (PCWSTR)Entry->Text);
    else
    {
        RtlInitAnsiString(&AnsiText, (PCSZ)Entry->Text);
        RtlInitEmptyUnicodeString(&Format, szFormat, sizeof(szFormat) - sizeof(WCHAR));
        RtlAnsiStringToUnicodeString(&Format, &AnsiText, FALSE);
        szFormat[Format.Length / sizeof(WCHAR)] = UNICODE_NULL;
    }

    /* Trace the format on one line */
    for (p = szFormat; *p; p++)
    {
        if (*p == L'\r' || *p == L'\n')
            *p = L' ';
    }
    trace("0x%08lx: '%ls'\n", Code, szFormat);

    _snwprintf(pszOut, cchOut, szFormat, Param0, Param1, Param2, (ULONG_PTR)0);
    pszOut[cchOut - 1] = UNICODE_NULL;
    _wcsupr(pszOut);
    return TRUE;
}

START_TEST(RtlFindMessage)
{
    WCHAR szText[1024];
    BOOL bFullAddress;

    /* Windows Server 2008 R2 still prints 32 bits of the address in the first two texts */
    bFullAddress = is_reactos() || GetNTVersion() >= _WIN32_WINNT_WIN10;

    /* The exception code is 32 bits, the address is pointer-sized */
    if (FormatStatusMessage(STATUS_UNHANDLED_EXCEPTION, szText, _countof(szText),
                            (ULONG_PTR)L"unknown software exception", 0xC0000417, CODE_ADDRESS))
    {
        ok(wcsstr(szText, L"C0000417") != NULL, "No exception code in '%ls'\n", szText);
        ok(!bFullAddress || wcsstr(szText, CODE_HEX) != NULL, "No full address in '%ls'\n", szText);
    }

    if (FormatStatusMessage(STATUS_ACCESS_VIOLATION, szText, _countof(szText),
                            CODE_ADDRESS, DATA_ADDRESS, (ULONG_PTR)L"read"))
    {
        ok(!bFullAddress || wcsstr(szText, CODE_HEX) != NULL, "No full instruction address in '%ls'\n", szText);
        ok(!bFullAddress || wcsstr(szText, DATA_HEX) != NULL, "No full data address in '%ls'\n", szText);
        ok(wcsstr(szText, L"READ") != NULL, "No access type in '%ls'\n", szText);
    }

    /* The third parameter is the I/O status */
    if (FormatStatusMessage(STATUS_IN_PAGE_ERROR, szText, _countof(szText),
                            CODE_ADDRESS, DATA_ADDRESS, 0xC000009C))
    {
        ok(wcsstr(szText, CODE_HEX) != NULL, "No full instruction address in '%ls'\n", szText);
        ok(wcsstr(szText, DATA_HEX) != NULL, "No full data address in '%ls'\n", szText);
        ok(wcsstr(szText, L"C000009C") != NULL, "No I/O status in '%ls'\n", szText);
    }
}
