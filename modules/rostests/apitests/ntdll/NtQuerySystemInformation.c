/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQuerySystemInformation
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 *              Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

static
void
Test_ThreadStackLimits(void)
{
    NTSTATUS Status;
    ULONG_PTR SystemRangeStart;
    PSYSTEM_PROCESS_INFORMATION Process;
    PSYSTEM_EXTENDED_THREAD_INFORMATION Thread;
    PVOID Buffer = NULL;
    SIZE_T BufferSize = 0x10000;
    ULONG Length, i, Threads = 0, Hidden = 0, Bad = 0;
    BOOL IsWow64 = FALSE;

    if (IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64)
    {
        skip("WOW64 reports kernel stack addresses truncated to 32 bits\n");
        return;
    }

    Status = NtQuerySystemInformation(SystemRangeStartInformation,
                                      &SystemRangeStart,
                                      sizeof(SystemRangeStart),
                                      NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    for (;;)
    {
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
        if (!Buffer)
        {
            skip("Out of memory\n");
            return;
        }
        Status = NtQuerySystemInformation(SystemExtendedProcessInformation,
                                          Buffer,
                                          (ULONG)BufferSize,
                                          &Length);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            break;
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        BufferSize *= 2;
    }

    if (!NT_SUCCESS(Status))
    {
        skip("SystemExtendedProcessInformation: 0x%lx\n", Status);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        return;
    }

    /* Every thread has a kernel stack. The two bounds are read separately
     * while a thread may switch to a large stack, so do not compare them. */
    Process = Buffer;
    for (;;)
    {
        Thread = (PSYSTEM_EXTENDED_THREAD_INFORMATION)(Process + 1);
        for (i = 0; i < Process->NumberOfThreads; i++, Thread++, Threads++)
        {
            if (!Thread->StackBase && !Thread->StackLimit)
            {
                Hidden++;
                continue;
            }
            if ((ULONG_PTR)Thread->StackLimit < SystemRangeStart ||
                (ULONG_PTR)Thread->StackBase < SystemRangeStart)
            {
                trace("Process %p thread %p: stack limit %p, base %p\n",
                      Thread->ThreadInfo.ClientId.UniqueProcess,
                      Thread->ThreadInfo.ClientId.UniqueThread,
                      Thread->StackLimit,
                      Thread->StackBase);
                Bad++;
            }
        }
        if (!Process->NextEntryOffset)
            break;
        Process = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Process + Process->NextEntryOffset);
    }

    ok(Threads > 0, "No threads listed\n");
    if (Hidden == Threads)
        skip("Kernel stack addresses are not disclosed\n");
    else
        ok(Bad == 0, "%lu of %lu threads have a stack bound below %p\n", Bad, Threads - Hidden, (PVOID)SystemRangeStart);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

static
PSYSTEM_PROCESS_INFORMATION
FindProcessEntry(
    _In_ PVOID Buffer,
    _In_ HANDLE ProcessId)
{
    PSYSTEM_PROCESS_INFORMATION Entry = Buffer;

    for (;;)
    {
        if (Entry->UniqueProcessId == ProcessId)
            return Entry;
        if (!Entry->NextEntryOffset)
            return NULL;
        Entry = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Entry + Entry->NextEntryOffset);
    }
}

