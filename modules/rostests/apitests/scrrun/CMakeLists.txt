
list(APPEND SOURCE
    Drives.c)

add_executable(scrrun_apitest ${SOURCE})
set_module_type(scrrun_apitest win32cui)
target_link_libraries(scrrun_apitest winetestlib uuid)
add_importlibs(scrrun_apitest oleaut32 ole32 msvcrt kernel32)
add_rostests_file(TARGET scrrun_apitest)
