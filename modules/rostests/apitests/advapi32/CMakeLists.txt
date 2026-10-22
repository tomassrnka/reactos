
list(APPEND SOURCE
    AddMandatoryAce.c
    ConvertStringSidToSid.c
    CreateService.c
    CloseTrace.c
    DuplicateTokenEx.c
    EncryptFile.c
    eventlog.c
    EventRegister.c
    EventProvider.c
    Hash.c
    HKEY_CLASSES_ROOT.c
    IsTextUnicode.c
    LockServiceDatabase.c
    LogonUser.c
    QueryServiceConfig2.c
    RegConcurrentWrite.c
    RegCreateKeyEx.c
    RegCreateKeyTransacted.c
    RegEnumKey.c
    RegEnumKeyClass.c
    RegEnumValueW.c
    RegFlushKey.c
    RegLoadKey.c
    RegOpenKeyExW.c
    RegQueryInfoKey.c
    RegQueryValueExW.c
    RegGetValue.c
    RtlEncryptMemory.c
    SaferIdentifyLevel.c
    TokenIntegrity.c
    TokenLogonSid.c
    ServiceArgs.c
    ServiceEnv.c
    ServiceFailureActions.c
    ServiceNetwork.c
    SetEntriesInAclOld.c
    SetNamedSecurityInfoInherit.c
    SddlRights.c
    ServiceProcess.c
    svchlp.c)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(advapi32_apitest
    ${SOURCE}
    ${PCH_SKIP_SOURCE})

target_link_libraries(advapi32_apitest wine ${PSEH_LIB})
set_module_type(advapi32_apitest win32cui)
add_importlibs(advapi32_apitest advapi32 iphlpapi ws2_32 netapi32 msvcrt kernel32 ntdll)
add_pch(advapi32_apitest precomp.h "${PCH_SKIP_SOURCE}")
add_rostests_file(TARGET advapi32_apitest)
