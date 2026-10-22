/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite MmMapLockedPagesSpecifyCache test user-mode part
 * PROGRAMMER:      Pierre Schweitzer <pierre@reactos.org>
 */

#include <kmt_test.h>
#include <ndk/exfuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>

#include "MmMapLockedPagesSpecifyCache.h"

#define ALIGN_DOWN_BY(size, align) \
        ((ULONG_PTR)(size) & ~((ULONG_PTR)(align) - 1))

#define SET_BUFFER_LENGTH(Var, Length)         \
{                                              \
    C_ASSERT(((Length) % sizeof(ULONG)) == 0); \
    Var = (Length);                            \
}

#define FILL_QUERY_BUFFER(QueryBuffer, BufferLength, UseCache) \
{                                                              \
    QueryBuffer.Length = BufferLength;                         \
    QueryBuffer.Buffer = NULL;                                 \
    QueryBuffer.Cached = UseCache;                             \
    QueryBuffer.Status = STATUS_SUCCESS;                       \
}

#define FILL_READ_BUFFER(QueryBuffer, ReadBuffer)               \
{                                                               \
    PULONG Buffer;                                              \
    ReadBuffer.Buffer = QueryBuffer.Buffer;                     \
    if (!skip(QueryBuffer.Buffer != NULL, "Buffer is NULL\n"))  \
    {                                                           \
        ReadBuffer.Pattern = WRITE_PATTERN;                     \
        ReadBuffer.Length = QueryBuffer.Length;                 \
        Buffer = QueryBuffer.Buffer;                            \
        for (i = 0; i < ReadBuffer.Length / sizeof(ULONG); ++i) \
        {                                                       \
            Buffer[i] = ReadBuffer.Pattern;                     \
        }                                                       \
    }                                                           \
}

#define CHECK_ALLOC(MappedBuffer, BufferLength)                 \
{                                                               \
    NTSTATUS Status;                                            \
    PVOID BaseAddress;                                          \
    SIZE_T Size;                                                \
    BaseAddress = MappedBuffer;                                 \
    Size = BufferLength;                                        \
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),        \
                                     &BaseAddress,              \
                                     0,                         \
                                     &Size,                     \
                                     MEM_RESERVE,               \
                                     PAGE_READWRITE);           \
    ok_eq_hex(Status, STATUS_CONFLICTING_ADDRESSES);            \
    BaseAddress = MappedBuffer;                                 \
    Size = 0;                                                   \
    Status = NtFreeVirtualMemory(NtCurrentProcess(),            \
                                 &BaseAddress,                  \
                                 &Size,                         \
                                 MEM_DECOMMIT);                 \
    ok_eq_hex(Status, STATUS_UNABLE_TO_DELETE_SECTION);         \
    BaseAddress = MappedBuffer;                                 \
    Size = 0;                                                   \
    Status = NtFreeVirtualMemory(NtCurrentProcess(),            \
                                 &BaseAddress,                  \
                                 &Size,                         \
                                 MEM_RELEASE);                  \
    ok_eq_hex(Status, STATUS_UNABLE_TO_DELETE_SECTION);         \
    Status = NtUnmapViewOfSection(NtCurrentProcess(),           \
                                  MappedBuffer);                \
    ok_eq_hex(Status, STATUS_NOT_MAPPED_VIEW);                  \
}

#define VIEW_LENGTH (16 * PAGE_SIZE)

static
ULONGLONG
TimeViewReads(
    _In_ volatile UCHAR *Base)
{
    LARGE_INTEGER Start, End;
    ULONGLONG Best = (ULONGLONG)-1;
    ULONG Run, Pass, Offset, Sum = 0;
    static volatile ULONG Sink;

    for (Run = 0; Run < 3; Run++)
    {
        QueryPerformanceCounter(&Start);
        for (Pass = 0; Pass < 500; Pass++)
            for (Offset = 0; Offset < VIEW_LENGTH; Offset += 64)
                Sum += Base[Offset];
        QueryPerformanceCounter(&End);
        if ((ULONGLONG)(End.QuadPart - Start.QuadPart) < Best)
            Best = End.QuadPart - Start.QuadPart;
    }
    Sink += Sum;
    return Best;
}

