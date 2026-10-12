
spec2def(untfs.dll untfs.spec)

list(APPEND SOURCE
    chkdsk.c
    untfs.c
    untfs.rc
    ${CMAKE_CURRENT_BINARY_DIR}/untfs.def)

add_library(untfs MODULE ${SOURCE})
set_module_type(untfs nativedll)
target_link_libraries(untfs ntfslib ${PSEH_LIB})
add_importlibs(untfs ntdll)
add_cd_file(TARGET untfs DESTINATION reactos/system32 FOR all)
