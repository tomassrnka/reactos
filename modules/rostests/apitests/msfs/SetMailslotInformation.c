/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Mailslot file system (MSFS) FileMailslotSetInformation contract test.
 *              FILE_MAILSLOT_SET_INFORMATION carries a caller-supplied pointer to the new
 *              read timeout. A valid pointer must update the timeout; a pointer that is
 *              NULL, kernel-mode or not readable must be rejected with an access violation
 *              and must leave the mailslot unchanged (the kernel must not dereference it).
 */

#include <apitest.h>
#include <ndk/ntndk.h>

#ifdef _WIN64
#define KERNEL_ADDRESS ((PVOID)0xFFFFF78000000000ULL) /* kernel alias of KUSER_SHARED_DATA */
#else
#define KERNEL_ADDRESS ((PVOID)0xFFDF0000)
#endif

static
NTSTATUS
SetReadTimeout(HANDLE hSlot, PLARGE_INTEGER Pointer)
{
    IO_STATUS_BLOCK Iosb;
    FILE_MAILSLOT_SET_INFORMATION Info;

    Info.ReadTimeout = Pointer;
    return NtSetInformationFile(hSlot, &Iosb, &Info, sizeof(Info), FileMailslotSetInformation);
}

static
LONGLONG
GetReadTimeout(HANDLE hSlot)
{
    IO_STATUS_BLOCK Iosb;
    FILE_MAILSLOT_QUERY_INFORMATION Info;
    NTSTATUS Status;

    Status = NtQueryInformationFile(hSlot, &Iosb, &Info, sizeof(Info), FileMailslotQueryInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return Info.ReadTimeout.QuadPart;
}

START_TEST(MsfsSetMailslotInformation)
{
    NTSTATUS Status;
    HANDLE hSlot;
    LARGE_INTEGER Timeout;
    PVOID Straddle;
    SYSTEM_INFO si;

    hSlot = CreateMailslotW(L"\\\\.\\mailslot\\MsfsSetInfoApiTest", 0, 5000, NULL);
    if (hSlot == INVALID_HANDLE_VALUE)
    {
        skip("Failed to create the mailslot: 0x%08lX\n", GetLastError());
        return;
    }
    GetSystemInfo(&si);

    /* A valid pointer works and the value is stored */
    Timeout.QuadPart = -12345678;
    Status = SetReadTimeout(hSlot, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(GetReadTimeout(hSlot) == -12345678, "ReadTimeout = %I64d\n", GetReadTimeout(hSlot));

    /* Every invalid pointer is rejected and the stored value is unchanged */
    Status = SetReadTimeout(hSlot, NULL);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);

    Status = SetReadTimeout(hSlot, KERNEL_ADDRESS);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);

    Status = SetReadTimeout(hSlot, (PLARGE_INTEGER)MAXULONG_PTR);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Starts in user space but runs past the end of it */
    Straddle = (PUCHAR)si.lpMaximumApplicationAddress + 1 - 4;
    Status = SetReadTimeout(hSlot, Straddle);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Freed (unmapped) user memory */
    {
        PVOID Page = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (Page)
        {
            VirtualFree(Page, 0, MEM_RELEASE);
            Status = SetReadTimeout(hSlot, Page);
            ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);
        }
    }

    ok(GetReadTimeout(hSlot) == -12345678,
       "ReadTimeout changed to %I64d by a rejected request\n", GetReadTimeout(hSlot));

    /* Undersized buffer is still rejected */
    {
        IO_STATUS_BLOCK Iosb;
        FILE_MAILSLOT_SET_INFORMATION Info;
        Info.ReadTimeout = &Timeout;
        Status = NtSetInformationFile(hSlot, &Iosb, &Info, sizeof(Info) - 1, FileMailslotSetInformation);
        ok(Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_INFO_LENGTH_MISMATCH,
           "Status = 0x%08lX\n", Status);
    }

    CloseHandle(hSlot);
}
