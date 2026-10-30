
list(APPEND SOURCE
    WTHelperGetFileHash.c
    testlist.c)

add_executable(wintrust_apitest ${SOURCE})
target_link_libraries(wintrust_apitest wine ${PSEH_LIB})
set_module_type(wintrust_apitest win32cui)
add_importlibs(wintrust_apitest msvcrt kernel32 ntdll)
add_rostests_file(TARGET wintrust_apitest)
