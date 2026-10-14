/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for terminating a thread in NtConnectPort while the server accepts it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define TEST_CONNECTION_INFO_SIGNATURE 0xaabb0125
#define TEST_SECTION_SIZE 0x10000
#define TEST_CLIENT_STACK 0x10000
#define TEST_MAX_ITERATIONS 4000
#define TEST_MAX_TIME_MS 15000
#define TEST_WAIT_MS 10000
/* Both outcomes of the race must be seen this often for the leak checks to mean anything */
#define TEST_MIN_OUTCOMES 100
/* Messages in flight elsewhere in the system, and the lookaside list, move the count a little */
#define TEST_MAX_MESSAGE_GROWTH 32

typedef struct _TEST_CONNECTION_INFO
{
    ULONG Signature;
} TEST_CONNECTION_INFO, *PTEST_CONNECTION_INFO;

typedef struct _TEST_MESSAGE
{
    PORT_MESSAGE Header;
    ULONG Data;
} TEST_MESSAGE, *PTEST_MESSAGE;

static WCHAR PortNameBuffer[64];
static UNICODE_STRING PortName;
static HANDLE ReadyEvent;
static HANDLE ReceivedEvent;
static HANDLE AcceptedEvent;
static HANDLE SectionHandle;
static volatile LONG Iteration;
static volatile ULONG ServerDelay;
static volatile LONG StopServer;
static volatile NTSTATUS LastAcceptStatus;
static volatile NTSTATUS LastCompleteStatus;
static PVOID volatile ClientStack;
static HANDLE ClientPortHandle;
/* NT 5.x keeps the stack of a terminated thread, later versions free it */
static BOOLEAN FreeClientStacks;
/* A client that could not be ended may still use the shared objects */
static BOOLEAN Abandoned;

static
BOOLEAN
GetMessagePoolCount(
    _Out_ PLONG Count)
{
    NTSTATUS Status;
    PSYSTEM_POOLTAG_INFORMATION Info;
    ULONG Length = 0x10000;
    ULONG i;

    for (;;)
    {
        Info = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
        if (!Info)
            return FALSE;
        Status = NtQuerySystemInformation(SystemPoolTagInformation, Info, Length, NULL);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            break;
        RtlFreeHeap(RtlGetProcessHeap(), 0, Info);
        Length *= 2;
    }

    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < Info->Count; i++)
        {
            if (Info->TagInfo[i].TagUlong == 'McpL')
            {
                *Count = (LONG)(Info->TagInfo[i].PagedAllocs - Info->TagInfo[i].PagedFrees);
                RtlFreeHeap(RtlGetProcessHeap(), 0, Info);
                return TRUE;
            }
        }
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Info);
    return FALSE;
}

static
BOOLEAN
GetPointerCount(
    _In_ HANDLE Handle,
    _Out_ PULONG Count)
{
    OBJECT_BASIC_INFORMATION Info;

    if (!NT_SUCCESS(NtQueryObject(Handle, ObjectBasicInformation, &Info, sizeof(Info), NULL)))
        return FALSE;
    *Count = Info.PointerCount;
    return TRUE;
}

static
VOID
Spin(
    _In_ ULONG Count)
{
    volatile ULONG i;

    for (i = 0; i < Count; i++)
        YieldProcessor();
}

