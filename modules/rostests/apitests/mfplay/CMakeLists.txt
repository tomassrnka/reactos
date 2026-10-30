
list(APPEND SOURCE
    MfPlayer.c
    testlist.c)

add_executable(mfplay_apitest ${SOURCE})
target_link_libraries(mfplay_apitest wine uuid ${PSEH_LIB})
set_module_type(mfplay_apitest win32cui)
add_importlibs(mfplay_apitest ole32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET mfplay_apitest)
