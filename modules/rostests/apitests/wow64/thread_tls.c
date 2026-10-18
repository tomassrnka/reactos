/* Secondary guest threads in a WoW64 process whose image has no native TLS.
 * The native loader initializes every new thread, WoW64 or not; its TLS list
 * must exist even though the i386 loader owns the PE32 image's TLS.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>

static unsigned Checks, Failures;
static DWORD Slot;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

static DWORD WINAPI Worker(LPVOID Parameter)
{
    /* The thread starts with an empty guest TLS slot of its own */
    if (TlsGetValue(Slot)) return 0xbad;
    if (!TlsSetValue(Slot, Parameter)) return 0xbad + 1;
    Sleep(10);
    return TlsGetValue(Slot) == Parameter ? (DWORD)(ULONG_PTR)Parameter : 0xbad + 2;
}

int main(void)
{
    HANDLE Threads[8];
    DWORD Code, i;
    BOOL Wow64 = FALSE;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* A PE32 fixture: WoW64 on amd64, the native control on i386 */
    CHECK(sizeof(void *) == 4);
    CHECK(IsWow64Process(GetCurrentProcess(), &Wow64));
    printf("START thread-tls wow64=%u\n", Wow64);
    Slot = TlsAlloc();
    CHECK(Slot != TLS_OUT_OF_INDEXES);
    CHECK(TlsSetValue(Slot, (PVOID)0x1234));
    /* Before the fix the first new thread faulted in the native loader and
     * the process ended silently with STATUS_ACCESS_VIOLATION. */
    for (i = 0; i < RTL_NUMBER_OF(Threads); ++i)
    {
        Threads[i] = CreateThread(NULL, 0, Worker, (PVOID)(ULONG_PTR)(i + 1), 0, NULL);
        CHECK(Threads[i] != NULL);
    }
    for (i = 0; i < RTL_NUMBER_OF(Threads); ++i)
    {
        if (!Threads[i]) continue;
        CHECK(WaitForSingleObject(Threads[i], 30000) == WAIT_OBJECT_0);
        CHECK(GetExitCodeThread(Threads[i], &Code) && Code == i + 1);
        CloseHandle(Threads[i]);
    }
    CHECK(TlsGetValue(Slot) == (PVOID)0x1234);
    printf("THREAD TLS: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
