
list(APPEND SOURCE
    UiaDisconnectAllProviders.c)

add_executable(uiautomationcore_apitest
    ${SOURCE}
    testlist.c)

set_module_type(uiautomationcore_apitest win32cui)
add_importlibs(uiautomationcore_apitest msvcrt kernel32)
add_rostests_file(TARGET uiautomationcore_apitest)
