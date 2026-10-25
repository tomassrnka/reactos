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
#include <win/winmsg.h>
#include "csr32.h"

/* API names for the diagnostics, in the order of the server API enumerations */
static const char *BaseApiNames[] =
{
    "BasepCreateProcess", "BasepCreateThread", "BasepGetTempFile", "BasepExitProcess", "BasepDebugProcess",
    "BasepCheckVDM", "BasepUpdateVDMEntry", "BasepGetNextVDMCommand", "BasepExitVDM", "BasepIsFirstVDM",
    "BasepGetVDMExitCode", "BasepSetReenterCount", "BasepSetProcessShutdownParam",
    "BasepGetProcessShutdownParam", "BasepNlsSetUserInfo", "BasepNlsSetMultipleUserInfo",
    "BasepNlsCreateSection", "BasepSetVDMCurDirs", "BasepGetVDMCurDirs", "BasepBatNotification",
    "BasepRegisterWowExec", "BasepSoundSentryNotification", "BasepRefreshIniFileMapping",
    "BasepDefineDosDevice", "BasepSetTermsrvAppInstallMode", "BasepNlsUpdateCacheCount",
    "BasepSetTermsrvClientTimeZone", "BasepSxsCreateActivationContext", "BasepDebugProcessStop",
    "BasepRegisterThread", "BasepNlsGetUserInfo",
};
static const char *ConsoleApiNames[] =
{
    "ConsolepOpenConsole", "ConsolepGetConsoleInput", "ConsolepWriteConsoleInput",
    "ConsolepReadConsoleOutput", "ConsolepWriteConsoleOutput", "ConsolepReadConsoleOutputString",
    "ConsolepWriteConsoleOutputString", "ConsolepFillConsoleOutput", "ConsolepGetMode",
    "ConsolepGetNumberOfFonts", "ConsolepGetNumberOfInputEvents", "ConsolepGetScreenBufferInfo",
    "ConsolepGetCursorInfo", "ConsolepGetMouseInfo", "ConsolepGetFontInfo", "ConsolepGetFontSize",
    "ConsolepGetCurrentFont", "ConsolepSetMode", "ConsolepSetActiveScreenBuffer", "ConsolepFlushInputBuffer",
    "ConsolepGetLargestWindowSize", "ConsolepSetScreenBufferSize", "ConsolepSetCursorPosition",
    "ConsolepSetCursorInfo", "ConsolepSetWindowInfo", "ConsolepScrollScreenBuffer",
    "ConsolepSetTextAttribute", "ConsolepSetFont", "ConsolepSetIcon", "ConsolepReadConsole",
    "ConsolepWriteConsole", "ConsolepDuplicateHandle", "ConsolepGetHandleInformation",
    "ConsolepSetHandleInformation", "ConsolepCloseHandle", "ConsolepVerifyIoHandle", "ConsolepAlloc",
    "ConsolepFree", "ConsolepGetTitle", "ConsolepSetTitle", "ConsolepCreateScreenBuffer",
    "ConsolepInvalidateBitMapRect", "ConsolepVDMOperation", "ConsolepSetCursor", "ConsolepShowCursor",
    "ConsolepMenuControl", "ConsolepSetPalette", "ConsolepSetDisplayMode", "ConsolepRegisterVDM",
    "ConsolepGetHardwareState", "ConsolepSetHardwareState", "ConsolepGetDisplayMode", "ConsolepAddAlias",
    "ConsolepGetAlias", "ConsolepGetAliasesLength", "ConsolepGetAliasExesLength", "ConsolepGetAliases",
    "ConsolepGetAliasExes", "ConsolepExpungeCommandHistory", "ConsolepSetNumberOfCommands",
    "ConsolepGetCommandHistoryLength", "ConsolepGetCommandHistory", "ConsolepSetCommandHistoryMode",
    "ConsolepGetCP", "ConsolepSetCP", "ConsolepSetKeyShortcuts", "ConsolepSetMenuClose",
    "ConsolepNotifyLastClose", "ConsolepGenerateCtrlEvent", "ConsolepGetKeyboardLayoutName",
    "ConsolepGetConsoleWindow", "ConsolepCharType", "ConsolepSetLocalEUDC", "ConsolepSetCursorMode",
    "ConsolepGetCursorMode", "ConsolepRegisterOS2", "ConsolepSetOS2OemFormat", "ConsolepGetNlsMode",
    "ConsolepSetNlsMode", "ConsolepRegisterConsoleIME", "ConsolepUnregisterConsoleIME", "ConsolepGetLangId",
    "ConsolepAttach", "ConsolepGetSelectionInfo", "ConsolepGetProcessList", "ConsolepGetHistory",
    "ConsolepSetHistory",
};
static const char *UserApiNames[] =
{
    "UserpExitWindowsEx", "UserpEndTask", "UserpLogon", "UserpRegisterServicesProcess",
    "UserpActivateDebugger", "UserpGetThreadConsoleDesktop", "UserpDeviceEvent", "UserpRegisterLogonProcess",
    "UserpCreateSystemThreads", "UserpRecordShutdownReason",
};
C_ASSERT(RTL_NUMBER_OF(BaseApiNames) == BasepMaxApiNumber - BASESRV_FIRST_API_NUMBER);
C_ASSERT(RTL_NUMBER_OF(ConsoleApiNames) == ConsolepMaxApiNumber - CONSRV_FIRST_API_NUMBER);
C_ASSERT(RTL_NUMBER_OF(UserApiNames) == UserpMaxApiNumber - USERSRV_FIRST_API_NUMBER);

