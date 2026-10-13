
spec2def(ktmw32.dll ktmw32.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    ktmw32_main.c
    ${CMAKE_CURRENT_BINARY_DIR}/ktmw32_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/ktmw32.def)

add_library(ktmw32 MODULE ${SOURCE})
set_module_type(ktmw32 win32dll UNICODE ENTRYPOINT 0)
add_importlibs(ktmw32 kernel32 ntdll)
add_cd_file(TARGET ktmw32 DESTINATION reactos/system32 FOR all)
set_wine_module(ktmw32)
