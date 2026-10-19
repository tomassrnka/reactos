/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test for NtReadFile
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"

static
BOOL
Is64BitSystem(VOID)
{
#ifdef _WIN64
    return TRUE;
#else
    NTSTATUS Status;
    ULONG_PTR IsWow64;

    Status = NtQueryInformationProcess(NtCurrentProcess(),
                                       ProcessWow64Information,
                                       &IsWow64,
                                       sizeof(IsWow64),
                                       NULL);
    if (NT_SUCCESS(Status))
    {
        return IsWow64 != 0;
    }

    return FALSE;
#endif
}

#ifdef _WIN64
#define IsWow64() FALSE
#else
#define IsWow64() Is64BitSystem()
#endif

static
ULONG
SizeOfMdl(VOID)
{
    return Is64BitSystem() ? 48 : 28;
}

static
VOID
TestPartlyInvalidBuffer(VOID)
{
    NTSTATUS Status;
    HANDLE FileHandle;
    UNICODE_STRING FileName = RTL_CONSTANT_STRING(L"\\SystemRoot\\ntdll-apitest-NtReadFile-invalid.bin");
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER ByteOffset;
    FILE_DISPOSITION_INFORMATION DispositionInfo;
    PVOID Buffer = NULL;
    SIZE_T BufferSize = 2 * PAGE_SIZE;

    /* Reserve two pages, commit only the first one */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Buffer, 0, &BufferSize, MEM_RESERVE, PAGE_READWRITE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    BufferSize = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Buffer, 0, &BufferSize, MEM_COMMIT, PAGE_READWRITE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Free;
    RtlFillMemory(Buffer, PAGE_SIZE, 'A');

    InitializeObjectAttributes(&ObjectAttributes, &FileName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtCreateFile(&FileHandle,
                          FILE_READ_DATA | FILE_WRITE_DATA | DELETE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          0,
                          0,
                          FILE_SUPERSEDE,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Free;

    /* Two valid pages of data, so that the reads below reach the second page */
    ByteOffset.QuadPart = 0;
    Status = NtWriteFile(FileHandle, NULL, NULL, NULL, &IoStatus, Buffer, PAGE_SIZE, &ByteOffset, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ByteOffset.QuadPart = PAGE_SIZE;
    Status = NtWriteFile(FileHandle, NULL, NULL, NULL, &IoStatus, Buffer, PAGE_SIZE, &ByteOffset, NULL);
    ok_hex(Status, STATUS_SUCCESS);

    /* Cached write from a buffer whose second page is not committed: the
       probe checks only the range, so the cache manager faults in its copy */
    ByteOffset.QuadPart = 0;
    Status = NtWriteFile(FileHandle, NULL, NULL, NULL, &IoStatus, Buffer, 2 * PAGE_SIZE, &ByteOffset, NULL);
    ok(Status == STATUS_INVALID_USER_BUFFER || (!is_reactos() && broken(Status == STATUS_ACCESS_VIOLATION)),
       "Write from a partly invalid buffer returned 0x%lx\n", Status);

    /* Read into the same buffer: the probe touches every page, so this
       fails before the request reaches the file system */
    ByteOffset.QuadPart = 0;
    Status = NtReadFile(FileHandle, NULL, NULL, NULL, &IoStatus, Buffer, 2 * PAGE_SIZE, &ByteOffset, NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* The handle still works */
    ByteOffset.QuadPart = PAGE_SIZE;
    Status = NtReadFile(FileHandle, NULL, NULL, NULL, &IoStatus, Buffer, PAGE_SIZE, &ByteOffset, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(IoStatus.Information, PAGE_SIZE);

    DispositionInfo.DeleteFile = TRUE;
    Status = NtSetInformationFile(FileHandle,
                                  &IoStatus,
                                  &DispositionInfo,
                                  sizeof(DispositionInfo),
                                  FileDispositionInformation);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtClose(FileHandle);
    ok_hex(Status, STATUS_SUCCESS);

Free:
    BufferSize = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Buffer, &BufferSize, MEM_RELEASE);
    ok_hex(Status, STATUS_SUCCESS);
}

START_TEST(NtReadFile)
{
    NTSTATUS Status;
    HANDLE FileHandle;
    UNICODE_STRING FileName = RTL_CONSTANT_STRING(L"\\SystemRoot\\ntdll-apitest-NtReadFile-test.bin");
    PVOID Buffer;
    SIZE_T BufferSize;
    LARGE_INTEGER ByteOffset;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    FILE_DISPOSITION_INFORMATION DispositionInfo;
    ULONG TooLargeDataSize = (MAXUSHORT + 1 - SizeOfMdl()) / sizeof(ULONG_PTR) * PAGE_SIZE; // 0x3FF9000 on x86
    ULONG LargeMdlMaxDataSize = TooLargeDataSize - PAGE_SIZE;

    trace("System is %d bits, Size of MDL: %lu\n", Is64BitSystem() ? 64 : 32, SizeOfMdl());
    trace("Max MDL data size: 0x%lx bytes\n", LargeMdlMaxDataSize);

    TestPartlyInvalidBuffer();

    ByteOffset.QuadPart = 0;

    Buffer = NULL;
    BufferSize = TooLargeDataSize;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Buffer,
                                     0,
                                     &BufferSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        skip("Failed to allocate memory, status %lx\n", Status);
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes,
                               &FileName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    Status = NtCreateFile(&FileHandle,
                          FILE_READ_DATA | FILE_WRITE_DATA | DELETE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          0,
                          0,
                          FILE_SUPERSEDE,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                                                    FILE_NO_INTERMEDIATE_BUFFERING,
                          NULL,
                          0);
    ok_hex(Status, STATUS_SUCCESS);

    ByteOffset.QuadPart = 0x10000;
    Status = NtWriteFile(FileHandle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatus,
                         Buffer,
                         BufferSize - 0x10000,
                         &ByteOffset,
                         NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ByteOffset.QuadPart = 0;

    /* non-cached, max size -- succeeds */
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        LargeMdlMaxDataSize - PAGE_SIZE,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_SUCCESS);

    /* non-cached, max size -- succeeds */
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        LargeMdlMaxDataSize,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_SUCCESS);

    /* non-cached, too large -- fails to allocate MDL
     * Note: this returns STATUS_SUCCESS on Vista+ -- higher MDL size limit */
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        LargeMdlMaxDataSize + PAGE_SIZE,
                        &ByteOffset,
                        NULL);
    if (GetNTVersion() >= _WIN32_WINNT_VISTA)
        ok_hex(Status, STATUS_SUCCESS);
    else
        ok_hex(Status, STATUS_INSUFFICIENT_RESOURCES);

    /* Invalid buffer address */
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        LongToPtr(-1),
                        PAGE_SIZE,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Buffer probing fails */
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        2 * LargeMdlMaxDataSize,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION); // Different to NtWriteFile

    /* non-cached, unaligned -- fails with invalid parameter */
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        LargeMdlMaxDataSize + 1,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_INVALID_PARAMETER); // Different to NtWriteFile

    DispositionInfo.DeleteFile = TRUE;
    Status = NtSetInformationFile(FileHandle,
                                  &IoStatus,
                                  &DispositionInfo,
                                  sizeof(DispositionInfo),
                                  FileDispositionInformation);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtClose(FileHandle);
    ok_hex(Status, STATUS_SUCCESS);

    Status = NtCreateFile(&FileHandle,
                          FILE_READ_DATA | FILE_WRITE_DATA | DELETE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          0,
                          0,
                          FILE_SUPERSEDE,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    ok_hex(Status, STATUS_SUCCESS);

    ByteOffset.QuadPart = 0x10000;
    Status = NtWriteFile(FileHandle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatus,
                         Buffer,
                         BufferSize - 0x10000,
                         &ByteOffset,
                         NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ByteOffset.QuadPart = 0;

    /* cached: succeeds with arbitrary length */
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        LargeMdlMaxDataSize,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_SUCCESS);

    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        LargeMdlMaxDataSize + 1,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_SUCCESS);

    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatus,
                        Buffer,
                        TooLargeDataSize,
                        &ByteOffset,
                        NULL);
    ok_hex(Status, STATUS_SUCCESS);

    DispositionInfo.DeleteFile = TRUE;
    Status = NtSetInformationFile(FileHandle,
                                  &IoStatus,
                                  &DispositionInfo,
                                  sizeof(DispositionInfo),
                                  FileDispositionInformation);
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtClose(FileHandle);
    ok_hex(Status, STATUS_SUCCESS);

    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer,
                                 &BufferSize,
                                 MEM_RELEASE);
    ok_hex(Status, STATUS_SUCCESS);
}
