
add_subdirectory(redirptest)
add_subdirectory(tlsdyn)

add_message_headers(ANSI FormatMessage.mc)

list(APPEND SOURCE
    BindIoCompletionCallback.c
    CancelIoEx.c
    ApplicationRestart.c
    ConsoleCP.c
    CopyFile2.c
    CopyFile2Identity.c
    CreateEventMutexEx.c
    CreateRemoteThreadEx.c
    CreateProcess.c
    CreateProcessRace.c
    CreateWaitableTimerEx.c
    ThreadImpersonationToken.c
    DebugTrapInterrupts.c
    DefaultActCtx.c
    DeleteTimerQueueEx.c
    DeviceIoControl.c
    dosdev.c
    DynamicTls.c
    DynamicTimeZone.c
    GetTimeZoneInformationForYear.c
    EnumSystemCodePages.c
    EnumUILanguages.c
    ExitThread.c
    FindActCtxSectionStringW.c
    FindFiles.c
    FLS.c
    ForcedRelocation.c
    FormatMessage.c
    FormatMessageInsert.c
    GetBinaryType.c
    GetComputerNameEx.c
    GetCPInfo.c
    GetCurrentDirectory.c
    GetDriveType.c
    GetEnvironmentVariable.c
    GetFileType.c
    GetFinalPathNameByHandle.c
    GetLocaleInfo.c
    GetLogicalProcessorInformationEx.c
    GetModuleFileName.c
    GetQueuedCompletionStatusEx.c
    GetVolumeInformation.c
    HandleList.c
    IdleProcessor.c
    InitOnce.c
    KernelBaseTime.c
    interlck.c
    IsDBCSLeadByteEx.c
    JapaneseCalendar.c
    JobObject.c
    LCMapString.c
    LoadLibraryErrorMode.c
    LoadLibraryExW.c
    LoadLibraryMachine.c
    LocalDumps.c
    CrashDialog.c
    LocaleNameToLCID.c
    LocalTimeZone.c
    lstrcpynW.c
    lstrlen.c
    Mailslot.c
    MappedFileGrowth.c
    MultiByteToWideChar.c
    PerformanceCounter.c
    Pipes.c
    PipeSqos.c
    PrivMoveFileIdentityW.c
    QueryCycleTime.c
    QueueUserAPC.c
    RaiseFailFastException.c
    ReadFile.c
    RegisterWait.c
    ResolveLocaleName.c
    SetComputerNameExW.c
    SetConsoleWindowInfo.c
    SetCurrentDirectory.c
    SetSearchPathMode.c
    SetThreadStackGuarantee.c
    SetUnhandledExceptionFilter.c
    SuspendThreadContext.c
    SystemFirmware.c
    Toolhelp32.c
    TerminateProcess.c
    ThreadContextNpx.c
    ThreadContextObjects.c
    TunnelCache.c
    VolumeMountPoint.c
    TzSetPrivilege.c
    UEFIFirmware.c
    WerFlags.c
    WerRuntimeExceptionModule.c
    WideCharToMultiByte.c
    WriteFile.c
    SetFileInformationByHandle.c
)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(kernel32_apitest
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    kernel32_apitest.rc)

target_link_libraries(kernel32_apitest wine ${PSEH_LIB})
set_module_type(kernel32_apitest win32cui)
add_delay_importlibs(kernel32_apitest advapi32 shlwapi user32)
add_importlibs(kernel32_apitest netapi32 msvcrt kernel32 ntdll)
add_dependencies(kernel32_apitest FormatMessage)
set_source_files_properties(CopyFile2Identity.c PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/copy2_embedded")
add_pch(kernel32_apitest precomp.h "${PCH_SKIP_SOURCE}")
add_rostests_file(TARGET kernel32_apitest)

list(APPEND MANIFEST_FILES
    classtest.manifest
    classtest2.manifest
    comctl32dep.manifest
    dep1.manifest
    deptest.manifest
    xmlns.manifest)

foreach(item ${MANIFEST_FILES})
    add_rostests_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/${item}")
endforeach(item)

add_executable(handlelist_gui handlelist_gui.c)
set_module_type(handlelist_gui win32gui ENTRYPOINT WinMainCRTStartup)
add_importlibs(handlelist_gui kernel32)
add_dependencies(kernel32_apitest handlelist_gui)
add_rostests_file(TARGET handlelist_gui)
