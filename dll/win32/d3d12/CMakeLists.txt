
spec2def(d3d12.dll d3d12.spec ADD_IMPORTLIB)

add_library(d3d12 MODULE
    d3d12.c
    d3d12.rc
    ${CMAKE_CURRENT_BINARY_DIR}/d3d12.def)

set_module_type(d3d12 win32dll UNICODE ENTRYPOINT 0)
add_importlibs(d3d12 kernel32 ntdll)
add_cd_file(TARGET d3d12 DESTINATION reactos/system32 FOR all)
