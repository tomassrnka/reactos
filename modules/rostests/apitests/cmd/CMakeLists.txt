
list(APPEND SOURCE
    cmd.c
    fc.c
    start.c
    testlist.c)

add_executable(cmd_apitest ${SOURCE})
target_link_libraries(cmd_apitest wine ${PSEH_LIB})
set_module_type(cmd_apitest win32cui)
add_importlibs(cmd_apitest shlwapi msvcrt kernel32)
add_rostests_file(TARGET cmd_apitest)

add_executable(cmd_start_probe start_probe.c)
set_module_type(cmd_start_probe win32cui UNICODE)
add_importlibs(cmd_start_probe msvcrt kernel32)
add_rostests_file(TARGET cmd_start_probe SUBDIR testdata)
add_dependencies(cmd_apitest cmd_start_probe)