static
void
Test_ProcessImageNames(void)
{
    NTSTATUS Status;
    PUCHAR Buffer, End;
    SIZE_T BufferSize = 0x40000;
    ULONG Length, Entries = 0;
    PSYSTEM_PROCESS_INFORMATION Entry, Self;
    WCHAR Path[MAX_PATH];
    PWCHAR Name;

    Buffer = VirtualAlloc(NULL, BufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!Buffer)
    {
        skip("VirtualAlloc failed\n");
        return;
    }
    End = Buffer + BufferSize;

    /* Garbage in the output buffer, including where the image name headers
     * will go, must not influence the names the kernel composes. */
    memset(Buffer, 0xA5, BufferSize);
    Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, BufferSize, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return;
    }

    Entry = (PSYSTEM_PROCESS_INFORMATION)Buffer;
    for (;;)
    {
        Entries++;
        ok((PUCHAR)Entry >= Buffer && (PUCHAR)(Entry + 1) <= End, "Entry %p out of buffer\n", Entry);
        ok(Entry->ImageName.Length % sizeof(WCHAR) == 0, "Length %u is odd\n", Entry->ImageName.Length);
        ok(Entry->ImageName.Length <= Entry->ImageName.MaximumLength,
           "Length %u > MaximumLength %u\n", Entry->ImageName.Length, Entry->ImageName.MaximumLength);
        if (Entry->ImageName.Buffer)
        {
            ok((PUCHAR)Entry->ImageName.Buffer >= (PUCHAR)(Entry + 1) &&
               (PUCHAR)Entry->ImageName.Buffer + Entry->ImageName.MaximumLength <= End,
               "Name buffer %p (%u) outside the output buffer\n",
               Entry->ImageName.Buffer, Entry->ImageName.MaximumLength);
        }
        else
        {
            ok(Entry->ImageName.Length == 0 && Entry->ImageName.MaximumLength == 0,
               "NULL name with lengths %u/%u\n",
               Entry->ImageName.Length, Entry->ImageName.MaximumLength);
        }
        if (!Entry->NextEntryOffset)
            break;
        Entry = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Entry + Entry->NextEntryOffset);
    }
    ok(Entries > 1, "Only %lu entries\n", Entries);

    /* This process: the name comes from the image section */
    Self = FindProcessEntry(Buffer, (HANDLE)(ULONG_PTR)GetCurrentProcessId());
    ok(Self != NULL, "Own process not found\n");
    if (Self && GetModuleFileNameW(NULL, Path, RTL_NUMBER_OF(Path)))
    {
        Name = wcsrchr(Path, L'\\');
        Name = Name ? Name + 1 : Path;
        ok(Self->ImageName.Length == wcslen(Name) * sizeof(WCHAR) &&
           Self->ImageName.Buffer &&
           !_wcsnicmp(Self->ImageName.Buffer, Name, Self->ImageName.Length / sizeof(WCHAR)),
           "Own name is '%.*ls', expected '%ls'\n",
           Self->ImageName.Length / sizeof(WCHAR), Self->ImageName.Buffer, Name);
    }

    /* The System process has no image section: its name is converted from
     * EPROCESS::ImageFileName in the kernel. */
    Entry = FindProcessEntry(Buffer, (HANDLE)(ULONG_PTR)4);
    if (Entry)
    {
        ok(Entry->ImageName.Length == 6 * sizeof(WCHAR) &&
           Entry->ImageName.Buffer &&
           !_wcsnicmp(Entry->ImageName.Buffer, L"System", 6),
           "System name is '%.*ls' (%u)\n",
           Entry->ImageName.Length / sizeof(WCHAR), Entry->ImageName.Buffer, Entry->ImageName.Length);
    }

    /* A buffer that is too short fails cleanly and never writes past its
     * end: put it right in front of an inaccessible page. */
    {
        ULONG Old;
        PUCHAR Guard = Buffer + BufferSize - 0x1000;

        if (Length > 16 && Length - 8 <= BufferSize - 0x1000 &&
            VirtualProtect(Guard, 0x1000, PAGE_NOACCESS, &Old))
        {
            Status = NtQuerySystemInformation(SystemProcessInformation,
                                              Guard - (Length - 8), Length - 8, &Length);
            ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
            VirtualProtect(Guard, 0x1000, Old, &Old);
        }
    }

    VirtualFree(Buffer, 0, MEM_RELEASE);
}

typedef struct _FLIP_CONTEXT
{
    PVOID Address;
    volatile LONG Stop;
} FLIP_CONTEXT;

static
DWORD
WINAPI
FlipThread(PVOID Param)
{
    FLIP_CONTEXT *Flip = Param;
    DWORD Old;

    while (!Flip->Stop)
    {
        VirtualProtect(Flip->Address, 0x1000, PAGE_NOACCESS, &Old);
        Sleep(1);
        VirtualProtect(Flip->Address, 0x1000, PAGE_READWRITE, &Old);
        Sleep(1);
    }
    return 0;
}

static
DWORD
WINAPI
NopThread(PVOID Param)
{
    return 0;
}

