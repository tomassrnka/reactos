/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for LPC callback requests (NtRequestWaitReplyPort with LPC_REQUEST) with inconsistent fields
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define TEST_REQUEST_DATA 0x4c50a001
#define TEST_CALLBACK_DATA 0x4c50a002
#define TEST_REPLY_DATA 0x4c50a003
#define TEST_WAIT_MS 5000
/* Messages in flight elsewhere in the system, and the lookaside list, move the count a little */
#define TEST_MAX_MESSAGE_GROWTH 32
/* More than the deepest lookaside list, so that kept messages reach the pool counters */
#define TEST_LEAK_ROUNDS 400

typedef struct _TEST_MESSAGE
{
    PORT_MESSAGE Header;
    ULONG Data[4];
} TEST_MESSAGE, *PTEST_MESSAGE;

typedef enum _TEST_FIELD
{
    FieldNone,
    FieldMessageIdZero,
    FieldMessageIdWrong,
    FieldClientThreadMissing,
    FieldClientProcessWrong,
    FieldClientNotWaiting,
    FieldClientIsCaller,
    FieldCallbackIdWrong,
    FieldPortConnection,
    FieldPortUnrelated,
    FieldPortClient,
    FieldOtherThread,
    FieldAlreadyReplied,
    FieldDataLengthTooLong,
    FieldTotalLengthTooLong,
    FieldTotalLengthNegative,
    FieldDataInfoOffsetWrong,
    FieldTypeKernelMode,
    FieldPlainTotalLengthNegative,
    FieldReplyUnrelatedPort,
} TEST_FIELD;

typedef enum _TEST_EXPECT
{
    /* The call fails with Expected and the client is not called back */
    ExpectError,
    /* Windows delivers the callback; ReactOS does not implement callbacks */
    ExpectCallback,
    /* Windows delivers it despite the negative length; ReactOS refuses the length */
    ExpectCallbackOrTooLong,
    /* Windows queues it and the sender waits; ReactOS refuses the length */
    ExpectQueuedOrTooLong,
} TEST_EXPECT;

typedef struct _TEST_CASE
{
    PCSTR Name;
    TEST_FIELD Field;
    TEST_EXPECT Expect;
    NTSTATUS Expected;
} TEST_CASE;

/* Expectations measured on Windows Server 2008 R2 and Windows 10 22H2 (x64) */
static const TEST_CASE TestCases[] =
{
    { "consistent",                              FieldNone,                     ExpectCallback },
    { "MessageId zero",                          FieldMessageIdZero,            ExpectError, STATUS_INVALID_PARAMETER },
    { "MessageId wrong",                         FieldMessageIdWrong,           ExpectError, STATUS_INVALID_MESSAGE },
    { "ClientId thread missing",                 FieldClientThreadMissing,      ExpectCallback },
    { "ClientId process wrong",                  FieldClientProcessWrong,       ExpectCallback },
    { "ClientId thread not waiting",             FieldClientNotWaiting,         ExpectCallback },
    { "ClientId is the caller",                  FieldClientIsCaller,           ExpectCallback },
    { "CallbackId wrong",                        FieldCallbackIdWrong,          ExpectError, STATUS_INVALID_MESSAGE },
    { "port is the connection port",             FieldPortConnection,           ExpectCallback },
    { "port is an unrelated communication port", FieldPortUnrelated,            ExpectError, STATUS_ACCESS_DENIED },
    { "port is the client port",                 FieldPortClient,               ExpectError, STATUS_ACCESS_DENIED },
    { "sent by a thread that did not receive",   FieldOtherThread,              ExpectCallback },
    { "client already replied to",               FieldAlreadyReplied,           ExpectError, STATUS_INVALID_MESSAGE },
    { "DataLength too long",                     FieldDataLengthTooLong,        ExpectError, STATUS_INVALID_PARAMETER },
    { "TotalLength too long",                    FieldTotalLengthTooLong,       ExpectError, STATUS_PORT_MESSAGE_TOO_LONG },
    { "TotalLength negative",                    FieldTotalLengthNegative,      ExpectCallbackOrTooLong },
    { "DataInfoOffset wrong",                    FieldDataInfoOffsetWrong,      ExpectError, STATUS_INVALID_PARAMETER },
    { "kernel-mode type flag",                   FieldTypeKernelMode,           ExpectCallback },
    { "plain request, TotalLength negative",     FieldPlainTotalLengthNegative, ExpectQueuedOrTooLong },
    { "reply through an unrelated port",         FieldReplyUnrelatedPort,       ExpectError, STATUS_ACCESS_DENIED },
};

