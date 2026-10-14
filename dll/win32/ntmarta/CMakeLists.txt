
spec2def(ntmarta.dll ntmarta.spec)

list(APPEND SOURCE
    ntmarta.c
    ntmarta.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ntmarta.def)

add_library(ntmarta MODULE ${SOURCE})
set_module_type(ntmarta win32dll UNICODE)
if(SYSWOW64_GUEST)
    # Window-object security is only needed when those APIs are called.
    add_delay_importlibs(ntmarta user32)
else()
    add_importlibs(ntmarta user32)
endif()
add_importlibs(ntmarta advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET ntmarta DESTINATION reactos/system32 FOR all)
