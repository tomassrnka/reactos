
spec2def(tlsdyn.dll tlsdyn.spec)

list(APPEND SOURCE
    tlsdyn.c
    ${CMAKE_CURRENT_BINARY_DIR}/tlsdyn_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/tlsdyn.def)

add_library(tlsdyn MODULE ${SOURCE})
set_module_type(tlsdyn win32dll IMAGEBASE default)
add_importlibs(tlsdyn msvcrt kernel32 ntdll)
add_rostests_file(TARGET tlsdyn RENAME kernel32_tlsdyn.dll)
