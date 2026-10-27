
spec2def(optionalfeatures.cpl optionalfeatures.spec)

list(APPEND SOURCE
    optionalfeatures.c
    optionalfeatures.h)

add_rc_deps(optionalfeatures.rc ${CMAKE_CURRENT_SOURCE_DIR}/resources/optionalfeatures.ico)

add_library(optionalfeatures MODULE
    ${SOURCE}
    optionalfeatures.rc
    ${CMAKE_CURRENT_BINARY_DIR}/optionalfeatures.def)

set_module_type(optionalfeatures cpl UNICODE)
add_importlibs(optionalfeatures user32 shell32 advapi32 msvcrt kernel32)
add_pch(optionalfeatures optionalfeatures.h SOURCE)
add_cd_file(TARGET optionalfeatures DESTINATION reactos/system32 FOR all)