static
BOOLEAN
IsSlow(
    _In_ ULONGLONG Ticks,
    _In_ ULONGLONG CachedTicks)
{
    return Ticks > CachedTicks * 10 + 1000;
}

/* A second user mapping of locked view pages must not give them another memory type */
static
void
TestViewMappedTwice(
    _In_ ULONG ViewProtection,
    _In_ BOOLEAN Cached,
    _In_ PCSTR Name)
{
    WCHAR Path[MAX_PATH], File[MAX_PATH];
    HANDLE FileHandle, SectionHandle;
    PVOID Control = NULL, Uncached = NULL, View = NULL;
    SIZE_T Size, ViewSize = 0;
    LARGE_INTEGER MaximumSize;
    VIEW_BUFFER ViewBuffer;
    DWORD Length;
    ULONG Page;
    ULONGLONG ControlTicks, UncachedTicks, ViewTicks, MappedTicks;
    NTSTATUS Status;

    Size = VIEW_LENGTH;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Control, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Size = VIEW_LENGTH;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Uncached, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_NOCACHE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!Control || !Uncached)
        return;
    for (Page = 0; Page < VIEW_LENGTH; Page += PAGE_SIZE)
    {
        ((volatile UCHAR *)Control)[Page] = 1;
        ((volatile UCHAR *)Uncached)[Page] = 1;
    }
    ControlTicks = TimeViewReads(Control);
    UncachedTicks = TimeViewReads(Uncached);

    GetTempPathW(RTL_NUMBER_OF(Path), Path);
    GetTempFileNameW(Path, L"kmt", 0, File);
    FileHandle = CreateFileW(File, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(FileHandle != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
    if (FileHandle == INVALID_HANDLE_VALUE)
        goto Cleanup;

    MaximumSize.QuadPart = VIEW_LENGTH;
    Status = NtCreateSection(&SectionHandle, SECTION_ALL_ACCESS, NULL, &MaximumSize, PAGE_READWRITE, SEC_COMMIT, FileHandle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtMapViewOfSection(SectionHandle, NtCurrentProcess(), &View, 0, 0, NULL, &ViewSize, ViewShare, 0, ViewProtection);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            for (Page = 0; Page < VIEW_LENGTH; Page += PAGE_SIZE)
                ((volatile UCHAR *)View)[Page] = 1;
            ViewTicks = TimeViewReads(View);

            ViewBuffer.Address = View;
            ViewBuffer.Length = VIEW_LENGTH;
            ViewBuffer.Cached = Cached;
            ViewBuffer.Mapped = NULL;
            ViewBuffer.Status = STATUS_UNSUCCESSFUL;
            Length = sizeof(ViewBuffer);
            ok(KmtSendBufferToDriver(IOCTL_MAP_VIEW, &ViewBuffer, sizeof(ViewBuffer), &Length) == ERROR_SUCCESS, "\n");
            ok_eq_hex(ViewBuffer.Status, STATUS_SUCCESS);
            if (ViewBuffer.Mapped != NULL)
            {
                MappedTicks = TimeViewReads(ViewBuffer.Mapped);
                trace("%s: cached %I64u, PAGE_NOCACHE %I64u, view %I64u, second mapping %I64u ticks\n",
                      Name, ControlTicks, UncachedTicks, ViewTicks, MappedTicks);
                if (IsSlow(UncachedTicks, ControlTicks))
                    ok(IsSlow(MappedTicks, ControlTicks) == IsSlow(ViewTicks, ControlTicks),
                       "%s: the view reads in %I64u ticks, its second mapping in %I64u (cached %I64u)\n",
                       Name, ViewTicks, MappedTicks, ControlTicks);
                else
                    skip(FALSE, "PAGE_NOCACHE memory reads as fast as cached memory here\n");
            }
            Length = 0;
            KmtSendBufferToDriver(IOCTL_UNMAP_VIEW, NULL, 0, &Length);
            NtUnmapViewOfSection(NtCurrentProcess(), View);
        }
        NtClose(SectionHandle);
    }
    CloseHandle(FileHandle);

Cleanup:
    Size = 0;
    NtFreeVirtualMemory(NtCurrentProcess(), &Control, &Size, MEM_RELEASE);
    Size = 0;
    NtFreeVirtualMemory(NtCurrentProcess(), &Uncached, &Size, MEM_RELEASE);
}