static
void
Test_FaultReleasesProcessLock(void)
{
    NTSTATUS Status;
    PUCHAR Buffer, Tail;
    ULONG Needed, Length, Attempt, Faults = 0, Old;
    HANDLE Flipper, Thread;
    FLIP_CONTEXT Flip;
    DWORD Wait, Deadline;

    Status = NtQuerySystemInformation(SystemProcessInformation, NULL, 0, &Needed);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    Needed = (Needed + 0x1000) & ~0xFFF;

    /* The buffer ends in a page the flipper thread keeps making
     * inaccessible and accessible again. The kernel touches the whole
     * buffer when it starts, so most calls fail before any lock is taken;
     * a few are preempted later, with the entry for the newest process,
     * which is normally this one, half written and its lock held. */
    Buffer = VirtualAlloc(NULL, Needed + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!Buffer)
    {
        skip("VirtualAlloc failed\n");
        return;
    }
    Tail = Buffer + Needed - 0x1000;

    Flip.Address = Tail;
    Flip.Stop = 0;
    Flipper = CreateThread(NULL, 0, FlipThread, &Flip, 0, NULL);
    if (!Flipper)
    {
        skip("CreateThread failed\n");
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return;
    }
    SetThreadPriority(Flipper, THREAD_PRIORITY_TIME_CRITICAL);

    Deadline = GetTickCount() + 5000;
    for (Attempt = 0; (LONG)(Deadline - GetTickCount()) > 0; Attempt++)
    {
        Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Needed, &Length);
        if (Status == STATUS_ACCESS_VIOLATION)
            Faults++;
        else if (Status != STATUS_SUCCESS && Status != STATUS_INFO_LENGTH_MISMATCH)
            ok(0, "Unexpected status %08lx\n", Status);
    }

    Flip.Stop = 1;
    WaitForSingleObject(Flipper, INFINITE);
    CloseHandle(Flipper);
    VirtualProtect(Tail, 0x1000, PAGE_READWRITE, &Old);

    trace("%lu faulting calls out of %lu\n", Faults, Attempt);
    if (!Faults)
    {
        skip("Could not make a call fault\n");
    }
    else
    {
        /* The fault is reported, the caller is still usable... */
        Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Needed, &Length);
        ok(Status == STATUS_SUCCESS || Status == STATUS_INFO_LENGTH_MISMATCH,
           "Status %08lx after the faults\n", Status);

        /* ...and no process lock is left held: creating a thread takes the
         * lock of this process exclusively and would wait forever. */
        Thread = CreateThread(NULL, 0, NopThread, NULL, 0, NULL);
        ok(Thread != NULL, "CreateThread failed after the faults: %lu\n", GetLastError());
        if (Thread)
        {
            Wait = WaitForSingleObject(Thread, 10000);
            ok_dec(Wait, WAIT_OBJECT_0);
            CloseHandle(Thread);
        }
    }

    VirtualFree(Buffer, 0, MEM_RELEASE);
}

