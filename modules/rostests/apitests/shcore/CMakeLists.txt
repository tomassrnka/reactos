
list(APPEND SOURCE
    CommandLineToArgvW.c
    DpiFunctions.c
    StreamFunctions.c
    testlist.c)

add_executable(shcore_apitest ${SOURCE})
set_module_type(shcore_apitest win32cui)
target_link_libraries(shcore_apitest uuid)
add_importlibs(shcore_apitest shlwapi ole32 user32 gdi32 advapi32 msvcrt kernel32)
add_rostests_file(TARGET shcore_apitest)
