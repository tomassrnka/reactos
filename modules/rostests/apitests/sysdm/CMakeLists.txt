
list(APPEND SOURCE
    StartupRecovery.c
    testlist.c)

add_executable(sysdm_apitest ${SOURCE})
target_link_libraries(sysdm_apitest wine ${PSEH_LIB})
set_module_type(sysdm_apitest win32cui)
add_importlibs(sysdm_apitest advapi32 user32 msvcrt kernel32)
add_rostests_file(TARGET sysdm_apitest)
