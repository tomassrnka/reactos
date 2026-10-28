
list(APPEND SOURCE
    TaskScheduler.c
    testlist.c)

add_executable(taskschd_apitest ${SOURCE})
set_module_type(taskschd_apitest win32cui)
target_link_libraries(taskschd_apitest uuid)
add_importlibs(taskschd_apitest oleaut32 ole32 msvcrt kernel32)
add_rostests_file(TARGET taskschd_apitest)
