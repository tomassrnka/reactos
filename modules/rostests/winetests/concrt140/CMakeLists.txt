
# Hack to replace the old CRT include directory with the UCRT include directory
get_property(INCLUDE_DIRS DIRECTORY . PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM INCLUDE_DIRS "${REACTOS_SOURCE_DIR}/sdk/include/crt")
set_property(DIRECTORY . PROPERTY INCLUDE_DIRECTORIES ${INCLUDE_DIRS})
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/ucrt)

remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DWINVER=0x601)

add_definitions(
    -D_beginthreadex_start_routine_t=_beginthreadex_proc_type
    -DUSE_WINE_TODOS
    -D_USE_MATH_DEFINES
    -DWINETEST_USE_DBGSTR_LONGLONG
)

list(APPEND SOURCE
    concrt140.c
    testlist.c
)

add_executable(concrt140_winetest ${SOURCE})
if(NOT MSVC)
    target_compile_options(concrt140_winetest PRIVATE -Wno-format)
endif()
set_module_type(concrt140_winetest win32cui)

target_link_libraries(concrt140_winetest wine oldnames ${PSEH_LIB} chkstk)
if(NOT MSVC)
    # Add this manually here, so it can link to ucrtbase.dll and vcstartup
    target_link_libraries(concrt140_winetest -lgcc)
endif()

add_importlibs(concrt140_winetest ucrtbase kernel32 ntdll)
add_rostests_file(TARGET concrt140_winetest)
