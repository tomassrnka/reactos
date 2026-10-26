/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite NPFS Create test
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#include <kmt_test.h>
#include "npfs.h"

static
VOID
TestCreateNamedPipe(VOID)
{
    NTSTATUS Status;
    HANDLE ServerHandle;
    ULONG MaxInstances;
    ULONG InQuota, OutQuota;
    ULONG Quotas[] = { 0, 1, 2, 1024, PAGE_SIZE - 1, PAGE_SIZE, PAGE_SIZE + 1, 2 * PAGE_SIZE, 8 * PAGE_SIZE, 64 * PAGE_SIZE, 64 * PAGE_SIZE + 1, 128 * PAGE_SIZE };
    ULONG i;
    LARGE_INTEGER Timeout;

    /* Invalid pipe name */
    MaxInstances = 1;
    InQuota = 4096;
    OutQuota = 4096;
    ServerHandle = INVALID_HANDLE_VALUE;
    Status = NpCreatePipe(&ServerHandle,
                          DEVICE_NAMED_PIPE L"",
                          BYTE_STREAM, QUEUE, BYTE_STREAM, DUPLEX,
                          MaxInstances,
                          InQuota,
                          OutQuota);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);
    ok_eq_pointer(ServerHandle, INVALID_HANDLE_VALUE);
    if (ServerHandle != NULL && ServerHandle != INVALID_HANDLE_VALUE)
        ObCloseHandle(ServerHandle, KernelMode);

    ServerHandle = INVALID_HANDLE_VALUE;
    Status = NpCreatePipe(&ServerHandle,
                          DEVICE_NAMED_PIPE L"\\",
                          BYTE_STREAM, QUEUE, BYTE_STREAM, DUPLEX,
                          MaxInstances,
                          InQuota,
                          OutQuota);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);
    ok_eq_pointer(ServerHandle, INVALID_HANDLE_VALUE);
    if (ServerHandle != NULL && ServerHandle != INVALID_HANDLE_VALUE)
        ObCloseHandle(ServerHandle, KernelMode);

    ServerHandle = INVALID_HANDLE_VALUE;
    Status = NpCreatePipe(&ServerHandle,
                          DEVICE_NAMED_PIPE L"\\\\",
                          BYTE_STREAM, QUEUE, BYTE_STREAM, DUPLEX,
                          MaxInstances,
                          InQuota,
                          OutQuota);
    if (GetNTVersion() >= _WIN32_WINNT_VISTA)
    {
        /* Vista+ allows a pipe named "\" */
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(ServerHandle != NULL && ServerHandle != INVALID_HANDLE_VALUE,
           "ServerHandle = %p\n", ServerHandle);
    }
    else
    {
        ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);
        ok_eq_pointer(ServerHandle, INVALID_HANDLE_VALUE);
    }
    if (ServerHandle != NULL && ServerHandle != INVALID_HANDLE_VALUE)
        ObCloseHandle(ServerHandle, KernelMode);

    /* Test in-quota */
    MaxInstances = 1;
    OutQuota = 4096;
    for (i = 0; i < RTL_NUMBER_OF(Quotas); i++)
    {
        InQuota = Quotas[i];
        ServerHandle = INVALID_HANDLE_VALUE;
        Status = NpCreatePipe(&ServerHandle,
                              DEVICE_NAMED_PIPE L"\\KmtestNpfsCreateTestPipe",
                              BYTE_STREAM, QUEUE, BYTE_STREAM, DUPLEX,
                              MaxInstances,
                              InQuota,
                              OutQuota);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(ServerHandle != NULL && ServerHandle != INVALID_HANDLE_VALUE, "ServerHandle = %p\n", ServerHandle);
        if (!skip(NT_SUCCESS(Status) && ServerHandle != NULL && ServerHandle != INVALID_HANDLE_VALUE, "No pipe\n"))
        {
            NpCheckServerPipe(ServerHandle,
                              BYTE_STREAM, QUEUE, BYTE_STREAM, DUPLEX,
                              MaxInstances, 1,
                              InQuota, 0,
                              OutQuota, OutQuota,
                              FILE_PIPE_LISTENING_STATE);
            ObCloseHandle(ServerHandle, KernelMode);
            Timeout.QuadPart = -100 * 1000 * 10;
            Status = KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }
}

static
VOID
TestPrefixShadow(VOID)
{
    NTSTATUS Status;
    HANDLE BackslashHandle = INVALID_HANDLE_VALUE;
    HANDLE ServerHandle = INVALID_HANDLE_VALUE;

    if (GetNTVersion() < _WIN32_WINNT_VISTA)
    {
        skip(FALSE, "A pipe named \"\\\" requires Vista+\n");
        return;
    }

    /* While a pipe named "\" exists, "\foo" must still be creatable
     * and not be resolved against the "\" pipe's prefix entry */
    Status = NpCreatePipe(&BackslashHandle,
                          DEVICE_NAMED_PIPE L"\\\\",
                          BYTE_STREAM, QUEUE, BYTE_STREAM, DUPLEX,
                          1, 4096, 4096);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!skip(NT_SUCCESS(Status), "No pipe named \"\\\"\n"))
    {
        Status = NpCreatePipe(&ServerHandle,
                              DEVICE_NAMED_PIPE L"\\\\KmtestNpfsPrefixShadow",
                              BYTE_STREAM, QUEUE, BYTE_STREAM, DUPLEX,
                              1, 4096, 4096);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ObCloseHandle(ServerHandle, KernelMode);
        ObCloseHandle(BackslashHandle, KernelMode);
    }
}

static
VOID
TestCreate(VOID)
{
}

static KSTART_ROUTINE RunTest;
static
VOID
NTAPI
RunTest(
    IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    TestCreateNamedPipe();
    TestPrefixShadow();
    TestCreate();
}

START_TEST(NpfsCreate)
{
    PKTHREAD Thread;

    Thread = KmtStartThread(RunTest, NULL);
    KmtFinishThread(Thread, NULL);
}
