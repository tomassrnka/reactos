
spec2def(vcruntime140.dll vcruntime140.spec ADD_IMPORTLIB)

add_definitions(-D__WINESRC__)
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)

list(APPEND SOURCE
    misc.c
    vcruntime140.rc
    ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140.def)

add_library(vcruntime140 MODULE ${SOURCE})
set_module_type(vcruntime140 win32dll ENTRYPOINT 0)
target_link_libraries(vcruntime140 wine)
add_importlibs(vcruntime140 ucrtbase kernel32 kernel32_vista ntdll)
if(NOT MSVC)
    # binutils write their own version (2.36) into the header; programs such as PHP compare the linker
    # version of the runtime that they find with the one that built them. The version is the one that Wine declares.
    add_custom_command(TARGET vcruntime140 POST_BUILD
        COMMAND native-pefixup --linkerversion=14.50 $<TARGET_FILE:vcruntime140>)
    set_property(TARGET vcruntime140 APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:native-pefixup,IMPORTED_LOCATION>)
endif()
add_cd_file(TARGET vcruntime140 DESTINATION reactos/system32 FOR all)
