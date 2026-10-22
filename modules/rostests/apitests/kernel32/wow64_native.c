/* Native thread-startup regression. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>

static DWORD WINAPI ThreadEntry(void *Parameter)
{
    return 42;
}

void __cdecl mainCRTStartup(void)
{
    BYTE *Image = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *Header = (IMAGE_NT_HEADERS *)(Image + ((IMAGE_DOS_HEADER *)Image)->e_lfanew);
    DWORD Signature = Header->Signature, OldProtect, ExitCode, Wait;
    HANDLE Thread;

    if (!VirtualProtect(&Header->Signature, sizeof(Signature), PAGE_READWRITE, &OldProtect))
        ExitProcess(10);
    Thread = CreateThread(NULL, 0, ThreadEntry, NULL, CREATE_SUSPENDED, NULL);
    if (!Thread) ExitProcess(11);
    Header->Signature = 0;
    if (ResumeThread(Thread) != 1) ExitProcess(12);
    Wait = WaitForSingleObject(Thread, 10000);
    Header->Signature = Signature;
    VirtualProtect(&Header->Signature, sizeof(Signature), OldProtect, &OldProtect);
    if (Wait != WAIT_OBJECT_0 || !GetExitCodeThread(Thread, &ExitCode) || ExitCode != 42)
        ExitProcess(13);
    CloseHandle(Thread);
    ExitProcess(0);
}
