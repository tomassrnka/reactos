/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HRESULT_FROM_WIN32 is a constant expression in C++, also with INLINE_HRESULT_FROM_WIN32
 */

#define INLINE_HRESULT_FROM_WIN32 1
#include <apitest.h>
#include <winerror.h>

static const HRESULT g_StaticCpp = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
static constexpr HRESULT g_Constexpr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
static char g_Array[HRESULT_FROM_WIN32(0) + 1];

static int SwitchValueCpp(HRESULT hr)
{
    switch (hr)
    {
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): return 1;
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND): return 2;
        default: return 0;
    }
}

START_TEST(hresult_cpp)
{
    ok_hex(g_StaticCpp, 0x80070005);
    ok_hex(g_Constexpr, 0x80070002);
    ok_int((int)sizeof(g_Array), 1);
    ok_int(SwitchValueCpp(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)), 1);
    ok_int(SwitchValueCpp(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)), 2);
    ok_int(SwitchValueCpp(S_OK), 0);
}
