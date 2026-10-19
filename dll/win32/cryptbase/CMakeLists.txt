
spec2def(cryptbase.dll cryptbase.spec)

add_library(cryptbase MODULE
    stubs.c
    cryptbase.rc
    ${CMAKE_CURRENT_BINARY_DIR}/cryptbase.def)

set_module_type(cryptbase win32dll UNICODE)
add_importlibs(cryptbase advapi32 kernel32 ntdll)
add_cd_file(TARGET cryptbase DESTINATION reactos/system32 FOR all)
