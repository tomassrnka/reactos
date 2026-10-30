/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     CryptStringToBinary with CRYPT_STRING_STRICT, which OpenSSH for Windows passes to decode the
 *              file descriptor state it hands to its child (a refused flag made the child use garbage)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 *
 * The expected values are what Windows 10 22H2 does.
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>

#ifndef CRYPT_STRING_STRICT
#define CRYPT_STRING_STRICT 0x20000000
#endif

static const BYTE Binary[8] = {0x00, 0x2a, 0x4f, 0x74, 0x01, 0xbe, 0xe3, 0x08};
static const char Text[] = "ACpPdAG+4wg=";

static void DecodeA(DWORD Flags, DWORD ExpectedFlags, PCSTR What)
{
    BYTE Out[16] = {0};
    DWORD Length = 0, Skip = 9, Used = 9;
    BOOL Ok;

    Ok = CryptStringToBinaryA(Text, 0, Flags, NULL, &Length, &Skip, &Used);
    ok(Ok, "%s: the size query failed: %lu\n", What, GetLastError());
    ok(Length == sizeof(Binary), "%s: size %lu\n", What, Length);
    ok(Skip == 0, "%s: skipped %lu\n", What, Skip);
    ok(Used == ExpectedFlags, "%s: flags %lu, expected %lu\n", What, Used, ExpectedFlags);

    Length = sizeof(Out);
    Ok = CryptStringToBinaryA(Text, 0, Flags, Out, &Length, &Skip, &Used);
    ok(Ok, "%s: decoding failed: %lu\n", What, GetLastError());
    ok(Length == sizeof(Binary), "%s: decoded %lu bytes\n", What, Length);
    ok(!memcmp(Out, Binary, sizeof(Binary)), "%s: wrong bytes\n", What);
}

static void DecodeW(DWORD Flags, PCSTR What)
{
    WCHAR WideText[sizeof(Text)];
    BYTE Out[16] = {0};
    DWORD Length = sizeof(Out), Skip = 9, Used = 9, i;
    BOOL Ok;

    for (i = 0; i < sizeof(Text); i++)
        WideText[i] = Text[i];
    Ok = CryptStringToBinaryW(WideText, 0, Flags, Out, &Length, &Skip, &Used);
    ok(Ok, "%s: wide decoding failed: %lu\n", What, GetLastError());
    ok(Length == sizeof(Binary) && !memcmp(Out, Binary, sizeof(Binary)), "%s: wide: wrong bytes\n", What);
}

static void Refused(DWORD Flags, PCSTR What)
{
    DWORD Length = 0, Skip, Used;
    BOOL Ok;

    SetLastError(0xdeadbeef);
    Ok = CryptStringToBinaryA(Text, 0, Flags, NULL, &Length, &Skip, &Used);
    ok(!Ok, "%s: accepted\n", What);
    ok(GetLastError() == ERROR_INVALID_DATA, "%s: error %lu\n", What, GetLastError());
}

START_TEST(CryptStringToBinaryStrict)
{
    /* The plain flag, and with the strict bit: the same result */
    DecodeA(CRYPT_STRING_BASE64, CRYPT_STRING_BASE64, "BASE64");
    DecodeA(CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, CRYPT_STRING_BASE64, "BASE64|STRICT");
    DecodeA(CRYPT_STRING_ANY | CRYPT_STRING_STRICT, CRYPT_STRING_BASE64, "ANY|STRICT");
    DecodeW(CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, "BASE64|STRICT");

    /* Other bits above the type are refused */
    Refused(CRYPT_STRING_BASE64 | 0x10000000, "BASE64|0x10000000");
    Refused(CRYPT_STRING_BASE64 | 0x40000000, "BASE64|0x40000000");
    Refused(CRYPT_STRING_BASE64 | 0x80000000, "BASE64|0x80000000");
}
