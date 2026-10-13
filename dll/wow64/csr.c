/* ReactOS WoW64 CSR translation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"
#include <winuser.h>
#include <wincon.h>
#include <wingdi.h>
#include <wincon_undoc.h>
#include <csr/csr.h>
#include <win/base.h>
#include <win/basemsg.h>
#include <win/conmsg.h>
#include "csr32.h"

static NTSTATUS CopyBaseData(VOID)
{
    PBASE_STATIC_SERVER_DATA From = NtCurrentPeb()->ReadOnlyStaticServerData[BASESRV_SERVERDLL_INDEX];
    WOW64_BASE_DATA *To;
    ULONG *Table;
    NTSTATUS Status;
    SIZE_T Size;
    PVOID ProtectBase;
    ULONG OldProtect;
    PEB32 *Peb = WOW64_PEB32(NtCurrentPeb());

    if (Peb->ReadOnlyStaticServerData) return STATUS_SUCCESS;
    Status = Wow64Allocate(sizeof(*To), (PVOID *)&To);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Wow64Allocate(4 * sizeof(ULONG), (PVOID *)&Table);
    if (!NT_SUCCESS(Status)) return Status;
#define STR(Field) do { Status = Wow64CopyString(&To->Field, &From->Field); \
    if (!NT_SUCCESS(Status)) return Status; } while (0)
    STR(WindowsDirectory); STR(NamedObjectDirectory);
    STR(WindowsSys32x86Directory);
    /* GetSystemDirectory in the guest names the installed i386 DLLs. */
    {
        WCHAR Buffer[MAX_PATH];
        UNICODE_STRING Directory;
        RtlInitEmptyUnicodeString(&Directory, Buffer, sizeof(Buffer));
        RtlAppendUnicodeStringToString(&Directory, &From->WindowsDirectory);
        RtlAppendUnicodeToString(&Directory, L"\\SysWOW64");
        Status = Wow64CopyString(&To->WindowsSystemDirectory, &Directory);
        if (!NT_SUCCESS(Status)) return Status;
    }
#undef STR
#define COPY(Field) To->Field = From->Field
    COPY(WindowsMajorVersion); COPY(WindowsMinorVersion); COPY(BuildNumber);
    COPY(CSDNumber); COPY(RCNumber); COPY(TimeOfDay); COPY(NlsUserInfo);
    COPY(DefaultSeparateVDM); COPY(IsWowTaskReady); COPY(fTermsrvAppInstallMode);
    COPY(tziTermsrvClientTimeZone); COPY(ktTermsrvClientBias); COPY(TermsrvClientTimeZoneId);
    COPY(LUIDDeviceMapsEnabled); COPY(TermsrvClientTimeZoneChangeNum);
    COPY(SysInfo.Reserved); COPY(SysInfo.TimerResolution); COPY(SysInfo.PageSize);
    COPY(SysInfo.NumberOfPhysicalPages); COPY(SysInfo.LowestPhysicalPageNumber);
    COPY(SysInfo.HighestPhysicalPageNumber); COPY(SysInfo.AllocationGranularity);
    COPY(SysInfo.NumberOfProcessors);
#undef COPY
    RtlCopyMemory(To->CSDVersion, From->CSDVersion, sizeof(To->CSDVersion));
    To->SysInfo.MinimumUserModeAddress = 0x10000;
    To->SysInfo.MaximumUserModeAddress = 0x7ffeffff;
    To->SysInfo.ActiveProcessorsAffinityMask = (ULONG)From->SysInfo.ActiveProcessorsAffinityMask;
    Table[BASESRV_SERVERDLL_INDEX] = PtrToUlong(To);
    ProtectBase = To; Size = sizeof(*To);
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &Size, PAGE_READONLY, &OldProtect);
    if (!NT_SUCCESS(Status)) return Status;
    ProtectBase = Table; Size = 4 * sizeof(ULONG);
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &Size, PAGE_READONLY, &OldProtect);
    if (!NT_SUCCESS(Status)) return Status;
    Peb->ReadOnlyStaticServerData = PtrToUlong(Table);
    return STATUS_SUCCESS;
}