typedef struct _TEST_PAIR
{
    WCHAR NameBuffer[96];
    UNICODE_STRING Name;
    HANDLE ConnectionPort;
    HANDLE CommunicationPort;
    HANDLE ClientPort;
    HANDLE ClientThread;
    HANDLE ConnectedEvent;
    HANDLE AnsweredEvent;
    HANDLE ReleaseEvent;
    DWORD ClientThreadId;
    volatile NTSTATUS ConnectStatus;
    volatile NTSTATUS RequestStatus;
    volatile ULONG ReplyData;
    volatile BOOLEAN CallbackDelivered;
    volatile NTSTATUS CallbackReplyStatus;
    TEST_MESSAGE Received;
} TEST_PAIR, *PTEST_PAIR;

typedef struct _TEST_CALL
{
    HANDLE Port;
    TEST_MESSAGE Message;
    TEST_MESSAGE Reply;
    volatile NTSTATUS Status;
} TEST_CALL, *PTEST_CALL;

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
VOID
InitMessage(
    _Out_ PTEST_MESSAGE Message,
    _In_ ULONG Data)
{
    RtlZeroMemory(Message, sizeof(*Message));
    Message->Header.u1.s1.DataLength = sizeof(Message->Data);
    Message->Header.u1.s1.TotalLength = sizeof(*Message);
    Message->Data[0] = Data;
}

static
DWORD
WINAPI
ClientThread(
    _In_ PVOID Parameter)
{
    PTEST_PAIR Pair = Parameter;
    SECURITY_QUALITY_OF_SERVICE SecurityQos;
    TEST_MESSAGE Request, Reply, CallbackReply;
    NTSTATUS Status;

    SecurityQos.Length = sizeof(SecurityQos);
    SecurityQos.ImpersonationLevel = SecurityIdentification;
    SecurityQos.EffectiveOnly = TRUE;
    SecurityQos.ContextTrackingMode = SECURITY_STATIC_TRACKING;

    Status = NtConnectPort(&Pair->ClientPort, &Pair->Name, &SecurityQos, NULL, NULL, NULL, NULL, NULL);
    Pair->ConnectStatus = Status;
    SetEvent(Pair->ConnectedEvent);
    if (!NT_SUCCESS(Status))
        return 0;

    InitMessage(&Request, TEST_REQUEST_DATA);
    RtlZeroMemory(&Reply, sizeof(Reply));
    Status = NtRequestWaitReplyPort(Pair->ClientPort, &Request.Header, &Reply.Header);
    Pair->ReplyData = Reply.Data[0];
    if (NT_SUCCESS(Status) && LOBYTE(Reply.Header.u2.s2.Type) == LPC_REQUEST)
    {
        /* The callback reached us: answer it, so that its sender does not stay blocked */
        Pair->CallbackDelivered = TRUE;
        CallbackReply = Reply;
        CallbackReply.Header.u1.s1.DataLength = sizeof(CallbackReply.Data);
        CallbackReply.Header.u1.s1.TotalLength = sizeof(CallbackReply);
        CallbackReply.Data[0] = TEST_REPLY_DATA;
        Pair->CallbackReplyStatus = NtReplyPort(Pair->ClientPort, &CallbackReply.Header);
    }
    Pair->RequestStatus = Status;
    SetEvent(Pair->AnsweredEvent);

    /* Keep the thread (and its client id) alive until the case is done */
    WaitForSingleObject(Pair->ReleaseEvent, TEST_WAIT_MS);
    return 0;
}

