
spec2def(cng.sys cng.spec)

include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/cryptlib)

list(APPEND SOURCE
    cng.c
    ${CMAKE_CURRENT_BINARY_DIR}/cng.def)

add_library(cng MODULE ${SOURCE})
target_link_libraries(cng cryptlib)
set_module_type(cng kernelmodedriver)
add_importlibs(cng ntoskrnl)
add_cd_file(TARGET cng DESTINATION reactos/system32/drivers NO_CAB FOR all)
