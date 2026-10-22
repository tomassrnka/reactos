/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for ws2_32!GetHostNameW and GetAddrInfoExCancel (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

#include <winnls.h>

#define PENDING_ERROR 0xdeadbeef
#define GUARD 0x5555

typedef int (WSAAPI *PFN_GetHostNameW)(WCHAR *, int);
typedef int (WSAAPI *PFN_GetAddrInfoExCancel)(HANDLE *);

/* Checks that Capacity characters were not exceeded and that a failed call left the buffer alone */
static void CheckGuards(const WCHAR *Buffer, int Capacity, int Used, int Line)
{
    int i;

    for (i = Used; i < Capacity + 4; i++)
        ok(Buffer[i] == GUARD, "line %d: character %d was written\n", Line, i);
}

START_TEST(GetHostNameW)
{
    HMODULE Ws2 = GetModuleHandleW(L"ws2_32.dll");
    PFN_GetHostNameW pGetHostNameW = (PFN_GetHostNameW)GetProcAddress(Ws2, "GetHostNameW");
    PFN_GetAddrInfoExCancel pGetAddrInfoExCancel = (PFN_GetAddrInfoExCancel)GetProcAddress(Ws2, "GetAddrInfoExCancel");
    /* ReactOS has both functions from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    BOOL Expected = is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    WSADATA Data;
    CHAR Host[256];
    WCHAR Wide[300], Converted[256];
    HANDLE Handle;
    INT Result, Length, Capacity;

    /* Windows 7 and older have neither: they came with Windows 8 */
    if (!pGetHostNameW)
    {
        ok(!Expected, "GetHostNameW is not exported\n");
        skip("GetHostNameW is not exported\n");
    }
    if (!pGetAddrInfoExCancel)
    {
        ok(!Expected, "GetAddrInfoExCancel is not exported\n");
        skip("GetAddrInfoExCancel is not exported\n");
    }

    if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    if (pGetHostNameW)
    {
        if (gethostname(Host, sizeof(Host)) != 0)
        {
            skip("gethostname failed with %d\n", WSAGetLastError());
        }
        else
        {
            /* the length is the one of the converted name with its terminator, not the length in bytes */
            Length = MultiByteToWideChar(CP_ACP, 0, Host, -1, Converted, RTL_NUMBER_OF(Converted));
            ok(Length > 1 && Length + 6 <= (INT)RTL_NUMBER_OF(Wide), "the host name does not convert (%d)\n", Length);

            if (Length > 1 && Length + 6 <= (INT)RTL_NUMBER_OF(Wide))
            {
                /* the name is the one of gethostname, converted to UTF-16, and nothing else is written */
                for (Capacity = Length; Capacity < Length + 2; Capacity++)
                {
                    int i;

                    for (i = 0; i < (INT)RTL_NUMBER_OF(Wide); i++)
                        Wide[i] = GUARD;
                    WSASetLastError(0);
                    Result = pGetHostNameW(Wide, Capacity);
                    ok_dec(Result, 0);
                    ok(!memcmp(Wide, Converted, Length * sizeof(WCHAR)), "the name is not the converted one\n");
                    CheckGuards(Wide, Capacity, Length, __LINE__);
                }

                /* the terminator needs room as well, and a failed call writes nothing */
                {
                    int i;

                    for (i = 0; i < (INT)RTL_NUMBER_OF(Wide); i++)
                        Wide[i] = GUARD;
                    WSASetLastError(0);
                    Result = pGetHostNameW(Wide, Length - 1);
                    ok_dec(Result, SOCKET_ERROR);
                    ok_dec(WSAGetLastError(), WSAEFAULT);
                    CheckGuards(Wide, Length - 1, 0, __LINE__);
                }
            }
        }

        for (Length = -1; Length <= 1; Length++)
        {
            int i;

            for (i = 0; i < (INT)RTL_NUMBER_OF(Wide); i++)
                Wide[i] = GUARD;
            WSASetLastError(0);
            Result = pGetHostNameW(Wide, Length);
            ok_dec(Result, SOCKET_ERROR);
            ok_dec(WSAGetLastError(), WSAEFAULT);
            CheckGuards(Wide, 1, 0, __LINE__);
        }

        WSASetLastError(0);
        Result = pGetHostNameW(NULL, 300);
        ok_dec(Result, SOCKET_ERROR);
        ok_dec(WSAGetLastError(), WSAEFAULT);
        WSASetLastError(0);
        Result = pGetHostNameW(NULL, 0);
        ok_dec(Result, SOCKET_ERROR);
        ok_dec(WSAGetLastError(), WSAEFAULT);
    }

    if (pGetAddrInfoExCancel)
    {
        /* no lookup is pending, so no handle is valid; the last error is not touched */
        WSASetLastError(PENDING_ERROR);
        Result = pGetAddrInfoExCancel(NULL);
        ok_dec(Result, WSA_INVALID_HANDLE);
        ok_hex(WSAGetLastError(), PENDING_ERROR);

        Handle = NULL;
        WSASetLastError(PENDING_ERROR);
        Result = pGetAddrInfoExCancel(&Handle);
        ok_dec(Result, WSA_INVALID_HANDLE);
        ok_hex(WSAGetLastError(), PENDING_ERROR);
    }

    WSACleanup();
}
