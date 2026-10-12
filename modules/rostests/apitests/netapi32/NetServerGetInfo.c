/*
 * PROJECT:     ReactOS netapi32.dll API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for NetServerGetInfo
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <lmcons.h>
#include <lmerr.h>
#include <lmapibuf.h>
#include <lmserver.h>

typedef struct _CALL_DATA
{
    DWORD Level;
    LPBYTE Buffer;
    NET_API_STATUS Status;
} CALL_DATA, *PCALL_DATA;

static DWORD WINAPI CallThread(LPVOID Parameter)
{
    PCALL_DATA Data = Parameter;

    Data->Status = NetServerGetInfo(NULL, Data->Level, &Data->Buffer);
    return 0;
}

/* A broken server can block the call for minutes: give it a bounded time */
static BOOL bHung = FALSE;

static BOOL CallServerGetInfo(PCALL_DATA Data, DWORD Level)
{
    HANDLE hThread;
    DWORD dwWait;

    if (bHung)
        return FALSE;

    Data->Level = Level;
    Data->Buffer = (LPBYTE)(ULONG_PTR)0xdeadbeef;
    Data->Status = 0xdeadbeef;

    hThread = CreateThread(NULL, 0, CallThread, Data, 0, NULL);
    ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (hThread == NULL)
        return FALSE;

    dwWait = WaitForSingleObject(hThread, 30000);
    CloseHandle(hThread);
    ok(dwWait == WAIT_OBJECT_0, "Level %lu: call did not return within 30 s\n", Level);
    bHung = (dwWait != WAIT_OBJECT_0);
    return !bHung;
}

START_TEST(NetServerGetInfo)
{
    static const DWORD Levels[] = { 100, 101, 102 };
    WCHAR szComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD dwLength = _countof(szComputerName);
    static CALL_DATA Data;
    LPCWSTR pszName;
    DWORD dwPlatform;
    ULONG Round, i;

    ok(GetComputerNameW(szComputerName, &dwLength), "GetComputerNameW failed: %lu\n", GetLastError());

    /* Repeat: the first call must leave the service able to answer the next ones */
    for (Round = 0; Round < 3; Round++)
    {
        for (i = 0; i < _countof(Levels); i++)
        {
            if (!CallServerGetInfo(&Data, Levels[i]))
            {
                skip("Server service does not answer, remaining calls skipped\n");
                return;
            }

            ok(Data.Status == NERR_Success, "Round %lu level %lu: status %lu (0x%lx)\n",
               Round, Levels[i], Data.Status, Data.Status);
            if (Data.Status != NERR_Success)
            {
                ok(Data.Buffer == NULL, "Round %lu level %lu: buffer %p on failure\n", Round, Levels[i], Data.Buffer);
                continue;
            }

            ok(Data.Buffer != NULL, "Round %lu level %lu: no buffer\n", Round, Levels[i]);
            if (Data.Buffer == NULL)
                continue;

            switch (Levels[i])
            {
                case 100:
                    dwPlatform = ((PSERVER_INFO_100)Data.Buffer)->sv100_platform_id;
                    pszName = ((PSERVER_INFO_100)Data.Buffer)->sv100_name;
                    break;

                case 101:
                    dwPlatform = ((PSERVER_INFO_101)Data.Buffer)->sv101_platform_id;
                    pszName = ((PSERVER_INFO_101)Data.Buffer)->sv101_name;
                    ok(((PSERVER_INFO_101)Data.Buffer)->sv101_type & SV_TYPE_NT,
                       "Round %lu: sv101_type 0x%lx lacks SV_TYPE_NT\n", Round,
                       ((PSERVER_INFO_101)Data.Buffer)->sv101_type);
                    break;

                default:
                    dwPlatform = ((PSERVER_INFO_102)Data.Buffer)->sv102_platform_id;
                    pszName = ((PSERVER_INFO_102)Data.Buffer)->sv102_name;
                    ok(((PSERVER_INFO_102)Data.Buffer)->sv102_type & SV_TYPE_NT,
                       "Round %lu: sv102_type 0x%lx lacks SV_TYPE_NT\n", Round,
                       ((PSERVER_INFO_102)Data.Buffer)->sv102_type);
                    break;
            }

            ok(dwPlatform == PLATFORM_ID_NT, "Round %lu level %lu: platform %lu\n", Round, Levels[i], dwPlatform);
            ok(pszName != NULL && _wcsicmp(pszName, szComputerName) == 0,
               "Round %lu level %lu: name '%ls', expected '%ls'\n", Round, Levels[i],
               pszName ? pszName : L"(null)", szComputerName);

            ok(NetApiBufferFree(Data.Buffer) == NERR_Success, "Round %lu level %lu: NetApiBufferFree failed\n",
               Round, Levels[i]);
        }
    }

    if (CallServerGetInfo(&Data, 99))
    {
        ok(Data.Status == ERROR_INVALID_LEVEL, "Level 99: status %lu\n", Data.Status);
        ok(Data.Buffer == NULL, "Level 99: buffer %p\n", Data.Buffer);
    }

    /* The server still answers after the failed call */
    if (CallServerGetInfo(&Data, 100))
    {
        ok(Data.Status == NERR_Success, "Level 100 after level 99: status %lu\n", Data.Status);
        if (Data.Status == NERR_Success && Data.Buffer != NULL)
            NetApiBufferFree(Data.Buffer);
    }
}