static
DWORD
WINAPI
ServerThread(
    _In_ PVOID Parameter)
{
    NTSTATUS Status;
    TEST_MESSAGE Message;
    HANDLE PortHandle;
    HANDLE ServerPortHandle = Parameter;
    REMOTE_PORT_VIEW ClientView;
    BOOLEAN Accept;
    ULONG Seed = GetTickCount() ^ 0x5a5a5a5a;

    for (;;)
    {
        RtlZeroMemory(&Message, sizeof(Message));
        Status = NtListenPort(ServerPortHandle, &Message.Header);
        if (StopServer)
            break;
        if (!NT_SUCCESS(Status))
            continue;

        /* One in four refuses (the kernel prints each refusal), the others accept */
        Accept = ((Iteration & 3) != 1);
        NtSetEvent(ReceivedEvent, NULL);
        if (ServerDelay)
            Spin(RtlRandom(&Seed) % (2 * ServerDelay + 1));

        ClientView.Length = sizeof(ClientView);
        PortHandle = NULL;
        Status = NtAcceptConnectPort(&PortHandle,
                                     NULL,
                                     &Message.Header,
                                     Accept,
                                     NULL,
                                     &ClientView);
        LastCompleteStatus = STATUS_SUCCESS;
        if (NT_SUCCESS(Status) && PortHandle)
        {
            LastCompleteStatus = NtCompleteConnectPort(PortHandle);
            NtClose(PortHandle);
        }

        LastAcceptStatus = Status;
        NtSetEvent(AcceptedEvent, NULL);
    }

    return 0;
}

static
DWORD
WINAPI
ClientThread(
    _In_ PVOID Parameter)
{
    TEST_CONNECTION_INFO ConnectInfo;
    ULONG ConnectInfoLength = sizeof(ConnectInfo);
    SECURITY_QUALITY_OF_SERVICE SecurityQos;
    PORT_VIEW ClientView;
    BOOLEAN UseSection = (BOOLEAN)(ULONG_PTR)Parameter;

    /* Past the loader now: the test may terminate this thread from here on */
    ClientStack = NtCurrentTeb()->DeallocationStack;
    NtSetEvent(ReadyEvent, NULL);

    SecurityQos.Length = sizeof(SecurityQos);
    SecurityQos.ImpersonationLevel = SecurityIdentification;
    SecurityQos.EffectiveOnly = TRUE;
    SecurityQos.ContextTrackingMode = SECURITY_STATIC_TRACKING;

    RtlZeroMemory(&ClientView, sizeof(ClientView));
    ClientView.Length = sizeof(ClientView);
    ClientView.SectionHandle = SectionHandle;
    ClientView.ViewSize = TEST_SECTION_SIZE;

    ConnectInfo.Signature = TEST_CONNECTION_INFO_SIGNATURE;
    /* The kernel stores the handle before the termination can take effect: the test closes it */
    NtConnectPort(&ClientPortHandle,
                  &PortName,
                  &SecurityQos,
                  UseSection ? &ClientView : NULL,
                  NULL,
                  NULL,
                  &ConnectInfo,
                  &ConnectInfoLength);

    /* Wait for the termination here: ending in the loader could orphan its lock */
    for (;;)
        SleepEx(INFINITE, FALSE);

    return 0;
}

static
HANDLE
StartClient(
    _In_ BOOLEAN UseSection)
{
    HANDLE ClientHandle;

    ClientStack = NULL;
    ClientPortHandle = NULL;
    ClientHandle = CreateThread(NULL, TEST_CLIENT_STACK, ClientThread, (PVOID)(ULONG_PTR)UseSection,
                                STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!ClientHandle)
    {
        ok(FALSE, "CreateThread failed: %lu\n", GetLastError());
        return NULL;
    }

    /* Never terminate a thread that may still be in the loader */
    if (WaitForSingleObject(ReadyEvent, TEST_WAIT_MS) != WAIT_OBJECT_0)
    {
        ok(FALSE, "Client thread did not start\n");
        CloseHandle(ClientHandle);
        Abandoned = TRUE;
        return NULL;
    }

    return ClientHandle;
}

static
BOOLEAN
EndClient(
    _In_ HANDLE ClientHandle)
{
    NTSTATUS Status;
    PVOID Base;
    SIZE_T Size = 0;

    if (!TerminateThread(ClientHandle, 0xdead))
    {
        ok(FALSE, "TerminateThread failed: %lu\n", GetLastError());
        CloseHandle(ClientHandle);
        Abandoned = TRUE;
        return FALSE;
    }

    if (WaitForSingleObject(ClientHandle, TEST_WAIT_MS) != WAIT_OBJECT_0)
    {
        ok(FALSE, "Client thread did not end\n");
        CloseHandle(ClientHandle);
        Abandoned = TRUE;
        return FALSE;
    }
    CloseHandle(ClientHandle);

    Base = ClientStack;
    if (FreeClientStacks && Base)
    {
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
        ok(NT_SUCCESS(Status), "Freeing the client stack failed: 0x%lx\n", Status);
    }
    if (ClientPortHandle)
        NtClose(ClientPortHandle);
    return TRUE;
}