static NTSTATUS UnsupportedApi(ULONG Api, ULONG Size, BOOLEAN *Reported)
{
    ULONG Server = CSR_API_NUMBER_TO_SERVER_ID(Api), Number = CSR_API_NUMBER_TO_API_ID(Api);
    const char *ServerName = "server", *Name = "unknown";
#define NAME(Index, Table, First) \
    if (Server == Index) { ServerName = #Index; \
        if (Number - First < RTL_NUMBER_OF(Table)) Name = Table[Number - First]; }
    NAME(BASESRV_SERVERDLL_INDEX, BaseApiNames, BASESRV_FIRST_API_NUMBER)
    NAME(CONSRV_SERVERDLL_INDEX, ConsoleApiNames, CONSRV_FIRST_API_NUMBER)
    NAME(USERSRV_SERVERDLL_INDEX, UserApiNames, USERSRV_FIRST_API_NUMBER)
#undef NAME
    if (Wow64FirstReport(WOW64_SERVICE_CSR, WOW64_UNSUPPORTED_CSR_API, Api))
        DbgPrint("WoW64: unsupported CSR API %lx (%s %s), size %lu, pid %lx\n", Api, ServerName, Name, Size,
                 HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess));
    *Reported = TRUE;
    return STATUS_NOT_SUPPORTED;
}

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
    /* This API returns the logical System32 path. Filesystem redirection
     * selects SysWOW64; disabling it must let installers start native helpers
     * using the very same path returned by GetSystemDirectory. */
    STR(WindowsSystemDirectory);
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
    To->SysInfo.MaximumUserModeAddress = Wow64HighestUserAddress;
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

/* Guest CONSOLE_START_INFO to native, shared by the console connect and ConsolepAttach. The
 * handles are widened; the property tail contains only fixed-width fields. InitEvents are
 * outputs and stay zero. */
static VOID ConsoleStartToNative(CONSOLE_START_INFO *To, const WOW64_CONSOLE_START *From)
{
#define H(Field) To->Field = HANDLE32(From->Field)
    H(ConsoleHandle); H(InputWaitHandle); H(InputHandle); H(OutputHandle); H(ErrorHandle);
    H(hIcon); H(hIconSm);
#undef H
    To->IconIndex = From->IconIndex;
    RtlCopyMemory((PUCHAR)To + FIELD_OFFSET(CONSOLE_START_INFO, dwHotKey),
                  (PUCHAR)From + FIELD_OFFSET(WOW64_CONSOLE_START, dwHotKey),
                  sizeof(WOW64_CONSOLE_START) - FIELD_OFFSET(WOW64_CONSOLE_START, dwHotKey));
}

