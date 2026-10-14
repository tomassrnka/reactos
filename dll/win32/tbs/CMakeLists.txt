add_definitions(-D_TBS_SOURCE_)
spec2def(tbs.dll tbs.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    tbs.c
    ${CMAKE_CURRENT_BINARY_DIR}/tbs_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/tbs.def)

add_library(tbs MODULE ${SOURCE})
set_module_type(tbs win32dll UNICODE ENTRYPOINT 0)
target_link_libraries(tbs wine)
add_importlibs(tbs kernel32 ntdll)
add_dependencies(tbs psdk)
add_cd_file(TARGET tbs DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(tbs)