START_TEST(NtConnectPortTerminate)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    LARGE_INTEGER SectionSize;
    HANDLE PortHandle = NULL;
    HANDLE ServerHandle = NULL, ClientHandle;
    BOOL bIsWow64;
    LONG CountBefore = 0, CountAfter = 0;
    ULONG SectionRefsBefore = 0, SectionRefsAfter = 0;
    BOOLEAN HaveCounts, HaveSectionRefs, ServerEnded = FALSE;
    DWORD Version;
    ULONG Delay = 64, Seed, Start;
    ULONG AcceptedCount = 0, WithdrawnCount = 0, OtherCount = 0, CompleteFailures = 0;
    LONG i;

    IsWow64Process(GetCurrentProcess(), &bIsWow64);
    if (bIsWow64)
    {
        skip("Skipping on WOW64, due to LPC message layout differences.\n");
        return;
    }

    StringCchPrintfW(PortNameBuffer, RTL_NUMBER_OF(PortNameBuffer),
                     L"\\NtdllApitestNtConnectPortTerminate%lu", GetCurrentProcessId());
    RtlInitUnicodeString(&PortName, PortNameBuffer);
    Seed = GetTickCount();
    Version = GetVersion();
    FreeClientStacks = (LOBYTE(LOWORD(Version)) < 6);

    ReadyEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    ReceivedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    AcceptedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(ReadyEvent && ReceivedEvent && AcceptedEvent, "CreateEventW failed\n");
    if (!ReadyEvent || !ReceivedEvent || !AcceptedEvent)
        goto Cleanup;

    SectionSize.QuadPart = TEST_SECTION_SIZE;
    Status = NtCreateSection(&SectionHandle, SECTION_ALL_ACCESS, NULL, &SectionSize,
                             PAGE_READWRITE, SEC_COMMIT, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    InitializeObjectAttributes(&ObjectAttributes, &PortName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtCreatePort(&PortHandle,
                          &ObjectAttributes,
                          sizeof(TEST_CONNECTION_INFO),
                          sizeof(TEST_MESSAGE),
                          2 * sizeof(TEST_MESSAGE));
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    ServerHandle = CreateThread(NULL, 0, ServerThread, PortHandle, 0, NULL);
    ok(ServerHandle != NULL, "CreateThread failed\n");
    if (!ServerHandle)
        goto Cleanup;

    HaveCounts = GetMessagePoolCount(&CountBefore);
    HaveSectionRefs = GetPointerCount(SectionHandle, &SectionRefsBefore);

    Start = GetTickCount();
    for (i = 0; i < TEST_MAX_ITERATIONS && GetTickCount() - Start < TEST_MAX_TIME_MS; i++)
    {
        Iteration = i;
        /* Refusal is i % 4 == 1; the section alternates every four iterations */
        ClientHandle = StartClient((i & 4) != 0);
        if (!ClientHandle)
            break;

        /* Terminate the client around the time the server accepts its request */
        if (WaitForSingleObject(ReceivedEvent, TEST_WAIT_MS) != WAIT_OBJECT_0)
        {
            ok(FALSE, "No connection request at iteration %ld\n", i);
            EndClient(ClientHandle);
            break;
        }
        Spin(RtlRandom(&Seed) % (2 * Delay + 1));
        if (!EndClient(ClientHandle))
            break;

        if (WaitForSingleObject(AcceptedEvent, TEST_WAIT_MS) != WAIT_OBJECT_0)
        {
            ok(FALSE, "Server did not finish iteration %ld\n", i);
            break;
        }
        if (!NT_SUCCESS(LastCompleteStatus))
            CompleteFailures++;

        /*
         * Steer the delays towards the point where the termination races the
         * accept: delay the termination, or failing that, the accept.
         */
        if (NT_SUCCESS(LastAcceptStatus))
        {
            /* The accept won: terminate earlier */
            AcceptedCount++;
            if (Delay)
                Delay -= Delay / 8 + 1;
            else
                ServerDelay += ServerDelay / 8 + 1;
        }
        else if (LastAcceptStatus == STATUS_REPLY_MESSAGE_MISMATCH ||
                 LastAcceptStatus == STATUS_INVALID_CID)
        {
            /* The client withdrew its request first: terminate later */
            WithdrawnCount++;
            if (ServerDelay)
                ServerDelay -= ServerDelay / 8 + 1;
            else
                Delay += Delay / 8 + 1;
        }
        else
        {
            trace("Accept failed with 0x%lx at iteration %ld\n", LastAcceptStatus, i);
            OtherCount++;
        }
        if (Delay > 0x100000)
            Delay = 0x100000;
        if (ServerDelay > 0x100000)
            ServerDelay = 0x100000;
    }

    /* Wake the server with a last request it will not answer */
    if (!Abandoned)
    {
        StopServer = TRUE;
        ClientHandle = StartClient(FALSE);
        if (ClientHandle)
        {
            ServerEnded = (WaitForSingleObject(ServerHandle, TEST_WAIT_MS) == WAIT_OBJECT_0);
            ok(ServerEnded, "Server thread did not exit\n");
            EndClient(ClientHandle);
        }
    }

    trace("%ld iterations: %lu accepted or refused, %lu withdrawn, %lu other, delays %lu/%lu\n",
          i, AcceptedCount, WithdrawnCount, OtherCount, Delay, ServerDelay);
    ok(i > 0, "No iterations\n");
    ok(OtherCount == 0, "%lu unexpected accept results\n", OtherCount);
    ok(CompleteFailures == 0, "NtCompleteConnectPort failed %lu times\n", CompleteFailures);

    if (Abandoned || !ServerEnded)
    {
        skip("A test thread did not end, not checking for leaks\n");
    }
    else if (AcceptedCount < TEST_MIN_OUTCOMES || WithdrawnCount < TEST_MIN_OUTCOMES)
    {
        skip("The race was not exercised enough to check for leaks\n");
    }
    else
    {
        if (HaveSectionRefs && GetPointerCount(SectionHandle, &SectionRefsAfter))
        {
            trace("Section references: %lu before, %lu after\n", SectionRefsBefore, SectionRefsAfter);
            ok(SectionRefsAfter == SectionRefsBefore, "Section references grew from %lu to %lu\n",
               SectionRefsBefore, SectionRefsAfter);
        }
        else
        {
            skip("No section reference count\n");
        }

        if (HaveCounts && GetMessagePoolCount(&CountAfter))
        {
            trace("LPC messages in use: %ld before, %ld after\n", CountBefore, CountAfter);
            ok(CountAfter - CountBefore <= TEST_MAX_MESSAGE_GROWTH,
               "LPC messages grew by %ld\n", CountAfter - CountBefore);
        }
        else
        {
            skip("No pool tag information for LPC messages\n");
        }
    }

Cleanup:
    if (ServerHandle)
        CloseHandle(ServerHandle);

    /* A thread that did not end may still use the shared objects: leave them to process exit */
    if (Abandoned || (ServerHandle && !ServerEnded))
        return;

    if (PortHandle)
        NtClose(PortHandle);
    if (SectionHandle)
        NtClose(SectionHandle);
    if (ReadyEvent)
        CloseHandle(ReadyEvent);
    if (ReceivedEvent)
        CloseHandle(ReceivedEvent);
    if (AcceptedEvent)
        CloseHandle(AcceptedEvent);
}
