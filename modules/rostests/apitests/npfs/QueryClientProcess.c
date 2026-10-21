/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Named Pipe File System (NPFS) FSCTL_PIPE_QUERY_CLIENT_PROCESS contract test.
 *              Output buffers smaller than the two leading pointer-sized fields must be
 *              rejected, and the returned process value must be a process ID, not a
 *              kernel pointer.
 */

#include <apitest.h>
#include <ndk/ntndk.h>

typedef struct _QUERY_CLIENT_PROCESS_BUFFER
{
    PVOID ClientSession;
    PVOID ClientProcess;
    USHORT DataLength;
    WCHAR Buffer[17];
} QUERY_CLIENT_PROCESS_BUFFER;

START_TEST(NpfsQueryClientProcess)
{
    NTSTATUS Status;
    HANDLE hPipe, hClient;
    IO_STATUS_BLOCK Iosb;
    QUERY_CLIENT_PROCESS_BUFFER Out;
    const ULONG MinLength = FIELD_OFFSET(QUERY_CLIENT_PROCESS_BUFFER, DataLength);
    ULONG Length;

    hPipe = CreateNamedPipeW(L"\\\\.\\pipe\\NpfsQcpApiTestPipe",
                             PIPE_ACCESS_DUPLEX,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             1, 256, 256, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE)
    {
        skip("Failed to create the named pipe: 0x%08lX\n", GetLastError());
        return;
    }

    /* The process is recorded when a client connects */
    hClient = CreateFileW(L"\\\\.\\pipe\\NpfsQcpApiTestPipe", GENERIC_READ | GENERIC_WRITE,
                          0, NULL, OPEN_EXISTING, 0, NULL);
    if (hClient == INVALID_HANDLE_VALUE)
    {
        skip("Failed to connect to the named pipe: 0x%08lX\n", GetLastError());
        CloseHandle(hPipe);
        return;
    }

    /* Every length below the two leading fields must be rejected without writing
       (on 64-bit, lengths 8..15 used to pass the check and overflow the buffer) */
    for (Length = 0; Length < MinLength; Length++)
    {
        RtlFillMemory(&Out, sizeof(Out), 0xCC);
        Iosb.Information = 0x12345678;
        Status = NtFsControlFile(hPipe, NULL, NULL, NULL, &Iosb,
                                 FSCTL_PIPE_QUERY_CLIENT_PROCESS,
                                 NULL, 0, &Out, Length);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
        ok(Out.DataLength == 0xCCCC && Out.ClientSession == (PVOID)(ULONG_PTR)(~(ULONG_PTR)0 / 255 * 0xCC),
           "Output buffer modified on failure\n");
    }

    /* Exactly the minimum: success, never reports more than the buffer */
    RtlFillMemory(&Out, sizeof(Out), 0xCC);
    Status = NtFsControlFile(hPipe, NULL, NULL, NULL, &Iosb,
                             FSCTL_PIPE_QUERY_CLIENT_PROCESS,
                             NULL, 0, &Out, MinLength);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong((ULONG)Iosb.Information, MinLength);
    ok((ULONG_PTR)Out.ClientProcess == GetCurrentProcessId(),
       "ClientProcess = %p, expected process ID %lu (kernel pointer leaked?)\n",
       Out.ClientProcess, GetCurrentProcessId());
    ok(Out.ClientSession == NULL, "ClientSession = %p\n", Out.ClientSession);
    /* Bytes beyond the requested length must be untouched */
    ok(Out.DataLength == 0xCCCC, "Output past the buffer was written\n");

    /* Full-size buffer */
    RtlFillMemory(&Out, sizeof(Out), 0xCC);
    Status = NtFsControlFile(hPipe, NULL, NULL, NULL, &Iosb,
                             FSCTL_PIPE_QUERY_CLIENT_PROCESS,
                             NULL, 0, &Out, sizeof(Out));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong((ULONG)Iosb.Information, (ULONG)sizeof(Out));
    ok((ULONG_PTR)Out.ClientProcess == GetCurrentProcessId(),
       "ClientProcess = %p, expected process ID %lu (kernel pointer leaked?)\n",
       Out.ClientProcess, GetCurrentProcessId());
    ok_eq_uint(Out.DataLength, 0);
    ok_eq_uint(Out.Buffer[0], 0);

    CloseHandle(hClient);
    CloseHandle(hPipe);
}
