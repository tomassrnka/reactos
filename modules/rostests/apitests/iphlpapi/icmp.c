/*
 * PROJECT:         ReactOS API Tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Tests for ICMP functions
 * PROGRAMMERS:     Tim Crawford
 *                  Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include <apitest.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

static
void
test_IcmpCreateFile(void)
{
    HANDLE hIcmp;

    SetLastError(0xDEADBEEF);
    hIcmp = IcmpCreateFile();
    ok(hIcmp != INVALID_HANDLE_VALUE, "IcmpCreateFile failed unexpectedly: %lu\n", GetLastError());

    if (hIcmp != INVALID_HANDLE_VALUE)
        IcmpCloseHandle(hIcmp);
}

static
void
test_Icmp6CreateFile(void)
{
    HANDLE hIcmp;

    SetLastError(0xDEADBEEF);
    hIcmp = Icmp6CreateFile();

    if (GetLastError() == ERROR_FILE_NOT_FOUND)
    {
        /* On Windows Server 2003, the IPv6 protocol must be installed. */
        skip("IPv6 is not available.\n");
        return;
    }

    ok(hIcmp != INVALID_HANDLE_VALUE, "Icmp6CreateFile failed unexpectedly: %lu\n", GetLastError());

    if (hIcmp != INVALID_HANDLE_VALUE)
        IcmpCloseHandle(hIcmp);
}

static
void
test_IcmpCloseHandle(void)
{
    HANDLE hIcmp;
    BOOL bRet;
    DWORD LastError;

    SetLastError(0xDEADBEEF);
    hIcmp = IcmpCreateFile();
    if (hIcmp != INVALID_HANDLE_VALUE)
    {
        bRet = IcmpCloseHandle(hIcmp);
        ok(bRet, "IcmpCloseHandle failed unexpectedly: %lu\n", GetLastError());
    }

    SetLastError(0xDEADBEEF);
    hIcmp = Icmp6CreateFile();
    if (hIcmp != INVALID_HANDLE_VALUE)
    {
        bRet = IcmpCloseHandle(hIcmp);
        ok(bRet, "IcmpCloseHandle failed unexpectedly: %lu\n", GetLastError());
    }

    hIcmp = INVALID_HANDLE_VALUE;
    SetLastError(0xDEADBEEF);
    bRet = IcmpCloseHandle(hIcmp);
    ok(!bRet, "IcmpCloseHandle succeeded unexpectedly\n");
    LastError = GetLastError();
    ok(LastError == ERROR_INVALID_HANDLE || LastError == ERROR_INVALID_PARAMETER,
       "Unexpected last error (0x%lX)\n", LastError);

    hIcmp = NULL;
    SetLastError(0xDEADBEEF);
    bRet = IcmpCloseHandle(hIcmp);
    ok(!bRet, "IcmpCloseHandle succeeded unexpectedly\n");
    LastError = GetLastError();
    ok(LastError == ERROR_INVALID_HANDLE || LastError == ERROR_INVALID_PARAMETER,
       "Unexpected last error (0x%lX)\n", LastError);
}

static
void
test_IcmpSendEcho(void)
{
    HANDLE hIcmp;
    unsigned long ipaddr = INADDR_NONE;
    DWORD bRet = 0, error = 0;
    char SendData[32] = "Data Buffer";
    PVOID ReplyBuffer;
    DWORD ReplySize = 0;

    SetLastError(0xDEADBEEF);
    hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE)
    {
        skip("IcmpCreateFile failed unexpectedly: %lu\n", GetLastError());
        return;
    }

    ipaddr = 0x08080808; // 8.8.8.8
    ReplyBuffer = malloc(sizeof(ICMP_ECHO_REPLY) + sizeof(SendData));

    ReplySize = sizeof(ICMP_ECHO_REPLY);
    SetLastError(0xDEADBEEF);
    bRet = IcmpSendEcho(hIcmp, ipaddr, SendData, sizeof(SendData),
        NULL, ReplyBuffer, ReplySize, 5000);

    ok(!bRet, "IcmpSendEcho succeeded unexpectedly\n");
    error = GetLastError();
    ok(error == IP_BUF_TOO_SMALL /* Win2003 */ ||
       error == IP_GENERAL_FAILURE /* Win10 */,
       "IcmpSendEcho returned unexpected error: %lu\n", error);

    ReplySize = sizeof(ICMP_ECHO_REPLY) + sizeof(SendData);
    SetLastError(0xDEADBEEF);
    bRet = IcmpSendEcho(hIcmp, ipaddr, SendData, sizeof(SendData),
        NULL, ReplyBuffer, ReplySize, 5000);

    ok(bRet, "IcmpSendEcho failed unexpectedly: %lu\n", GetLastError());

    free(ReplyBuffer);
    IcmpCloseHandle(hIcmp);
}