static
ULONGLONG
TimePageReads(
    _In_ volatile UCHAR *Base)
{
    LARGE_INTEGER Start, End;
    ULONGLONG Best = (ULONGLONG)-1;
    ULONG Run, Pass, Offset, Sum = 0;
    static volatile ULONG Sink;

    for (Run = 0; Run < 3; Run++)
    {
        QueryPerformanceCounter(&Start);
        for (Pass = 0; Pass < 2000; Pass++)
            for (Offset = 0; Offset < PAGE_SIZE; Offset += 64)
                Sum += Base[Offset];
        QueryPerformanceCounter(&End);
        if ((ULONGLONG)(End.QuadPart - Start.QuadPart) < Best)
            Best = End.QuadPart - Start.QuadPart;
    }
    Sink += Sum;
    return Best;
}

/* Two uncached \Device\PhysicalMemory views of a page with no cache type of its own stay uncached when one is reprotected */
static
void
TestPhysicalViewsReprotected(void)
{
    PVOID Control = NULL, Uncached = NULL;
    SIZE_T Size;
    PHYSICAL_BUFFER Physical;
    DWORD Length, Old;
    ULONGLONG ControlTicks, UncachedTicks, Ticks[2];
    ULONG i;
    NTSTATUS Status;

    Size = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Control, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Size = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Uncached, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_NOCACHE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!Control || !Uncached)
        return;
    *(volatile UCHAR *)Control = 1;
    *(volatile UCHAR *)Uncached = 1;
    ControlTicks = TimePageReads(Control);
    UncachedTicks = TimePageReads(Uncached);

    RtlZeroMemory(&Physical, sizeof(Physical));
    Physical.Status = STATUS_UNSUCCESSFUL;
    Length = sizeof(Physical);
    ok(KmtSendBufferToDriver(IOCTL_MAP_PHYSICAL, &Physical, sizeof(Physical), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_hex(Physical.Status, STATUS_SUCCESS);
    if (Physical.View[0] != NULL && Physical.View[1] != NULL)
    {
        /* Both views must be present, so the protection change rewrites a valid PTE */
        for (i = 0; i < 2; i++)
            Ticks[i] = TimePageReads(Physical.View[i]);
        /* Windows probably refuses to reprotect a physical view (as ARM3 does); then nothing may change either */
        if (!VirtualProtect(Physical.View[0], PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE, &Old))
            trace("VirtualProtect on the physical view failed: %lu\n", GetLastError());
        for (i = 0; i < 2; i++)
            Ticks[i] = TimePageReads(Physical.View[i]);
        trace("cached %I64u, PAGE_NOCACHE %I64u, reprotected view %I64u, other view %I64u ticks\n",
              ControlTicks, UncachedTicks, Ticks[0], Ticks[1]);
        if (IsSlow(UncachedTicks, ControlTicks))
        {
            ok(IsSlow(Ticks[0], ControlTicks), "the reprotected PAGE_NOCACHE view reads in %I64u ticks, cached memory in %I64u\n", Ticks[0], ControlTicks);
            ok(IsSlow(Ticks[1], ControlTicks), "the other PAGE_NOCACHE view reads in %I64u ticks, cached memory in %I64u\n", Ticks[1], ControlTicks);
        }
        else
        {
            skip(FALSE, "PAGE_NOCACHE memory reads as fast as cached memory here\n");
        }
    }
    Length = 0;
    KmtSendBufferToDriver(IOCTL_UNMAP_PHYSICAL, NULL, 0, &Length);

    Size = 0;
    NtFreeVirtualMemory(NtCurrentProcess(), &Control, &Size, MEM_RELEASE);
    Size = 0;
    NtFreeVirtualMemory(NtCurrentProcess(), &Uncached, &Size, MEM_RELEASE);
}

START_TEST(MmMapLockedPagesSpecifyCache)
{
    QUERY_BUFFER QueryBuffer;
    READ_BUFFER ReadBuffer;
    DWORD Length;
    USHORT i;
    USHORT BufferLength;
    SYSTEM_BASIC_INFORMATION BasicInfo;
    NTSTATUS Status;
    ULONG_PTR HighestAddress;
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"MmMapLockedPagesSpecifyCache", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    // Less than a page
    SET_BUFFER_LENGTH(BufferLength, 2048);
    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, TRUE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    // 1 page
    SET_BUFFER_LENGTH(BufferLength, 4096);
    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, TRUE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    // more than 1 page
    SET_BUFFER_LENGTH(BufferLength, 4096 + 2048);
    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, TRUE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    // 2 pages
    SET_BUFFER_LENGTH(BufferLength, 2 * 4096);
    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    Length = sizeof(QUERY_BUFFER);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, TRUE);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    // more than 2 pages
    SET_BUFFER_LENGTH(BufferLength, 2 * 4096 + 2048);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    Length = sizeof(QUERY_BUFFER);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, TRUE);
    Length = sizeof(QUERY_BUFFER);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer != NULL, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    // ask for a specific address (we know that ReadBuffer.Buffer is free)
    SET_BUFFER_LENGTH(BufferLength, 4096);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    QueryBuffer.Buffer = ReadBuffer.Buffer;
    Length = sizeof(QUERY_BUFFER);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer == ReadBuffer.Buffer, "Buffer is NULL\n");
    CHECK_ALLOC(QueryBuffer.Buffer, BufferLength);

    Length = 0;
    FILL_READ_BUFFER(QueryBuffer, ReadBuffer);
    ok(KmtSendBufferToDriver(IOCTL_READ_BUFFER, &ReadBuffer, sizeof(READ_BUFFER), &Length) == ERROR_SUCCESS, "\n");

    // ask for an unaligned address
    SET_BUFFER_LENGTH(BufferLength, 4096);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    QueryBuffer.Buffer = (PVOID)((ULONG_PTR)ReadBuffer.Buffer + 2048);
    QueryBuffer.Status = STATUS_INVALID_ADDRESS;
    Length = sizeof(QUERY_BUFFER);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer == NULL, "Buffer is %p\n", QueryBuffer.Buffer);

    Length = 0;
    ok(KmtSendBufferToDriver(IOCTL_CLEAN, NULL, 0, &Length) == ERROR_SUCCESS, "\n");

    // get system info for MmHighestUserAddress
    Status = NtQuerySystemInformation(SystemBasicInformation,
                                      &BasicInfo,
                                      sizeof(BasicInfo),
                                      NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    trace("MaximumUserModeAddress: %lx\n", BasicInfo.MaximumUserModeAddress);
    HighestAddress = ALIGN_DOWN_BY(BasicInfo.MaximumUserModeAddress, PAGE_SIZE);

    // near MmHighestUserAddress
    SET_BUFFER_LENGTH(BufferLength, 4096);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    QueryBuffer.Buffer = (PVOID)(HighestAddress - 15 * PAGE_SIZE); // 7ffe0000
    QueryBuffer.Status = STATUS_INVALID_ADDRESS;
    trace("QueryBuffer.Buffer %p\n", QueryBuffer.Buffer);
    Length = sizeof(QUERY_BUFFER);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Buffer == NULL, "Buffer is %p\n", QueryBuffer.Buffer);

    Length = 0;
    ok(KmtSendBufferToDriver(IOCTL_CLEAN, NULL, 0, &Length) == ERROR_SUCCESS, "\n");

    // far enough away from MmHighestUserAddress
    SET_BUFFER_LENGTH(BufferLength, 4096);
    FILL_QUERY_BUFFER(QueryBuffer, BufferLength, FALSE);
    QueryBuffer.Buffer = (PVOID)(HighestAddress - 16 * PAGE_SIZE); // 7ffdf000
    QueryBuffer.Status = -1;
    trace("QueryBuffer.Buffer %p\n", QueryBuffer.Buffer);
    Length = sizeof(QUERY_BUFFER);
    ok(KmtSendBufferToDriver(IOCTL_QUERY_BUFFER, &QueryBuffer, sizeof(QUERY_BUFFER), &Length) == ERROR_SUCCESS, "\n");
    ok_eq_int(QueryBuffer.Length, BufferLength);
    ok(QueryBuffer.Status == STATUS_SUCCESS ||
       QueryBuffer.Status == STATUS_CONFLICTING_ADDRESSES, "Status = %lx\n", QueryBuffer.Status);

    Length = 0;
    ok(KmtSendBufferToDriver(IOCTL_CLEAN, NULL, 0, &Length) == ERROR_SUCCESS, "\n");

    TestViewMappedTwice(PAGE_READWRITE | PAGE_NOCACHE, FALSE, "PAGE_NOCACHE file view, MmNonCached");
    TestViewMappedTwice(PAGE_READWRITE, TRUE, "file view, MmCached");
    TestPhysicalViewsReprotected();

    KmtCloseDriver();
    KmtUnloadDriver();
}

