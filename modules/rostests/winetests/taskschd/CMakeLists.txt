
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)
add_definitions(-DUSE_WINE_TODOS)

list(APPEND SOURCE
    scheduler.c
    testlist.c)

add_executable(taskschd_winetest ${SOURCE})
set_module_type(taskschd_winetest win32cui)
add_importlibs(taskschd_winetest advapi32 oleaut32 ole32 msvcrt kernel32)
add_rostests_file(TARGET taskschd_winetest)
