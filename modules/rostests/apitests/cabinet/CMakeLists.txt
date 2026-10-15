list(APPEND SOURCE
    FdiNotify.c
    testlist.c)

add_executable(cabinet_apitest ${SOURCE})
set_module_type(cabinet_apitest win32cui)
add_importlibs(cabinet_apitest cabinet msvcrt kernel32)
add_rostests_file(TARGET cabinet_apitest)
