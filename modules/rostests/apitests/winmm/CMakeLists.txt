
list(APPEND SOURCE
    timeGetTime.c
    testlist.c)

add_executable(winmm_apitest ${SOURCE})
set_module_type(winmm_apitest win32cui)
target_link_libraries(winmm_apitest ${PSEH_LIB})
add_importlibs(winmm_apitest winmm msvcrt kernel32)
add_rostests_file(TARGET winmm_apitest)
