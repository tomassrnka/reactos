
list(APPEND SOURCE
    UiaDisconnectAllProviders.c
    testlist.c)

add_executable(uiautomationcore_apitest ${SOURCE})
set_module_type(uiautomationcore_apitest win32cui)
target_link_libraries(uiautomationcore_apitest uuid)
add_importlibs(uiautomationcore_apitest ole32 oleaut32 user32 msvcrt kernel32)
add_rostests_file(TARGET uiautomationcore_apitest)
