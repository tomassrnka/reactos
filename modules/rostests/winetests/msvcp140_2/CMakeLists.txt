
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
    math.c
    testlist.c
)

add_executable(msvcp140_2_winetest ${SOURCE})
if(NOT MSVC)
    target_compile_options(msvcp140_2_winetest PRIVATE -Wno-format)
endif()
set_module_type(msvcp140_2_winetest win32cui)

target_link_libraries(msvcp140_2_winetest wine oldnames ${PSEH_LIB} chkstk)
if(NOT MSVC)
    # Add this manually here, so it can link to ucrtbase.dll and vcstartup
    target_link_libraries(msvcp140_2_winetest -lgcc)
endif()

add_importlibs(msvcp140_2_winetest ucrtbase kernel32 ntdll)
add_rostests_file(TARGET msvcp140_2_winetest)
