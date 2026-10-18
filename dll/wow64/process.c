/* ReactOS WoW64 process creation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"
#include <reactos/wow64process.h>
#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif

typedef struct
{
    ULONG Size, Reserved, Desktop, Title;
    ULONG X, Y, XSize, YSize, XCountChars, YCountChars, FillAttribute, Flags;
    USHORT ShowWindow, ReservedSize;
    ULONG ReservedData, Input, Output, Error;
} STARTUPINFO32;

typedef BOOL (WINAPI *CREATE_PROCESS)(HANDLE, PCWSTR, PWSTR,
    SECURITY_ATTRIBUTES *, SECURITY_ATTRIBUTES *, BOOL, DWORD, PVOID, PCWSTR,
    STARTUPINFOW *, PROCESS_INFORMATION *, PHANDLE);

static SECURITY_ATTRIBUTES *
CaptureSecurity(ULONG Address, SECURITY_ATTRIBUTES *To, SECURITY_DESCRIPTOR *Sd)
{
    ULONG *From = PTR32(Address);
    if (!From) return NULL;
    To->nLength = sizeof(*To);
    To->lpSecurityDescriptor = PTR32(From[1]);
    To->bInheritHandle = From[2];
    if (To->lpSecurityDescriptor &&
        !(((SECURITY_DESCRIPTOR *)To->lpSecurityDescriptor)->Control & SE_SELF_RELATIVE))
    {
        SECURITY_DESCRIPTOR_RELATIVE *In = To->lpSecurityDescriptor;
        Sd->Revision = In->Revision;
        Sd->Sbz1 = In->Sbz1;
        Sd->Control = In->Control;
        Sd->Owner = PTR32(In->Owner);
        Sd->Group = PTR32(In->Group);
        Sd->Sacl = PTR32(In->Sacl);
        Sd->Dacl = PTR32(In->Dacl);
        To->lpSecurityDescriptor = Sd;
    }
    return To;
}

static VOID CaptureString(UNICODE_STRING *To, const WOW64_STRING *From)
{
    To->Length = From->Length;
    To->MaximumLength = From->MaximumLength;
    To->Buffer = PTR32(From->Buffer);
}

NTSTATUS Wow64CreateProcess(PVOID Buffer)
{
    WOW64_CREATE_PROCESS *Out = Buffer, Request = *Out;
    WOW64_CPU_STATE *Cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
    PVOID SavedParameters = Cpu->ProcessParameters;
    WOW64_PARAMETERS *From = PTR32(Request.ParentParameters);
    RTL_USER_PROCESS_PARAMETERS Parent = *NtCurrentPeb()->ProcessParameters;
    STARTUPINFO32 In;
    STARTUPINFOW Startup = {sizeof(Startup)};
    SECURITY_ATTRIBUTES ProcessSecurity, ThreadSecurity, *Ps, *Ts;
    SECURITY_DESCRIPTOR ProcessSd, ThreadSd;
    PROCESS_INFORMATION Info = {0};
    HANDLE NewToken = NULL;
    UNICODE_STRING Path = {0}, Name, ModuleName;
    OBJECT_ATTRIBUTES Attributes;
    PVOID Redirected = NULL, Module;
    PWSTR Application = NULL;
    WCHAR ModulePath[MAX_PATH];
    ANSI_STRING Export = RTL_CONSTANT_STRING("CreateProcessInternalW");
    CREATE_PROCESS Create;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG *Result = PTR32(Request.ProcessInformation);
    ULONG i;


    _SEH2_TRY
    {
        /* Validate destinations before creating a suspended child. Ownership
         * stays here until every output and the optional resume succeed. */
        for (i = 0; i < 4; ++i) ((volatile ULONG *)Result)[i] = 0;
        Out->Error = 0;
        if (Request.NewToken) *(volatile ULONG *)PTR32(Request.NewToken) = 0;
        if (Request.CreationFlags & EXTENDED_STARTUPINFO_PRESENT)
            _SEH2_YIELD(return STATUS_NOT_SUPPORTED);
        In = *(STARTUPINFO32 *)PTR32(Request.StartupInfo);
        Startup.lpReserved = PTR32(In.Reserved);
        Startup.lpDesktop = PTR32(In.Desktop);
        Startup.lpTitle = PTR32(In.Title);
        RtlCopyMemory(&Startup.dwX, &In.X, 8 * sizeof(ULONG));
        Startup.wShowWindow = In.ShowWindow;
        Startup.cbReserved2 = In.ReservedSize;
        Startup.lpReserved2 = PTR32(In.ReservedData);
        Startup.hStdInput = HANDLE32(In.Input);
        Startup.hStdOutput = HANDLE32(In.Output);
        Startup.hStdError = HANDLE32(In.Error);
        Ps = CaptureSecurity(Request.ProcessAttributes, &ProcessSecurity, &ProcessSd);
        Ts = CaptureSecurity(Request.ThreadAttributes, &ThreadSecurity, &ThreadSd);

        Parent.Flags = From->Flags;
        Parent.ConsoleHandle = HANDLE32(From->ConsoleHandle);
        Parent.ConsoleFlags = From->ConsoleFlags;
        Parent.StandardInput = HANDLE32(From->StandardInput);
        Parent.StandardOutput = HANDLE32(From->StandardOutput);
        Parent.StandardError = HANDLE32(From->StandardError);
        Parent.CurrentDirectory.Handle = HANDLE32(From->CurrentDirectory.Handle);
        CaptureString(&Parent.CurrentDirectory.DosPath, &From->CurrentDirectory.DosPath);
        CaptureString(&Parent.DesktopInfo, &From->DesktopInfo);
        Parent.Environment = PTR32(From->Environment);

        if (!RtlDosPathNameToNtPathName_U(PTR32(Request.Application), &Path, NULL, NULL))
        {
            Status = STATUS_OBJECT_PATH_INVALID;
            _SEH2_LEAVE;
        }
        InitializeObjectAttributes(&Attributes, &Path, OBJ_CASE_INSENSITIVE, NULL, NULL);
        Status = Wow64RedirectFile(&Attributes, &Name, &Redirected);
        if (!NT_SUCCESS(Status)) _SEH2_LEAVE;
        Name = *Attributes.ObjectName;
        /* The input is an absolute DOS path resolved by guest kernel32.
         * Preserve its ordinary Win32 form: adding a device prefix changes
         * GetModuleFileName in the child and breaks path-based applications. */
        if (Name.Length < 4 * sizeof(WCHAR) || wcsncmp(Name.Buffer, L"\\??\\", 4))
        {
            Status = STATUS_OBJECT_PATH_INVALID;
            _SEH2_LEAVE;
        }
        Application = RtlAllocateHeap(RtlGetProcessHeap(), 0, Name.Length + sizeof(WCHAR));
        if (!Application) { Status = STATUS_NO_MEMORY; _SEH2_LEAVE; }
        if (!wcsncmp(PTR32(Request.Application), L"\\\\?\\", 4))
        {
            RtlCopyMemory(Application, Name.Buffer, Name.Length);
            Application[1] = L'\\';
            Application[Name.Length / sizeof(WCHAR)] = 0;
        }
        else if (Name.Length >= 8 * sizeof(WCHAR) && !_wcsnicmp(Name.Buffer + 4, L"UNC\\", 4))
        {
            Application[0] = Application[1] = L'\\';
            RtlCopyMemory(Application + 2, Name.Buffer + 8, Name.Length - 8 * sizeof(WCHAR));
            Application[Name.Length / sizeof(WCHAR) - 6] = 0;
        }
        else
        {
            RtlCopyMemory(Application, Name.Buffer + 4, Name.Length - 4 * sizeof(WCHAR));
            Application[Name.Length / sizeof(WCHAR) - 4] = 0;
        }

        wcscpy(ModulePath, SharedUserData->NtSystemRoot);
        wcscat(ModulePath, L"\\System32\\kernel32.dll");
        RtlInitUnicodeString(&ModuleName, ModulePath);
        Status = LdrGetDllHandle(NULL, NULL, &ModuleName, &Module);
        if (!NT_SUCCESS(Status)) Status = LdrLoadDll(NULL, NULL, &ModuleName, &Module);
        if (!NT_SUCCESS(Status)) _SEH2_LEAVE;
        Status = LdrGetProcedureAddress(Module, &Export, 0, (PVOID *)&Create);
        if (!NT_SUCCESS(Status)) _SEH2_LEAVE;

        Cpu->ProcessParameters = &Parent;
        if (!Create(HANDLE32(Request.Token), Application, PTR32(Request.CommandLine), Ps, Ts,
                    Request.InheritHandles, Request.CreationFlags | CREATE_SUSPENDED,
                    Request.Environment ? PTR32(Request.Environment) : Parent.Environment,
                    Request.CurrentDirectory ? PTR32(Request.CurrentDirectory) : Parent.CurrentDirectory.DosPath.Buffer,
                    &Startup, &Info, Request.NewToken ? &NewToken : NULL))
        {
            Out->Error = NtCurrentTeb()->LastErrorValue;
            if (!Out->Error) Out->Error = ERROR_GEN_FAILURE;
            _SEH2_LEAVE;
        }
        Result[0] = HandleToUlong(Info.hProcess);
        Result[1] = HandleToUlong(Info.hThread);
        Result[2] = Info.dwProcessId;
        Result[3] = Info.dwThreadId;
        if (Request.NewToken) *(ULONG *)PTR32(Request.NewToken) = HandleToUlong(NewToken);
        if (!(Request.CreationFlags & CREATE_SUSPENDED))
            Status = NtResumeThread(Info.hThread, NULL);
        if (NT_SUCCESS(Status))
        {
            Info.hProcess = Info.hThread = NewToken = NULL;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { Status = _SEH2_GetExceptionCode(); }
    _SEH2_END;
    Cpu->ProcessParameters = SavedParameters;
    if (Info.hProcess)
    {
        NtTerminateProcess(Info.hProcess, STATUS_UNSUCCESSFUL);
        NtClose(Info.hProcess);
    }
    if (Info.hThread) NtClose(Info.hThread);
    if (NewToken) NtClose(NewToken);
    if (Application) RtlFreeHeap(RtlGetProcessHeap(), 0, Application);
    if (Redirected) RtlFreeHeap(RtlGetProcessHeap(), 0, Redirected);
    RtlFreeUnicodeString(&Path);
    return Status;
}
