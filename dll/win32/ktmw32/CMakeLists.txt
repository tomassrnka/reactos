spec2def(ktmw32.dll ktmw32.spec)
add_library(ktmw32 MODULE
    ktmw32.c
    ktmw32.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ktmw32_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/ktmw32.def)
set_module_type(ktmw32 win32dll ENTRYPOINT 0)
add_importlibs(ktmw32 kernel32 ntdll)
add_cd_file(TARGET ktmw32 DESTINATION reactos/system32 FOR all)
