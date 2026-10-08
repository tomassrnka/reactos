
remove_definitions(-D_WIN32_WINNT=0x502)

set(BALLOON_DIR ${VIRTIO_WIN_DIR}/Balloon/sys)

list(APPEND SOURCE
    ${BALLOON_DIR}/balloon.c
    ${BALLOON_DIR}/Device.c
    ${BALLOON_DIR}/Driver.c
    ${BALLOON_DIR}/memstat.c
    ${BALLOON_DIR}/queue.c
    ${BALLOON_DIR}/utils.c)

add_library(balloon MODULE ${SOURCE} balloon.rc)
set_module_type(balloon kmdfdriver)
target_include_directories(balloon PRIVATE ${BALLOON_DIR})

if(NOT MSVC)
    target_compile_options(balloon PRIVATE
        -Wno-unused-function
        -Wno-unknown-pragmas
        -Wno-attributes
        -Wno-old-style-declaration)
endif()

target_link_libraries(balloon virtio_wdf ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(balloon wdfldr ntoskrnl hal)
add_cd_file(TARGET balloon DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(balloon balloon.inf)
