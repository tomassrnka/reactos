/* NtQuerySystemInformation(SystemProcessInformation), Toolhelp32 process snapshots and
 * NtSuspendProcess/NtResumeProcess from a 32-bit process. Under WoW64 the process and
 * thread records are narrowed to the 32-bit layout (0xB8 bytes per process, 0x40 per
 * thread, image name after the threads). Google Updater's service and its Crashpad
 * handler depend on all three. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"
#include <tlhelp32.h>

static void TestProcessList(void)
{
    SYSTEM_PROCESS_INFORMATION *Process;
    SYSTEM_THREAD_INFORMATION *Thread;
    PUCHAR Buffer;
    ULONG Length = 0, Size, Offset, i, Count = 0;
    NTSTATUS Status;
    BOOLEAN FoundSelf = FALSE, FoundThread = FALSE;
    WCHAR Module[MAX_PATH], *Name;

    Status = NtQuerySystemInformation(SystemProcessInformation, NULL, 0, &Length);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Length > sizeof(*Process), "Length %lu\n", Length);
    if (Status != STATUS_INFO_LENGTH_MISMATCH || !Length) return;

    /* Too small a buffer reports the needed length again */
    Size = sizeof(*Process);
    Buffer = HeapAlloc(GetProcessHeap(), 0, Size);
    Length = 0;
    Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Size, &Length);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Length > Size, "Length %lu\n", Length);
    HeapFree(GetProcessHeap(), 0, Buffer);

    Size = Length + 0x10000;
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Size, &Length);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto done;
    ok(Length <= Size, "Length %lu, size %lu\n", Length, Size);

    GetModuleFileNameW(NULL, Module, MAX_PATH);
    Name = wcsrchr(Module, L'\\') ? wcsrchr(Module, L'\\') + 1 : Module;

    for (Offset = 0;; Offset += Process->NextEntryOffset)
    {
        Process = (SYSTEM_PROCESS_INFORMATION *)(Buffer + Offset);
        Count++;
        if (Process->NextEntryOffset)
            ok(Process->NextEntryOffset >= sizeof(*Process) + Process->NumberOfThreads * sizeof(*Thread),
               "Entry %lu: next %lu for %lu threads\n", Count, Process->NextEntryOffset, Process->NumberOfThreads);
        if (Process->ImageName.Buffer)
        {
            ok((PUCHAR)Process->ImageName.Buffer >= Buffer + Offset + sizeof(*Process) &&
               (PUCHAR)Process->ImageName.Buffer + Process->ImageName.Length <= Buffer + Length,
               "Entry %lu: image name %p outside the buffer\n", Count, Process->ImageName.Buffer);
        }
        if (Process->UniqueProcessId == ULongToHandle(GetCurrentProcessId()))
        {
            FoundSelf = TRUE;
            ok(Process->NumberOfThreads >= 1, "Threads %lu\n", Process->NumberOfThreads);
            ok(Process->ImageName.Buffer != NULL, "No image name\n");
            if (Process->ImageName.Buffer)
                ok(Process->ImageName.Length == wcslen(Name) * sizeof(WCHAR) &&
                   !_wcsnicmp(Process->ImageName.Buffer, Name, wcslen(Name)),
                   "Image name %.*ls, expected %ls\n", Process->ImageName.Length / 2,
                   Process->ImageName.Buffer, Name);
            Thread = (SYSTEM_THREAD_INFORMATION *)(Process + 1);
            for (i = 0; i < Process->NumberOfThreads; i++)
            {
                ok(Thread[i].ClientId.UniqueProcess == Process->UniqueProcessId,
                   "Thread %lu: process %p\n", i, Thread[i].ClientId.UniqueProcess);
                if (Thread[i].ClientId.UniqueThread == ULongToHandle(GetCurrentThreadId()))
                    FoundThread = TRUE;
            }
            ok(Process->HandleCount > 0, "Handle count %lu\n", Process->HandleCount);
            ok(Process->VirtualSize > 0, "Virtual size %Iu\n", Process->VirtualSize);
        }
        if (!Process->NextEntryOffset) break;
    }
    ok(Count > 5, "Only %lu processes\n", Count);
    ok(FoundSelf, "Current process not listed\n");
    ok(FoundThread, "Current thread not listed\n");
done:
    HeapFree(GetProcessHeap(), 0, Buffer);
}

static void TestToolhelp(void)
{
    PROCESSENTRY32W Entry = { sizeof(Entry) };
    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    BOOL More, Found = FALSE;
    ULONG Count = 0;

    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed, error %lu\n", GetLastError());
    if (Snapshot == INVALID_HANDLE_VALUE) return;
    for (More = Process32FirstW(Snapshot, &Entry); More; More = Process32NextW(Snapshot, &Entry))
    {
        Count++;
        if (Entry.th32ProcessID == GetCurrentProcessId())
        {
            Found = TRUE;
            ok(Entry.cntThreads >= 1, "Threads %lu\n", Entry.cntThreads);
        }
    }
    ok(Count > 5, "Only %lu processes\n", Count);
    ok(Found, "Current process not in the snapshot\n");
    CloseHandle(Snapshot);
}

static void TestSuspendResume(void)
{
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Info;
    WCHAR CommandLine[] = L"cmd.exe /c ping -n 30 127.0.0.1 >nul";
    NTSTATUS Status;

    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &Startup, &Info))
    {
        skip("CreateProcess failed, error %lu\n", GetLastError());
        return;
    }
    Status = NtSuspendProcess(Info.hProcess);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtResumeProcess(Info.hProcess);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtSuspendProcess(NULL);
    ok_ntstatus(Status, STATUS_INVALID_HANDLE);
    TerminateProcess(Info.hProcess, 0);
    WaitForSingleObject(Info.hProcess, 5000);
    CloseHandle(Info.hThread);
    CloseHandle(Info.hProcess);
}

START_TEST(Wow64SystemProcessInformation)
{
    TestProcessList();
    TestToolhelp();
    TestSuspendResume();
}