/* Until guest callbacks are implemented, never register a PE32 address as
 * a native thread entry point in CSRSS. */
static DWORD WINAPI ConsoleControl(PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    NtTerminateProcess(NtCurrentProcess(), STATUS_CONTROL_C_EXIT);
    return 0;
}

static DWORD WINAPI UnsupportedConsoleCallback(PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    return ERROR_NOT_SUPPORTED;
}

static NTSTATUS Connect(ULONG *a)
{
    ULONG *Length = PTR32(a[3]);
    NTSTATUS Status;
    BOOLEAN InServer = FALSE;
    if (!Length || !a[2]) return STATUS_INVALID_PARAMETER;
    if (a[1] == BASESRV_SERVERDLL_INDEX)
    {
        if (*Length != sizeof(BASESRV_API_CONNECTINFO)) return STATUS_INFO_LENGTH_MISMATCH;
        Status = CsrClientConnectToServer(PTR32(a[0]), a[1], PTR32(a[2]), Length, &InServer);
        if (NT_SUCCESS(Status)) Status = CopyBaseData();
    }
    else if (a[1] == CONSRV_SERVERDLL_INDEX)
    {
        WOW64_CONSOLE_CONNECT *From = PTR32(a[2]);
        CONSRV_API_CONNECTINFO To = {0};
        ULONG NativeLength = sizeof(To), i;
        if (*Length != sizeof(*From)) return STATUS_INFO_LENGTH_MISMATCH;
#define H(Field) To.ConsoleStartInfo.Field = HANDLE32(From->ConsoleStartInfo.Field)
        H(ConsoleHandle); H(InputWaitHandle); H(InputHandle); H(OutputHandle); H(ErrorHandle);
        H(hIcon); H(hIconSm);
#undef H
        To.ConsoleStartInfo.IconIndex = From->ConsoleStartInfo.IconIndex;
        /* The property tail contains only fixed-width fields. */
        RtlCopyMemory((PUCHAR)&To.ConsoleStartInfo + FIELD_OFFSET(CONSOLE_START_INFO, dwHotKey),
                      (PUCHAR)&From->ConsoleStartInfo + FIELD_OFFSET(WOW64_CONSOLE_START, dwHotKey),
                      sizeof(WOW64_CONSOLE_START) - FIELD_OFFSET(WOW64_CONSOLE_START, dwHotKey));
        To.IsConsoleApp = From->IsConsoleApp;
        To.IsWindowVisible = From->IsWindowVisible;
        To.CtrlRoutine = ConsoleControl;
        To.PropRoutine = To.ImeRoutine = UnsupportedConsoleCallback;
        To.TitleLength = From->TitleLength;
        To.DesktopLength = From->DesktopLength;
        To.Desktop = PTR32(From->Desktop);
        To.AppNameLength = From->AppNameLength;
        To.CurDirLength = From->CurDirLength;
        RtlCopyMemory(To.ConsoleTitle, From->ConsoleTitle, sizeof(To.ConsoleTitle));
        RtlCopyMemory(To.AppName, From->AppName, sizeof(To.AppName));
        RtlCopyMemory(To.CurDir, From->CurDir, sizeof(To.CurDir));
        Status = CsrClientConnectToServer(PTR32(a[0]), a[1], &To, &NativeLength, &InServer);
        if (NT_SUCCESS(Status))
        {
#define H(Field) From->ConsoleStartInfo.Field = HandleToUlong(To.ConsoleStartInfo.Field)
            H(ConsoleHandle); H(InputWaitHandle); H(InputHandle); H(OutputHandle); H(ErrorHandle);
#undef H
            for (i = 0; i < MAX_INIT_EVENTS; ++i)
                From->ConsoleStartInfo.InitEvents[i] = HandleToUlong(To.ConsoleStartInfo.InitEvents[i]);
        }
    }
    else return STATUS_NOT_SUPPORTED;
    if (a[4]) *(BOOLEAN *)PTR32(a[4]) = InServer;
    return Status;
}

