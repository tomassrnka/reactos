/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     With INLINE_HRESULT_FROM_WIN32 the argument of HRESULT_FROM_WIN32 is evaluated once
 */

#define INLINE_HRESULT_FROM_WIN32 1
#include <apitest.h>
#include <winerror.h>

static int g_Calls;

static DWORD CountedCall(DWORD Value)
{
    g_Calls++;
    return Value;
}

START_TEST(hresult_inline)
{
    HRESULT hr;

    g_Calls = 0;
    hr = HRESULT_FROM_WIN32(CountedCall(ERROR_ACCESS_DENIED));
    ok_hex(hr, 0x80070005);
    ok_int(g_Calls, 1);

    g_Calls = 0;
    hr = HRESULT_FROM_WIN32(CountedCall(ERROR_SUCCESS));
    ok_hex(hr, S_OK);
    ok_int(g_Calls, 1);

    g_Calls = 0;
    hr = HRESULT_FROM_WIN32(CountedCall((DWORD)E_FAIL));
    ok_hex(hr, E_FAIL);
    ok_int(g_Calls, 1);

    g_Calls = 0;
    hr = HRESULT_FROM_WIN32(CountedCall(0x12345));
    ok_hex(hr, 0x80072345);
    ok_int(g_Calls, 1);
}
