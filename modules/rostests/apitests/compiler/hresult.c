/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HRESULT_FROM_WIN32 keeps its values and stays a constant expression in C
 */

#include <apitest.h>
#include <winerror.h>

static const HRESULT g_Static = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);

enum { ENUM_VALUE = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) & 0xFFFF };

static int SwitchValue(HRESULT hr)
{
    switch (hr)
    {
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): return 1;
        default: return 0;
    }
}

static HRESULT FromValue(DWORD Value)
{
    return HRESULT_FROM_WIN32(Value);
}

START_TEST(hresult)
{
    char Array[HRESULT_FROM_WIN32(0) + 1];

    ok_hex(FromValue(ERROR_ACCESS_DENIED), 0x80070005);
    ok_hex(FromValue(ERROR_SUCCESS), S_OK);
    ok_hex(FromValue((DWORD)E_FAIL), E_FAIL);
    ok_hex(FromValue(0x12345), 0x80072345);

    ok_hex(g_Static, 0x80070005);
    ok_int(ENUM_VALUE, ERROR_FILE_NOT_FOUND);
    ok_int(SwitchValue(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)), 1);
    ok_int(SwitchValue(S_OK), 0);
    ok_int((int)sizeof(Array), 1);
}
