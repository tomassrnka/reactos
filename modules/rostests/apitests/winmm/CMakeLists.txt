
list(APPEND SOURCE
    DriverLoad.c
    LoadUnload.c
    MidiMapperLongMsg.c
    timeBeginPeriod.c
    timeGetTime.c
    testlist.c)

add_executable(winmm_apitest ${SOURCE})
set_module_type(winmm_apitest win32cui)
target_link_libraries(winmm_apitest ${PSEH_LIB})
add_importlibs(winmm_apitest advapi32 msvcrt kernel32)
add_delay_importlibs(winmm_apitest winmm)
add_rostests_file(TARGET winmm_apitest)