static
BOOLEAN
OpenPair(
    _Out_ PTEST_PAIR Pair,
    _In_ ULONG Index,
    _In_ ULONG Number)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    TEST_MESSAGE ConnectMessage;
    NTSTATUS Status;
    HRESULT hr;

    RtlZeroMemory(Pair, sizeof(*Pair));
    hr = StringCbPrintfW(Pair->NameBuffer, sizeof(Pair->NameBuffer),
                         L"\\NtdllApitestLpcCallback-%lu-%lu-%lu",
                         GetCurrentProcessId(), Index, Number);
    ok_hex(hr, S_OK);
    RtlInitUnicodeString(&Pair->Name, Pair->NameBuffer);
    Pair->RequestStatus = STATUS_PENDING;
    Pair->ConnectedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Pair->AnsweredEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Pair->ReleaseEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Pair->ConnectedEvent || !Pair->AnsweredEvent || !Pair->ReleaseEvent)
    {
        skip("CreateEventW failed: %lu\n", GetLastError());
        return FALSE;
    }

    InitializeObjectAttributes(&ObjectAttributes, &Pair->Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtCreatePort(&Pair->ConnectionPort, &ObjectAttributes, 0, sizeof(TEST_MESSAGE), 0);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Pair->ClientThread = CreateThread(NULL, 0, ClientThread, Pair, 0, &Pair->ClientThreadId);
    ok(Pair->ClientThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Pair->ClientThread)
        return FALSE;

    RtlZeroMemory(&ConnectMessage, sizeof(ConnectMessage));
    Status = NtListenPort(Pair->ConnectionPort, &ConnectMessage.Header);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = NtAcceptConnectPort(&Pair->CommunicationPort, Pair, &ConnectMessage.Header, TRUE, NULL, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = NtCompleteConnectPort(Pair->CommunicationPort);
    ok_hex(Status, STATUS_SUCCESS);
    WaitForSingleObject(Pair->ConnectedEvent, TEST_WAIT_MS);
    ok_hex(Pair->ConnectStatus, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !NT_SUCCESS(Pair->ConnectStatus))
        return FALSE;

    /* Take the client's request off the queue: the client now waits for its reply */
    RtlZeroMemory(&Pair->Received, sizeof(Pair->Received));
    Status = NtReplyWaitReceivePort(Pair->ConnectionPort, NULL, NULL, &Pair->Received.Header);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return FALSE;
    ok_hex(LOBYTE(Pair->Received.Header.u2.s2.Type), LPC_REQUEST);
    ok_hex(Pair->Received.Data[0], TEST_REQUEST_DATA);
    ok(Pair->Received.Header.ClientId.UniqueThread == UlongToHandle(Pair->ClientThreadId),
       "UniqueThread = %p, expected %lx\n", Pair->Received.Header.ClientId.UniqueThread, Pair->ClientThreadId);
    return TRUE;
}

static
NTSTATUS
ReplyToClient(
    _In_ PTEST_PAIR Pair,
    _In_ HANDLE Port)
{
    TEST_MESSAGE Reply;

    InitMessage(&Reply, TEST_REPLY_DATA);
    Reply.Header.MessageId = Pair->Received.Header.MessageId;
    Reply.Header.ClientId = Pair->Received.Header.ClientId;
    Reply.Header.CallbackId = Pair->Received.Header.CallbackId;
    return NtReplyPort(Port, &Reply.Header);
}

/* The client got the server's normal reply: LPC still works for it */
static
VOID
CheckAnswered(
    _In_ PTEST_PAIR Pair,
    _In_ PCSTR Name)
{
    ok(WaitForSingleObject(Pair->AnsweredEvent, TEST_WAIT_MS) == WAIT_OBJECT_0,
       "%s: the client got no reply\n", Name);
    ok(Pair->RequestStatus == STATUS_SUCCESS, "%s: client request status 0x%lx\n", Name, Pair->RequestStatus);
    ok(Pair->ReplyData == TEST_REPLY_DATA, "%s: client reply data 0x%lx\n", Name, Pair->ReplyData);
}

static
VOID
ClosePair(
    _Inout_ PTEST_PAIR Pair)
{
    if (Pair->ReleaseEvent)
        SetEvent(Pair->ReleaseEvent);
    if (Pair->CommunicationPort)
        NtClose(Pair->CommunicationPort);
    if (Pair->ClientThread)
    {
        if (WaitForSingleObject(Pair->ClientThread, TEST_WAIT_MS) != WAIT_OBJECT_0)
        {
            ok(0, "Client thread did not end\n");
            TerminateThread(Pair->ClientThread, 1);
            WaitForSingleObject(Pair->ClientThread, TEST_WAIT_MS);
        }
        CloseHandle(Pair->ClientThread);
    }
    if (Pair->ClientPort)
        NtClose(Pair->ClientPort);
    if (Pair->ConnectionPort)
        NtClose(Pair->ConnectionPort);
    if (Pair->ConnectedEvent)
        CloseHandle(Pair->ConnectedEvent);
    if (Pair->AnsweredEvent)
        CloseHandle(Pair->AnsweredEvent);
    if (Pair->ReleaseEvent)
        CloseHandle(Pair->ReleaseEvent);
    RtlZeroMemory(Pair, sizeof(*Pair));
}

static
DWORD
WINAPI
CallThread(
    _In_ PVOID Parameter)
{
    PTEST_CALL Call = Parameter;

    Call->Status = NtRequestWaitReplyPort(Call->Port, &Call->Message.Header, &Call->Reply.Header);
    return 0;
}

static
DWORD
WINAPI
IdleThread(
    _In_ PVOID Parameter)
{
    WaitForSingleObject(Parameter, TEST_WAIT_MS);
    return 0;
}

static
VOID
InitCallback(
    _Out_ PTEST_CALL Call,
    _In_ PTEST_PAIR Pair)
{
    RtlZeroMemory(Call, sizeof(*Call));
    InitMessage(&Call->Message, TEST_CALLBACK_DATA);
    Call->Message.Header.u2.s2.Type = LPC_REQUEST;
    Call->Message.Header.MessageId = Pair->Received.Header.MessageId;
    Call->Message.Header.ClientId = Pair->Received.Header.ClientId;
    Call->Message.Header.CallbackId = Pair->Received.Header.CallbackId;
    Call->Port = Pair->CommunicationPort;
    Call->Status = STATUS_PENDING;
}

static
VOID
CheckResult(
    _In_ const TEST_CASE *Case,
    _In_ NTSTATUS Status,
    _In_ BOOLEAN Delivered)
{
    switch (Case->Expect)
    {
        case ExpectError:
            ok(Status == Case->Expected, "%s: status 0x%lx, expected 0x%lx\n", Case->Name, Status, Case->Expected);
            ok(!Delivered, "%s: the client was called back\n", Case->Name);
            break;

        case ExpectCallback:
            todo_if(is_reactos())
            ok(Delivered, "%s: the client was not called back (status 0x%lx)\n", Case->Name, Status);
            ok(Status == (Delivered ? STATUS_SUCCESS : STATUS_NOT_IMPLEMENTED),
               "%s: status 0x%lx, delivered %d\n", Case->Name, Status, Delivered);
            break;

        case ExpectCallbackOrTooLong:
            ok(Status == (Delivered ? STATUS_SUCCESS : STATUS_PORT_MESSAGE_TOO_LONG),
               "%s: status 0x%lx, delivered %d\n", Case->Name, Status, Delivered);
            break;

        case ExpectQueuedOrTooLong:
            ok(Status == STATUS_PENDING || Status == STATUS_PORT_MESSAGE_TOO_LONG,
               "%s: status 0x%lx\n", Case->Name, Status);
            ok(!Delivered, "%s: the client was called back\n", Case->Name);
            break;
    }
}

static
VOID
RunReplyCase(
    _In_ const TEST_CASE *Case,
    _In_ PTEST_PAIR Pair,
    _In_ PTEST_PAIR Other)
{
    NTSTATUS Status;

    /* Not a callback: a reply aimed at the first pair's client, sent through the second pair */
    Status = ReplyToClient(Pair, Other->CommunicationPort);
    ok(WaitForSingleObject(Pair->AnsweredEvent, 200) == WAIT_TIMEOUT,
       "%s: the client was answered (request status 0x%lx)\n", Case->Name, Pair->RequestStatus);
    CheckResult(Case, Status, FALSE);

    Status = ReplyToClient(Pair, Pair->CommunicationPort);
    ok_hex(Status, STATUS_SUCCESS);
    CheckAnswered(Pair, Case->Name);
    Status = ReplyToClient(Other, Other->CommunicationPort);
    ok_hex(Status, STATUS_SUCCESS);
    CheckAnswered(Other, Case->Name);
}

static
VOID
RunCase(
    _In_ ULONG Index)
{
    const TEST_CASE *Case = &TestCases[Index];
    TEST_PAIR Pair, Other;
    TEST_CALL Call;
    HANDLE CallThreadHandle, IdleThreadHandle = NULL, IdleEvent = NULL;
    DWORD CallThreadId, IdleThreadId;
    NTSTATUS Status;
    BOOLEAN Ended;

    RtlZeroMemory(&Other, sizeof(Other));
    if (!OpenPair(&Pair, Index, 0))
    {
        skip("%s: no connection\n", Case->Name);
        ClosePair(&Pair);
        return;
    }

    if (Case->Field == FieldPortUnrelated || Case->Field == FieldReplyUnrelatedPort)
    {
        if (!OpenPair(&Other, Index, 1))
        {
            skip("%s: no second connection\n", Case->Name);
            ReplyToClient(&Pair, Pair.CommunicationPort);
            ClosePair(&Other);
            ClosePair(&Pair);
            return;
        }
    }

    if (Case->Field == FieldReplyUnrelatedPort)
    {
        RunReplyCase(Case, &Pair, &Other);
        ClosePair(&Other);
        ClosePair(&Pair);
        return;
    }

    if (Case->Field == FieldAlreadyReplied)
    {
        Status = ReplyToClient(&Pair, Pair.CommunicationPort);
        ok_hex(Status, STATUS_SUCCESS);
        CheckAnswered(&Pair, Case->Name);
    }

    InitCallback(&Call, &Pair);
    switch (Case->Field)
    {
        case FieldMessageIdZero:
            Call.Message.Header.MessageId = 0;
            break;
        case FieldMessageIdWrong:
            Call.Message.Header.MessageId += 0x10000;
            break;
        case FieldClientThreadMissing:
            Call.Message.Header.ClientId.UniqueThread = (HANDLE)(ULONG_PTR)0x3FFFFFF0;
            break;
        case FieldClientProcessWrong:
            Call.Message.Header.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)4;
            break;
        case FieldClientNotWaiting:
            IdleEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            IdleThreadHandle = CreateThread(NULL, 0, IdleThread, IdleEvent, 0, &IdleThreadId);
            ok(IdleThreadHandle != NULL, "CreateThread failed: %lu\n", GetLastError());
            Call.Message.Header.ClientId.UniqueThread = UlongToHandle(IdleThreadId);
            break;
        case FieldCallbackIdWrong:
            Call.Message.Header.CallbackId = Pair.Received.Header.CallbackId + 0x12345678;
            break;
        case FieldPortConnection:
            Call.Port = Pair.ConnectionPort;
            break;
        case FieldPortUnrelated:
            Call.Port = Other.CommunicationPort;
            break;
        case FieldPortClient:
            Call.Port = Pair.ClientPort;
            break;
        case FieldDataLengthTooLong:
            Call.Message.Header.u1.s1.DataLength = sizeof(Call.Message);
            break;
        case FieldTotalLengthTooLong:
            Call.Message.Header.u1.s1.TotalLength = 0x7000;
            Call.Message.Header.u1.s1.DataLength = 0x7000 - sizeof(PORT_MESSAGE);
            break;
        case FieldTotalLengthNegative:
            Call.Message.Header.u1.s1.TotalLength = (CSHORT)0xFFF8;
            break;
        case FieldDataInfoOffsetWrong:
            Call.Message.Header.u2.s2.DataInfoOffset = 0x7000;
            break;
        case FieldTypeKernelMode:
            /* LPC_KERNELMODE_MESSAGE */
            Call.Message.Header.u2.s2.Type = (CSHORT)(LPC_REQUEST | 0x8000);
            break;
        case FieldPlainTotalLengthNegative:
            Call.Message.Header.u2.s2.Type = 0;
            Call.Message.Header.u1.s1.TotalLength = (CSHORT)0xFFF8;
            Call.Port = Pair.ClientPort;
            break;
        default:
            break;
    }

    /* Another thread sends it, so that a delivered callback cannot block the test; it names itself for "is the caller" */
    CallThreadHandle = CreateThread(NULL, 0, CallThread, &Call, CREATE_SUSPENDED, &CallThreadId);
    ok(CallThreadHandle != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!CallThreadHandle)
    {
        ReplyToClient(&Pair, Pair.CommunicationPort);
        ClosePair(&Pair);
        return;
    }
    if (Case->Field == FieldClientIsCaller)
        Call.Message.Header.ClientId.UniqueThread = UlongToHandle(CallThreadId);
    ResumeThread(CallThreadHandle);

    Ended = (WaitForSingleObject(CallThreadHandle, TEST_WAIT_MS) == WAIT_OBJECT_0);
    Status = Ended ? Call.Status : STATUS_PENDING;
    if (Pair.CallbackDelivered)
    {
        ok_hex(Pair.CallbackReplyStatus, STATUS_SUCCESS);
        if (Ended)
            ok_hex(Call.Reply.Data[0], TEST_REPLY_DATA);
    }
    CheckResult(Case, Status, Pair.CallbackDelivered);

    /* A client that was not called back still waits for its request: answer it */
    if (Case->Field != FieldAlreadyReplied && !Pair.CallbackDelivered)
    {
        ok(WaitForSingleObject(Pair.AnsweredEvent, 0) == WAIT_TIMEOUT,
           "%s: the client was answered by the callback (request status 0x%lx)\n", Case->Name, Pair.RequestStatus);
        Status = ReplyToClient(&Pair, Pair.CommunicationPort);
        ok_hex(Status, STATUS_SUCCESS);
        CheckAnswered(&Pair, Case->Name);
    }
    if (Other.ConnectionPort)
    {
        Status = ReplyToClient(&Other, Other.CommunicationPort);
        ok_hex(Status, STATUS_SUCCESS);
        CheckAnswered(&Other, Case->Name);
    }

    if (!Ended)
    {
        /* Closing the connection ends a sender that waits for a reply nobody will send */
        NtClose(Pair.CommunicationPort);
        Pair.CommunicationPort = NULL;
        NtClose(Pair.ConnectionPort);
        Pair.ConnectionPort = NULL;
        if (WaitForSingleObject(CallThreadHandle, TEST_WAIT_MS) != WAIT_OBJECT_0)
        {
            ok(0, "%s: the sender stays blocked\n", Case->Name);
            TerminateThread(CallThreadHandle, 1);
            WaitForSingleObject(CallThreadHandle, TEST_WAIT_MS);
        }
    }
    CloseHandle(CallThreadHandle);

    if (IdleThreadHandle)
    {
        SetEvent(IdleEvent);
        WaitForSingleObject(IdleThreadHandle, TEST_WAIT_MS);
        CloseHandle(IdleThreadHandle);
    }
    if (IdleEvent)
        CloseHandle(IdleEvent);
    if (Other.ConnectionPort)
        ClosePair(&Other);
    ClosePair(&Pair);
}

