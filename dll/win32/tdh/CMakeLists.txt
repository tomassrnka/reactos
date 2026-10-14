add_definitions(-D_TDH_SOURCE_)
spec2def(tdh.dll tdh.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    tdh_main.c
    ${CMAKE_CURRENT_BINARY_DIR}/tdh_stubs.c)

add_library(tdh MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/tdh.def)

set_module_type(tdh win32dll UNICODE ENTRYPOINT 0)
target_link_libraries(tdh wine)
add_importlibs(tdh kernel32 ntdll)
add_dependencies(tdh psdk)
add_cd_file(TARGET tdh DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(tdh)
