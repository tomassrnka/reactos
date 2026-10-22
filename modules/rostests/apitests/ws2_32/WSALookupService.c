/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:     Test for the WSALookupServiceBeginA query-set conversion
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <pseh/pseh2.h>

/*
 * WSALookupServiceBeginA converts its ANSI WSAQUERYSETA into a Unicode one.
 * ComputeQuerySetSize sizes the relative buffer and WsBufferAllocate fills it.
 * The two used to disagree on alignment: the size rounded the GUID and the
 * other pointer-sized parts to four bytes, the fill aligned them to
 * sizeof(PVOID). On amd64 a pointer is eight bytes, so the fill wrote up to
 * four bytes past the computed size and corrupted the next heap block.
 *
 * A valid lpServiceClassId GUID is needed to pass the pointer validation and
 * also forces the mismatched alignment; a one-character instance name leaves
 * that GUID at an offset the two rounding rules disagree on. On the regular
 * heap the overrun lands in the block's slack and is invisible, so this test
 * must run under page heap (GlobalFlag 0x02000000 on the test image, e.g.
 * `gflags /p /enable <image> /full`) to exercise the regression: before the
 * fix it faults, after it runs clean. Without page heap it passes either way.
 */

START_TEST(WSALookupService)
{
    WSADATA WsaData;
    GUID ServiceClass = { 0x11112222, 0x3333, 0x4444,
                          { 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc } };
    GUID ProviderId   = { 0xdddd0000, 0x1111, 0x2222,
                          { 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa } };
    WSAQUERYSETA Query;
    HANDLE Lookup;
    INT Ret;
    unsigned int i;
    volatile LONG Faulted = 0;
    volatile ULONG Code = 0;
    int Started;

    Started = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Started == 0, "WSAStartup failed: %d\n", Started);
    if (Started != 0)
        return;

    _SEH2_TRY
    {
        for (i = 0; i < 64; i++)
        {
            ZeroMemory(&Query, sizeof(Query));
            Query.dwSize = sizeof(Query);
            Query.lpszServiceInstanceName = (LPSTR)"a";
            Query.lpServiceClassId = &ServiceClass;
            Query.lpNSProviderId = &ProviderId;
            Query.dwNameSpace = NS_ALL;

            Lookup = NULL;
            Ret = WSALookupServiceBeginA(&Query, LUP_RETURN_ALL, &Lookup);
            if (Ret == ERROR_SUCCESS && Lookup != NULL)
                WSALookupServiceEnd(Lookup);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Faulted = 1;
        Code = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    ok(Faulted == 0,
       "WSALookupServiceBeginA faulted (0x%lx): the query-set buffer was overrun\n",
       (unsigned long)Code);

    WSACleanup();
}
