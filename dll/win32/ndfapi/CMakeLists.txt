spec2def(ndfapi.dll ndfapi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    ndfapi_main.c
    ${CMAKE_CURRENT_BINARY_DIR}/ndfapi_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/ndfapi.def)

add_library(ndfapi MODULE ${SOURCE})
set_module_type(ndfapi win32dll UNICODE ENTRYPOINT 0)
target_link_libraries(ndfapi wine)
add_importlibs(ndfapi ws2_32 kernel32 ntdll)
add_dependencies(ndfapi psdk)
add_cd_file(TARGET ndfapi DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(ndfapi)
