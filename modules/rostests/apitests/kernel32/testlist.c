
#define STANDALONE
#include <apitest.h>

extern void func_Wow64Process(void);
extern void func_Wow64Run(void);
extern void func_Wow64ReviewFixes(void);

extern void func_ActCtxWithXmlNamespaces(void);
extern void func_BindIoCompletionCallback(void);
extern void func_CancelIoEx(void);
extern void func_ApplicationRestart(void);
extern void func_ConsoleCP(void);
extern void func_CreateEventMutexEx(void);
extern void func_CreateRemoteThreadEx(void);
extern void func_CreateProcess(void);
extern void func_CreateWaitableTimerEx(void);
extern void func_DebugTrapInterrupts(void);
extern void func_DefaultActCtx(void);
extern void func_DeleteTimerQueueEx(void);
extern void func_DeviceIoControl(void);
extern void func_dosdev(void);
extern void func_DynamicTls(void);
extern void func_DynamicTimeZone(void);
extern void func_EnumSystemCodePages(void);
extern void func_ExitThread(void);
extern void func_FindActCtxSectionStringW(void);
extern void func_FindFiles(void);
extern void func_FLS(void);
extern void func_ForcedRelocation(void);
extern void func_FormatMessage(void);
extern void func_GetComputerNameEx(void);
extern void func_GetCPInfo(void);
extern void func_GetCurrentDirectory(void);
extern void func_GetDriveType(void);
extern void func_GetEnvironmentVariable(void);
extern void func_GetFileType(void);
extern void func_GetFinalPathNameByHandle(void);
extern void func_GetLocaleInfo(void);
extern void func_GetLogicalProcessorInformationEx(void);
extern void func_GetModuleFileName(void);
extern void func_GetQueuedCompletionStatusEx(void);
extern void func_GetVolumeInformation(void);
extern void func_HandleList(void);
extern void func_IdleProcessor(void);
extern void func_InitOnce(void);
extern void func_interlck(void);
extern void func_IsDBCSLeadByteEx(void);
extern void func_JapaneseCalendar(void);
extern void func_JobObject(void);
extern void func_LCMapString(void);
extern void func_LoadLibraryErrorMode(void);
extern void func_LoadLibraryExW(void);
extern void func_LoadLibraryMachine(void);
extern void func_LocalDumps(void);
extern void func_CrashDialog(void);
extern void func_LocaleNameToLCID(void);
extern void func_LocalTimeZone(void);
extern void func_lstrcpynW(void);
extern void func_lstrlen(void);
extern void func_MailslotRead(void);
extern void func_MultiByteToWideChar(void);
extern void func_Pipes(void);
extern void func_PipeSqos(void);
extern void func_PowerRequest(void);
extern void func_PrivMoveFileIdentityW(void);
extern void func_ProcessAffinity(void);
extern void func_QueryCycleTime(void);
extern void func_QueryUnbiasedInterruptTime(void);
extern void func_ProcessToken(void);
extern void func_ProcessorGroups(void);
extern void func_QueueUserAPC(void);
extern void func_RaiseFailFastException(void);
extern void func_RegisterWait(void);
extern void func_ResolveLocaleName(void);
extern void func_SetComputerNameExW(void);
extern void func_SetConsoleWindowInfo(void);
extern void func_SetCurrentDirectory(void);
extern void func_SetThreadStackGuarantee(void);
extern void func_SetUnhandledExceptionFilter(void);
extern void func_SuspendThreadContext(void);
extern void func_SetWaitableTimerEx(void);
extern void func_SystemFirmware(void);
extern void func_Toolhelp32(void);
extern void func_TerminateProcess(void);
extern void func_ThreadContextNpx(void);
extern void func_ThreadContextObjects(void);
extern void func_TunnelCache(void);
extern void func_TzSetPrivilege(void);
extern void func_UEFIFirmware(void);
extern void func_WerFlags(void);
extern void func_WerRuntimeExceptionModule(void);
extern void func_WideCharToMultiByte(void);
extern void func_SetFileInformationByHandle(void);

extern void func_Wow64Environment(void);
extern void func_Wow64ProgramFiles(void);
extern void func_Wow64EnvironmentBlock(void);

