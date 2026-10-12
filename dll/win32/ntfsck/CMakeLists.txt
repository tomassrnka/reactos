
spec2def(ntfsck.dll ntfsck.spec)

list(APPEND SOURCE
    ntfsck.c
    ntfsck.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ntfsck.def)

add_library(ntfsck MODULE ${SOURCE})
set_module_type(ntfsck nativedll)
target_link_libraries(ntfsck ntfsckros ntfscklib ntfsckros ${PSEH_LIB})
add_importlibs(ntfsck ntdll)
add_cd_file(TARGET ntfsck DESTINATION reactos/system32 FOR all)