#define CONCURRENT_DATA_SIZE 16
#define CONCURRENT_REPLIES 4

typedef struct _CONCURRENT_ECHO
{
    char Data[CONCURRENT_DATA_SIZE];
    UCHAR ReplyBuffer[CONCURRENT_REPLIES * (sizeof(ICMP_ECHO_REPLY) + CONCURRENT_DATA_SIZE + 8)];
    DWORD ReplySize;
    DWORD Replies;
    DWORD Error;
    DWORD Start;
    DWORD End;
} CONCURRENT_ECHO, *PCONCURRENT_ECHO;

static
void
SendConcurrentEcho(PCONCURRENT_ECHO Echo)
{
    HANDLE hIcmp;

    Echo->Replies = 0;
    Echo->Error = 0;
    hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE)
    {
        Echo->Error = GetLastError();
        return;
    }
    SetLastError(0xDEADBEEF);
    Echo->Start = GetTickCount();
    Echo->Replies = IcmpSendEcho(hIcmp, htonl(INADDR_LOOPBACK), Echo->Data, sizeof(Echo->Data),
                                 NULL, Echo->ReplyBuffer, Echo->ReplySize, 3000);
    Echo->End = GetTickCount();
    if (!Echo->Replies)
        Echo->Error = GetLastError();
    IcmpCloseHandle(hIcmp);
}

static
DWORD
WINAPI
ConcurrentEchoThread(LPVOID Parameter)
{
    SendConcurrentEcho(Parameter);
    return 0;
}

static
void
CheckConcurrentReplies(PCONCURRENT_ECHO Echo, const char *Name)
{
    PICMP_ECHO_REPLY Reply = (PICMP_ECHO_REPLY)Echo->ReplyBuffer;
    DWORD i;

    ok(Echo->Replies == 1, "%s: %lu replies, error %lu\n", Name, Echo->Replies, Echo->Error);
    for (i = 0; i < Echo->Replies && i < CONCURRENT_REPLIES; i++)
    {
        ok(Reply[i].Status == IP_SUCCESS, "%s: reply %lu status %lu\n", Name, i, Reply[i].Status);
        ok(Reply[i].Address == htonl(INADDR_LOOPBACK), "%s: reply %lu from 0x%lx\n", Name, i, Reply[i].Address);
        ok(Reply[i].DataSize == CONCURRENT_DATA_SIZE, "%s: reply %lu has %u data bytes\n", Name, i, Reply[i].DataSize);
        if (Reply[i].Data && Reply[i].DataSize == CONCURRENT_DATA_SIZE)
            ok(!memcmp(Reply[i].Data, Echo->Data, CONCURRENT_DATA_SIZE), "%s: reply %lu holds another request's data\n", Name, i);
    }
}

/* Two echo requests outstanding at the same time: each must get only the reply to itself.
 * The first one has room for several replies, so it would also collect the second one's.
 * A round in which the first request returned before the second one was sent proves nothing
 * (a system may return on the first reply) and is reported as skipped. */
static
void
test_IcmpSendEchoConcurrent(void)
{
    static CONCURRENT_ECHO First, Second;
    HANDLE hThread;
    DWORD Round, Wait, Overlapped = 0;

    for (Round = 0; Round < 3; Round++)
    {
        memset(&First, 0, sizeof(First));
        memset(&Second, 0, sizeof(Second));
        memset(First.Data, 'A', sizeof(First.Data));
        memset(Second.Data, 'B', sizeof(Second.Data));
        First.ReplySize = sizeof(First.ReplyBuffer);
        Second.ReplySize = sizeof(ICMP_ECHO_REPLY) + CONCURRENT_DATA_SIZE + 8;

        hThread = CreateThread(NULL, 0, ConcurrentEchoThread, &First, 0, NULL);
        ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!hThread)
            return;
        Sleep(500);
        SendConcurrentEcho(&Second);
        Wait = WaitForSingleObject(hThread, 10000);
        ok(Wait == WAIT_OBJECT_0, "Wait returned %lu\n", Wait);
        CloseHandle(hThread);
        if (Wait != WAIT_OBJECT_0)
            return;

        CheckConcurrentReplies(&First, "First");
        CheckConcurrentReplies(&Second, "Second");
        if ((LONG)(First.Start - Second.Start) <= 0 && (LONG)(Second.Start - First.End) < 0)
            Overlapped++;
    }
    if (!Overlapped)
        skip("The echo requests never overlapped\n");
}

START_TEST(icmp)
{
    test_IcmpCreateFile();
    test_Icmp6CreateFile();
    test_IcmpCloseHandle();
    test_IcmpSendEcho();
    test_IcmpSendEchoConcurrent();
}
