
list(APPEND SOURCE
    devclass.c
    RootDeviceRemove.c
    SetupDiInstallClassExA.c
    SetupDiGetActualModelsSection.c
    SetupInstallServicesFromInfSectionEx.c
    testlist.c)

add_executable(setupapi_apitest ${SOURCE})
set_module_type(setupapi_apitest win32cui)
add_importlibs(setupapi_apitest advapi32 setupapi shlwapi user32 msvcrt kernel32)
add_rostests_file(TARGET setupapi_apitest)