static
void
Test_PageFileInformation(void)
{
    PSYSTEM_PAGEFILE_INFORMATION Entry;
    UCHAR Buffer[4096];
    NTSTATUS Status;
    ULONG Length, Offset, Entries = 0;
    BOOL IsWow64 = FALSE;
    WCHAR Path[MAX_PATH];
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    IsWow64Process(GetCurrentProcess(), &IsWow64);

    /* A buffer smaller than one entry fails (the required length is not defined by Windows for WoW64) */
    Length = 0xdeadbeef;
    Status = NtQuerySystemInformation(SystemPageFileInformation, Buffer, 0, &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    RtlFillMemory(Buffer, sizeof(Buffer), 0xcd);
    Length = 0;
    Status = NtQuerySystemInformation(SystemPageFileInformation, Buffer, sizeof(Buffer), &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    if (Length == 0)
    {
        skip("No paging file\n");
        return;
    }
    ok(Length >= sizeof(*Entry), "Length = %lu\n", Length);

    /* One entry per paging file, each followed by its NT name */
    for (Offset = 0; ; )
    {
        PWCHAR Name;
        SIZE_T NameChars;

        ok(Offset + sizeof(*Entry) <= Length, "entry at %lu beyond Length %lu\n", Offset, Length);
        if (Offset + sizeof(*Entry) > Length)
            break;
        Entry = (PSYSTEM_PAGEFILE_INFORMATION)(Buffer + Offset);
        Entries++;

        ok(Entry->TotalSize > 0, "TotalSize = %lu\n", Entry->TotalSize);
        ok(Entry->TotalInUse <= Entry->TotalSize, "TotalInUse %lu > TotalSize %lu\n", Entry->TotalInUse, Entry->TotalSize);
        ok(Entry->PeakUsage >= Entry->TotalInUse, "PeakUsage %lu < TotalInUse %lu\n", Entry->PeakUsage, Entry->TotalInUse);
        ok(Entry->PeakUsage <= Entry->TotalSize, "PeakUsage %lu > TotalSize %lu\n", Entry->PeakUsage, Entry->TotalSize);

        ok(Entry->PageFileName.Length != 0, "empty paging file name\n");
        ok_int(Entry->PageFileName.MaximumLength, Entry->PageFileName.Length + sizeof(WCHAR));
        Name = Entry->PageFileName.Buffer;
        ok((PUCHAR)Name >= (PUCHAR)(Entry + 1), "name %p before the end of the entry %p\n", Name, Entry + 1);
        ok((PUCHAR)Name + Entry->PageFileName.MaximumLength <= Buffer + Length,
           "name %p +%u beyond Length %lu\n", Name, Entry->PageFileName.MaximumLength, Length);
        if ((PUCHAR)Name >= (PUCHAR)(Entry + 1) &&
            (PUCHAR)Name + Entry->PageFileName.MaximumLength <= Buffer + Length)
        {
            NameChars = Entry->PageFileName.Length / sizeof(WCHAR);
            ok(Name[NameChars] == UNICODE_NULL, "name is not terminated\n");
            ok(NameChars > 4 && !_wcsnicmp(Name, L"\\??\\", 4), "name %.*ls is not an NT DOS path\n", (int)NameChars, Name);
            ok(NameChars > 8 && !_wcsnicmp(Name + NameChars - 4, L".sys", 4), "name %.*ls does not end in .sys\n", (int)NameChars, Name);
            if (NameChars > 4 && NameChars < MAX_PATH + 4)
            {
                WIN32_FIND_DATAW FindData;
                HANDLE Find;

                /* The file is in use: GetFileAttributes fails with a sharing violation, enumeration works */
                wcsncpy(Path, Name + 4, NameChars - 4);
                Path[NameChars - 4] = UNICODE_NULL;
                Find = FindFirstFileW(Path, &FindData);
                ok(Find != INVALID_HANDLE_VALUE, "%ls does not exist (error %lu)\n", Path, GetLastError());
                if (Find != INVALID_HANDLE_VALUE)
                {
                    ULONGLONG FileSize = ((ULONGLONG)FindData.nFileSizeHigh << 32) | FindData.nFileSizeLow;

                    /* TotalSize is the size of the file in pages (the file may be growing) */
                    ok(FileSize >= (ULONGLONG)Entry->TotalSize * si.dwPageSize,
                       "%ls: file size %I64u, TotalSize %lu pages\n", Path, FileSize, Entry->TotalSize);
                    FindClose(Find);
                }
            }
        }

        if (Entry->NextEntryOffset == 0)
            break;
        ok(Entry->NextEntryOffset >= sizeof(*Entry), "NextEntryOffset %lu\n", Entry->NextEntryOffset);
        if (Entry->NextEntryOffset < sizeof(*Entry))
            break;
        Offset += Entry->NextEntryOffset;
    }
    ok(Entries >= 1, "Entries = %lu\n", Entries);

    /* One byte short of everything fails and reports the full length */
    if (!IsWow64)
    {
        ULONG Full = Length, Reported = 0;

        Status = NtQuerySystemInformation(SystemPageFileInformation, Buffer, Full - 1, &Reported);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        ok_dec(Reported, Full);
    }
}

START_TEST(NtQuerySystemInformation)
{
    NTSTATUS Status;

    Status = NtQuerySystemInformation(0, NULL, 0, NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    Status = NtQuerySystemInformation(0x80000000, NULL, 0, NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);

    Test_ThreadStackLimits();
    Test_PageFileInformation();
    Test_ProcessImageNames();
    Test_FaultReleasesProcessLock();
}
