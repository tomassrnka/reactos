list(APPEND SOURCE
    craftdlls.c
    testlist.c)

add_executable(craftdlls_apitest ${SOURCE})
set_module_type(craftdlls_apitest win32cui)
add_importlibs(craftdlls_apitest msvcrt kernel32)
add_rostests_file(TARGET craftdlls_apitest)
