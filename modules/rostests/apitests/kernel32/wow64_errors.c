/* Thunk output-fault regressions. SPDX-License-Identifier: LGPL-2.1-or-later */
#define WIN32_NO_STATUS
#include <windows.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>

static void Check(BOOL Result, DWORD Code)
{
    if (!Result) ExitProcess(Code);
}

void __cdecl mainCRTStartup(void)
{
    struct OUTPUT { PVOID Base; SIZE_T Size; HANDLE Handle; IO_STATUS_BLOCK Io; } *Output;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\Wow64OutputFault");
    OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;
    ULONG OldProtect, i;
    HANDLE Handle, Section;
    PVOID Candidate, Base;
    SIZE_T Size;
    LARGE_INTEGER MaximumSize;
    MEMORY_BASIC_INFORMATION Info;
    WCHAR Mode = GetCommandLineW()[lstrlenW(GetCommandLineW()) - 1];

    Output = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Check(Output != NULL, 10);
    Check(VirtualProtect(Output, 4096, PAGE_READONLY, &OldProtect), 11);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    for (i = 0; Mode == L'h' && i < 2; ++i)
    {
        Status = NtCreateEvent(i ? &Output->Handle : (PHANDLE)0x1000, EVENT_ALL_ACCESS,
                               &Attributes, NotificationEvent, FALSE);
        Check(Status == STATUS_ACCESS_VIOLATION, 12);
        /* An inaccessible destination must not leave a native handle keeping
         * the named object alive after the failed create. */
        Status = NtOpenEvent(&Handle, EVENT_ALL_ACCESS, &Attributes);
        if (NT_SUCCESS(Status)) NtClose(Handle);
        Check(Status == STATUS_OBJECT_NAME_NOT_FOUND, 13);
    }
    if (Mode == L'h') ExitProcess(0);

    MaximumSize.QuadPart = 0x10000;
    Check(NT_SUCCESS(NtCreateSection(&Section, SECTION_ALL_ACCESS, NULL,
                                    &MaximumSize, PAGE_READWRITE, SEC_COMMIT, NULL)), 14);
    for (i = (Mode == L'm' ? 2 : 0); i < (Mode == L'm' ? 4 : 2); ++i)
    {
        Candidate = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE);
        Check(Candidate != NULL, 15);
        Check(VirtualFree(Candidate, 0, MEM_RELEASE), 16);
        Check(VirtualProtect(Output, 4096, PAGE_READWRITE, &OldProtect), 17);
        Output->Base = Base = Candidate;
        Output->Size = Size = 4096;
        Check(VirtualProtect(Output, 4096, PAGE_READONLY, &OldProtect), 18);
        if (i < 2)
            Status = NtAllocateVirtualMemory(NtCurrentProcess(), i ? &Base : &Output->Base,
                                              0, i ? &Output->Size : &Size,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        else
            Status = NtMapViewOfSection(Section, NtCurrentProcess(),
                                        (i & 1) ? &Base : &Output->Base, 0, 0, NULL,
                                        (i & 1) ? &Output->Size : &Size, ViewUnmap, 0, PAGE_READWRITE);
        Check(Status == STATUS_ACCESS_VIOLATION, 19 + i);
        Check(VirtualQuery(Candidate, &Info, sizeof(Info)) == sizeof(Info), 23);
        Check(Info.State == MEM_FREE, 24 + i);
    }
    NtClose(Section);
    VirtualFree(Output, 0, MEM_RELEASE);
    ExitProcess(0);
}
