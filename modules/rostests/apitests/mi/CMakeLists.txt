
list(APPEND SOURCE
    mi.c
    testlist.c)

add_executable(mi_apitest ${SOURCE})
set_module_type(mi_apitest win32cui)
add_importlibs(mi_apitest msvcrt kernel32)
add_rostests_file(TARGET mi_apitest)
