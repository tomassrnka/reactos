list(APPEND SOURCE
    wevtapi.c
    testlist.c)

add_executable(wevtapi_apitest ${SOURCE})
set_module_type(wevtapi_apitest win32cui)
add_importlibs(wevtapi_apitest advapi32 msvcrt kernel32)
add_rostests_file(TARGET wevtapi_apitest)
