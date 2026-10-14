
add_subdirectory(redirptest)

add_message_headers(ANSI FormatMessage.mc)

list(APPEND SOURCE
    Wow64ReviewThreads.c
    Wow64RemoteThread.c
    Wow64ThreadApc.c
    Wow64Threads.c
    Wow64Environment.c
    BindIoCompletionCallback.c
    CancelIoEx.c
    ConsoleCP.c
    CreateEventMutexEx.c
    CreateRemoteThreadEx.c
    CreateProcess.c
    CreateWaitableTimerEx.c
    DebugTrapInterrupts.c
    DefaultActCtx.c
    DeleteTimerQueueEx.c
    DeviceIoControl.c
    dosdev.c
    EnumSystemCodePages.c
    ExitThread.c
    FindActCtxSectionStringW.c
    FindFiles.c
    FLS.c
    FormatMessage.c
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
    IdleProcessor.c
    InitOnce.c
    interlck.c
    IsDBCSLeadByteEx.c
    JapaneseCalendar.c
    JobObject.c
    LCMapString.c
    LoadLibraryErrorMode.c
    LoadLibraryExW.c
    LoadLibraryMachine.c
    LocalDumps.c
    LocaleNameToLCID.c
    LocalTimeZone.c
    lstrcpynW.c
    lstrlen.c
    Mailslot.c
    MultiByteToWideChar.c
    Pipes.c
    PipeSqos.c
    PrivMoveFileIdentityW.c
    ProcessAffinity.c
    QueryCycleTime.c
    QueueUserAPC.c
    RaiseFailFastException.c
    ReadFile.c
    RegisterWait.c
    SetComputerNameExW.c
    SetConsoleWindowInfo.c
    SetCurrentDirectory.c
    SetSearchPathMode.c
    SetThreadStackGuarantee.c
    SetUnhandledExceptionFilter.c
    SuspendThreadContext.c
    SystemFirmware.c
    TerminateProcess.c
    ThreadContextNpx.c
    ThreadContextObjects.c
    TunnelCache.c
    TzSetPrivilege.c
    UEFIFirmware.c
    WerFlags.c
    WideCharToMultiByte.c
    Wow64Process.c
    Wow64Run.c
    WriteFile.c
)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(kernel32_apitest
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    kernel32_apitest.rc)

target_link_libraries(kernel32_apitest wine ${PSEH_LIB})
set_module_type(kernel32_apitest win32cui)
add_delay_importlibs(kernel32_apitest advapi32 shlwapi)
add_importlibs(kernel32_apitest netapi32 msvcrt kernel32 ntdll)
add_dependencies(kernel32_apitest FormatMessage)
add_pch(kernel32_apitest precomp.h "${PCH_SKIP_SOURCE}")
add_rostests_file(TARGET kernel32_apitest)

add_executable(wow64_native wow64_native.c)
set_module_type(wow64_native win32cui ENTRYPOINT mainCRTStartup)
add_importlibs(wow64_native kernel32)
add_rostests_file(TARGET wow64_native)

if(ARCH STREQUAL "i386")
    add_executable(wow64_hello wow64_hello.c)
    set_module_type(wow64_hello win32cui ENTRYPOINT mainCRTStartup)
    add_importlibs(wow64_hello kernel32 ntdll)
    add_rostests_file(TARGET wow64_hello)

    add_executable(wow64_errors wow64_errors.c)
    set_module_type(wow64_errors win32cui ENTRYPOINT mainCRTStartup)
    add_importlibs(wow64_errors kernel32 ntdll)
    add_rostests_file(TARGET wow64_errors)

    spec2def(wow64_local.dll wow64_local.spec ADD_IMPORTLIB)
    add_library(wow64_local MODULE wow64_local.c ${CMAKE_CURRENT_BINARY_DIR}/wow64_local.def)
    set_module_type(wow64_local win32dll ENTRYPOINT 0)
    add_rostests_file(TARGET wow64_local)
    add_executable(wow64_localhello wow64_localhello.c)
    set_module_type(wow64_localhello win32cui ENTRYPOINT mainCRTStartup)
    add_importlibs(wow64_localhello wow64_local kernel32)
    add_rostests_file(TARGET wow64_localhello)
endif()

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

spec2def(wow64_thread.dll wow64_thread.spec ADD_IMPORTLIB)
add_library(wow64_thread MODULE wow64_thread.c ${CMAKE_CURRENT_BINARY_DIR}/wow64_thread.def)
set_module_type(wow64_thread win32dll ENTRYPOINT DllMain 12)
add_importlibs(wow64_thread kernel32)
add_rostests_file(TARGET wow64_thread)
