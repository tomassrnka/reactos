list(APPEND SOURCE
    ndfapi.c
    testlist.c)

add_executable(ndfapi_apitest ${SOURCE})
set_module_type(ndfapi_apitest win32cui)
add_importlibs(ndfapi_apitest ws2_32 advapi32 msvcrt kernel32)
add_rostests_file(TARGET ndfapi_apitest)