/* The handles the server returns, back into the guest structure */
static VOID ConsoleStartToGuest(WOW64_CONSOLE_START *To, const CONSOLE_START_INFO *From)
{
    ULONG i;
#define H(Field) To->Field = HandleToUlong(From->Field)
    H(ConsoleHandle); H(InputWaitHandle); H(InputHandle); H(OutputHandle); H(ErrorHandle);
#undef H
    for (i = 0; i < MAX_INIT_EVENTS; ++i)
        To->InitEvents[i] = HandleToUlong(From->InitEvents[i]);
}

static NTSTATUS Connect(ULONG *a, BOOLEAN *Reported)
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
        ULONG NativeLength = sizeof(To);
        if (*Length != sizeof(*From)) return STATUS_INFO_LENGTH_MISMATCH;
        ConsoleStartToNative(&To.ConsoleStartInfo, &From->ConsoleStartInfo);
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
            ConsoleStartToGuest(&From->ConsoleStartInfo, &To.ConsoleStartInfo);
        }
    }
    else return Wow64Unsupported(WOW64_SERVICE_CSR, WOW64_UNSUPPORTED_CSR_SERVER, a[1], Reported);
    if (a[4]) *(BOOLEAN *)PTR32(a[4]) = InServer;
    return Status;
}

static NTSTATUS Call(ULONG *a, BOOLEAN *Reported)
{
    /* PORT_MESSAGE32 is 24 bytes, followed by four ULONG CSR fields. */
    ULONG *Data = (ULONG *)((PUCHAR)PTR32(a[0]) + 40);
    ULONG Api = a[2];
    CONSOLE_API_MESSAGE Message = {0};
    NTSTATUS Status;

    /* The message and its request data (a[3] bytes) must lie in the guest's range: replies
     * are written there. A fault inside the range is caught by the caller's SEH. */
    if ((ULONG64)a[0] + 40 + a[3] > (ULONG64)Wow64HighestUserAddress + 1) return STATUS_ACCESS_VIOLATION;
    /* The guest library stores the status in the header after the call: the whole message must
     * be writable before a request changes server state */
    {
        volatile UCHAR *Bytes = PTR32(a[0]);
        ULONG i;
        for (i = 0; i < 40 + a[3]; ++i) Bytes[i] = Bytes[i];
    }
    switch (Api)
    {
    case CSR_CREATE_API_NUMBER(USERSRV_SERVERDLL_INDEX, UserpGetThreadConsoleDesktop):
    {
        USER_API_MESSAGE UserMessage = {0};
        if (a[3] != 2 * sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
        UserMessage.Data.GetThreadConsoleDesktopRequest.ThreadId = Data[0];
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&UserMessage, NULL, Api,
                                    sizeof(USER_GET_THREAD_CONSOLE_DESKTOP));
        if (NT_SUCCESS(Status))
            Data[1] = HandleToUlong(UserMessage.Data.GetThreadConsoleDesktopRequest.ConsoleDesktop);
        return Status;
    }
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
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetScreenBufferInfo):
    {
        WOW64_CONSOLE_SCREEN_BUFFER_INFO *Guest = (PVOID)Data;
        CONSOLE_GETSCREENBUFFERINFO *Info = &Message.Data.ScreenBufferInfoRequest;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        Info->ConsoleHandle = HANDLE32(Guest->ConsoleHandle);
        Info->OutputHandle = HANDLE32(Guest->OutputHandle);
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(*Info));
        if (NT_SUCCESS(Status))
        {
            Guest->ScreenBufferSize = Info->ScreenBufferSize;
            Guest->CursorPosition = Info->CursorPosition;
            Guest->ViewOrigin = Info->ViewOrigin;
            Guest->Attributes = Info->Attributes;
            Guest->ViewSize = Info->ViewSize;
            Guest->MaximumViewSize = Info->MaximumViewSize;
        }
        return Status;
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetCP):
    {
        WOW64_CONSOLE_GET_CP *Guest = (PVOID)Data;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        Message.Data.GetConsoleCPRequest.ConsoleHandle = HANDLE32(Guest->ConsoleHandle);
        Message.Data.GetConsoleCPRequest.OutputCP = Guest->OutputCP;
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(Message.Data.GetConsoleCPRequest));
        if (NT_SUCCESS(Status)) Guest->CodePage = Message.Data.GetConsoleCPRequest.CodePage;
        return Status;
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepSetCP):
    {
        WOW64_CONSOLE_SET_CP *Guest = (PVOID)Data;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        Message.Data.SetConsoleCPRequest.ConsoleHandle = HANDLE32(Guest->ConsoleHandle);
        Message.Data.SetConsoleCPRequest.CodePage = Guest->CodePage;
        Message.Data.SetConsoleCPRequest.OutputCP = Guest->OutputCP;
        Message.Data.SetConsoleCPRequest.EventHandle = HANDLE32(Guest->EventHandle);
        return CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(Message.Data.SetConsoleCPRequest));
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepGetLangId):
    {
        WOW64_CONSOLE_LANGID *Guest = (PVOID)Data;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        Message.Data.LangIdRequest.ConsoleHandle = HANDLE32(Guest->ConsoleHandle);
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(Message.Data.LangIdRequest));
        if (NT_SUCCESS(Status)) Guest->LangId = Message.Data.LangIdRequest.LangId;
        return Status;
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepVerifyIoHandle):
        if (a[3] != 12) return STATUS_INFO_LENGTH_MISMATCH;
        Message.Data.VerifyHandleRequest.ConsoleHandle = HANDLE32(Data[1]);
        Message.Data.VerifyHandleRequest.Handle = HANDLE32(Data[2]);
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(Message.Data.VerifyHandleRequest));
        Data[0] = Message.Data.VerifyHandleRequest.IsValid;
        return Status;
    case CSR_CREATE_API_NUMBER(BASESRV_SERVERDLL_INDEX, BasepCreateThread):
    {
        BASE_API_MESSAGE BaseMessage = {0};
        if (a[3] != 3 * sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
        BaseMessage.Data.CreateThreadRequest.ThreadHandle = HANDLE32(Data[0]);
        BaseMessage.Data.CreateThreadRequest.ClientId.UniqueProcess = HANDLE32(Data[1]);
        BaseMessage.Data.CreateThreadRequest.ClientId.UniqueThread = HANDLE32(Data[2]);
        return CsrClientCallServer((PCSR_API_MESSAGE)&BaseMessage, NULL, Api,
                                  sizeof(BASE_CREATE_THREAD));
    }
    case CSR_CREATE_API_NUMBER(BASESRV_SERVERDLL_INDEX, BasepGetTempFile):
    {
        BASE_API_MESSAGE BaseMessage = {0};
        if (a[3] != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&BaseMessage, NULL, Api,
                                     sizeof(BASE_GET_TEMP_FILE));
        Data[0] = BaseMessage.Data.GetTempFileRequest.UniqueID;
        return Status;
    }
    case CSR_CREATE_API_NUMBER(BASESRV_SERVERDLL_INDEX, BasepSetProcessShutdownParam):
    case CSR_CREATE_API_NUMBER(BASESRV_SERVERDLL_INDEX, BasepGetProcessShutdownParam):
    {
        /* Two ULONGs, the same for both sizes */
        BASE_API_MESSAGE BaseMessage = {0};
        BASE_GETSET_PROCESS_SHUTDOWN_PARAMS *Guest = (PVOID)Data;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        BaseMessage.Data.ShutdownParametersRequest = *Guest;
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&BaseMessage, NULL, Api, sizeof(*Guest));
        if (NT_SUCCESS(Status) && Api == CSR_CREATE_API_NUMBER(BASESRV_SERVERDLL_INDEX, BasepGetProcessShutdownParam))
            *Guest = BaseMessage.Data.ShutdownParametersRequest;
        return Status;
    }
    case CSR_CREATE_API_NUMBER(BASESRV_SERVERDLL_INDEX, BasepExitProcess):
    {
        BASE_API_MESSAGE BaseMessage = {0};
        if (a[3] != sizeof(BASE_EXIT_PROCESS)) return STATUS_INFO_LENGTH_MISMATCH;
        BaseMessage.Data.ExitProcessRequest.uExitCode = Data[0];
        return CsrClientCallServer((PCSR_API_MESSAGE)&BaseMessage, NULL, Api, sizeof(BASE_EXIT_PROCESS));
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepOpenConsole):
    {
        WOW64_CONSOLE_OPENCONSOLE *Guest = (PVOID)Data;
        CONSOLE_OPENCONSOLE *Open = &Message.Data.OpenConsoleRequest;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        Open->ConsoleHandle = HANDLE32(Guest->ConsoleHandle);
        Open->HandleType = Guest->HandleType;
        Open->DesiredAccess = Guest->DesiredAccess;
        Open->InheritHandle = Guest->InheritHandle;
        Open->ShareMode = Guest->ShareMode;
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(*Open));
        if (NT_SUCCESS(Status)) Guest->Handle = HandleToUlong(Open->Handle);
        return Status;
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepCreateScreenBuffer):
    {
        WOW64_CONSOLE_CREATESCREENBUFFER *Guest = (PVOID)Data;
        CONSOLE_CREATESCREENBUFFER *Create = &Message.Data.CreateScreenBufferRequest;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        /* A graphics screen buffer carries a BITMAPINFO pointer and a shared
         * section mapping; only text-mode buffers are a pure fixed-width request. */
        if (Guest->ScreenBufferType != CONSOLE_TEXTMODE_BUFFER)
            return UnsupportedApi(Api, a[3], Reported);
        Create->ConsoleHandle = HANDLE32(Guest->ConsoleHandle);
        Create->DesiredAccess = Guest->DesiredAccess;
        Create->InheritHandle = Guest->InheritHandle;
        Create->ShareMode = Guest->ShareMode;
        Create->ScreenBufferType = Guest->ScreenBufferType;
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, NULL, Api, sizeof(*Create));
        if (NT_SUCCESS(Status)) Guest->OutputHandle = HandleToUlong(Create->OutputHandle);
        return Status;
    }
    case CSR_CREATE_API_NUMBER(CONSRV_SERVERDLL_INDEX, ConsolepAttach):
    {
        WOW64_CONSOLE_ATTACH *Guest = (PVOID)Data;
        CONSOLE_ATTACHCONSOLE *Attach = &Message.Data.AttachConsoleRequest;
        WOW64_CONSOLE_START *GuestStart;
        CONSOLE_START_INFO NativeStart = {0};
        PCSR_CAPTURE_BUFFER Capture;
        if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
        /* The guest start info lives in the guest's capture buffer; the reply is written there. */
        if (!Guest->ConsoleStartInfo ||
            (ULONG64)Guest->ConsoleStartInfo + sizeof(*GuestStart) > (ULONG64)Wow64HighestUserAddress + 1)
            return STATUS_INVALID_PARAMETER;
        GuestStart = PTR32(Guest->ConsoleStartInfo);
        {
            volatile UCHAR *Bytes = (volatile UCHAR *)GuestStart;
            ULONG i;
            for (i = 0; i < sizeof(*GuestStart); ++i) Bytes[i] = Bytes[i];
        }
        ConsoleStartToNative(&NativeStart, GuestStart);
        Capture = CsrAllocateCaptureBuffer(1, sizeof(NativeStart));
        if (!Capture) return STATUS_NO_MEMORY;
        Attach->ProcessId = Guest->ProcessId; /* ATTACH_PARENT_PROCESS (-1) stays -1 */
        Attach->CtrlRoutine = ConsoleControl;
        Attach->PropRoutine = UnsupportedConsoleCallback;
        CsrCaptureMessageBuffer(Capture, &NativeStart, sizeof(NativeStart), (PVOID *)&Attach->ConsoleStartInfo);
        Status = CsrClientCallServer((PCSR_API_MESSAGE)&Message, Capture, Api, sizeof(*Attach));
        /* The server filled the captured copy (handles and init events) */
        if (NT_SUCCESS(Status)) ConsoleStartToGuest(GuestStart, Attach->ConsoleStartInfo);
        CsrFreeCaptureBuffer(Capture);
        return Status;
    }
    default:
        return UnsupportedApi(Api, a[3], Reported);
    }
}

NTSTATUS Wow64CsrDispatch(ULONG Operation, ULONG *Arguments, BOOLEAN *Reported)
{
    switch (Operation)
    {
    case 0: return Connect(Arguments, Reported);
    case 1: return Call(Arguments, Reported);
    case 2: return CsrNewThread();
    default: return Wow64Unsupported(WOW64_SERVICE_CSR, WOW64_UNSUPPORTED_OPERATION, Operation, Reported);
    }
}
