/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQueryVirtualMemory
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static
VOID
CheckViewPage(
    PVOID BaseAddress,
    PVOID Address,
    ULONG State,
    ULONG Protect,
    SIZE_T RegionSize)
{
    NTSTATUS Status;
    MEMORY_BASIC_INFORMATION Info;
    SIZE_T ReturnLength = 0;

    RtlFillMemory(&Info, sizeof(Info), 0x55);
    Status = NtQueryVirtualMemory(NtCurrentProcess(),
                                  Address,
                                  MemoryBasicInformation,
                                  &Info,
                                  sizeof(Info),
                                  &ReturnLength);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_size_t(ReturnLength, sizeof(Info));
    ok_ptr(Info.BaseAddress, Address);
    ok_ptr(Info.AllocationBase, BaseAddress);
    ok_hex(Info.State, State);
    ok_hex(Info.Protect, Protect);
    ok_hex(Info.Type, MEM_MAPPED);
    ok_size_t(Info.RegionSize, RegionSize);
}

static
VOID
Test_SecReserveAccessedPage(VOID)
{
    NTSTATUS Status;
    HANDLE hSection;
    LARGE_INTEGER MaximumSize;
    PVOID BaseAddress = NULL;
    PVOID CommitAddress;
    SIZE_T ViewSize = 0;
    SIZE_T CommitSize;
    PUCHAR Page1;
    volatile UCHAR Value = 0;

    MaximumSize.QuadPart = 3 * PAGE_SIZE;
    Status = NtCreateSection(&hSection,
                             SECTION_ALL_ACCESS,
                             NULL,
                             &MaximumSize,
                             PAGE_READWRITE,
                             SEC_RESERVE,
                             NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtMapViewOfSection(hSection,
                                NtCurrentProcess(),
                                &BaseAddress,
                                0,
                                0,
                                NULL,
                                &ViewSize,
                                ViewUnmap,
                                0,
                                PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtClose(hSection);
        return;
    }
    ok_size_t(ViewSize, 3 * PAGE_SIZE);

    /* Pages 1 and 2 of the view are reserved */
    Page1 = (PUCHAR)BaseAddress + PAGE_SIZE;
    CheckViewPage(BaseAddress, Page1, MEM_RESERVE, 0, 2 * PAGE_SIZE);

    /* Touching a reserved page faults and leaves a prototype-format PTE */
    Status = STATUS_SUCCESS;
    _SEH2_TRY
    {
        Value = *Page1;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);

    /* The page is still reserved, like the one after it */
    CheckViewPage(BaseAddress, Page1, MEM_RESERVE, 0, 2 * PAGE_SIZE);

    /* Commit the touched page: now it is committed on its own */
    CommitAddress = Page1;
    CommitSize = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &CommitAddress,
                                     0,
                                     &CommitSize,
                                     MEM_COMMIT,
                                     PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        CheckViewPage(BaseAddress, Page1, MEM_COMMIT, PAGE_READWRITE, PAGE_SIZE);

        Status = STATUS_SUCCESS;
        _SEH2_TRY
        {
            *Page1 = 0x5A;
            Value = *Page1;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_hex(Value, 0x5A);
    }

    Status = NtUnmapViewOfSection(NtCurrentProcess(), BaseAddress);
    ok_ntstatus(Status, STATUS_SUCCESS);
    NtClose(hSection);
}

/*
 * MemorySectionName
 *
 * The result is a UNICODE_STRING header followed by its characters. The
 * kernel has to build it from its own state, never from what the caller's
 * buffer happens to hold while the call is running.
 */
static
VOID
Test_SectionNameBasic(VOID)
{
    NTSTATUS Status;
    PMEMORY_SECTION_NAME Name;
    PVOID Buffer;
    SIZE_T Size = 0x1000, ReturnLength = 0;
    PVOID Ntdll = GetModuleHandleW(L"ntdll.dll");
    PUCHAR Canary;

    Buffer = VirtualAlloc(NULL, 2 * PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(Buffer != NULL, "VirtualAlloc failed\n");
    if (!Buffer)
        return;
    Name = Buffer;
    Canary = (PUCHAR)Buffer + Size;
    RtlFillMemory(Buffer, 2 * PAGE_SIZE, 0x77);

    /* Header fields supplied by the caller are meaningless input: garbage must be ignored, not followed */
    Name->SectionFileName.Buffer = (PWSTR)(ULONG_PTR)(sizeof(PVOID) == 8 ? 0xFFFFF78000000000ULL : 0x80001000);
    Name->SectionFileName.Length = 0xFFFE;
    Name->SectionFileName.MaximumLength = 0xFFFF;

    Status = NtQueryVirtualMemory(NtCurrentProcess(), Ntdll, MemorySectionName, Buffer, Size, &ReturnLength);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_ptr(Name->SectionFileName.Buffer, (PWSTR)(Name + 1));
        ok(Name->SectionFileName.Length > 0 && Name->SectionFileName.Length <= Name->SectionFileName.MaximumLength,
           "Length %u Max %u\n", Name->SectionFileName.Length, Name->SectionFileName.MaximumLength);
        ok_int(Name->SectionFileName.MaximumLength, Size - sizeof(*Name));
        ok_size_t(ReturnLength, Name->SectionFileName.Length + sizeof(*Name));
        ok(_wcsnicmp((PWSTR)((PUCHAR)Name->SectionFileName.Buffer + Name->SectionFileName.Length - 9 * sizeof(WCHAR)),
                     L"ntdll.dll", 9) == 0, "Name is %wZ\n", &Name->SectionFileName);
    }
    ok_int(*Canary, 0x77);

    /* A buffer that cannot hold the name must never be overrun */
    RtlFillMemory(Buffer, 2 * PAGE_SIZE, 0x77);
    Size = sizeof(*Name) + 8;
    Canary = (PUCHAR)Buffer + Size;
    Status = NtQueryVirtualMemory(NtCurrentProcess(), Ntdll, MemorySectionName, Buffer, Size, &ReturnLength);
    if (NT_SUCCESS(Status))
    {
        ok(Name->SectionFileName.Length <= 8, "Length %u\n", Name->SectionFileName.Length);
        ok(Name->SectionFileName.MaximumLength <= 8, "Max %u\n", Name->SectionFileName.MaximumLength);
    }
    ok_int(Canary[0], 0x77);
    ok_int(Canary[1], 0x77);

    VirtualFree(Buffer, 0, MEM_RELEASE);
}

static volatile LONG SectionNameRaceStop;
static PMEMORY_SECTION_NAME SectionNameRaceHeader;
static PVOID SectionNameRaceVictim;

static
DWORD
WINAPI
SectionNameRaceFlipper(PVOID Context)
{
    volatile UNICODE_STRING *Header = &SectionNameRaceHeader->SectionFileName;

    UNREFERENCED_PARAMETER(Context);
    while (!SectionNameRaceStop)
    {
        Header->Buffer = SectionNameRaceVictim;
        Header->MaximumLength = 0xFFFF;
    }
    return 0;
}

/*
 * Double fetch: a second thread rewrites the header in the caller's buffer
 * while the call is in progress. The target is a harmless user page, not
 * kernel memory. If the kernel ever reads the header back from the buffer, the
 * name lands in that page. The window is a few instructions wide, so this is
 * a stress test: a pass proves little by itself, a failure is a real bug.
 */
static
VOID
Test_SectionNameHeaderRace(VOID)
{
    NTSTATUS Status = STATUS_SUCCESS;
    SYSTEM_INFO SystemInfo;
    PVOID Buffer, Victim;
    HANDLE Thread;
    ULONG i, Iterations = 0, Hits = 0;
    ULONG Start;
    SIZE_T ReturnLength;
    PVOID Ntdll = GetModuleHandleW(L"ntdll.dll");

    GetSystemInfo(&SystemInfo);
    if (SystemInfo.dwNumberOfProcessors < 2)
    {
        skip("Needs more than one processor\n");
        return;
    }

    Buffer = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Victim = VirtualAlloc(NULL, 4 * PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(Buffer && Victim, "VirtualAlloc failed\n");
    if (!Buffer || !Victim)
        return;

    SectionNameRaceHeader = Buffer;
    SectionNameRaceVictim = Victim;
    SectionNameRaceStop = 0;
    Thread = CreateThread(NULL, 0, SectionNameRaceFlipper, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed\n");
    if (Thread)
    {
        Start = GetTickCount();
        while (GetTickCount() - Start < 5000 && Iterations < 200000 && !Hits)
        {
            for (i = 0; i < 1000; i++)
            {
                Status = NtQueryVirtualMemory(NtCurrentProcess(), Ntdll, MemorySectionName,
                                              Buffer, PAGE_SIZE, &ReturnLength);
                if (!NT_SUCCESS(Status))
                {
                    ok_ntstatus(Status, STATUS_SUCCESS);
                    break;
                }
                Iterations++;
                if (*(volatile ULONG *)Victim != 0)
                {
                    Hits++;
                    break;
                }
            }
            if (!NT_SUCCESS(Status))
                break;
        }
        SectionNameRaceStop = 1;
        WaitForSingleObject(Thread, INFINITE);
        CloseHandle(Thread);
    }
    ok(Hits == 0, "Kernel followed a header rewritten by another thread (%lu iterations)\n", Iterations);
    trace("SectionName race: %lu iterations, %lu hits\n", Iterations, Hits);

    VirtualFree(Buffer, 0, MEM_RELEASE);
    VirtualFree(Victim, 0, MEM_RELEASE);
}

/* The address is looked up in the process that is being queried, not in the caller */
static
VOID
Test_SectionNameOtherProcess(VOID)
{
    NTSTATUS Status;
    WCHAR Path[MAX_PATH];
    STARTUPINFOW Si = { sizeof(Si) };
    PROCESS_INFORMATION Pi;
    PROCESS_BASIC_INFORMATION Pbi;
    PVOID ImageBase = NULL;
    SIZE_T Read;
    UCHAR Buffer[0x400];
    PMEMORY_SECTION_NAME Name = (PMEMORY_SECTION_NAME)Buffer;
    SIZE_T ReturnLength;

    GetSystemDirectoryW(Path, _countof(Path));
    StringCchCatW(Path, _countof(Path), L"\\cmd.exe");
    if (!CreateProcessW(Path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &Si, &Pi))
    {
        skip("Could not start %ls: %lu\n", Path, GetLastError());
        return;
    }

    Status = NtQueryInformationProcess(Pi.hProcess, ProcessBasicInformation, &Pbi, sizeof(Pbi), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status) &&
        ReadProcessMemory(Pi.hProcess,
                          (PUCHAR)Pbi.PebBaseAddress + FIELD_OFFSET(PEB, ImageBaseAddress),
                          &ImageBase, sizeof(ImageBase), &Read) && ImageBase)
    {
        RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
        Status = NtQueryVirtualMemory(Pi.hProcess, ImageBase, MemorySectionName,
                                      Buffer, sizeof(Buffer), &ReturnLength);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok(Name->SectionFileName.Length >= 7 * sizeof(WCHAR) &&
               _wcsnicmp((PWSTR)((PUCHAR)Name->SectionFileName.Buffer + Name->SectionFileName.Length - 7 * sizeof(WCHAR)),
                         L"cmd.exe", 7) == 0,
               "Expected cmd.exe, got %wZ\n", &Name->SectionFileName);
        }

        /* Same address, caller's own space: must not resolve to the child's image */
        Status = NtQueryVirtualMemory(NtCurrentProcess(), ImageBase, MemorySectionName,
                                      Buffer, sizeof(Buffer), &ReturnLength);
        if (NT_SUCCESS(Status))
        {
            ok(_wcsnicmp((PWSTR)((PUCHAR)Name->SectionFileName.Buffer + Name->SectionFileName.Length - 7 * sizeof(WCHAR)),
                         L"cmd.exe", 7) != 0 || Name->SectionFileName.Length < 7 * sizeof(WCHAR),
               "Own space returned %wZ\n", &Name->SectionFileName);
        }
    }
    else
    {
        skip("Could not find the child image base\n");
    }

    TerminateProcess(Pi.hProcess, 0);
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);
}

START_TEST(NtQueryVirtualMemory)
{
    Test_SecReserveAccessedPage();
    Test_SectionNameBasic();
    Test_SectionNameHeaderRace();
    Test_SectionNameOtherProcess();
}
