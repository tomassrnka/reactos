/* FS of a guest thread preempted while NtReadVirtualMemory is attached to a
 * native process. The scheduler loaded the compatibility-mode TEB descriptor
 * only when the attached process was a WoW64 one, so the thread returned with
 * the TEB32 of the last WoW64 thread that ran on the processor (the
 * Total Commander installer faulted in kernel32 this way, through psapi).
 * Both threads share one processor so that the spinner runs in between;
 * reads of untouched pages fault in the target while attached.
 * Also valid on native i386, where the target is an i386 process.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <intrin.h>

#define REGION (64 * 1024 * 1024)
#define CHUNK 512 /* Small reads copy while attached (MI_POOL_COPY_BYTES) */
static unsigned Checks, Failures;
static volatile LONG Stop, Spins;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

static ULONG TebSelf(void) { return __readfsdword(0x18); }

static DWORD WINAPI Spinner(LPVOID Parameter)
{
    ULONG Teb = TebSelf();
    (void)Parameter;
    while (!Stop)
    {
        if (TebSelf() != Teb) return 1;
        InterlockedIncrement(&Spins);
    }
    return 0;
}

int main(void)
{
    WCHAR Windows[MAX_PATH], Path[MAX_PATH + 32];
    STARTUPINFOW Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Process = {0};
    HANDLE Thread;
    PVOID Remote = NULL, Local;
    SIZE_T Read;
    ULONG Teb, i, Wrong = 0;
    DWORD Start;
    DWORD Code = 2;
    BOOL Wow64 = FALSE;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* A PE32 fixture: WoW64 on amd64, the native control on i386 */
    CHECK(sizeof(void *) == 4);
    if (!IsWow64Process(GetCurrentProcess(), &Wow64))
    {
        CHECK(!"IsWow64Process");
        goto Done;
    }
    printf("START attach-fs wow64=%u\n", Wow64);

    /* A native target: Sysnative bypasses the System32 redirection */
    GetSystemWindowsDirectoryW(Windows, MAX_PATH);
    _snwprintf(Path, RTL_NUMBER_OF(Path), L"%s\\%s\\cmd.exe", Windows, Wow64 ? L"Sysnative" : L"System32");
    CHECK(CreateProcessW(Path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &Startup, &Process));
    if (!Process.hProcess) goto Done;
    if (Wow64)
    {
        /* The target must be native for the read to attach to it */
        BOOL Child = TRUE;
        CHECK(IsWow64Process(Process.hProcess, &Child) && !Child);
    }
    Remote = VirtualAllocEx(Process.hProcess, NULL, REGION, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Local = VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(Remote != NULL && Local != NULL);
    if (!Remote || !Local) goto Done;

    CHECK(SetThreadAffinityMask(GetCurrentThread(), 1) != 0);
    Thread = CreateThread(NULL, 0, Spinner, NULL, CREATE_SUSPENDED, NULL);
    CHECK(Thread != NULL);
    if (!Thread) goto Done;
    CHECK(SetThreadAffinityMask(Thread, 1) != 0);
    ResumeThread(Thread);
    Teb = TebSelf();
    Start = GetTickCount();
    for (i = 0; GetTickCount() - Start < 5000; ++i)
    {
        PUCHAR Source = (PUCHAR)Remote + (i * 4096 + (i / (REGION / 4096)) * CHUNK) % REGION;
        if (!ReadProcessMemory(Process.hProcess, Source, Local, CHUNK, &Read) || Read != CHUNK)
        {
            CHECK(!"ReadProcessMemory");
            break;
        }
        /* Compare the raw FS view only: TEB-based calls fault or lie here */
        if (TebSelf() != Teb) ++Wrong;
    }
    Stop = TRUE;
    CHECK(WaitForSingleObject(Thread, 30000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeThread(Thread, &Code) && Code == 0);
    CloseHandle(Thread);
    printf("ATTACH FS: reads=%lu wrong-fs=%lu spins=%ld\n", i, Wrong, Spins);
    CHECK(Wrong == 0);
    /* Without preemption the test proves nothing */
    CHECK(Spins != 0);

Done:
    if (Process.hProcess)
    {
        TerminateProcess(Process.hProcess, 0);
        CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
    }
    printf("ATTACH FS: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