static NTSTATUS Call(ULONG *a)
{
    /* PORT_MESSAGE32 is 24 bytes, followed by four ULONG CSR fields. */
    ULONG *Data = (ULONG *)((PUCHAR)PTR32(a[0]) + 40);
    ULONG Api = a[2];
    CONSOLE_API_MESSAGE Message = {0};
    NTSTATUS Status;

    switch (Api)
    {
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepWriteConsole):
    {
        WOW64_CONSOLE_WRITE *From = (PVOID)Data;
        CONSOLE_WRITECONSOLE *To = &Message.Data.WriteConsoleRequest;
        PCSR_CAPTURE_BUFFER Capture = NULL;
        if (a[3] != sizeof(*From)) return STATUS_INFO_LENGTH_MISMATCH;
        To->ConsoleHandle = HANDLE32(From->ConsoleHandle);
        To->OutputHandle = HANDLE32(From->OutputHandle);
        To->Unicode = From->Unicode;
        To->NumBytes = From->NumBytes;
        if (From->NumBytes <= sizeof(To->StaticBuffer))
        {
            To->UsingStaticBuffer = TRUE;
            To->Buffer = To->StaticBuffer;
            RtlCopyMemory(To->Buffer, PTR32(From->Buffer), From->NumBytes);
        }
        else
        {
            Capture = CsrAllocateCaptureBuffer(1, From->NumBytes);
            if (!Capture) return STATUS_NO_MEMORY;
            CsrCaptureMessageBuffer(Capture, PTR32(From->Buffer), From->NumBytes, &To->Buffer);
        }
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, Capture, Api, sizeof(*To));
        if (Capture) CsrFreeCaptureBuffer(Capture);
        From->NumBytes = To->NumBytes;
        return Status;
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetMode):
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetMode):
        if (a[3] != 12) return STATUS_INFO_LENGTH_MISMATCH;
        Message.Data.ConsoleModeRequest.ConsoleHandle = HANDLE32(Data[0]);
        Message.Data.ConsoleModeRequest.Handle = HANDLE32(Data[1]);
        Message.Data.ConsoleModeRequest.Mode = Data[2];
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(Message.Data.ConsoleModeRequest));
        Data[2] = Message.Data.ConsoleModeRequest.Mode;
        return Status;
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepVerifyIoHandle):
        if (a[3] != 12) return STATUS_INFO_LENGTH_MISMATCH;
        Message.Data.VerifyHandleRequest.ConsoleHandle = HANDLE32(Data[1]);
        Message.Data.VerifyHandleRequest.Handle = HANDLE32(Data[2]);
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(Message.Data.VerifyHandleRequest));
        Data[0] = Message.Data.VerifyHandleRequest.IsValid;
        return Status;
    case CSR_CREATE_API_NUMBER(BASESRV_SERVERDLL_INDEX, BasepExitProcess):
    {
        BASE_API_MESSAGE BaseMessage = {0};
        if (a[3] != sizeof(BASE_EXIT_PROCESS)) return STATUS_INFO_LENGTH_MISMATCH;
        BaseMessage.Data.ExitProcessRequest.uExitCode = Data[0];
        return CsrClientCallServer((PCSR_API_MESSAGE)&BaseMessage, NULL, Api, sizeof(BASE_EXIT_PROCESS));
    }
    default:
        DbgPrint("WoW64: unsupported CSR API %lx, size %lu\n", Api, a[3]);
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS Wow64CsrDispatch(ULONG Operation, ULONG *Arguments)
{
    switch (Operation)
    {
    case 0: return Connect(Arguments);
    case 1: return Call(Arguments);
    case 2: return CsrNewThread();
    default: return STATUS_INVALID_SYSTEM_SERVICE;
    }
}
