
include_directories(../include)

#
# CmFlush
#
list(APPEND CMFLUSH_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    CmFlush_drv.c)

add_library(cmflush_drv MODULE ${CMFLUSH_DRV_SOURCE})
set_module_type(cmflush_drv kernelmodedriver)
target_link_libraries(cmflush_drv kmtest_printf ${PSEH_LIB})
add_importlibs(cmflush_drv ntoskrnl hal)
target_compile_definitions(cmflush_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET cmflush_drv)
