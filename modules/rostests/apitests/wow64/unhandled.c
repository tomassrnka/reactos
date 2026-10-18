/* Unhandled exceptions of a WoW64 main thread and their hard-error report.
 * The i386 main thread must start under kernel32's top-level filter, as
 * BaseProcessStartup starts it on native i386; NtRaiseHardError must reach
 * the native service. Identical results are expected on native i386.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define FILTER_EXIT 0x70
static unsigned Checks, Failures;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

typedef LONG (NTAPI *RAISE_HARD_ERROR)(LONG, ULONG, ULONG, PULONG_PTR, ULONG, PULONG);

static LONG WINAPI Filter(EXCEPTION_POINTERS *Exception)
{
    const EXCEPTION_RECORD *Record = Exception->ExceptionRecord;
    printf("FILTER code=%08lx address=%p info1=%p\n", Record->ExceptionCode,
           Record->ExceptionAddress, (PVOID)Record->ExceptionInformation[1]);
    ExitProcess(Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                Record->NumberParameters == 2 &&
                Record->ExceptionInformation[1] == 0x24 ? FILTER_EXIT : FILTER_EXIT + 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int Child(void)
{
    /* No frame of the image handles this fault */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(Filter);
    *(volatile LONG *)0x24 = 1;
    return 2;
}

int main(int argc, char **argv)
{
    RAISE_HARD_ERROR RaiseHardError;
    STARTUPINFOA Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Process;
    char Path[MAX_PATH], Command[MAX_PATH + 16];
    ULONG_PTR Parameters[6] = {0};
    ULONG Response = 0xdead;
    DWORD Code = 0;
    BOOL Wow64 = FALSE;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "child")) return Child();
    /* A PE32 fixture: WoW64 on amd64, the native control on i386 */
    CHECK(sizeof(void *) == 4);
    CHECK(IsWow64Process(GetCurrentProcess(), &Wow64));
    printf("START unhandled wow64=%u\n", Wow64);

    /* Before the fix the second-chance dispatch ended the child with the
     * exception code; the filter never ran and nothing was reported. */
    GetModuleFileNameA(NULL, Path, sizeof(Path));
    sprintf(Command, "\"%s\" child", Path);
    CHECK(CreateProcessA(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Process));
    if (Process.hProcess)
    {
        CHECK(WaitForSingleObject(Process.hProcess, 30000) == WAIT_OBJECT_0);
        CHECK(GetExitCodeProcess(Process.hProcess, &Code));
        printf("CHILD exit=%08lx\n", Code);
        CHECK(Code == FILTER_EXIT);
        CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
    }

    /* The service is reached: the kernel's own parameter checks answer,
     * none of which shows a dialog. Before the fix every call returned
     * STATUS_INVALID_SYSTEM_SERVICE. */
    RaiseHardError = (RAISE_HARD_ERROR)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtRaiseHardError");
    CHECK(RaiseHardError != NULL);
    if (RaiseHardError)
    {
        CHECK(RaiseHardError(0xC0000001, 6, 0, Parameters, 1 /* OptionOk */, &Response) == (LONG)0xC00000F0);
        CHECK(RaiseHardError(0xC0000001, 1, 0, Parameters, 0x77, &Response) == (LONG)0xC00000F2);
        /* A string parameter is a UNICODE_STRING pointer; a bad one faults */
        Parameters[0] = 0x10;
        CHECK(RaiseHardError(0xC0000001, 1, 1, Parameters, 1, &Response) == (LONG)0xC0000005);
        /* The service checks the response options before the parameters,
         * the array's alignment before its contents */
        CHECK(RaiseHardError(0xC0000001, 1, 0, (PULONG_PTR)0x10, 0x77, &Response) == (LONG)0xC00000F2);
        CHECK(RaiseHardError(0xC0000001, 1, 0, (PULONG_PTR)0x10, 1, &Response) == (LONG)0xC0000005);
        CHECK(RaiseHardError(0xC0000001, 1, 0, (PULONG_PTR)((PUCHAR)Parameters + 1), 1, &Response) == (LONG)0x80000002);
        /* A string descriptor is aligned like the array */
        Parameters[0] = (ULONG_PTR)((PUCHAR)&Parameters[2] + 1);
        CHECK(RaiseHardError(0xC0000001, 1, 1, Parameters, 1, &Response) == (LONG)0x80000002);
        CHECK(Response == 0xdead);
    }
    printf("UNHANDLED: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