const struct test winetest_testlist[] =
{
    { "Wow64Environment", func_Wow64Environment },
    { "Wow64ProgramFiles", func_Wow64ProgramFiles },
    { "Wow64EnvironmentBlock", func_Wow64EnvironmentBlock },
    { "Wow64Process", func_Wow64Process },
    { "Wow64Run", func_Wow64Run },
    { "Wow64ReviewFixes", func_Wow64ReviewFixes },
    { "ActCtxWithXmlNamespaces",     func_ActCtxWithXmlNamespaces },
    { "BindIoCompletionCallback",    func_BindIoCompletionCallback },
    { "CancelIoEx",                  func_CancelIoEx },
    { "ApplicationRestart",          func_ApplicationRestart },
    { "ConsoleCP",                   func_ConsoleCP },
    { "CreateEventMutexEx",          func_CreateEventMutexEx },
    { "CreateRemoteThreadEx",        func_CreateRemoteThreadEx },
    { "CreateProcess",               func_CreateProcess },
    { "CreateWaitableTimerEx",       func_CreateWaitableTimerEx },
    { "DebugTrapInterrupts",         func_DebugTrapInterrupts },
    { "DefaultActCtx",               func_DefaultActCtx },
    { "DeleteTimerQueueEx",          func_DeleteTimerQueueEx },
    { "DeviceIoControl",             func_DeviceIoControl },
    { "dosdev",                      func_dosdev },
    { "DynamicTls",                  func_DynamicTls },
    { "DynamicTimeZone",             func_DynamicTimeZone },
    { "EnumSystemCodePages",         func_EnumSystemCodePages },
    { "ExitThread",                  func_ExitThread },
    { "FindActCtxSectionStringW",    func_FindActCtxSectionStringW },
    { "FindFiles",                   func_FindFiles },
    { "FLS",                         func_FLS },
    { "ForcedRelocation",            func_ForcedRelocation },
    { "FormatMessage",               func_FormatMessage },
    { "GetComputerNameEx",           func_GetComputerNameEx },
    { "GetCPInfo",                   func_GetCPInfo },
    { "GetCurrentDirectory",         func_GetCurrentDirectory },
    { "GetDriveType",                func_GetDriveType },
    { "GetEnvironmentVariable",      func_GetEnvironmentVariable },
    { "GetFileType",                 func_GetFileType },
    { "GetFinalPathNameByHandle",    func_GetFinalPathNameByHandle },
    { "GetLocaleInfo",               func_GetLocaleInfo },
    { "GetLogicalProcessorInformationEx", func_GetLogicalProcessorInformationEx },
    { "GetModuleFileName",           func_GetModuleFileName },
    { "GetQueuedCompletionStatusEx", func_GetQueuedCompletionStatusEx },
    { "GetVolumeInformation",        func_GetVolumeInformation },
    { "HandleList",                  func_HandleList },
    { "IdleProcessor",               func_IdleProcessor },
    { "InitOnce",                    func_InitOnce },
    { "interlck",                    func_interlck },
    { "IsDBCSLeadByteEx",            func_IsDBCSLeadByteEx },
    { "JapaneseCalendar",            func_JapaneseCalendar },
    { "JobObject",                   func_JobObject },
    { "LCMapString",                 func_LCMapString },
    { "LoadLibraryErrorMode",        func_LoadLibraryErrorMode },
    { "LoadLibraryExW",              func_LoadLibraryExW },
    { "LoadLibraryMachine",          func_LoadLibraryMachine },
    { "LocalDumps",                  func_LocalDumps },
    { "CrashDialog",                 func_CrashDialog },
    { "LocaleNameToLCID",            func_LocaleNameToLCID },
    { "LocalTimeZone",               func_LocalTimeZone },
    { "lstrcpynW",                   func_lstrcpynW },
    { "lstrlen",                     func_lstrlen },
    { "MailslotRead",                func_MailslotRead },
    { "MultiByteToWideChar",         func_MultiByteToWideChar },
    { "Pipes",                       func_Pipes },
    { "PipeSqos",                    func_PipeSqos },
    { "PowerRequest",                func_PowerRequest },
    { "PrivMoveFileIdentityW",       func_PrivMoveFileIdentityW },
    { "ProcessAffinity",             func_ProcessAffinity },
    { "QueryCycleTime",              func_QueryCycleTime },
    { "QueryUnbiasedInterruptTime",  func_QueryUnbiasedInterruptTime },
    { "ProcessToken",                func_ProcessToken },
    { "ProcessorGroups",             func_ProcessorGroups },
    { "QueueUserAPC",                func_QueueUserAPC },
    { "RaiseFailFastException",      func_RaiseFailFastException },
    { "RegisterWait",                func_RegisterWait },
    { "ResolveLocaleName",           func_ResolveLocaleName },
    { "SetComputerNameExW",          func_SetComputerNameExW },
    { "SetConsoleWindowInfo",        func_SetConsoleWindowInfo },
    { "SetCurrentDirectory",         func_SetCurrentDirectory },
    { "SetThreadStackGuarantee",     func_SetThreadStackGuarantee },
    { "SetUnhandledExceptionFilter", func_SetUnhandledExceptionFilter },
    { "SuspendThreadContext",        func_SuspendThreadContext },
    { "SetWaitableTimerEx",          func_SetWaitableTimerEx },
    { "SystemFirmware",              func_SystemFirmware },
    { "Toolhelp32",                  func_Toolhelp32 },
    { "TerminateProcess",            func_TerminateProcess },
    { "ThreadContextNpx",            func_ThreadContextNpx },
    { "ThreadContextObjects",        func_ThreadContextObjects },
    { "TunnelCache",                 func_TunnelCache },
    { "TzSetPrivilege",              func_TzSetPrivilege },
    { "UEFIFirmware",                func_UEFIFirmware },
    { "WerFlags",                    func_WerFlags },
    { "WerRuntimeExceptionModule",   func_WerRuntimeExceptionModule },
    { "WideCharToMultiByte",         func_WideCharToMultiByte },
    { "SetFileInformationByHandle",  func_SetFileInformationByHandle },
    { 0, 0 }
};
