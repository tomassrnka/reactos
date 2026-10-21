
list(APPEND SOURCE
    PendingIrp.c)

add_executable(kbdmouclass_apitest ${SOURCE})
set_module_type(kbdmouclass_apitest win32cui)
target_link_libraries(kbdmouclass_apitest winetestlib)
add_importlibs(kbdmouclass_apitest msvcrt kernel32 ntdll)

add_rostests_file(TARGET kbdmouclass_apitest)
