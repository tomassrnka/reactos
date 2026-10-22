
list(APPEND SOURCE
    AddHardwareWizard.c
    testlist.c)

add_executable(hdwwiz_apitest ${SOURCE})
target_link_libraries(hdwwiz_apitest wine)
set_module_type(hdwwiz_apitest win32cui)
add_importlibs(hdwwiz_apitest user32 msvcrt kernel32)
add_rostests_file(TARGET hdwwiz_apitest)