/* Refused callbacks must not keep LPC messages (ReactOS pool tag only) */
static
VOID
TestLeak(VOID)
{
    TEST_PAIR Pair;
    TEST_CALL Call;
    LONG Before, After;
    ULONG i;
    NTSTATUS Status;

    /* A callback that names a waiting client blocks on Windows, where it is delivered */
    if (!is_reactos() || !GetMessagePoolCount(&Before))
    {
        skip("No LPC message pool tag\n");
        return;
    }

    if (!OpenPair(&Pair, RTL_NUMBER_OF(TestCases), 0))
    {
        skip("No connection\n");
        ClosePair(&Pair);
        return;
    }

    for (i = 0; i < TEST_LEAK_ROUNDS; i++)
    {
        /* Both return at once on ReactOS: one is refused as wrong, one as not implemented */
        InitCallback(&Call, &Pair);
        Call.Message.Header.MessageId += 0x10000;
        Status = NtRequestWaitReplyPort(Call.Port, &Call.Message.Header, &Call.Reply.Header);
        ok_hex(Status, STATUS_INVALID_MESSAGE);
        InitCallback(&Call, &Pair);
        Status = NtRequestWaitReplyPort(Call.Port, &Call.Message.Header, &Call.Reply.Header);
        ok_hex(Status, STATUS_NOT_IMPLEMENTED);
        if (Status != STATUS_NOT_IMPLEMENTED)
            break;
    }

    Status = ReplyToClient(&Pair, Pair.CommunicationPort);
    ok_hex(Status, STATUS_SUCCESS);
    CheckAnswered(&Pair, "leak");
    ClosePair(&Pair);

    ok(GetMessagePoolCount(&After), "No LPC message pool tag\n");
    ok(After - Before <= TEST_MAX_MESSAGE_GROWTH,
       "LPC messages grew from %ld to %ld over %lu callback pairs\n", Before, After, i);
}

START_TEST(LpcCallbackRequest)
{
    ULONG i;
    BOOL bIsWow64;

    IsWow64Process(GetCurrentProcess(), &bIsWow64);
    if (bIsWow64)
    {
        skip("Skipping on WOW64, due to LPC message layout differences.\n");
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(TestCases); i++)
        RunCase(i);

    TestLeak();
}
