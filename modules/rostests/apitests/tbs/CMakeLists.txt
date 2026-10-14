list(APPEND SOURCE
    tbs.c
    testlist.c)

add_executable(tbs_apitest ${SOURCE})
set_module_type(tbs_apitest win32cui)
add_importlibs(tbs_apitest msvcrt kernel32)
add_rostests_file(TARGET tbs_apitest)