START_TEST(MmMapLockedPagesProcessExit)
{
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    WCHAR FileName[MAX_PATH];
    EXIT_BUFFER ExitBuffer;
    DWORD Length;
    DWORD Error;
    DWORD Wait;
    BOOL Ret;

    Error = KmtLoadAndOpenDriver(L"MmMapLockedPagesSpecifyCache", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    /* A suspended copy of ourselves provides an address space that never runs */
    Length = GetModuleFileNameW(NULL, FileName, RTL_NUMBER_OF(FileName));
    ok(Length != 0 && Length < RTL_NUMBER_OF(FileName), "GetModuleFileNameW failed: %lu\n", GetLastError());
    if (Length == 0 || Length >= RTL_NUMBER_OF(FileName))
        goto Cleanup;
    Ret = CreateProcessW(FileName, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED,
                         NULL, NULL, &StartupInfo, &ProcessInfo);
    ok(Ret, "CreateProcessW failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

    ExitBuffer.ProcessId = ProcessInfo.dwProcessId;
    ExitBuffer.Buffer = NULL;
    ExitBuffer.Status = STATUS_UNSUCCESSFUL;
    Length = sizeof(ExitBuffer);
    Error = KmtSendBufferToDriver(IOCTL_MAP_IN_PROCESS, &ExitBuffer, sizeof(ExitBuffer), &Length);
    ok_eq_int(Error, ERROR_SUCCESS);
    ok_eq_hex(ExitBuffer.Status, STATUS_SUCCESS);
    ok(ExitBuffer.Buffer != NULL, "Buffer is NULL\n");

    /* The process exits while the driver's pages are still mapped into it */
    Ret = TerminateProcess(ProcessInfo.hProcess, 0);
    ok(Ret, "TerminateProcess failed: %lu\n", GetLastError());
    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30 * 1000);
    ok_eq_int(Wait, WAIT_OBJECT_0);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);

    if (Error == ERROR_SUCCESS && ExitBuffer.Buffer != NULL && Wait == WAIT_OBJECT_0)
    {
        Error = KmtSendToDriver(IOCTL_CHECK_EXITED);
        ok_eq_int(Error, ERROR_SUCCESS);
    }

Cleanup:
    KmtCloseDriver();
    KmtUnloadDriver();
}
